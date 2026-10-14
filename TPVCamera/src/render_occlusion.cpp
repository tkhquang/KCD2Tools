/**
 * @file render_occlusion.cpp
 * @brief Implementation of the render-octree occluder scan (see render_occlusion.hpp).
 */

#include "render_occlusion.hpp"
#include "aob_resolver.hpp"
#include "camera_rules_files.hpp"
#include "constants.hpp"
#include "dev/hook_profile.hpp"
#include "fence_coverage.hpp"
#include "global_state.hpp"
#include "physics_raycast.hpp"
#include "rtti_types.hpp"

#include <DetourModKit.hpp>

#include <windows.h>
#include <xmmintrin.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace TPVCamera
{

    // I3DEngine::GetObjectsInBox(this, const AABB* bbox, IRenderNode** p_out) -> count (see constants.hpp).
    using GetObjectsInBoxFn = std::uint32_t(__fastcall *)(void *p3d_engine, const float *bbox, void **out_list);
    // C3DEngine::GetObjectsByTypeInBox(this, EERType type, const AABB* bbox, IRenderNode** p_out, uint64 rnd_flags)
    // -> count (see constants.hpp).
    using GetObjectsByTypeInBoxFn = std::uint32_t(__fastcall *)(void *p3d_engine, std::uint32_t type, const float *bbox,
                                                                void **out_list, std::uint64_t rnd_flags_mask);
    // IRenderNode vtable methods (stock IShadowCaster slots; see constants.hpp). GetBBox fills the
    // caller buffer and returns a pointer to {Vec3 min @0, Vec3 max @0xC}.
    using GetBBoxFn = void *(__fastcall *)(void *node, void *out_aabb);
    using GetRenderNodeTypeFn = int(__fastcall *)(void *node);
    using GetOwnerEntityFn = void *(__fastcall *)(void *node);
    // IRenderMesh::GetPosPtr(int32& stride, uint32 flags, int32 offset) -> uint8* (vtable slot, see constants).
    // FSL_READ returns the engine-decoded, tightly packed float3 CPU position cache.
    using GetPosPtrFn =
        std::uint8_t *(__fastcall *)(void *render_mesh, int *out_stride, unsigned int flags, int offset);
    // IRenderMesh::GetIndexPtr(uint32 flags, int32 offset) -> vtx_idx* (uint16 triangle index list).
    using GetIndexPtrFn = std::uint16_t *(__fastcall *)(void *render_mesh, unsigned int flags, int offset);

    static GetObjectsInBoxFn s_get_objects_in_box = nullptr;
    // Game image bounds, for screening a render-mesh GetPosPtr function pointer before the indirect call.
    static uintptr_t s_mod_lo = 0;
    static uintptr_t s_mod_hi = 0;
    // Address of the p3DEngine global (a g_env member). Read fresh per query: it is set once the 3DEngine
    // is created and is screened before use; deriving it from the patch-resilient g_env base avoids a
    // second hardcoded address.
    static uintptr_t s_p3d_engine_slot_addr = 0;
    // Mapped range of the scanned game module, captured once. A freshly read p3DEngine must carry a vtable
    // inside this image or it is rejected before the indirect call (branch-only contains() test, no syscall).
    static DMK::Region s_game_module{};

    // The brush-typed octree query, read from the live engine vtable by refresh_brush_query() and cleared when that
    // vtable does not have the expected layout. s_brush_query_vtable is the engine vtable the decision was made for,
    // so it is re-made only when the engine object changes. Main-thread only (the frustum-builder detour runs on the
    // game's main thread, not the renderer's thread).
    static GetObjectsByTypeInBoxFn s_get_objects_by_type_in_box = nullptr;
    static uintptr_t s_brush_query_vtable = 0;

    /**
     * @brief Selects the octree query for the engine vtable @p p3d_vtable: the typed one when its slot sits next to
     *        the resolved GetObjectsInBox, else the untyped one.
     * @details The typed query visits only the requested node type instead of every render node in the box; the
     *          occluder scan issues one per type it keeps, the hit trace one for brushes. Its slot is trusted only
     *          while the neighbouring GetObjectsInBox slot holds the anchored entry, since that pins the slot
     *          numbering; both slots must lie in the game image. The decision is logged when it changes. Called
     *          outside any structured-exception frame, before the guarded queries.
     * @param p3d_vtable The vtable of the engine object read this call (in-image, screened by the caller).
     */
    static void refresh_brush_query(uintptr_t p3d_vtable) noexcept
    {
        if (p3d_vtable == s_brush_query_vtable)
        {
            return;
        }
        s_brush_query_vtable = p3d_vtable;

        GetObjectsByTypeInBoxFn typed = nullptr;
        const auto untyped_slot = DMK::memory::read<uintptr_t>(
            DMK::Address{p3d_vtable + Constants::ENGINE3D_VTABLE_GET_OBJECTS_IN_BOX_OFFSET});
        const auto typed_slot = DMK::memory::read<uintptr_t>(
            DMK::Address{p3d_vtable + Constants::ENGINE3D_VTABLE_GET_OBJECTS_BY_TYPE_IN_BOX_OFFSET});
        if (untyped_slot && *untyped_slot == reinterpret_cast<uintptr_t>(s_get_objects_in_box) && typed_slot &&
            s_game_module.contains(DMK::Address{*typed_slot}))
        {
            typed = reinterpret_cast<GetObjectsByTypeInBoxFn>(*typed_slot);
        }
        s_get_objects_by_type_in_box = typed;

        if (typed != nullptr)
        {
            // format_address builds a std::string, which can throw before try_log runs.
            try
            {
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "RenderOcclusion: typed octree queries through GetObjectsByTypeInBox {}",
                                         DMK::format::format_address(reinterpret_cast<uintptr_t>(typed)));
            }
            catch (...)
            {
            }
        }
        else
        {
            (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                     "RenderOcclusion: GetObjectsByTypeInBox is not next to GetObjectsInBox in the "
                                     "engine vtable; octree queries filter the full untyped query instead");
        }
    }

    /**
     * @brief Fills @p nodes with the render nodes of @p type in @p bbox and returns how many it wrote.
     * @details The typed query when refresh_brush_query() selected it, else the untyped one (whose other node types
     *          the callers' own type filter drops). Both copy their full result without a size limit, so the count is
     *          taken first and the list is filled only when it fits with RENDER_OCCLUSION_NODE_SLACK to spare, covering
     *          nodes registered between the two calls. The return is the FILL call's count (a node removed in between
     *          must not leave a stale slot in range), bounded by the buffer. POD-only: called inside the callers'
     *          structured-exception frames, which contain a fault.
     * @param p3d The engine object.
     * @param type The EERType to collect (ignored by the untyped fallback).
     * @param bbox Query box {min.xyz, max.xyz}.
     * @param nodes Buffer of RENDER_OCCLUSION_MAX_NODES entries.
     * @param overflow Set to true when the box holds more candidates than fit; left untouched otherwise. May be
     *        nullptr.
     * @return The number of entries written, 0 when the box is empty or holds more candidates than fit.
     */
    static std::uint32_t query_nodes(QueryLedger::Tag tag, void *p3d, std::uint32_t type, const float *bbox,
                                     void **nodes, bool *overflow = nullptr) noexcept
    {
        constexpr auto k_capacity = static_cast<std::uint32_t>(Constants::RENDER_OCCLUSION_MAX_NODES);
        constexpr auto k_fit =
            static_cast<std::uint32_t>(Constants::RENDER_OCCLUSION_MAX_NODES - Constants::RENDER_OCCLUSION_NODE_SLACK);
        const GetObjectsByTypeInBoxFn typed = s_get_objects_by_type_in_box;

        const std::int64_t ledger_started = QueryLedger::begin();
        const std::uint32_t count = (typed != nullptr)
                                        ? typed(p3d, type, bbox, nullptr, Constants::ENGINE3D_QUERY_ANY_RNDFLAGS)
                                        : s_get_objects_in_box(p3d, bbox, nullptr);
        if (count == 0 || count > k_fit)
        {
            QueryLedger::end(tag, QueryLedger::Kind::Octree, ledger_started);
            if (overflow != nullptr && count > k_fit)
            {
                *overflow = true;
            }
            return 0;
        }
        const std::uint32_t copied = (typed != nullptr)
                                         ? typed(p3d, type, bbox, nodes, Constants::ENGINE3D_QUERY_ANY_RNDFLAGS)
                                         : s_get_objects_in_box(p3d, bbox, nodes);
        QueryLedger::end(tag, QueryLedger::Kind::Octree, ledger_started, 2);
        return (copied < k_capacity) ? copied : k_capacity;
    }

    /** @brief query_nodes for brushes (EERType 1). */
    static std::uint32_t query_brush_nodes(QueryLedger::Tag tag, void *p3d, const float *bbox, void **nodes) noexcept
    {
        return query_nodes(tag, p3d, static_cast<std::uint32_t>(Constants::EERTYPE_BRUSH), bbox, nodes);
    }

    bool initialize_render_occlusion(uintptr_t module_base, size_t module_size, uintptr_t g_env)
    {
        DMK::Logger &logger = DMK::log();

        const uintptr_t fn = gated_anchor_address(Feature::Occlusion, AnchorId::GetObjectsInBox);
        if (fn == 0 || g_env == 0)
        {
            logger.warning(
                "RenderOcclusion: GetObjectsInBox or g_env not found (game patched?); render occlusion unavailable");
            return false;
        }

        s_get_objects_in_box = reinterpret_cast<GetObjectsInBoxFn>(fn);
        s_p3d_engine_slot_addr = g_env + Constants::GENV_3DENGINE_OFFSET;
        s_game_module = DMK::Region{DMK::Address{module_base}, module_size};
        s_mod_lo = module_base;
        s_mod_hi = module_base + module_size;

        logger.info("RenderOcclusion: GetObjectsInBox at {}, p3DEngine slot at {}", DMK::format::format_address(fn),
                    DMK::format::format_address(s_p3d_engine_slot_addr));
        return true;
    }

    // Probe that the whole uint16 index buffer is actually mapped. Some static meshes return a NON-null but FREED
    // index pointer (the mesh streams its index stream out while keeping positions, so GetIndexPtr != null yet
    // reading it faults), and the stream can come and go from frame to frame (bird_feeder.cgf faults the triangle
    // read on some frames and not on others). Page mapping is 4 KB-granular, so one touch per 4 KB page (2048 uint16)
    // plus the last index proves every page the buffer spans is readable; if all succeed the triangle walk in
    // mesh_crosses cannot fault. POD-only body so its __try carries no object unwinding. False on a fault ->
    // mesh_crosses returns -1, and the sightline scan judges that node by its box alone.
    static bool indices_readable(const std::uint16_t *indices, int n_indices) noexcept
    {
        if (indices == nullptr || n_indices < 3)
        {
            return false;
        }
        __try
        {
            volatile std::uint16_t sink = indices[n_indices - 1];
            for (int i = 0; i < n_indices; i += 2048)
            {
                sink = indices[i];
            }
            (void)sink;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    /**
     * @brief A brush's own sub-object hide mask (see Constants::CBRUSH_HIDEMASK_WORDS_OFFSET): sub-object i is not
     *        drawn while bit i is set. Empty for any other node.
     */
    struct HideMask
    {
        const std::uint32_t *words = nullptr;
        std::uint32_t count = 0;
    };

    /** @brief @p node's own hide mask when it is a brush or a movable brush, else an empty one. POD body. */
    static HideMask hide_mask_of(void *node) noexcept
    {
        void **vt = *reinterpret_cast<void ***>(node);
        const auto get_type =
            reinterpret_cast<GetRenderNodeTypeFn>(vt[Constants::RENDERNODE_VTABLE_GETTYPE_OFFSET / 8]);
        const int type = get_type(node);
        if (type != Constants::EERTYPE_BRUSH && type != Constants::EERTYPE_MOVABLE_BRUSH)
        {
            return {};
        }
        const auto *nb = reinterpret_cast<const std::byte *>(node);
        return {*reinterpret_cast<const std::uint32_t *const *>(nb + Constants::CBRUSH_HIDEMASK_WORDS_OFFSET),
                *reinterpret_cast<const std::uint32_t *>(nb + Constants::CBRUSH_HIDEMASK_COUNT_OFFSET) &
                    Constants::HIDEMASK_COUNT_MASK};
    }

    /**
     * @brief True when compound sub-object @p index (@p so, with child statobj @p child) is drawn in the view, as the
     *        engine's own sub-object render loop decides: it is neither hidden nor a shadow proxy, its statobj is not
     *        flagged STATIC_OBJECT_HIDDEN, and @p mask leaves it shown. POD body.
     */
    static bool subobj_drawn(const std::byte *so, const void *child, int index, const HideMask &mask) noexcept
    {
        const auto flags = *reinterpret_cast<const std::uint32_t *>(so + Constants::SUBOBJ_FLAGS_OFFSET);
        const auto child_flags = *reinterpret_cast<const std::uint32_t *>(static_cast<const std::byte *>(child) +
                                                                          Constants::STATOBJ_FLAGS_OFFSET);
        if ((flags & (Constants::SUBOBJ_FLAG_HIDDEN | Constants::SUBOBJ_FLAG_SHADOW_PROXY)) != 0 ||
            (child_flags & Constants::STATIC_OBJECT_HIDDEN) != 0)
        {
            return false;
        }
        const auto word = static_cast<std::uint32_t>(index) >> 5;
        return mask.words == nullptr || word >= mask.count || ((mask.words[word] >> (index & 31)) & 1u) == 0;
    }

    /**
     * @brief The number of sub-objects walked in the vector [@p begin, @p end), capped at SUBOBJ_MAX before it narrows
     *        to int, so a corrupt end pointer can never wrap into a small positive count.
     */
    static int subobj_count(uintptr_t begin, uintptr_t end) noexcept
    {
        if (end <= begin)
        {
            return 0;
        }
        const uintptr_t count = (end - begin) / Constants::SUBOBJ_STRIDE;
        return static_cast<int>(std::min(count, static_cast<uintptr_t>(Constants::SUBOBJ_MAX)));
    }

    // Compose two row-major Matrix34 affine transforms (each 12 floats, [R|t] with the translation in column 3):
    // out applies b then a, so out * v == a * (b * v). Folds a compound statobj's sub-object local transform
    // into the parent brush world transform before a sub-mesh's vertices are projected.
    static void mat34_compose(const float *a, const float *b, float *out) noexcept
    {
        for (int r = 0; r < 3; ++r)
        {
            const float a0 = a[r * 4 + 0], a1 = a[r * 4 + 1], a2 = a[r * 4 + 2], a3 = a[r * 4 + 3];
            out[r * 4 + 0] = a0 * b[0] + a1 * b[4] + a2 * b[8];
            out[r * 4 + 1] = a0 * b[1] + a1 * b[5] + a2 * b[9];
            out[r * 4 + 2] = a0 * b[2] + a1 * b[6] + a2 * b[10];
            out[r * 4 + 3] = a0 * b[3] + a1 * b[7] + a2 * b[11] + a3;
        }
    }

    // Best-effort, SEH-guarded copy of a brush's full .cgf path, used by the occluder scan to classify it by the camera
    // rules, and by the trace log. The field at
    // statobj + STATOBJ_CGF_NAME_OFFSET is a CryString (a POINTER to the path chars), so it is dereferenced
    // once. POD body; non-printable bytes become '.', so a wrong offset / null / bad pointer logs harmlessly.
    static void copy_brush_name(void *statobj, char *out, int cap) noexcept
    {
        if (cap > 0)
        {
            out[0] = '\0';
        }
        if (statobj == nullptr || cap <= 1)
        {
            return;
        }
        __try
        {
            const char *p = *reinterpret_cast<const char *const *>(reinterpret_cast<std::byte *>(statobj) +
                                                                   Constants::STATOBJ_CGF_NAME_OFFSET);
            if (p == nullptr)
            {
                return;
            }
            int i = 0;
            for (; i < cap - 1; ++i)
            {
                const char c = p[i];
                if (c == '\0')
                {
                    break;
                }
                out[i] = (c >= 32 && c < 127) ? c : '.';
            }
            out[i] = '\0';
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out[0] = '\0';
        }
    }

    // True if brush @p node's render mesh has a vertex within @p eps of the world-space @p hit. Confirms a
    // candidate brush is the one a physics ray ACTUALLY hit (its mesh is there), not merely a neighbour whose
    // bbox contains the point. Reads the render mesh's decoded position buffer (GetPosPtr slot, FSL_READ),
    // transforms by the CBrush world matrix, scans EVERY vertex with an early-out (a sampled scan can miss a thin
    // pole's nearest vertex between the sampled indices; the reads are raw, so a full scan is cheap).
    static bool mesh_has_vertex_near(void *rmesh, const float *M, Vector3 hit, float eps2, uintptr_t mod_lo,
                                     uintptr_t mod_hi) noexcept
    {
        if (rmesh == nullptr)
        {
            return false;
        }
        const int n_verts =
            *reinterpret_cast<int *>(reinterpret_cast<std::byte *>(rmesh) + Constants::RENDERMESH_NVERTS_OFFSET);
        if (n_verts <= 0 || n_verts > Constants::RENDER_OCCLUSION_VERT_MAX)
        {
            return false;
        }
        void **rm_vt = *reinterpret_cast<void ***>(rmesh);
        const auto get_pos = reinterpret_cast<GetPosPtrFn>(rm_vt[Constants::RENDERMESH_VTABLE_GETPOSPTR_OFFSET / 8]);
        const auto get_pos_addr = reinterpret_cast<uintptr_t>(get_pos);
        if (get_pos_addr < mod_lo || get_pos_addr >= mod_hi)
        {
            return false;
        }
        int stride = 0;
        const std::uint8_t *pos = get_pos(rmesh, &stride, Constants::IRENDERMESH_FSL_READ, 0);
        if (pos == nullptr)
        {
            return false;
        }
        if (stride <= 0)
        {
            stride = 12;
        }
        for (int v = 0; v < n_verts; ++v)
        {
            const auto *lp = reinterpret_cast<const float *>(pos + static_cast<size_t>(v) * stride);
            const float lx = lp[0], ly = lp[1], lz = lp[2];
            const float wx = M[0] * lx + M[1] * ly + M[2] * lz + M[3];
            const float wy = M[4] * lx + M[5] * ly + M[6] * lz + M[7];
            const float wz = M[8] * lx + M[9] * ly + M[10] * lz + M[11];
            const float dx = wx - hit.x, dy = wy - hit.y, dz = wz - hit.z;
            if (dx * dx + dy * dy + dz * dz < eps2)
            {
                return true;
            }
        }
        return false;
    }

    // Confirms a candidate brush is the one a physics ray ACTUALLY hit (its mesh is there), not merely a neighbour
    // whose bbox contains the point. Transforms the brush's verts to world and looks for one within @p eps of the
    // hit; for a compound statobj (null root mesh) it checks the child sub-meshes the same way (see
    // the sub-object walk node_crosses uses). POD body (runs in the caller's SEH frame).
    static bool mesh_near_point(void *node, Vector3 hit, float eps, uintptr_t mod_lo, uintptr_t mod_hi) noexcept
    {
        auto *bytes = reinterpret_cast<std::byte *>(node);
        void *statobj = *reinterpret_cast<void **>(bytes + Constants::CBRUSH_STATOBJ_OFFSET);
        if (statobj == nullptr)
        {
            return false;
        }
        const float eps2 = eps * eps;
        const float *brush_m = reinterpret_cast<const float *>(bytes + Constants::CBRUSH_MATRIX_OFFSET);
        void *rmesh =
            *reinterpret_cast<void **>(reinterpret_cast<std::byte *>(statobj) + Constants::STATOBJ_RENDERMESH_OFFSET);
        if (rmesh != nullptr)
        {
            return mesh_has_vertex_near(rmesh, brush_m, hit, eps2, mod_lo, mod_hi);
        }
        // Compound statobj: scan the child sub-meshes (same vector layout node_crosses walks).
        auto *sb = reinterpret_cast<std::byte *>(statobj);
        const uintptr_t self_vt = *reinterpret_cast<uintptr_t *>(sb);
        const uintptr_t begin = *reinterpret_cast<uintptr_t *>(sb + Constants::STATOBJ_SUBOBJ_BEGIN_OFFSET);
        const uintptr_t end = *reinterpret_cast<uintptr_t *>(sb + Constants::STATOBJ_SUBOBJ_END_OFFSET);
        if (begin == 0 || end <= begin || !DMK::memory::is_plausible_ptr(DMK::Address{begin}) ||
            (end - begin) % Constants::SUBOBJ_STRIDE != 0)
        {
            return false;
        }
        const int count = subobj_count(begin, end);
        const HideMask mask = hide_mask_of(node);
        for (int i = 0; i < count; ++i)
        {
            auto *so = reinterpret_cast<std::byte *>(begin + static_cast<uintptr_t>(i) * Constants::SUBOBJ_STRIDE);
            void *child = *reinterpret_cast<void **>(so + Constants::SUBOBJ_PSTATOBJ_OFFSET);
            if (child == nullptr || !DMK::memory::is_plausible_ptr(DMK::Address{reinterpret_cast<uintptr_t>(child)}) ||
                *reinterpret_cast<uintptr_t *>(child) != self_vt || !subobj_drawn(so, child, i, mask))
            {
                continue;
            }
            void *child_rmesh =
                *reinterpret_cast<void **>(reinterpret_cast<std::byte *>(child) + Constants::STATOBJ_RENDERMESH_OFFSET);
            float world_m[12];
            mat34_compose(brush_m, reinterpret_cast<const float *>(so + Constants::SUBOBJ_TM_OFFSET), world_m);
            if (mesh_has_vertex_near(child_rmesh, world_m, hit, eps2, mod_lo, mod_hi))
            {
                return true;
            }
        }
        return false;
    }

    // IRenderNode::GetEntityStatObj(this, sub-part, Matrix34A* world, bool only_visible) -> IStatObj* (see
    // constants.hpp).
    using GetEntityStatObjFn = void *(__fastcall *)(void *node, unsigned int sub_part, float *world, bool only_visible);
    static std::span<const std::uintptr_t> s_player_nodes;

    void set_occluder_player_nodes(std::span<const std::uintptr_t> nodes) noexcept
    {
        s_player_nodes = nodes;
    }

    /**
     * @brief Number of an entity proxy's slots, one for other node types, or -1 when the slot vector is implausible.
     *        Called inside an SEH frame.
     */
    static int visible_slot_count(void *node) noexcept
    {
        if (!vtable_is(GameClass::RenderProxy, *reinterpret_cast<uintptr_t *>(node)))
        {
            return 1;
        }
        const auto *b = static_cast<const std::byte *>(node);
        const auto begin = *reinterpret_cast<const uintptr_t *>(b + Constants::RENDER_PROXY_SLOTS_BEGIN_OFFSET);
        const auto end = *reinterpret_cast<const uintptr_t *>(b + Constants::RENDER_PROXY_SLOTS_END_OFFSET);
        if (end < begin || (end - begin) % sizeof(void *) != 0 ||
            (end - begin) / sizeof(void *) > Constants::OCCLUDER_ENTITY_SLOT_MAX)
        {
            return -1;
        }
        return static_cast<int>((end - begin) / sizeof(void *));
    }

    /**
     * @brief Whether entity proxy @p node draws an animated character slot, or its slots cannot be read: a cached
     *        static mesh cannot represent it. False for other node types. SEH caller.
     */
    static bool has_visible_animation(void *node) noexcept
    {
        if (!vtable_is(GameClass::RenderProxy, *reinterpret_cast<uintptr_t *>(node)))
        {
            return false;
        }
        const int count = visible_slot_count(node);
        if (count < 0)
        {
            return true;
        }
        const auto *slots = *reinterpret_cast<const std::byte *const *const *>(
            static_cast<const std::byte *>(node) + Constants::RENDER_PROXY_SLOTS_BEGIN_OFFSET);
        for (int i = 0; i < count; ++i)
        {
            const auto *slot = slots[i];
            if (slot != nullptr &&
                (*reinterpret_cast<const std::uint32_t *>(slot + Constants::ENTITY_SLOT_FLAGS_OFFSET) &
                 Constants::ENTITY_SLOT_RENDER) != 0 &&
                *reinterpret_cast<void *const *>(slot + Constants::ENTITY_SLOT_CHARACTER_OFFSET) != nullptr)
            {
                return true;
            }
        }
        return false;
    }

    /** @brief Reads a currently rendered static slot, with that slot's actual world transform. SEH caller. */
    static void *visible_statobj(void *node, unsigned int slot, float *world = nullptr) noexcept
    {
        void **vt = *reinterpret_cast<void ***>(node);
        const auto get =
            reinterpret_cast<GetEntityStatObjFn>(vt[Constants::RENDERNODE_VTABLE_GETENTITYSTATOBJ_OFFSET / 8]);
        if (!s_game_module.contains(DMK::Address{reinterpret_cast<uintptr_t>(get)}))
        {
            return nullptr;
        }
        if (!vtable_is(GameClass::RenderProxy, reinterpret_cast<uintptr_t>(vt)))
        {
            return get(node, slot, world, true);
        }
        // Retail GetEntityStatObj always supplies slot 0's matrix. Never use its matrix for another slot.
        void *statobj = get(node, slot, nullptr, true);
        if (statobj == nullptr || world == nullptr)
        {
            return statobj;
        }
        void *owner =
            *reinterpret_cast<void **>(static_cast<std::byte *>(node) + Constants::RENDER_PROXY_ENTITY_OFFSET);
        if (owner == nullptr || !vtable_is(GameClass::Entity, *reinterpret_cast<uintptr_t *>(owner)))
        {
            return nullptr;
        }
        using GetWorldFn = const float *(__fastcall *)(void *, unsigned int);
        const auto get_world = reinterpret_cast<GetWorldFn>(
            (*reinterpret_cast<void ***>(owner))[Constants::ENTITY_VTABLE_GETSLOTWORLDTM_OFFSET / 8]);
        if (!s_game_module.contains(DMK::Address{reinterpret_cast<uintptr_t>(get_world)}))
        {
            return nullptr;
        }
        const float *matrix = get_world(owner, slot | Constants::ENTITY_SLOT_ACTUAL);
        if (matrix == nullptr)
        {
            return nullptr;
        }
        std::copy_n(matrix, 12, world);
        return statobj;
    }

    /**
     * @brief The one static model @p node draws, with its world matrix in @p world, or nullptr when it draws none,
     *        several or an animated one. A fence verdict is shared per model, so mixed geometry keeps its collision.
     *        SEH caller.
     */
    static void *single_visible_statobj(void *node, float *world) noexcept
    {
        if (has_visible_animation(node))
        {
            return nullptr;
        }
        void *result = nullptr;
        const int count = visible_slot_count(node);
        for (int slot = 0; slot < count; ++slot)
        {
            void *model = visible_statobj(node, static_cast<unsigned int>(slot));
            if (model == nullptr)
            {
                continue;
            }
            if (result != nullptr)
            {
                return nullptr;
            }
            result = visible_statobj(node, static_cast<unsigned int>(slot), world);
            if (result == nullptr)
            {
                return nullptr;
            }
        }
        return result;
    }

    /**
     * @brief Whether @p node is one of the player's nodes, or an entity proxy whose entity or an attachment parent of
     *        it is a registered actor. Decided by the actor registry and the attachment parents, never by the model
     *        name. SEH caller.
     */
    static bool entity_is_actor_or_attachment(void *node) noexcept
    {
        if (std::find(s_player_nodes.begin(), s_player_nodes.end(), reinterpret_cast<uintptr_t>(node)) !=
            s_player_nodes.end())
        {
            return true;
        }
        if (!vtable_is(GameClass::RenderProxy, *reinterpret_cast<uintptr_t *>(node)))
        {
            return false;
        }
        const auto slot = g_global_context_ptr_address.load(std::memory_order_relaxed);
        const auto *context = slot != nullptr ? *reinterpret_cast<const std::byte *const *>(slot) : nullptr;
        if (context == nullptr)
        {
            return false;
        }
        void *actors = *reinterpret_cast<void *const *>(context + Constants::CONTEXT_ACTOR_SYSTEM_OFFSET);
        if (actors == nullptr || !vtable_is(GameClass::ActorSystem, *reinterpret_cast<uintptr_t *>(actors)))
        {
            return false;
        }
        using GetActorFn = void *(__fastcall *)(void *, std::uint32_t);
        const auto get_actor = reinterpret_cast<GetActorFn>(
            (*reinterpret_cast<void ***>(actors))[Constants::ACTOR_SYSTEM_VTABLE_GET_ACTOR_OFFSET / 8]);
        if (!s_game_module.contains(DMK::Address{reinterpret_cast<uintptr_t>(get_actor)}))
        {
            return false;
        }
        void *entity =
            *reinterpret_cast<void **>(static_cast<std::byte *>(node) + Constants::RENDER_PROXY_ENTITY_OFFSET);
        for (int depth = 0; depth < 8 && entity != nullptr; ++depth)
        {
            void **vt = *reinterpret_cast<void ***>(entity);
            if (!vtable_is(GameClass::Entity, reinterpret_cast<uintptr_t>(vt)))
            {
                return false;
            }
            const auto id = *reinterpret_cast<const std::uint32_t *>(static_cast<const std::byte *>(entity) +
                                                                     Constants::ENTITY_ID_OFFSET);
            if (id != 0 && get_actor(actors, id) != nullptr)
            {
                return true;
            }
            using GetParentFn = void *(__fastcall *)(void *);
            const auto get_parent = reinterpret_cast<GetParentFn>(vt[Constants::ENTITY_VTABLE_GETPARENT_OFFSET / 8]);
            if (!s_game_module.contains(DMK::Address{reinterpret_cast<uintptr_t>(get_parent)}))
            {
                return false;
            }
            void *parent = get_parent(entity);
            if (parent == entity)
            {
                return false;
            }
            entity = parent;
        }
        return false;
    }

    /** @brief The engine revision of a static model, read inside the caller's SEH frame. */
    static std::uint32_t statobj_revision(const void *statobj) noexcept
    {
        return *reinterpret_cast<const std::uint32_t *>(static_cast<const std::byte *>(statobj) +
                                                        Constants::STATOBJ_MODIFICATION_ID_OFFSET);
    }

    /** @brief One render node the occluder scan found around the camera arm. */
    struct ScannedNode
    {
        void *node = nullptr;
        // The node's vtable when it was scanned. A node that no longer carries it was freed and is skipped.
        uintptr_t vtable = 0;
        float bbox[6] = {}; // world AABB {min.xyz, max.xyz}
        OccluderClass cls = OccluderClass::Structure;
        // An entity's render proxy (a cart, a door, a sign, a character): a prop one fades only on an exact mesh
        // crossing.
        bool entity = false;
        bool movable = false;
        bool brush = false;
        // The camera rule's verdict: cls is its behaviour, or its outcome when its check fires. A verdict with the
        // mesh option (buildings) lets the drawn walls and roof stop the camera inside the physics, while the
        // ground, banks and rocks are drawn where their physics is.
        Rules::Verdict verdict{};
        // An entity proxy's CEntity (GetOwnerEntity), which the entity's physics names as its foreign data.
        uintptr_t owner = 0;
        // Pending fences keep normal collision until a complete coverage measurement permits fading.
        int wall_slot = -1;
        std::uint32_t owner_id = 0;
        const void *model = nullptr;
        std::uint32_t model_revision = 0;
        // A brush's material (CBRUSH_MATERIAL_OFFSET), which decides its no-draw state: a new one re-classifies it.
        const void *material = nullptr;
    };

    // Fence height only rejects low models. Upper-body coverage distinguishes broad walls with seams from low
    // panels with tall posts. One cached-mesh row is measured per game frame, across all pending fences together.
    constexpr float k_wall_min_height = 1.8f;
    constexpr float k_wall_probe_pad = 0.05f;
    constexpr int k_wall_work_budget = 8192;
    constexpr std::int64_t k_wall_slice_us = 200;
    // Wall verdicts kept, one per model as it is placed: its statobj and .cgf path, the scale it is placed at and the
    // parts the brush hides. A fence model repeats along a whole street, so it is tested once; a brush given another
    // model or another hide mask (a game event dressing a tent) is a different entry and is tested again.
    constexpr int k_wall_verdicts = 64;

    struct WallVerdict
    {
        const void *statobj = nullptr;
        const void *path = nullptr;
        std::uint64_t model_state = 0;
        std::uint64_t mask = 0;
        float up = 0.0f;
        float along = 0.0f;
        float across = 0.0f;
        FenceCoverage::Measurement coverage{};
        std::uint64_t mesh_signature = 0;
        std::uint64_t retry_ms = 0;
        std::uint64_t retry_builds = 0;
        std::uint64_t started_ms = 0;
        std::uint32_t slices = 0;
        std::uint32_t yields = 0;
        std::uint32_t waits = 0;
        bool narrow_probes = false;
    };
    static WallVerdict s_wall_verdicts[k_wall_verdicts];
    static int s_wall_next = 0;
    static int s_wall_cursor = 0;

    /** @brief The class the scan stores for a camera rule's behaviour (Exclude is never stored: it is not scanned). */
    static OccluderClass class_of(Rules::Behaviour behaviour) noexcept
    {
        switch (behaviour)
        {
        case Rules::Behaviour::Fade:
            return OccluderClass::Prop;
        case Rules::Behaviour::Bush:
            return OccluderClass::Vegetation;
        case Rules::Behaviour::Tree:
            return OccluderClass::Tree;
        case Rules::Behaviour::Grass:
            return OccluderClass::Grass;
        case Rules::Behaviour::Frame:
            return OccluderClass::Frame;
        case Rules::Behaviour::Pass:
            return OccluderClass::Invisible;
        case Rules::Behaviour::Character:
            return OccluderClass::Character;
        case Rules::Behaviour::Collide:
        case Rules::Behaviour::Exclude:
            break;
        }
        return OccluderClass::Structure;
    }

    /**
     * @brief Whether render mesh @p rmesh has a piece drawn two-sided (MTL_FLAG_2SIDED: a sheet of cloth), or its piece
     *        list cannot be read. POD body (runs in the caller's SEH frame).
     */
    static bool mesh_two_sided(const std::byte *rmesh) noexcept
    {
        const auto *chunks = *reinterpret_cast<const std::byte *const *>(rmesh + Constants::RENDERMESH_CHUNKS_OFFSET);
        if (chunks == nullptr)
        {
            return true;
        }
        const int count = *reinterpret_cast<const int *>(chunks - Constants::DYNARRAY_COUNT_BACK);
        if (count <= 0 || count > Constants::RENDERMESH_CHUNKS_MAX)
        {
            return true;
        }
        for (int i = 0; i < count; ++i)
        {
            const auto flags = *reinterpret_cast<const std::uint16_t *>(
                chunks + static_cast<std::size_t>(i) * Constants::RENDERCHUNK_STRIDE +
                Constants::RENDERCHUNK_MATFLAGS_OFFSET);
            if ((flags & Constants::MTL_FLAG_2SIDED) != 0)
            {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Whether @p node draws anything two-sided: a piece of its render mesh, or of a drawn sub-mesh of a compound
     *        statobj. True when it cannot be read, so a tent whose pieces are unknown keeps its canvas. POD-only body
     *        under SEH.
     */
    static bool draws_two_sided_guarded(void *node) noexcept
    {
        __try
        {
            void **vt = *reinterpret_cast<void ***>(node);
            const auto get_stat_obj =
                reinterpret_cast<GetEntityStatObjFn>(vt[Constants::RENDERNODE_VTABLE_GETENTITYSTATOBJ_OFFSET / 8]);
            const auto fn_addr = reinterpret_cast<uintptr_t>(get_stat_obj);
            if (fn_addr < s_mod_lo || fn_addr >= s_mod_hi)
            {
                return true;
            }
            void *statobj = nullptr;
            const int slots = visible_slot_count(node);
            for (int slot = 0; slot < slots && statobj == nullptr; ++slot)
            {
                statobj = visible_statobj(node, static_cast<unsigned int>(slot));
            }
            if (statobj == nullptr)
            {
                return true;
            }
            auto *sb = reinterpret_cast<std::byte *>(statobj);
            const auto *rmesh = *reinterpret_cast<const std::byte *const *>(sb + Constants::STATOBJ_RENDERMESH_OFFSET);
            if (rmesh != nullptr)
            {
                return mesh_two_sided(rmesh);
            }
            const uintptr_t self_vt = *reinterpret_cast<uintptr_t *>(sb);
            const uintptr_t begin = *reinterpret_cast<uintptr_t *>(sb + Constants::STATOBJ_SUBOBJ_BEGIN_OFFSET);
            const uintptr_t end = *reinterpret_cast<uintptr_t *>(sb + Constants::STATOBJ_SUBOBJ_END_OFFSET);
            if (begin == 0 || end <= begin || (end - begin) % Constants::SUBOBJ_STRIDE != 0)
            {
                return true;
            }
            const int count = subobj_count(begin, end);
            const HideMask mask = hide_mask_of(node);
            bool drawn = false;
            for (int i = 0; i < count; ++i)
            {
                const auto *so =
                    reinterpret_cast<const std::byte *>(begin + static_cast<uintptr_t>(i) * Constants::SUBOBJ_STRIDE);
                void *child = *reinterpret_cast<void *const *>(so + Constants::SUBOBJ_PSTATOBJ_OFFSET);
                if (child == nullptr || *reinterpret_cast<uintptr_t *>(child) != self_vt ||
                    !subobj_drawn(so, child, i, mask))
                {
                    continue;
                }
                const auto *child_rmesh = *reinterpret_cast<const std::byte *const *>(
                    reinterpret_cast<std::byte *>(child) + Constants::STATOBJ_RENDERMESH_OFFSET);
                if (child_rmesh == nullptr)
                {
                    continue;
                }
                drawn = true;
                if (mesh_two_sided(child_rmesh))
                {
                    return true;
                }
            }
            return !drawn;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return true;
        }
    }

    /**
     * @brief Whether brush @p node is drawn with one material that is a collision proxy or draws nothing (see
     *        Constants::CBRUSH_MATERIAL_OFFSET): an invisible blocker. POD-only body under SEH: false on a fault.
     */
    static bool brush_invisible_guarded(void *node) noexcept
    {
        __try
        {
            const auto *mtl = *reinterpret_cast<const std::byte *const *>(reinterpret_cast<std::byte *>(node) +
                                                                          Constants::CBRUSH_MATERIAL_OFFSET);
            if (mtl == nullptr)
            {
                return false;
            }
            const auto flags = *reinterpret_cast<const std::uint32_t *>(mtl + Constants::MATINFO_FLAGS_OFFSET);
            return (flags & Constants::MTL_FLAG_MULTI_SUBMTL) == 0 &&
                   (flags & (Constants::MTL_FLAG_COLLISION_PROXY | Constants::MTL_FLAG_NODRAW)) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    const char *occluder_class_name(OccluderClass cls) noexcept
    {
        switch (cls)
        {
        case OccluderClass::Prop:
            return "prop";
        case OccluderClass::Vegetation:
            return "vegetation";
        case OccluderClass::Tree:
            return "tree";
        case OccluderClass::Grass:
            return "grass";
        case OccluderClass::Frame:
            return "frame";
        case OccluderClass::Character:
            return "character";
        case OccluderClass::Invisible:
            return "invisible";
        case OccluderClass::Structure:
            break;
        }
        return "structure";
    }

    // The cached scan. Main-thread only (the camera frame), so plain statics are race-free.
    static ScannedNode s_scan[Constants::OCCLUDER_SCAN_MAX];
    static int s_scan_count = 0;
    static float s_scan_box[6] = {};
    // When the scan was taken (GetTickCount64), so a scan from before a pause, a first-person stretch or a load is
    // never reused.
    static std::uint64_t s_scan_stamp_ms = 0;
    static bool s_scan_valid = false;
    // The rule set the scan was classified with: its verdicts index its rules, and a set of another generation takes a
    // new scan. A scan with a new set reports a class change once (advance_fence_classification()).
    static std::shared_ptr<const Rules::RuleSet> s_scan_rules;
    static bool s_scan_rules_changed = false;

    // The per-object rules trace (Trace level): each (model, kind, state) once per rule set. The scan and the refresh
    // run inside structured-exception frames, so they only queue a new verdict; refresh_occluder_scan() logs it after.
    static_assert((Constants::RULES_TRACE_SET & (Constants::RULES_TRACE_SET - 1)) == 0,
                  "The rules trace set's size must be a power of two.");
    struct PendingVerdict
    {
        char name[160];
        Rules::Kind kind;
        Rules::State state;
        Rules::Verdict verdict;
    };
    static std::uint64_t s_traced[Constants::RULES_TRACE_SET] = {};
    static int s_traced_count = 0;
    static std::uint64_t s_traced_generation = 0;
    static bool s_traced_full = false;
    static PendingVerdict s_trace_queue[32];
    static int s_trace_queued = 0;
    // The first scanned names of an unusual form (Rules::log_name_form()), logged after the scan.
    struct PendingForm
    {
        char name[160];
        Rules::NameForm form;
        std::uint32_t count;
    };
    static PendingForm s_form_queue[6];
    static int s_forms_queued = 0;

    static void name_of_node_guarded(void *node, char *out, int cap) noexcept;
    static bool fence_is_wall_guarded(void *node, int &slot) noexcept;
    static bool node_alive(const ScannedNode &s) noexcept;

    /** @brief Copies the C string @p name (at most 159 bytes) into @p out. POD body. */
    static void copy_scan_name(const char *name, char *out) noexcept
    {
        int i = 0;
        for (; i < 159 && name[i] != '\0'; ++i)
        {
            out[i] = name[i];
        }
        out[i] = '\0';
    }

    /**
     * @brief Queues the trace line of a verdict whose (model, kind, state) this rule set has not listed yet. A full
     *        queue leaves it unmarked for a later scan. POD body (the scan's and the refresh's SEH frames call it).
     */
    static void queue_verdict_trace(const char *name, Rules::Kind kind, Rules::State state,
                                    const Rules::Verdict &verdict) noexcept
    {
        if (s_trace_queued >= static_cast<int>(std::size(s_trace_queue)) ||
            s_traced_count * 4 >= Constants::RULES_TRACE_SET * 3)
        {
            return;
        }
        std::uint64_t hash =
            0xCBF29CE484222325ull ^ ((static_cast<std::uint64_t>(kind) << 8) | static_cast<std::uint64_t>(state));
        for (int i = 0; i < 159 && name[i] != '\0'; ++i)
        {
            const char c = (name[i] >= 'A' && name[i] <= 'Z') ? static_cast<char>(name[i] + 32) : name[i];
            hash = (hash ^ static_cast<unsigned char>(c)) * 0x100000001B3ull;
        }
        hash |= 1; // 0 marks a free slot
        constexpr std::size_t k_mask = Constants::RULES_TRACE_SET - 1;
        std::size_t at = static_cast<std::size_t>(hash >> 17) & k_mask;
        while (s_traced[at] != 0)
        {
            if (s_traced[at] == hash)
            {
                return;
            }
            at = (at + 1) & k_mask;
        }
        s_traced[at] = hash;
        ++s_traced_count;
        PendingVerdict &pending = s_trace_queue[s_trace_queued++];
        copy_scan_name(name, pending.name);
        pending.kind = kind;
        pending.state = state;
        pending.verdict = verdict;
    }

    /** @brief Counts a scanned name with a backslash or a leading '/', and queues the first ones. POD body. */
    static void note_name_forms(const char *name) noexcept
    {
        const auto note = [name](Rules::NameForm form)
        {
            const std::uint32_t count = Rules::count_name_form(Rules::NameSite::Scan, form);
            if (count <= 3 && s_forms_queued < static_cast<int>(std::size(s_form_queue)))
            {
                PendingForm &pending = s_form_queue[s_forms_queued++];
                copy_scan_name(name, pending.name);
                pending.form = form;
                pending.count = count;
            }
        };
        if (name[0] == '/')
        {
            note(Rules::NameForm::LeadingSlash);
        }
        for (int i = 0; i < 159 && name[i] != '\0'; ++i)
        {
            if (name[i] == '\\')
            {
                note(Rules::NameForm::Backslash);
                break;
            }
        }
    }

    /** @brief Logs what the last scan or refresh queued for the rules trace and the name-form log. */
    static void flush_rules_trace(const Rules::RuleSet &rules) noexcept
    {
        for (int i = 0; i < s_forms_queued; ++i)
        {
            Rules::log_name_form(Rules::NameSite::Scan, s_form_queue[i].form, s_form_queue[i].name,
                                 s_form_queue[i].count);
        }
        s_forms_queued = 0;
        for (int i = 0; i < s_trace_queued; ++i)
        {
            const PendingVerdict &pending = s_trace_queue[i];
            Rules::log_verdict(rules, pending.name, pending.kind, pending.state, pending.verdict);
        }
        s_trace_queued = 0;
        if (!s_traced_full && s_traced_count * 4 >= Constants::RULES_TRACE_SET * 3)
        {
            s_traced_full = true;
            (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                     "Rules: the trace has listed {} models; it lists no more until the rules change",
                                     s_traced_count);
        }
    }

    /**
     * @brief Refreshes moving bounds, the current fence and cloth checks and the entities' verdicts without another
     *        octree query.
     * @details An entity is named and classified again (its visible model, actor and animation can change), and one
     *          the rules now exclude leaves the scan. Brushes keep their verdict and re-run only its check.
     * @return False when a brush's model, its revision or its material changed: the scan must be taken again.
     */
    static bool refresh_scanned_state(const Rules::RuleSet &rules, bool trace) noexcept
    {
        int kept = 0;
        for (int i = 0; i < s_scan_count; ++i)
        {
            ScannedNode node = s_scan[i];
            if (!node_alive(node))
            {
                continue;
            }
            __try
            {
                if (node.brush)
                {
                    const auto *bytes = static_cast<const std::byte *>(node.node);
                    const auto *model =
                        *reinterpret_cast<const void *const *>(bytes + Constants::CBRUSH_STATOBJ_OFFSET);
                    if (model != node.model || (model != nullptr && statobj_revision(model) != node.model_revision) ||
                        *reinterpret_cast<const void *const *>(bytes + Constants::CBRUSH_MATERIAL_OFFSET) !=
                            node.material)
                    {
                        // Replacement / stream commit / a material change can change the verdict and bounds. Re-query
                        // only on a change, keeping unchanged brush checks to three field reads without rescanning
                        // their geometry.
                        return false;
                    }
                }
                if (node.entity || node.movable)
                {
                    void **vt = *reinterpret_cast<void ***>(node.node);
                    const auto get_bbox =
                        reinterpret_cast<GetBBoxFn>(vt[Constants::RENDERNODE_VTABLE_GETBBOX_OFFSET / 8]);
                    if (!s_game_module.contains(DMK::Address{reinterpret_cast<uintptr_t>(get_bbox)}))
                    {
                        continue;
                    }
                    float box[6] = {};
                    const auto *bounds = static_cast<const float *>(get_bbox(node.node, box));
                    if (bounds == nullptr)
                    {
                        continue;
                    }
                    std::copy_n(bounds, 6, node.bbox);
                    if (node.entity)
                    {
                        node.wall_slot = -1;
                        char name[160] = {};
                        name_of_node_guarded(node.node, name, sizeof(name));
                        const Rules::State state = entity_is_actor_or_attachment(node.node) ? Rules::State::Actor
                                                   : has_visible_animation(node.node)       ? Rules::State::Animated
                                                                                            : Rules::State::None;
                        node.verdict = rules.classify(name, Rules::Kind::Entity, state);
                        if (trace)
                        {
                            queue_verdict_trace(name, Rules::Kind::Entity, state, node.verdict);
                        }
                        if (node.verdict.behaviour == Rules::Behaviour::Exclude)
                        {
                            continue;
                        }
                        node.cls = class_of(node.verdict.behaviour);
                    }
                }
                if (node.verdict.check == Rules::Check::Wall)
                {
                    node.cls = class_of(fence_is_wall_guarded(node.node, node.wall_slot) ? node.verdict.outcome
                                                                                         : node.verdict.behaviour);
                }
                else if (node.verdict.check == Rules::Check::Bare)
                {
                    node.cls =
                        class_of(draws_two_sided_guarded(node.node) ? node.verdict.behaviour : node.verdict.outcome);
                }
                s_scan[kept++] = node;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                // An unreadable candidate cannot justify stepping past native collision.
            }
        }
        s_scan_count = kept;
        return true;
    }

    /**
     * @brief Collects the visible brushes, vegetation and entity proxies in @p qbox into @p out, each with the verdict
     *        of its camera rule and its class.
     * @details One typed octree query per type when the engine vtable has the typed query, else one untyped query
     *          whose other node types are dropped here. Hidden nodes (ERF_HIDDEN) and the nodes a rule excludes (the
     *          shipped rules exclude the HLOD stand-ins of whole blocks of buildings) are skipped. A node's state comes
     *          from engine facts in a fixed order: a merged patch, a brush drawn with one no-draw material, an actor
     *          or what is attached to one, an entity that plays an animation, vegetation without a model path.
     *          Vegetation and entities are taken first: they are few, and the brushes, which fill a town, then take the
     *          rest of the table. POD-only body: a fault ends the scan with what was collected.
     * @param truncated Set when a type had more nodes than the octree query or the table holds.
     * @return The number of nodes written.
     */
    static int scan_nodes_guarded(void *p3d, const float *qbox, const Rules::RuleSet &rules, bool trace,
                                  ScannedNode *out, int cap, bool *truncated) noexcept
    {
        int n = 0;
        __try
        {
            void *nodes[Constants::RENDER_OCCLUSION_MAX_NODES];
            const bool typed = s_get_objects_by_type_in_box != nullptr;
            static constexpr std::uint32_t k_types[] = {static_cast<std::uint32_t>(Constants::EERTYPE_VEGETATION),
                                                        static_cast<std::uint32_t>(Constants::EERTYPE_MERGED_MESH),
                                                        static_cast<std::uint32_t>(Constants::EERTYPE_ENTITY_PROXY),
                                                        static_cast<std::uint32_t>(Constants::EERTYPE_MOVABLE_BRUSH),
                                                        static_cast<std::uint32_t>(Constants::EERTYPE_BRUSH)};
            const int passes = typed ? static_cast<int>(std::size(k_types)) : 1;
            for (int pass = 0; pass < passes; ++pass)
            {
                const std::uint32_t count =
                    query_nodes(QueryLedger::Tag::OccluderScan, p3d, k_types[pass], qbox, nodes, truncated);
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    if (n >= cap)
                    {
                        *truncated = true;
                        break;
                    }
                    void *node = nodes[i];
                    if (node == nullptr)
                    {
                        continue;
                    }
                    const unsigned int rnd_flags = *reinterpret_cast<unsigned int *>(
                        reinterpret_cast<std::byte *>(node) + Constants::RENDERNODE_RNDFLAGS_OFFSET);
                    if (rnd_flags & Constants::ERF_HIDDEN)
                    {
                        continue;
                    }
                    void **vt = *reinterpret_cast<void ***>(node);
                    const auto get_type =
                        reinterpret_cast<GetRenderNodeTypeFn>(vt[Constants::RENDERNODE_VTABLE_GETTYPE_OFFSET / 8]);
                    const int type = get_type(node);
                    const bool brush = type == Constants::EERTYPE_BRUSH || type == Constants::EERTYPE_MOVABLE_BRUSH;
                    const bool entity = type == Constants::EERTYPE_ENTITY_PROXY;
                    const bool merged = type == Constants::EERTYPE_MERGED_MESH;
                    if (!brush && !entity && !merged && type != Constants::EERTYPE_VEGETATION)
                    {
                        continue;
                    }
                    const auto get_bbox =
                        reinterpret_cast<GetBBoxFn>(vt[Constants::RENDERNODE_VTABLE_GETBBOX_OFFSET / 8]);
                    float aabb[6] = {};
                    const float *b = reinterpret_cast<const float *>(get_bbox(node, aabb));
                    if (b == nullptr)
                    {
                        continue;
                    }
                    char name[160];
                    name[0] = '\0';
                    if (merged)
                    {
                        // A merged patch holds many plants and no single mesh (its GetEntityStatObj returns null).
                    }
                    else if (brush)
                    {
                        copy_brush_name(*reinterpret_cast<void **>(reinterpret_cast<std::byte *>(node) +
                                                                   Constants::CBRUSH_STATOBJ_OFFSET),
                                        name, static_cast<int>(sizeof(name)));
                    }
                    else
                    {
                        // Name a current visible model for diagnostics / asset categories. Actor identity below
                        // comes from the engine actor registry and attachment parents, not from this name.
                        name_of_node_guarded(node, name, static_cast<int>(sizeof(name)));
                    }
                    if (name[0] != '\0')
                    {
                        note_name_forms(name);
                    }
                    const Rules::Kind kind = merged                                     ? Rules::Kind::Merged
                                             : type == Constants::EERTYPE_BRUSH         ? Rules::Kind::Brush
                                             : type == Constants::EERTYPE_MOVABLE_BRUSH ? Rules::Kind::Movable
                                             : entity                                   ? Rules::Kind::Entity
                                                                                        : Rules::Kind::Vegetation;
                    Rules::State state = Rules::State::None;
                    if (merged)
                    {
                        state = Rules::State::Merged;
                    }
                    else if (brush && brush_invisible_guarded(node))
                    {
                        state = Rules::State::NoDraw;
                    }
                    else if (entity && entity_is_actor_or_attachment(node))
                    {
                        state = Rules::State::Actor;
                    }
                    else if (entity && has_visible_animation(node))
                    {
                        state = Rules::State::Animated;
                    }
                    else if (kind == Rules::Kind::Vegetation && name[0] == '\0')
                    {
                        state = Rules::State::Unnamed;
                    }
                    const Rules::Verdict verdict = rules.classify(name, kind, state);
                    if (trace)
                    {
                        queue_verdict_trace(name, kind, state, verdict);
                    }
                    if (verdict.behaviour == Rules::Behaviour::Exclude)
                    {
                        continue;
                    }
                    // The check a rule asks for picks its behaviour or its outcome: a fence or gate no one sees over
                    // (a wall), a tent model that draws no cloth (a frame, a pole, a rope).
                    OccluderClass cls = class_of(verdict.behaviour);
                    int wall_slot = -1;
                    if (verdict.check == Rules::Check::Wall)
                    {
                        if (fence_is_wall_guarded(node, wall_slot))
                        {
                            cls = class_of(verdict.outcome);
                        }
                    }
                    else if (verdict.check == Rules::Check::Bare && !draws_two_sided_guarded(node))
                    {
                        cls = class_of(verdict.outcome);
                    }
                    ScannedNode &s = out[n++];
                    s.wall_slot = wall_slot;
                    s.node = node;
                    s.vtable = reinterpret_cast<uintptr_t>(vt);
                    s.cls = cls;
                    s.verdict = verdict;
                    s.entity = entity;
                    s.movable = type == Constants::EERTYPE_MOVABLE_BRUSH;
                    s.brush = brush;
                    s.model = brush ? *reinterpret_cast<const void *const *>(static_cast<const std::byte *>(node) +
                                                                             Constants::CBRUSH_STATOBJ_OFFSET)
                                    : nullptr;
                    s.model_revision = s.model != nullptr ? statobj_revision(s.model) : 0;
                    s.material = brush ? *reinterpret_cast<const void *const *>(static_cast<const std::byte *>(node) +
                                                                                Constants::CBRUSH_MATERIAL_OFFSET)
                                       : nullptr;
                    s.owner = 0;
                    s.owner_id = 0;
                    if (entity)
                    {
                        const auto get_owner = reinterpret_cast<GetOwnerEntityFn>(
                            vt[Constants::RENDERNODE_VTABLE_GETOWNERENTITY_OFFSET / 8]);
                        const auto fn_addr = reinterpret_cast<uintptr_t>(get_owner);
                        if (fn_addr >= s_mod_lo && fn_addr < s_mod_hi)
                        {
                            s.owner = reinterpret_cast<uintptr_t>(get_owner(node));
                            if (s.owner != 0 && vtable_is(GameClass::Entity, *reinterpret_cast<uintptr_t *>(s.owner)))
                            {
                                s.owner_id =
                                    *reinterpret_cast<const std::uint32_t *>(s.owner + Constants::ENTITY_ID_OFFSET);
                            }
                        }
                    }
                    for (int k = 0; k < 6; ++k)
                    {
                        s.bbox[k] = b[k];
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return n;
    }

    void refresh_occluder_scan(const Vector3 *points, int n_points)
    {
        DMK_PROFILE_SCOPE("render.occluder_scan");
        const std::shared_ptr<const Rules::RuleSet> rules = Rules::current();
        if (s_get_objects_in_box == nullptr || s_p3d_engine_slot_addr == 0 || n_points <= 0 || rules == nullptr)
        {
            s_scan_count = 0;
            s_scan_valid = false;
            return;
        }
        const bool trace = DMK::log().is_enabled(DMK::LogLevel::Trace);
        if (trace && s_traced_generation != rules->generation())
        {
            std::fill(std::begin(s_traced), std::end(s_traced), 0);
            s_traced_count = 0;
            s_traced_full = false;
            s_traced_generation = rules->generation();
        }
        float want[6] = {points[0].x, points[0].y, points[0].z, points[0].x, points[0].y, points[0].z};
        bool finite = true;
        for (int i = 0; i < n_points; ++i)
        {
            finite = finite && std::isfinite(points[i].x) && std::isfinite(points[i].y) && std::isfinite(points[i].z);
            want[0] = std::min(want[0], points[i].x);
            want[1] = std::min(want[1], points[i].y);
            want[2] = std::min(want[2], points[i].z);
            want[3] = std::max(want[3], points[i].x);
            want[4] = std::max(want[4], points[i].y);
            want[5] = std::max(want[5], points[i].z);
        }
        // std::min and std::max drop a NaN after the first point, and a NaN box defeats the octree's overlap tests, so
        // a non-finite point drops the scan (every hit then collides) instead of querying an unbounded box.
        if (!finite)
        {
            s_scan_count = 0;
            s_scan_valid = false;
            return;
        }
        // Reuse the scan while every point is still inside the scanned box and it is young enough: the nodes near a
        // walking camera stay the same for a while, and the octree query is the expensive part.
        constexpr float k_inner = 0.25f; // keep this much of the margin in hand, for the triangles near the edge
        const std::uint64_t now_ms = GetTickCount64();
        const bool young =
            now_ms - s_scan_stamp_ms < static_cast<std::uint64_t>(Constants::OCCLUDER_SCAN_MAX_AGE * 1000.0f);
        // A scan classified with another rule set is never reused: every node takes the new rules.
        const bool same_rules = s_scan_rules != nullptr && s_scan_rules->generation() == rules->generation();
        if (s_scan_valid && young && same_rules && want[0] > s_scan_box[0] + k_inner &&
            want[1] > s_scan_box[1] + k_inner && want[2] > s_scan_box[2] + k_inner &&
            want[3] < s_scan_box[3] - k_inner && want[4] < s_scan_box[4] - k_inner && want[5] < s_scan_box[5] - k_inner)
        {
            DMK_PROFILE_SCOPE("render.instance_refresh");
            const bool reused = refresh_scanned_state(*rules, trace);
            flush_rules_trace(*rules);
            if (reused)
            {
                return;
            }
        }

        s_scan_count = 0;
        s_scan_valid = false;
        const auto p3d = DMK::memory::read<uintptr_t>(DMK::Address{s_p3d_engine_slot_addr});
        if (!p3d || *p3d == 0 || !DMK::memory::is_plausible_ptr(DMK::Address{*p3d}))
        {
            return;
        }
        const auto vtable = DMK::memory::read<uintptr_t>(DMK::Address{*p3d});
        if (!vtable || !DMK::memory::is_plausible_ptr(DMK::Address{*vtable}) ||
            !s_game_module.contains(DMK::Address{*vtable}))
        {
            return;
        }
        refresh_brush_query(*vtable);
        const float m = Constants::OCCLUDER_SCAN_MARGIN;
        float qbox[6] = {want[0] - m, want[1] - m, want[2] - m, want[3] + m, want[4] + m, want[5] + m};
        bool truncated = false;
        s_scan_count = scan_nodes_guarded(reinterpret_cast<void *>(*p3d), qbox, *rules, trace, s_scan,
                                          Constants::OCCLUDER_SCAN_MAX, &truncated);
        if (!same_rules)
        {
            s_scan_rules_changed = s_scan_rules != nullptr;
            s_scan_rules = rules;
        }
        flush_rules_trace(*rules);
        for (int k = 0; k < 6; ++k)
        {
            s_scan_box[k] = qbox[k];
        }
        // A truncated scan holds only part of the box, so it is not reused: the next refresh queries again.
        s_scan_stamp_ms = truncated ? 0 : now_ms;
        if (truncated)
        {
            HookProfile::count(HookProfile::Counter::ScanTruncated);
        }
        s_scan_valid = true;
        static bool s_logged_truncation = false;
        if (truncated && !s_logged_truncation)
        {
            s_logged_truncation = true;
            DMK::log().warning("RenderOcclusion: the occluder scan around the camera holds more render nodes than it "
                               "keeps ({} kept); an object left out may not fade, a plant or prop left out stops "
                               "the camera like a wall, and the see-through may miss it",
                               s_scan_count);
        }
    }

    bool read_collider_box(uintptr_t collider, float *out) noexcept
    {
        // A terrain hit carries no collider: screen it out before the read instead of faulting into the handler.
        if (out == nullptr || !DMK::memory::is_plausible_ptr(DMK::Address{collider}))
        {
            return false;
        }
        __try
        {
            const auto *mn = reinterpret_cast<const float *>(collider + Constants::PHYS_ENTITY_BBOX_MIN_OFFSET);
            const auto *mx = reinterpret_cast<const float *>(collider + Constants::PHYS_ENTITY_BBOX_MAX_OFFSET);
            for (int k = 0; k < 3; ++k)
            {
                out[k] = mn[k];
                out[k + 3] = mx[k];
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return out[3] >= out[0] && out[4] >= out[1] && out[5] >= out[2] && (out[3] - out[0]) < 1e4f &&
               (out[4] - out[1]) < 1e4f && (out[5] - out[2]) < 1e4f;
    }

    /** @brief Screens current visibility and entity identity as well as the scanned vtable. False on a fault. */
    static bool node_alive(const ScannedNode &s) noexcept
    {
        __try
        {
            const auto *b = static_cast<const std::byte *>(s.node);
            if (*reinterpret_cast<const uintptr_t *>(b) != s.vtable ||
                (*reinterpret_cast<const std::uint32_t *>(b + Constants::RENDERNODE_RNDFLAGS_OFFSET) &
                 Constants::ERF_HIDDEN) != 0)
            {
                return false;
            }
            return !s.entity ||
                   (*reinterpret_cast<const uintptr_t *>(b + Constants::RENDER_PROXY_ENTITY_OFFSET) == s.owner &&
                    s.owner != 0 &&
                    *reinterpret_cast<const std::uint32_t *>(s.owner + Constants::ENTITY_ID_OFFSET) == s.owner_id);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void *occluder_node_for_collider(uintptr_t collider, OccluderClass *cls) noexcept
    {
        if (collider == 0 || !s_scan_valid || s_scan_count == 0)
        {
            return nullptr;
        }
        const auto found = [cls](const ScannedNode &s) -> void *
        {
            if (cls != nullptr)
            {
                *cls = s.cls;
            }
            return s.node;
        };
        // A brush the engine physicalized at runtime (a cart, a basket, furniture) or a tree names its own render node
        // in the collider's foreign data: that is the exact owner, so no box comparison is needed.
        if (const uintptr_t linked = static_brush_render_node(collider); linked != 0)
        {
            for (int i = 0; i < s_scan_count; ++i)
            {
                const ScannedNode &s = s_scan[i];
                if (reinterpret_cast<uintptr_t>(s.node) == linked)
                {
                    return node_alive(s) ? found(s) : nullptr;
                }
            }
            return nullptr; // owned by a node the scan dropped (hidden, an HLOD): a collision
        }
        const auto foreign_id =
            DMK::memory::read<std::int16_t>(DMK::Address{collider + Constants::PHYS_ENTITY_FOREIGN_TYPE_OFFSET});
        // An entity's physics (a pickable sack, a crate or a cart placed as an entity, a door) names its CEntity: the
        // scanned entity proxy owned by that entity is its node, and its class comes from its model as for a brush.
        if (foreign_id && *foreign_id == Constants::PHYS_FOREIGN_ID_ENTITY)
        {
            const auto owner =
                DMK::memory::read<uintptr_t>(DMK::Address{collider + Constants::PHYS_ENTITY_FOREIGN_DATA_OFFSET});
            if (!owner || *owner == 0)
            {
                return nullptr;
            }
            for (int i = 0; i < s_scan_count; ++i)
            {
                const ScannedNode &s = s_scan[i];
                if (s.entity && s.owner == *owner)
                {
                    return node_alive(s) ? found(s) : nullptr;
                }
            }
            return nullptr; // owned by an entity the scan does not hold: a collision
        }
        // Box matching names the owner of baked level geometry only (the static foreign id with no link). Anything else
        // unlinked has an owner the box test cannot name, so it is a collision.
        float cb[6];
        if (!foreign_id || *foreign_id != Constants::PHYS_FOREIGN_ID_STATIC || !read_collider_box(collider, cb))
        {
            return nullptr;
        }
        // Pad both boxes a little so a flat collider (a wall plane, a board) has a volume to compare.
        constexpr float k_pad = 0.05f;
        for (int k = 0; k < 3; ++k)
        {
            cb[k] -= k_pad;
            cb[k + 3] += k_pad;
        }
        const float collider_volume = (cb[3] - cb[0]) * (cb[4] - cb[1]) * (cb[5] - cb[2]);
        // The collider belongs to the smallest scanned node that holds at least half of its box. A post's collider
        // sits inside its own brush; a building's collider spans the building and never fits inside a window frame
        // on its wall, so a hit on the wall never borrows the frame's node. Trees never compete: a tree's trunk
        // collider names its tree through the link, and a crown's box can hold a whole shed whose own node the scan
        // missed, which would let the camera through the shed. Nor do grass patches, which have no physics and whose
        // 16 m box holds any rock beside them.
        const ScannedNode *best = nullptr;
        float best_volume = 1e30f;
        for (int i = 0; i < s_scan_count; ++i)
        {
            const ScannedNode &s = s_scan[i];
            if (s.cls == OccluderClass::Vegetation || s.cls == OccluderClass::Tree || s.cls == OccluderClass::Grass ||
                s.cls == OccluderClass::Character)
            {
                continue;
            }
            const float ox = std::min(cb[3], s.bbox[3] + k_pad) - std::max(cb[0], s.bbox[0] - k_pad);
            const float oy = std::min(cb[4], s.bbox[4] + k_pad) - std::max(cb[1], s.bbox[1] - k_pad);
            const float oz = std::min(cb[5], s.bbox[5] + k_pad) - std::max(cb[2], s.bbox[2] - k_pad);
            if (ox <= 0.0f || oy <= 0.0f || oz <= 0.0f || ox * oy * oz < 0.5f * collider_volume)
            {
                continue;
            }
            const float volume = (s.bbox[3] - s.bbox[0] + 2.0f * k_pad) * (s.bbox[4] - s.bbox[1] + 2.0f * k_pad) *
                                 (s.bbox[5] - s.bbox[2] + 2.0f * k_pad);
            if (volume < best_volume && node_alive(s))
            {
                best = &s;
                best_volume = volume;
            }
        }
        return (best != nullptr) ? found(*best) : nullptr;
    }

    /**
     * @brief Where segment @p a -> @p b enters @p box, as a fraction of the segment (0 when @p a is inside it), or -1
     *        when it misses the box (slab test).
     */
    static float segment_box_entry(const Vector3 &a, const Vector3 &b, const float *box) noexcept
    {
        const float o[3] = {a.x, a.y, a.z};
        const float d[3] = {b.x - a.x, b.y - a.y, b.z - a.z};
        float t0 = 0.0f, t1 = 1.0f;
        for (int k = 0; k < 3; ++k)
        {
            if (std::fabs(d[k]) < 1e-9f)
            {
                if (o[k] < box[k] || o[k] > box[k + 3])
                {
                    return -1.0f;
                }
                continue;
            }
            const float inv = 1.0f / d[k];
            float ta = (box[k] - o[k]) * inv;
            float tb = (box[k + 3] - o[k]) * inv;
            if (ta > tb)
            {
                std::swap(ta, tb);
            }
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            if (t0 > t1)
            {
                return -1.0f;
            }
        }
        return t0;
    }

    /** @brief True when segment @p a -> @p b passes through @p box (slab test). */
    static bool segment_hits_box(const Vector3 &a, const Vector3 &b, const float *box) noexcept
    {
        return segment_box_entry(a, b, box) >= 0.0f;
    }

    /** @brief Inverse of a row-major affine Matrix34 (translation in column 3). False when it is singular. */
    static bool mat34_inverse(const float *m, float *out) noexcept
    {
        const float a = m[0], b = m[1], c = m[2];
        const float d = m[4], e = m[5], f = m[6];
        const float g = m[8], h = m[9], i = m[10];
        const float co0 = e * i - f * h, co1 = f * g - d * i, co2 = d * h - e * g;
        const float det = a * co0 + b * co1 + c * co2;
        if (std::fabs(det) < 1e-12f)
        {
            return false;
        }
        const float inv = 1.0f / det;
        out[0] = co0 * inv;
        out[1] = (c * h - b * i) * inv;
        out[2] = (b * f - c * e) * inv;
        out[4] = co1 * inv;
        out[5] = (a * i - c * g) * inv;
        out[6] = (c * d - a * f) * inv;
        out[8] = co2 * inv;
        out[9] = (b * g - a * h) * inv;
        out[10] = (a * e - b * d) * inv;
        const float tx = m[3], ty = m[7], tz = m[11];
        out[3] = -(out[0] * tx + out[1] * ty + out[2] * tz);
        out[7] = -(out[4] * tx + out[5] * ty + out[6] * tz);
        out[11] = -(out[8] * tx + out[9] * ty + out[10] * tz);
        return true;
    }

    // Lines one mesh test takes at once: the three sightlines, the camera arm's centre and four offsets, the six
    // envelope probes, or the nine rays across the near end of the view.
    static constexpr int k_max_lines = 9;

    /** @brief A sightline in a mesh's local space: origin, full-length direction, and the last counted fraction. */
    struct LocalLine
    {
        float o[3];
        float d[3];
        float s_max;
    };

    // Triangle-run boxes per render mesh, with a copy of its positions. A mesh's first test reads its positions once
    // (GetPosPtr decodes the mesh's whole vertex stream on every call), keeps a tightly packed copy, and records the
    // mesh-space box of each run of k_run_tris consecutive triangles. Every later test reads only the runs whose box
    // meets the lines' box, from the copy, so a house of a hundred thousand triangles costs a few hundred triangle
    // tests instead of a pass over all of them, and no decode. An entry includes the owning statobj and its native
    // geometry revision, plus mesh / index pointers and counts. A stream commit or geometry replacement invalidates
    // copied positions even when buffer addresses and counts stay unchanged. Each entry's boxes and positions share
    // one process-heap block, so the entries and
    // their bytes are bounded together (OCCLUDER_MESH_CACHE_ENTRIES, OCCLUDER_MESH_CACHE_BYTES), and the least
    // recently used entries make room. Main-thread only (the camera frame), and plain data with Win32 heap calls,
    // which raise no C++ exception, so it is safe inside the callers' structured-exception frames.
    constexpr int k_run_tris = 64;
    constexpr int k_run_max = 4096; // 262,144 triangles; a larger mesh is tested without the runs
    constexpr int k_run_cache_entries = Constants::OCCLUDER_MESH_CACHE_ENTRIES;
    struct RunCache
    {
        const void *rmesh = nullptr;
        const void *statobj = nullptr;
        std::uint32_t revision = 0;
        const void *idx = nullptr;
        int n_verts = 0;
        int n_indices = 0;
        int n_runs = 0;
        std::uint64_t last_use = 0;
        float *box = nullptr;       // n_runs boxes of six floats (min xyz, max xyz), then the positions
        const float *pos = nullptr; // n_verts positions of three floats and one zero float, inside the same block
        std::size_t bytes = 0;      // the block's size
    };
    static RunCache s_run_cache[k_run_cache_entries];
    static std::uint64_t s_run_clock = 0;
    static std::size_t s_run_cache_bytes = 0;
    // Meshes measured so far, for prefetch_occluder_meshes: it measures nothing in a frame that already measured one.
    static std::uint64_t s_mesh_builds = 0;

    /** @brief True when a mesh of @p n_indices indices has few enough triangles for the run boxes. */
    static bool fits_run_cache(int n_indices) noexcept
    {
        return (n_indices / 3 + k_run_tris - 1) / k_run_tris <= k_run_max;
    }

    /** @brief The cached run boxes of a mesh, or nullptr when they have not been measured. */
    static const RunCache *run_cache_find(const void *statobj, const void *rmesh, const std::uint16_t *indices,
                                          int n_verts, int n_indices) noexcept
    {
        ++s_run_clock;
        const std::uint32_t revision = statobj_revision(statobj);
        for (RunCache &c : s_run_cache)
        {
            if (c.statobj == statobj && c.revision == revision && c.rmesh == rmesh && c.idx == indices &&
                c.n_verts == n_verts && c.n_indices == n_indices)
            {
                c.last_use = s_run_clock;
                return &c;
            }
        }
        return nullptr;
    }

    /** @brief Empties @p c and returns its block to the process heap. */
    static void run_cache_release(RunCache &c) noexcept
    {
        c.rmesh = nullptr;
        if (c.box != nullptr)
        {
            (void)HeapFree(GetProcessHeap(), 0, c.box);
            s_run_cache_bytes -= c.bytes;
        }
        c.box = nullptr;
        c.pos = nullptr;
        c.bytes = 0;
    }

    /**
     * @brief Copies @p n_verts positions into @p copy (three floats each, then one zero float, so a four-float load of
     *        the last position stays inside it) and writes the box of each of the @p n_runs runs of k_run_tris
     *        triangles into @p boxes (six floats each).
     * @details A run's box is kept in SSE registers, one pair per triangle corner so the three min/max chains run side
     *          by side, and written once at the run's end. A load's fourth lane (the next position's x) is ignored.
     *          The position is the first operand of each min and max, so a NaN coordinate is skipped.
     */
    static void fill_runs(float *boxes, float *copy, const std::uint8_t *pos, int stride, const std::uint16_t *indices,
                          int n_verts, int n_tris, int n_runs) noexcept
    {
        DMK_PROFILE_SCOPE("render.mesh_runs");
        for (int v = 0; v < n_verts; ++v)
        {
            const auto *p = reinterpret_cast<const float *>(pos + static_cast<size_t>(v) * stride);
            copy[v * 3 + 0] = p[0];
            copy[v * 3 + 1] = p[1];
            copy[v * 3 + 2] = p[2];
        }
        copy[static_cast<std::size_t>(n_verts) * 3] = 0.0f;
        const auto take = [copy, n_verts](int i, __m128 &lo, __m128 &hi)
        {
            if (i < n_verts)
            {
                const __m128 p = _mm_loadu_ps(copy + static_cast<std::size_t>(i) * 3);
                lo = _mm_min_ps(p, lo);
                hi = _mm_max_ps(p, hi);
            }
        };
        for (int r = 0; r < n_runs; ++r)
        {
            __m128 lo0 = _mm_set1_ps(1e30f);
            __m128 lo1 = lo0;
            __m128 lo2 = lo0;
            __m128 hi0 = _mm_set1_ps(-1e30f);
            __m128 hi1 = hi0;
            __m128 hi2 = hi0;
            const int t_end = std::min(n_tris, (r + 1) * k_run_tris);
            for (int t = r * k_run_tris; t < t_end; ++t)
            {
                const std::uint16_t *tri = indices + static_cast<std::size_t>(t) * 3;
                take(tri[0], lo0, hi0);
                take(tri[1], lo1, hi1);
                take(tri[2], lo2, hi2);
            }
            alignas(16) float lo[4];
            alignas(16) float hi[4];
            _mm_store_ps(lo, _mm_min_ps(_mm_min_ps(lo0, lo1), lo2));
            _mm_store_ps(hi, _mm_max_ps(_mm_max_ps(hi0, hi1), hi2));
            float *b = boxes + static_cast<std::size_t>(r) * 6;
            b[0] = lo[0];
            b[1] = lo[1];
            b[2] = lo[2];
            b[3] = hi[0];
            b[4] = hi[1];
            b[5] = hi[2];
        }
    }

    /**
     * @brief Measures the run boxes of a mesh and copies its positions into the least recently used entry, freeing
     *        older entries until the copy fits OCCLUDER_MESH_CACHE_BYTES. The mesh must fit (fits_run_cache). POD body
     *        (runs in the caller's SEH frame); the entry is keyed only once it is fully measured, so a fault while
     *        measuring leaves no half-built entry, and its block stays with the entry for the next build to free.
     * @return The entry, or nullptr when the process heap refused the block (the caller then tests the mesh without
     *         the runs).
     */
    static const RunCache *run_cache_build(const void *statobj, const void *rmesh, const std::uint8_t *pos, int stride,
                                           const std::uint16_t *indices, int n_verts, int n_indices) noexcept
    {
        const int n_tris = n_indices / 3;
        const int n_runs = (n_tris + k_run_tris - 1) / k_run_tris;
        const std::size_t bytes =
            (static_cast<std::size_t>(n_runs) * 6 + static_cast<std::size_t>(n_verts) * 3 + 1) * sizeof(float);
        const auto least_recent = []
        {
            RunCache *oldest = &s_run_cache[0];
            for (RunCache &c : s_run_cache)
            {
                if (c.last_use < oldest->last_use)
                {
                    oldest = &c;
                }
            }
            return oldest;
        };
        RunCache *slot = least_recent();
        run_cache_release(*slot);
        while (s_run_cache_bytes + bytes > Constants::OCCLUDER_MESH_CACHE_BYTES && s_run_cache_bytes != 0)
        {
            RunCache *older = nullptr;
            for (RunCache &c : s_run_cache)
            {
                if (&c != slot && c.box != nullptr && (older == nullptr || c.last_use < older->last_use))
                {
                    older = &c;
                }
            }
            if (older == nullptr)
            {
                break;
            }
            run_cache_release(*older);
        }
        RunCache &c = *slot;
        c.box = static_cast<float *>(HeapAlloc(GetProcessHeap(), 0, bytes));
        if (c.box == nullptr)
        {
            return nullptr;
        }
        c.bytes = bytes;
        s_run_cache_bytes += bytes;
        float *copy = c.box + static_cast<std::size_t>(n_runs) * 6;
        fill_runs(c.box, copy, pos, stride, indices, n_verts, n_tris, n_runs);
        c.pos = copy;
        c.idx = indices;
        c.n_verts = n_verts;
        c.n_indices = n_indices;
        c.n_runs = n_runs;
        c.last_use = s_run_clock;
        c.statobj = statobj;
        c.revision = statobj_revision(statobj);
        c.rmesh = rmesh;
        ++s_mesh_builds;
        return &c;
    }

    /** @brief The position reader and the index list of a render mesh whose counts were checked. */
    struct MeshReader
    {
        GetPosPtrFn get_pos = nullptr;
        const std::uint16_t *indices = nullptr;
    };

    /** @brief True when @p n_verts and @p n_indices are counts the tests can read (16-bit indices, sane caps). */
    static bool mesh_counts_valid(int n_verts, int n_indices) noexcept
    {
        return n_verts > 0 && n_verts <= 65535 && n_indices >= 3 && n_indices <= Constants::RENDER_OCCLUSION_INDEX_MAX;
    }

    /**
     * @brief Fills @p out with @p rmesh's position reader and readable index list. The reader and the index call must
     *        lie in the game image. POD body (runs in the caller's SEH frame).
     * @return False when the mesh cannot be read.
     */
    static bool read_mesh(void *rmesh, int n_indices, uintptr_t mod_lo, uintptr_t mod_hi, MeshReader &out) noexcept
    {
        void **rm_vt = *reinterpret_cast<void ***>(rmesh);
        const auto get_pos = reinterpret_cast<GetPosPtrFn>(rm_vt[Constants::RENDERMESH_VTABLE_GETPOSPTR_OFFSET / 8]);
        const auto get_idx =
            reinterpret_cast<GetIndexPtrFn>(rm_vt[Constants::RENDERMESH_VTABLE_GETINDEXPTR_OFFSET / 8]);
        const auto get_pos_addr = reinterpret_cast<uintptr_t>(get_pos);
        const auto get_idx_addr = reinterpret_cast<uintptr_t>(get_idx);
        if (get_pos_addr < mod_lo || get_pos_addr >= mod_hi || get_idx_addr < mod_lo || get_idx_addr >= mod_hi)
        {
            return false;
        }
        const std::uint16_t *indices = get_idx(rmesh, Constants::IRENDERMESH_FSL_READ, 0);
        if (!indices_readable(indices, n_indices))
        {
            return false;
        }
        out.get_pos = get_pos;
        out.indices = indices;
        return true;
    }

    /** @brief The engine's decoded positions of @p rmesh and their stride, or nullptr. Decodes the whole stream. */
    static const std::uint8_t *decode_positions(void *rmesh, GetPosPtrFn get_pos, int &stride) noexcept
    {
        DMK_PROFILE_SCOPE("render.mesh_decode");
        stride = 0;
        const auto *pos = get_pos(rmesh, &stride, Constants::IRENDERMESH_FSL_READ, 0);
        if (stride <= 0)
        {
            stride = 12;
        }
        return pos;
    }

    /**
     * @brief The fence classification's limit on a mesh test: a QueryPerformanceCounter deadline, and a hash of the
     *        cached meshes the test read. The camera's own tests pass none: they have the triangle budget alone, and
     *        they can measure a mesh into the run cache.
     */
    struct MeshTestLimit
    {
        std::int64_t deadline = 0;
        // The identity of every cached mesh the test read, so probes of different mesh snapshots never combine.
        std::uint64_t signature = 0xCBF29CE484222325ull;
        // The test stopped short (the deadline, an unmeasured or unreadable mesh): its result proves nothing.
        bool incomplete = false;
        bool deadline_reached = false;
    };

    /** @brief Whether @p limit is spent, marking it incomplete once its deadline has passed. False without a limit. */
    static bool mesh_limit_reached(MeshTestLimit *limit) noexcept
    {
        if (limit == nullptr)
        {
            return false;
        }
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        limit->deadline_reached = limit->deadline_reached || now.QuadPart >= limit->deadline;
        limit->incomplete = limit->incomplete || limit->deadline_reached;
        return limit->incomplete;
    }

    /**
     * @brief Tests @p n_lines local-space sightlines against one render mesh's triangles.
     * @details The sightlines were moved into the mesh's local space once, so no vertex is transformed: an affine map
     *          keeps a segment a segment with the same parameter. Only the triangle runs whose box meets the lines' box
     *          are read (see RunCache), and a per-vertex outcode against a box aligned with the lines drops most of
     *          their triangles before the two-sided Moller-Trumbore test. Spends @p budget (a full triangle test is one
     *          unit, a run box, a vertex or a rejected triangle an eighth). With @p nearest (one entry per line) the
     *          test runs through every triangle and lowers each line's entry to the smallest fraction at which it
     *          crosses one, instead of stopping at the first crossing. POD body (runs in the caller's SEH frame).
     * @return 1 when a sightline crosses a triangle, 0 when none does or the budget ran out, -1 when the mesh cannot be
     *         read (no positions or no readable index list).
     */
    static int mesh_crosses(void *statobj, void *rmesh, const LocalLine *lines, int n_lines, int &budget,
                            uintptr_t mod_lo, uintptr_t mod_hi, float *nearest = nullptr,
                            MeshTestLimit *limit = nullptr) noexcept
    {
        if (rmesh == nullptr)
        {
            return -1;
        }
        const int n_verts =
            *reinterpret_cast<int *>(reinterpret_cast<std::byte *>(rmesh) + Constants::RENDERMESH_NVERTS_OFFSET);
        const int n_indices =
            *reinterpret_cast<int *>(reinterpret_cast<std::byte *>(rmesh) + Constants::RENDERMESH_NINDICES_OFFSET);
        if (!mesh_counts_valid(n_verts, n_indices))
        {
            return -1;
        }
        if (budget <= 0 || mesh_limit_reached(limit))
        {
            return 0;
        }
        MeshReader reader{};
        if (!read_mesh(rmesh, n_indices, mod_lo, mod_hi, reader))
        {
            return -1;
        }
        const std::uint16_t *indices = reader.indices;
        // The positions are fetched only when a triangle has to be read, from the cached copy when the mesh was
        // measured, else from the engine, which decodes the mesh's whole vertex stream on every call.
        int stride = 0;
        const std::uint8_t *pos = nullptr;
        const auto fetch_positions = [&]
        {
            if (pos == nullptr)
            {
                pos = decode_positions(rmesh, reader.get_pos, stride);
            }
            return pos != nullptr;
        };
        const auto vtx = [&](int idx)
        { return reinterpret_cast<const float *>(pos + static_cast<size_t>(idx) * stride); };

        // Trivial reject first. The box around the lines is aligned with them (one axis along the first line, two
        // across it), so for an arm or a few sightlines it is a thin slab however the lines lie, small next to a house
        // or a tree; a triangle whose three vertices all lie past the same face of it cannot meet a line.
        float axis[3][3]; // the box axes: along the first line, then two across it
        {
            const float *d = lines[0].d;
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len > 1e-6f)
            {
                axis[0][0] = d[0] / len;
                axis[0][1] = d[1] / len;
                axis[0][2] = d[2] / len;
            }
            else
            {
                axis[0][0] = 1.0f;
                axis[0][1] = 0.0f;
                axis[0][2] = 0.0f;
            }
            const float seed[3] = {std::fabs(axis[0][2]) < 0.9f ? 0.0f : 1.0f, 0.0f,
                                   std::fabs(axis[0][2]) < 0.9f ? 1.0f : 0.0f};
            float a[3] = {axis[0][1] * seed[2] - axis[0][2] * seed[1], axis[0][2] * seed[0] - axis[0][0] * seed[2],
                          axis[0][0] * seed[1] - axis[0][1] * seed[0]};
            const float a_len = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            for (int k = 0; k < 3; ++k)
            {
                axis[1][k] = a[k] / a_len;
            }
            axis[2][0] = axis[0][1] * axis[1][2] - axis[0][2] * axis[1][1];
            axis[2][1] = axis[0][2] * axis[1][0] - axis[0][0] * axis[1][2];
            axis[2][2] = axis[0][0] * axis[1][1] - axis[0][1] * axis[1][0];
        }
        const auto project = [&axis](const float *q, int k)
        { return q[0] * axis[k][0] + q[1] * axis[k][1] + q[2] * axis[k][2]; };
        // The aligned box, and the plain axis-aligned box of the lines, which every crossing lies in (for the runs).
        float box[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
        float aabb[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
        for (int l = 0; l < n_lines; ++l)
        {
            const LocalLine &L = lines[l];
            const float tip[3] = {L.o[0] + L.d[0] * L.s_max, L.o[1] + L.d[1] * L.s_max, L.o[2] + L.d[2] * L.s_max};
            for (int k = 0; k < 3; ++k)
            {
                const float a = project(L.o, k);
                const float b = project(tip, k);
                box[k] = std::min({box[k], a, b});
                box[k + 3] = std::max({box[k + 3], a, b});
                aabb[k] = std::min({aabb[k], L.o[k], tip[k]});
                aabb[k + 3] = std::max({aabb[k + 3], L.o[k], tip[k]});
            }
        }
        constexpr float k_box_pad = 0.01f;
        for (int k = 0; k < 3; ++k)
        {
            box[k] -= k_box_pad;
            box[k + 3] += k_box_pad;
            aabb[k] -= k_box_pad;
            aabb[k + 3] += k_box_pad;
        }
        const auto outcode = [&](const float *p)
        {
            const float q0 = project(p, 0);
            const float q1 = project(p, 1);
            const float q2 = project(p, 2);
            return static_cast<std::uint8_t>((q0 < box[0] ? 1 : 0) | (q0 > box[3] ? 2 : 0) | (q1 < box[1] ? 4 : 0) |
                                             (q1 > box[4] ? 8 : 0) | (q2 < box[2] ? 16 : 0) | (q2 > box[5] ? 32 : 0));
        };

        // The two-sided Moller-Trumbore test of one triangle against every line. True when the caller can stop: a
        // crossing, with no nearest to fill.
        bool crossed = false;
        const auto test_triangle = [&](const float *v0, const float *v1, const float *v2)
        {
            const float e1[3] = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]};
            const float e2[3] = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};
            for (int l = 0; l < n_lines; ++l)
            {
                const LocalLine &L = lines[l];
                const float p[3] = {L.d[1] * e2[2] - L.d[2] * e2[1], L.d[2] * e2[0] - L.d[0] * e2[2],
                                    L.d[0] * e2[1] - L.d[1] * e2[0]};
                const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
                if (std::fabs(det) < 1e-12f)
                {
                    continue;
                }
                const float inv = 1.0f / det;
                const float tv[3] = {L.o[0] - v0[0], L.o[1] - v0[1], L.o[2] - v0[2]};
                const float u = (tv[0] * p[0] + tv[1] * p[1] + tv[2] * p[2]) * inv;
                if (u < 0.0f || u > 1.0f)
                {
                    continue;
                }
                const float q[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2],
                                    tv[0] * e1[1] - tv[1] * e1[0]};
                const float v = (L.d[0] * q[0] + L.d[1] * q[1] + L.d[2] * q[2]) * inv;
                if (v < 0.0f || u + v > 1.0f)
                {
                    continue;
                }
                const float s = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
                if (s >= 0.0f && s <= L.s_max)
                {
                    if (nearest == nullptr)
                    {
                        return true;
                    }
                    crossed = true;
                    nearest[l] = std::min(nearest[l], s);
                }
            }
            return false;
        };

        const int n_tris = n_indices / 3;
        int rejected = 0;
        // A rejected triangle costs an eighth, charged in batches so a huge mesh still runs out. False once the budget
        // is spent.
        const auto charge_rejected = [&]() { return (++rejected & 63) != 0 || (budget -= 8) >= 0; };
        const RunCache *runs =
            fits_run_cache(n_indices) ? run_cache_find(statobj, rmesh, indices, n_verts, n_indices) : nullptr;
        if (limit != nullptr)
        {
            // A classification never decodes positions, allocates a cache entry, or walks an uncached mesh.
            if (runs == nullptr || mesh_limit_reached(limit))
            {
                limit->incomplete = true;
                return -1;
            }
            for (const auto identity :
                 {reinterpret_cast<std::uintptr_t>(runs->rmesh), reinterpret_cast<std::uintptr_t>(runs->idx),
                  reinterpret_cast<std::uintptr_t>(runs->pos), static_cast<std::uintptr_t>(runs->n_verts),
                  static_cast<std::uintptr_t>(runs->n_indices)})
            {
                limit->signature = (limit->signature ^ identity) * 0x100000001B3ull;
            }
        }
        if (runs == nullptr && fits_run_cache(n_indices))
        {
            if (!fetch_positions())
            {
                return -1;
            }
            runs = run_cache_build(statobj, rmesh, pos, stride, indices, n_verts, n_indices);
        }
        if (runs != nullptr)
        {
            pos = reinterpret_cast<const std::uint8_t *>(runs->pos);
            stride = 3 * sizeof(float);
            for (int r = 0; r < runs->n_runs; ++r)
            {
                // Check cheap rejected boxes in batches. Every run that reaches triangle work still checks below.
                if ((r & 7) == 0 && mesh_limit_reached(limit))
                {
                    return 0;
                }
                if ((r & 7) == 0 && --budget < 0)
                {
                    return crossed ? 1 : 0; // a run's box test costs an eighth
                }
                const float *b = runs->box + static_cast<std::size_t>(r) * 6;
                if (b[0] > aabb[3] || b[3] < aabb[0] || b[1] > aabb[4] || b[4] < aabb[1] || b[2] > aabb[5] ||
                    b[5] < aabb[2])
                {
                    continue;
                }
                // Then against the box aligned with the lines: the run's box projected on each of its axes.
                const float centre[3] = {(b[0] + b[3]) * 0.5f, (b[1] + b[4]) * 0.5f, (b[2] + b[5]) * 0.5f};
                const float half[3] = {(b[3] - b[0]) * 0.5f, (b[4] - b[1]) * 0.5f, (b[5] - b[2]) * 0.5f};
                bool apart = false;
                for (int k = 0; k < 3 && !apart; ++k)
                {
                    const float c = project(centre, k);
                    const float reach = std::fabs(axis[k][0]) * half[0] + std::fabs(axis[k][1]) * half[1] +
                                        std::fabs(axis[k][2]) * half[2];
                    apart = c + reach < box[k] || c - reach > box[k + 3];
                }
                if (apart)
                {
                    continue;
                }
                if (mesh_limit_reached(limit))
                {
                    return 0;
                }
                if (!fetch_positions())
                {
                    return -1;
                }
                const int t_end = std::min(n_tris, (r + 1) * k_run_tris);
                for (int t = r * k_run_tris; t < t_end; ++t)
                {
                    const int i0 = indices[t * 3 + 0], i1 = indices[t * 3 + 1], i2 = indices[t * 3 + 2];
                    if (i0 >= n_verts || i1 >= n_verts || i2 >= n_verts)
                    {
                        continue;
                    }
                    const float *v0 = vtx(i0);
                    const float *v1 = vtx(i1);
                    const float *v2 = vtx(i2);
                    if ((outcode(v0) & outcode(v1) & outcode(v2)) != 0)
                    {
                        if (!charge_rejected())
                        {
                            return crossed ? 1 : 0;
                        }
                        continue;
                    }
                    if (--budget < 0)
                    {
                        return crossed ? 1 : 0;
                    }
                    if (test_triangle(v0, v1, v2))
                    {
                        return 1;
                    }
                }
            }
            return crossed ? 1 : 0;
        }

        // A mesh too large for the runs: one sequential pass gives every vertex an outcode, so a rejected triangle is
        // dropped from three byte reads, without fetching its positions. Main-thread only (the camera frame), so the
        // static outcode buffer is race-free. The vertex pass is charged before it runs.
        budget -= n_verts / 8;
        if (budget <= 0)
        {
            return 0;
        }
        if (!fetch_positions())
        {
            return -1;
        }
        static std::uint8_t s_outcode[65536];
        for (int v = 0; v < n_verts; ++v)
        {
            s_outcode[v] = outcode(vtx(v));
        }
        for (int t = 0; t < n_tris; ++t)
        {
            const int i0 = indices[t * 3 + 0], i1 = indices[t * 3 + 1], i2 = indices[t * 3 + 2];
            if (i0 >= n_verts || i1 >= n_verts || i2 >= n_verts)
            {
                continue;
            }
            if ((s_outcode[i0] & s_outcode[i1] & s_outcode[i2]) != 0)
            {
                if (!charge_rejected())
                {
                    return crossed ? 1 : 0;
                }
                continue;
            }
            if (--budget < 0)
            {
                return crossed ? 1 : 0;
            }
            if (test_triangle(vtx(i0), vtx(i1), vtx(i2)))
            {
                return 1;
            }
        }
        return crossed ? 1 : 0;
    }

    /**
     * @brief Moves the world sightlines into the local space of world matrix @p W, each counted up to @p end_margin
     *        meters short of its end. False when W is singular.
     */
    static bool to_local_lines(const float *W, const Sightline *lines, int n_lines, float end_margin,
                               LocalLine *out) noexcept
    {
        float inv[12];
        if (!mat34_inverse(W, inv))
        {
            return false;
        }
        const auto xf = [&](const Vector3 &p, float *r)
        {
            r[0] = inv[0] * p.x + inv[1] * p.y + inv[2] * p.z + inv[3];
            r[1] = inv[4] * p.x + inv[5] * p.y + inv[6] * p.z + inv[7];
            r[2] = inv[8] * p.x + inv[9] * p.y + inv[10] * p.z + inv[11];
        };
        for (int l = 0; l < n_lines; ++l)
        {
            float a[3], b[3];
            xf(lines[l].from, a);
            xf(lines[l].to, b);
            out[l].o[0] = a[0];
            out[l].o[1] = a[1];
            out[l].o[2] = a[2];
            out[l].d[0] = b[0] - a[0];
            out[l].d[1] = b[1] - a[1];
            out[l].d[2] = b[2] - a[2];
            const float len = (lines[l].to - lines[l].from).magnitude();
            out[l].s_max = (len > 1e-3f) ? (1.0f - end_margin / len) : 0.0f;
        }
        return true;
    }

    /**
     * @brief Tests the sightlines against a render node's mesh: its root render mesh, or each sub-mesh of a compound
     *        statobj (a fence or gate whose parts are child statobjs). A crossing within @p end_margin meters of a
     *        line's end does not count. POD body (runs in the caller's SEH frame).
     * @return 1 crossed, 0 not crossed, -1 no readable mesh.
     */
    static int statobj_crosses(void *node, void *statobj, const float *world, const Sightline *lines, int n_lines,
                               float end_margin, int &budget, uintptr_t mod_lo, uintptr_t mod_hi, float *nearest,
                               MeshTestLimit *limit) noexcept
    {
        LocalLine local[k_max_lines];
        const int n = std::min(n_lines, k_max_lines);
        void *rmesh =
            *reinterpret_cast<void **>(reinterpret_cast<std::byte *>(statobj) + Constants::STATOBJ_RENDERMESH_OFFSET);
        if (rmesh != nullptr)
        {
            if (!to_local_lines(world, lines, n, end_margin, local))
            {
                return -1;
            }
            return mesh_crosses(statobj, rmesh, local, n, budget, mod_lo, mod_hi, nearest, limit);
        }
        // Compound statobj: test each child sub-mesh under the node matrix folded with the sub-object transform.
        auto *sb = reinterpret_cast<std::byte *>(statobj);
        const uintptr_t self_vt = *reinterpret_cast<uintptr_t *>(sb);
        const uintptr_t begin = *reinterpret_cast<uintptr_t *>(sb + Constants::STATOBJ_SUBOBJ_BEGIN_OFFSET);
        const uintptr_t end = *reinterpret_cast<uintptr_t *>(sb + Constants::STATOBJ_SUBOBJ_END_OFFSET);
        if (begin == 0 || end <= begin || !DMK::memory::is_plausible_ptr(DMK::Address{begin}) ||
            (end - begin) % Constants::SUBOBJ_STRIDE != 0)
        {
            return -1;
        }
        const int count = subobj_count(begin, end);
        if (limit != nullptr && (end - begin) / Constants::SUBOBJ_STRIDE > Constants::SUBOBJ_MAX)
        {
            limit->incomplete = true;
            return -1;
        }
        const HideMask mask = hide_mask_of(node);
        int result = -1;
        for (int i = 0; i < count; ++i)
        {
            if (mesh_limit_reached(limit))
            {
                return -1;
            }
            auto *so = reinterpret_cast<std::byte *>(begin + static_cast<uintptr_t>(i) * Constants::SUBOBJ_STRIDE);
            void *child = *reinterpret_cast<void **>(so + Constants::SUBOBJ_PSTATOBJ_OFFSET);
            if (child == nullptr || !DMK::memory::is_plausible_ptr(DMK::Address{reinterpret_cast<uintptr_t>(child)}) ||
                *reinterpret_cast<uintptr_t *>(child) != self_vt || !subobj_drawn(so, child, i, mask))
            {
                continue;
            }
            void *child_rmesh =
                *reinterpret_cast<void **>(reinterpret_cast<std::byte *>(child) + Constants::STATOBJ_RENDERMESH_OFFSET);
            if (child_rmesh == nullptr)
            {
                continue;
            }
            float sub_world[12];
            mat34_compose(world, reinterpret_cast<const float *>(so + Constants::SUBOBJ_TM_OFFSET), sub_world);
            if (!to_local_lines(sub_world, lines, n, end_margin, local))
            {
                if (limit != nullptr)
                {
                    limit->incomplete = true;
                    return -1;
                }
                continue;
            }
            const int r = mesh_crosses(child, child_rmesh, local, n, budget, mod_lo, mod_hi, nearest, limit);
            if (limit != nullptr && (r < 0 || budget <= 0))
            {
                limit->incomplete = true;
                return -1;
            }
            if (r == 1 && nearest == nullptr)
            {
                return 1;
            }
            result = std::max(result, r);
        }
        return result;
    }

    /** @brief Tests each current visible slot. Unreadable / animated geometry cannot prove an opening. */
    static int node_crosses(void *node, const Sightline *lines, int n_lines, float end_margin, int &budget,
                            uintptr_t mod_lo, uintptr_t mod_hi, float *nearest = nullptr,
                            MeshTestLimit *limit = nullptr) noexcept
    {
        if (has_visible_animation(node))
        {
            return -1;
        }
        const int slots = visible_slot_count(node);
        int result = -1;
        for (int slot = 0; slot < slots; ++slot)
        {
            if (budget <= 0 || mesh_limit_reached(limit))
            {
                if (limit != nullptr)
                {
                    limit->incomplete = true;
                }
                return -1;
            }
            alignas(16) float world[12] = {};
            void *statobj = visible_statobj(node, static_cast<unsigned int>(slot), world);
            if (statobj == nullptr)
            {
                continue;
            }
            const int crossed = statobj_crosses(node, statobj, world, lines, n_lines, end_margin, budget, mod_lo,
                                                mod_hi, nearest, limit);
            if (crossed == 1 && nearest == nullptr)
            {
                return 1;
            }
            if (limit != nullptr && crossed < 0)
            {
                limit->incomplete = true;
                return -1;
            }
            result = std::max(result, crossed);
        }
        return result;
    }

    // An entity whose box comes within this distance (squared, meters) of a sightline's character end is skipped.
    static constexpr float k_entity_near_character2 = 0.6f * 0.6f;

    /** @brief node_crosses under its own SEH frame. POD-only body: -2 when reading the node faults. */
    static int node_crosses_seh(void *node, const Sightline *lines, int n_lines, float end_margin, int &budget,
                                float *nearest) noexcept
    {
        __try
        {
            return node_crosses(node, lines, n_lines, end_margin, budget, s_mod_lo, s_mod_hi, nearest);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -2;
        }
    }

    /** @brief node_crosses_seh, charged to @p tag as one mesh test in the query ledger. */
    static int node_crosses_guarded(QueryLedger::Tag tag, void *node, const Sightline *lines, int n_lines,
                                    float end_margin, int &budget, float *nearest = nullptr) noexcept
    {
        const std::int64_t ledger_started = QueryLedger::begin();
        const int result = node_crosses_seh(node, lines, n_lines, end_margin, budget, nearest);
        QueryLedger::end(tag, QueryLedger::Kind::Mesh, ledger_started);
        return result;
    }

    /**
     * @brief A hash of fence model @p statobj and each of its sub-objects (pointer, revision, flags, render mesh,
     *        transform), the state every instance of the model shares, or 0 when its sub-object vector is implausible.
     *        Reads no vertex and prepares no mesh. SEH caller.
     */
    static std::uint64_t wall_model_state(const void *statobj) noexcept
    {
        std::uint64_t state = 0xCBF29CE484222325ull;
        const auto add = [&state](std::uint64_t value) { state = (state ^ value) * 0x100000001B3ull; };
        const auto model = [&add](const void *object)
        {
            const auto *b = static_cast<const std::byte *>(object);
            add(reinterpret_cast<uintptr_t>(object));
            add(statobj_revision(object));
            add(*reinterpret_cast<const std::uint32_t *>(b + Constants::STATOBJ_FLAGS_OFFSET));
            add(*reinterpret_cast<const uintptr_t *>(b + Constants::STATOBJ_RENDERMESH_OFFSET));
        };
        model(statobj);
        const auto *b = static_cast<const std::byte *>(statobj);
        const auto begin = *reinterpret_cast<const uintptr_t *>(b + Constants::STATOBJ_SUBOBJ_BEGIN_OFFSET);
        const auto end = *reinterpret_cast<const uintptr_t *>(b + Constants::STATOBJ_SUBOBJ_END_OFFSET);
        if (end < begin || (end - begin) % Constants::SUBOBJ_STRIDE != 0 ||
            (end - begin) / Constants::SUBOBJ_STRIDE > Constants::SUBOBJ_MAX)
        {
            return 0;
        }
        add(end - begin);
        for (auto at = begin; at < end; at += Constants::SUBOBJ_STRIDE)
        {
            const auto *child = *reinterpret_cast<const void *const *>(at + Constants::SUBOBJ_PSTATOBJ_OFFSET);
            add(reinterpret_cast<uintptr_t>(child));
            if (child != nullptr)
            {
                model(child);
            }
            add(*reinterpret_cast<const std::uint32_t *>(at + Constants::SUBOBJ_FLAGS_OFFSET));
            const auto *tm = reinterpret_cast<const std::uint32_t *>(at + Constants::SUBOBJ_TM_OFFSET);
            for (int i = 0; i < 12; ++i)
            {
                add(tm[i]);
            }
        }
        return state;
    }

    /**
     * @brief Whether fence @p node counts as a wall: the cached coverage verdict of its model as placed (path, model
     *        state, hide mask and scale). True while the verdict is pending, and for a fence that cannot be read, so it
     *        keeps its collision. A fence lower than k_wall_min_height is never a wall. Measures nothing: an unknown
     *        model takes a new verdict slot for advance_fence_classification(). POD-only body under SEH.
     * @param slot Receives the verdict slot, or -1 when the fence has none.
     */
    static bool fence_is_wall_guarded(void *node, int &slot) noexcept
    {
        slot = -1;
        __try
        {
            void **vt = *reinterpret_cast<void ***>(node);
            const auto get_stat_obj =
                reinterpret_cast<GetEntityStatObjFn>(vt[Constants::RENDERNODE_VTABLE_GETENTITYSTATOBJ_OFFSET / 8]);
            const auto fn_addr = reinterpret_cast<uintptr_t>(get_stat_obj);
            if (fn_addr < s_mod_lo || fn_addr >= s_mod_hi)
            {
                return true;
            }
            alignas(16) float world[12] = {};
            void *statobj = single_visible_statobj(node, world);
            if (statobj == nullptr)
            {
                return true;
            }
            const auto *sb = reinterpret_cast<const std::byte *>(statobj);
            const auto *lo = reinterpret_cast<const float *>(sb + Constants::STATOBJ_BOX_MIN_OFFSET);
            const auto *hi = reinterpret_cast<const float *>(sb + Constants::STATOBJ_BOX_MAX_OFFSET);
            for (int axis = 0; axis < 3; ++axis)
            {
                if (!std::isfinite(lo[axis]) || !std::isfinite(hi[axis]) || hi[axis] < lo[axis])
                {
                    return true;
                }
            }
            const auto scale = [&world](int axis)
            {
                return std::sqrt(world[axis] * world[axis] + world[4 + axis] * world[4 + axis] +
                                 world[8 + axis] * world[8 + axis]);
            };
            const float up = scale(2);
            const float sx = scale(0);
            const float sy = scale(1);
            if (!std::isfinite(up) || !std::isfinite(sx) || !std::isfinite(sy) ||
                !(up > 0.0f && sx > 0.0f && sy > 0.0f))
            {
                return true;
            }
            if (hi[2] * up < k_wall_min_height)
            {
                return false;
            }
            const bool along_y = (hi[1] - lo[1]) * sy >= (hi[0] - lo[0]) * sx;
            const float along = along_y ? sy : sx;
            const float across = along_y ? sx : sy;
            const HideMask hide = hide_mask_of(node);
            if (hide.count > (Constants::SUBOBJ_MAX + 31) / 32)
            {
                return true;
            }
            std::uint64_t mask = 0xCBF29CE484222325ull ^ hide.count;
            for (std::uint32_t w = 0; hide.words != nullptr && w < hide.count; ++w)
            {
                mask = (mask ^ hide.words[w]) * 0x100000001B3ull;
            }
            const void *path = *reinterpret_cast<const void *const *>(sb + Constants::STATOBJ_CGF_NAME_OFFSET);
            const std::uint64_t model_state = wall_model_state(statobj);
            if (model_state == 0)
            {
                return true;
            }
            for (int i = 0; i < k_wall_verdicts; ++i)
            {
                const WallVerdict &v = s_wall_verdicts[i];
                if (v.statobj == statobj && v.path == path && v.model_state == model_state && v.mask == mask &&
                    v.up == up && v.along == along && v.across == across)
                {
                    slot = i;
                    return v.coverage.verdict() != FenceCoverage::Verdict::Open;
                }
            }
            slot = s_wall_next;
            s_wall_next = (s_wall_next + 1) % k_wall_verdicts;
            s_wall_verdicts[slot] = WallVerdict{.statobj = statobj,
                                                .path = path,
                                                .model_state = model_state,
                                                .mask = mask,
                                                .up = up,
                                                .along = along,
                                                .across = across,
                                                .started_ms = GetTickCount64()};
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            slot = -1;
            return true;
        }
    }

    /**
     * @brief Advances fence @p node's coverage by at most one row before @p deadline (QueryPerformanceCounter ticks),
     *        keeping complete probes across slices. Reads only meshes the run cache holds and prepares none: a slice
     *        that meets an unmeasured mesh backs off until a mesh is measured or 100 ms pass. POD-only body under SEH.
     */
    static void measure_wall_row_guarded(ScannedNode &node, std::int64_t deadline) noexcept
    {
        __try
        {
            if (!node_alive(node))
            {
                node.wall_slot = -1;
                return;
            }
            int slot = -1;
            (void)fence_is_wall_guarded(node.node, slot);
            node.wall_slot = slot;
            if (slot < 0)
            {
                return;
            }
            WallVerdict &v = s_wall_verdicts[slot];
            if (v.coverage.verdict() != FenceCoverage::Verdict::Pending)
            {
                return;
            }
            ++v.slices;
            alignas(16) float world[12] = {};
            if (single_visible_statobj(node.node, world) != v.statobj)
            {
                return;
            }
            const auto *sb = static_cast<const std::byte *>(v.statobj);
            const auto *lo = reinterpret_cast<const float *>(sb + Constants::STATOBJ_BOX_MIN_OFFSET);
            const auto *hi = reinterpret_cast<const float *>(sb + Constants::STATOBJ_BOX_MAX_OFFSET);
            const float sx = std::sqrt(world[0] * world[0] + world[4] * world[4] + world[8] * world[8]);
            const float sy = std::sqrt(world[1] * world[1] + world[5] * world[5] + world[9] * world[9]);
            const int along = (hi[1] - lo[1]) * sy >= (hi[0] - lo[0]) * sx ? 1 : 0;
            const int across = 1 - along;
            const float pad = k_wall_probe_pad / v.across;
            const auto to_world = [&world](const float *m)
            {
                return Vector3{world[0] * m[0] + world[1] * m[1] + world[2] * m[2] + world[3],
                               world[4] * m[0] + world[5] * m[1] + world[6] * m[2] + world[7],
                               world[8] * m[0] + world[9] * m[1] + world[10] * m[2] + world[11]};
            };
            static_assert(FenceCoverage::k_columns <= k_max_lines);
            int budget = k_wall_work_budget;
            const int row = v.coverage.rows;
            while (v.coverage.rows == row)
            {
                MeshTestLimit limit{.deadline = deadline};
                if (budget <= 0 || mesh_limit_reached(&limit))
                {
                    ++v.yields;
                    return; // A spent slice resumes next frame, without the missing-mesh backoff.
                }
                const int count = v.narrow_probes ? 1 : FenceCoverage::k_columns;
                Sightline lines[FenceCoverage::k_columns];
                float nearest[FenceCoverage::k_columns];
                std::fill_n(nearest, count, 2.0f);
                for (int column = 0; column < count; ++column)
                {
                    float a[3];
                    float b[3];
                    a[along] = b[along] = lo[along] + (hi[along] - lo[along]) * FenceCoverage::width_fraction(
                                                                                    row, v.coverage.columns + column);
                    a[across] = lo[across] - pad;
                    b[across] = hi[across] + pad;
                    a[2] = b[2] =
                        (FenceCoverage::k_first_height + FenceCoverage::k_height_step * static_cast<float>(row)) / v.up;
                    lines[column] = {to_world(a), to_world(b)};
                }
                // Keep cheap rows batched. An overrun switches this verdict to narrower, resumable probes.
                const int crossed =
                    node_crosses(node.node, lines, count, 0.0f, budget, s_mod_lo, s_mod_hi, nearest, &limit);
                if (crossed < 0 || budget <= 0 || limit.incomplete)
                {
                    if (limit.deadline_reached || budget <= 0)
                    {
                        ++v.yields;
                        v.retry_ms = 0;
                        v.narrow_probes = true;
                    }
                    else
                    {
                        // Wake as soon as ordinary camera work fills a cache; otherwise back off unavailable data.
                        ++v.waits;
                        v.retry_ms = GetTickCount64() + 100;
                        v.retry_builds = s_mesh_builds;
                    }
                    return;
                }
                if ((v.coverage.rows != 0 || v.coverage.columns != 0) && v.mesh_signature != limit.signature)
                {
                    v.coverage = {};
                    v.mesh_signature = limit.signature;
                    return; // Never combine completed probes from different mesh snapshots.
                }
                v.mesh_signature = limit.signature;
                v.retry_ms = 0;
                for (int column = 0; column < count; ++column)
                {
                    v.coverage.observe_probe(nearest[column] <= 1.0f);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (node.wall_slot >= 0)
            {
                s_wall_verdicts[node.wall_slot].retry_ms = GetTickCount64() + 1000;
                s_wall_verdicts[node.wall_slot].retry_builds = s_mesh_builds;
                ++s_wall_verdicts[node.wall_slot].waits;
            }
        }
    }

    bool advance_fence_classification() noexcept
    {
        if (s_scan_rules_changed)
        {
            // A scan classified with a new rule set: the camera's cached collision and fade must follow it.
            s_scan_rules_changed = false;
            return true;
        }
        if (!s_scan_valid || s_scan_count == 0)
        {
            return false;
        }
        const std::uint64_t now_ms = GetTickCount64();
        for (int checked = 0; checked < s_scan_count; ++checked)
        {
            s_wall_cursor = (s_wall_cursor + 1) % s_scan_count;
            ScannedNode &node = s_scan[s_wall_cursor];
            const int slot = node.wall_slot;
            if (slot < 0 || s_wall_verdicts[slot].coverage.verdict() != FenceCoverage::Verdict::Pending ||
                (now_ms < s_wall_verdicts[slot].retry_ms && s_wall_verdicts[slot].retry_builds == s_mesh_builds))
            {
                continue;
            }
            DMK_PROFILE_SCOPE("render.wall_classify");
            static const std::int64_t frequency = []
            {
                LARGE_INTEGER value{};
                QueryPerformanceFrequency(&value);
                return value.QuadPart;
            }();
            LARGE_INTEGER started{}, finished{};
            QueryPerformanceCounter(&started);
            measure_wall_row_guarded(node, started.QuadPart + frequency * k_wall_slice_us / 1000000);
            QueryPerformanceCounter(&finished);
            QueryLedger::end(QueryLedger::Tag::FenceWall, QueryLedger::Kind::Mesh, started.QuadPart);
            const auto elapsed = finished.QuadPart - started.QuadPart;
            const int actual = node.wall_slot;
            if (actual < 0 || s_wall_verdicts[actual].coverage.verdict() == FenceCoverage::Verdict::Pending)
            {
                return false;
            }
            const auto &coverage = s_wall_verdicts[actual].coverage;
            const bool wall = coverage.verdict() == FenceCoverage::Verdict::Wall;
            char name[160]{};
            render_node_name(node.node, name, static_cast<int>(sizeof(name)));
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "RenderOcclusion: fence {}: {} -> {} (rows {}, hits {}/{}/{}/{}/{}/{}/{}/{} of {}; "
                "last slice {:.3f} ms; ready {} ms, slices {}, yields {}, waits {})",
                name, wall ? "wall" : "open",
                Rules::behaviour_name(wall ? node.verdict.outcome : node.verdict.behaviour), coverage.rows,
                coverage.hits[0], coverage.hits[1], coverage.hits[2], coverage.hits[3], coverage.hits[4],
                coverage.hits[5], coverage.hits[6], coverage.hits[7], FenceCoverage::k_columns,
                static_cast<double>(elapsed) * 1000.0 / static_cast<double>(frequency),
                GetTickCount64() - s_wall_verdicts[actual].started_ms, s_wall_verdicts[actual].slices,
                s_wall_verdicts[actual].yields, s_wall_verdicts[actual].waits);
            bool changed = false;
            for (int i = 0; i < s_scan_count; ++i)
            {
                ScannedNode &s = s_scan[i];
                if (s.wall_slot == actual && node_alive(s))
                {
                    // A cache slot may have been reused while scanning. Recheck the node's full key before
                    // applying the result, including its current model, scale and hidden parts.
                    int current = -1;
                    const auto cls =
                        class_of(fence_is_wall_guarded(s.node, current) ? s.verdict.outcome : s.verdict.behaviour);
                    s.wall_slot = current;
                    changed = changed || s.cls != cls;
                    s.cls = cls;
                }
            }
            return changed; // At most one measurement row across all models, even when it completed early.
        }
        return false;
    }

    // The half-width of the tube a sightline is widened to before a prop on it fades (see sightline_nodes): a post or
    // a rail thinner than twice this hides only a sliver of the character.
    constexpr float k_thin_half_width = 0.15f;

    /**
     * @brief Whether the drawn mesh of @p node spans a tube of five parallel lines both ways: one of its two side
     *        lines and one of its two lift lines (@p tube in the order centre, side, other side, lift, other lift)
     *        cross it. True when the mesh cannot be read.
     */
    static bool tube_spanned(QueryLedger::Tag tag, void *node, const Sightline *tube, float end_margin,
                             int &budget) noexcept
    {
        float nearest[k_max_lines];
        std::fill_n(nearest, k_max_lines, 2.0f);
        if (node_crosses_guarded(tag, node, tube + 1, 4, end_margin, budget, nearest) < 0)
        {
            return true;
        }
        return (nearest[0] <= 1.0f || nearest[1] <= 1.0f) && (nearest[2] <= 1.0f || nearest[3] <= 1.0f);
    }

    /** @brief The five lines of a tube of half-width @p half_width around @p line, in tube_spanned()'s order. */
    static void tube_around(const Sightline &line, float half_width, Sightline *tube) noexcept
    {
        const Vector3 dir = line.to - line.from;
        Vector3 side = dir.cross(Vector3{0.0f, 0.0f, 1.0f});
        if (side.magnitude() < 1e-4f)
        {
            side = dir.cross(Vector3{1.0f, 0.0f, 0.0f});
        }
        side = side.normalized() * half_width;
        const Vector3 lift = side.cross(dir).normalized() * half_width;
        const Vector3 offsets[5] = {Vector3{}, side, side * -1.0f, lift, lift * -1.0f};
        for (int i = 0; i < 5; ++i)
        {
            tube[i] = {line.from + offsets[i], line.to + offsets[i]};
        }
    }

    bool spans_tube(void *node, const Sightline *tube) noexcept
    {
        if (node == nullptr || tube == nullptr)
        {
            return true;
        }
        int budget = Constants::OCCLUDER_TRIANGLE_BUDGET;
        return tube_spanned(QueryLedger::Tag::Fan, node, tube, 0.0f, budget);
    }

    int frames_near(const Vector3 &point, float reach, void **out, int cap) noexcept
    {
        if (!s_scan_valid || s_scan_count == 0 || out == nullptr || cap <= 0)
        {
            return 0;
        }
        int n = 0;
        for (int i = 0; i < s_scan_count && n < cap; ++i)
        {
            const ScannedNode &s = s_scan[i];
            if (s.cls != OccluderClass::Frame)
            {
                continue;
            }
            // The point's offset from the box along each axis (0 inside it).
            const float dx = std::max({s.bbox[0] - point.x, 0.0f, point.x - s.bbox[3]});
            const float dy = std::max({s.bbox[1] - point.y, 0.0f, point.y - s.bbox[4]});
            const float dz = std::max({s.bbox[2] - point.z, 0.0f, point.z - s.bbox[5]});
            if (dx * dx + dy * dy + dz * dz <= reach * reach && node_alive(s))
            {
                out[n++] = s.node;
            }
        }
        return n;
    }

    bool structure_drawn_on(QueryLedger::Tag tag, void *node, const Sightline *lines, int n_lines) noexcept
    {
        if (node == nullptr || !s_scan_valid)
        {
            return true;
        }
        const ScannedNode *s = nullptr;
        for (int i = 0; i < s_scan_count && s == nullptr; ++i)
        {
            s = (s_scan[i].node == node) ? &s_scan[i] : nullptr;
        }
        if (s == nullptr || s->cls != OccluderClass::Structure || !s->verdict.mesh || !node_alive(*s) ||
            lines == nullptr || n_lines <= 0)
        {
            return true;
        }
        // mesh_crosses reports "no crossing" when the triangle budget runs out. A test the budget cut short proves
        // nothing, so it counts as drawn like a mesh that cannot be read, and the collision stays.
        int budget = Constants::OCCLUDER_TRIANGLE_BUDGET;
        if (node_crosses_guarded(tag, node, lines, n_lines, 0.0f, budget) != 0 || budget <= 0)
        {
            return true;
        }
        // Another man-made structure drawn there: a house draws its outer walls in one model while its interior model
        // owns their collision. Only a crossing counts for these.
        const int n = std::min(n_lines, k_max_lines);
        for (int i = 0; i < s_scan_count && budget > 0; ++i)
        {
            const ScannedNode &t = s_scan[i];
            if (t.node == node || t.cls != OccluderClass::Structure || !t.verdict.mesh)
            {
                continue;
            }
            bool box_hit = false;
            for (int l = 0; l < n && !box_hit; ++l)
            {
                box_hit = segment_hits_box(lines[l].from, lines[l].to, t.bbox);
            }
            if (box_hit && node_alive(t) && node_crosses_guarded(tag, t.node, lines, n, 0.0f, budget) == 1)
            {
                return true;
            }
        }
        return budget <= 0;
    }

    /**
     * @brief The scan pass of occluders_on_sightlines. Each node is read under its own SEH frame, so a fault in one
     *        (a mesh streaming out between the reads) skips that node only.
     */
    static int sightline_nodes(const Sightline *lines, int n_lines, std::span<void *const> skip, SightlineOccluder *out,
                               int cap) noexcept
    {
        const auto skipped = [skip](void *node) { return std::find(skip.begin(), skip.end(), node) != skip.end(); };
        int n = 0;
        int budget = Constants::OCCLUDER_TRIANGLE_BUDGET;
        for (int i = 0; i < s_scan_count && n < cap && budget > 0; ++i)
        {
            const ScannedNode &s = s_scan[i];
            // Only a prop fades for hiding the character. A tree, a bush or a grass patch opens the tree see-through
            // window instead (see trees_in_window), so a leaf on a sightline never fades a whole plant.
            if (s.cls != OccluderClass::Prop || skipped(s.node))
            {
                continue;
            }
            bool box_hit = false;
            for (int l = 0; l < n_lines && !box_hit; ++l)
            {
                box_hit = segment_hits_box(lines[l].from, lines[l].to, s.bbox);
            }
            if (!box_hit || !node_alive(s))
            {
                continue;
            }
            if (s.entity)
            {
                // An entity right at the character is its own (the torch or weapon he carries) or something he stands
                // at; it is not between him and the camera.
                bool at_character = false;
                for (int l = 0; l < n_lines && !at_character; ++l)
                {
                    const Vector3 &p = lines[l].to;
                    const float dx = std::max({s.bbox[0] - p.x, 0.0f, p.x - s.bbox[3]});
                    const float dy = std::max({s.bbox[1] - p.y, 0.0f, p.y - s.bbox[4]});
                    const float dz = std::max({s.bbox[2] - p.z, 0.0f, p.z - s.bbox[5]});
                    at_character = dx * dx + dy * dy + dz * dz < k_entity_near_character2;
                }
                if (at_character)
                {
                    continue;
                }
            }
            // A prop brush whose mesh cannot be read (-1) is judged by its box alone: fading a small prop by mistake
            // costs less than leaving the character hidden behind it. An entity counts only on an exact crossing. A
            // read that faulted (-2) is skipped this time. A prop a sightline crosses fades only where it is wide:
            // a tube of k_thin_half_width around that sightline meets it both ways, so a post or a rail thinner than
            // the tube stays.
            float nearest[k_max_lines];
            std::fill_n(nearest, k_max_lines, 2.0f);
            const int r = node_crosses_guarded(QueryLedger::Tag::Sightline, s.node, lines, n_lines,
                                               Constants::OCCLUDER_SIGHTLINE_END_MARGIN, budget, nearest);
            bool fades = r == -1 && !s.entity;
            for (int l = 0; l < n_lines && r == 1 && !fades; ++l)
            {
                if (nearest[l] <= 1.0f)
                {
                    Sightline tube[5];
                    tube_around(lines[l], k_thin_half_width, tube);
                    fades = tube_spanned(QueryLedger::Tag::Sightline, s.node, tube,
                                         Constants::OCCLUDER_SIGHTLINE_END_MARGIN, budget);
                }
            }
            if (fades)
            {
                out[n++] = {s.node, false};
            }
        }
        // The camera buried in a prop or a bush (the hay on a wagon, a hawthorn): it fills the view whichever way it
        // lies from the sightlines, and the near clip plane cuts only what touches the lens, so it fades all the way.
        // This is the one case a bush fades whole: the see-through window opens only the leaves between the lens and
        // the character, not the ones all around the lens. Its box holds the camera and its mesh meets nearly every one
        // of six probes along the world axes; an open cart bed or a fence the camera stands over leaves several ways
        // open. A tree is never faded whole for this: under its crown the near clip plane cuts the leaves at the lens.
        // Each probe is tested alone, so its thin box rejects most of the mesh (the six together span a cube around
        // the lens, which rejects nothing), and the probes stop once the verdict is known.
        const Vector3 eye = lines[0].from;
        constexpr float k_envelope_reach = 2.0f;
        constexpr int k_envelope_probes = 6;
        constexpr int k_envelope_min_blocked = 5;
        const Sightline envelope_probes[k_envelope_probes] = {
            {eye, eye + Vector3{k_envelope_reach, 0.0f, 0.0f}}, {eye, eye + Vector3{-k_envelope_reach, 0.0f, 0.0f}},
            {eye, eye + Vector3{0.0f, k_envelope_reach, 0.0f}}, {eye, eye + Vector3{0.0f, -k_envelope_reach, 0.0f}},
            {eye, eye + Vector3{0.0f, 0.0f, k_envelope_reach}}, {eye, eye + Vector3{0.0f, 0.0f, -k_envelope_reach}}};
        for (int i = 0; i < s_scan_count && n < cap && budget > 0; ++i)
        {
            const ScannedNode &s = s_scan[i];
            if ((s.cls != OccluderClass::Prop && s.cls != OccluderClass::Vegetation) || s.entity || eye.x < s.bbox[0] ||
                eye.x > s.bbox[3] || eye.y < s.bbox[1] || eye.y > s.bbox[4] || eye.z < s.bbox[2] || eye.z > s.bbox[5] ||
                skipped(s.node) || !node_alive(s))
            {
                continue;
            }
            int blocked = 0;
            int open = 0;
            while (blocked < k_envelope_min_blocked && open <= k_envelope_probes - k_envelope_min_blocked)
            {
                const int r = node_crosses_guarded(QueryLedger::Tag::Sightline, s.node,
                                                   &envelope_probes[blocked + open], 1, 0.0f, budget);
                if (r < 0)
                {
                    break;
                }
                (r == 1 ? blocked : open) += 1;
            }
            if (blocked < k_envelope_min_blocked)
            {
                continue;
            }
            int listed = -1;
            for (int k = 0; k < n && listed < 0; ++k)
            {
                listed = (out[k].node == s.node) ? k : -1;
            }
            if (listed >= 0)
            {
                out[listed].buried = true;
            }
            else if (n < cap)
            {
                out[n++] = {s.node, true};
            }
        }
        return n;
    }

    /**
     * @brief Logs at Trace the scanned nodes whose box comes within @p reach of the camera, when that set changes: each
     *        with its class and what the sightline and lens tests made of it, so an object that fills the view without
     *        fading is identifiable from the log.
     */
    static void log_nodes_at_camera(const Vector3 &eye, float reach, const SightlineOccluder *found, int n_found)
    {
        if (!DMK::log().is_enabled(DMK::LogLevel::Trace))
        {
            return;
        }
        constexpr int k_max = 12;
        static void *s_last[k_max] = {};
        static int s_last_count = 0;
        void *near_nodes[k_max];
        int n_near = 0;
        for (int i = 0; i < s_scan_count && n_near < k_max; ++i)
        {
            const ScannedNode &s = s_scan[i];
            if (s.bbox[0] - reach <= eye.x && eye.x <= s.bbox[3] + reach && s.bbox[1] - reach <= eye.y &&
                eye.y <= s.bbox[4] + reach && s.bbox[2] - reach <= eye.z && eye.z <= s.bbox[5] + reach && node_alive(s))
            {
                near_nodes[n_near++] = s.node;
            }
        }
        if (n_near == s_last_count && std::equal(near_nodes, near_nodes + n_near, s_last))
        {
            return;
        }
        std::copy_n(near_nodes, n_near, s_last);
        s_last_count = n_near;
        std::string text;
        for (int i = 0; i < n_near; ++i)
        {
            const ScannedNode *s = nullptr;
            for (int k = 0; k < s_scan_count && s == nullptr; ++k)
            {
                s = (s_scan[k].node == near_nodes[i]) ? &s_scan[k] : nullptr;
            }
            char name[160];
            name_of_node_guarded(near_nodes[i], name, static_cast<int>(sizeof(name)));
            const char *verdict = "-";
            for (int k = 0; k < n_found; ++k)
            {
                if (found[k].node == near_nodes[i])
                {
                    verdict = found[k].buried ? "buried" : "sightline";
                }
            }
            text += std::format(
                "{}{}({}{}, {}, {})", i == 0 ? "" : ", ", name[0] != '\0' ? name : "?",
                s != nullptr ? occluder_class_name(s->cls) : "?", (s != nullptr && s->entity) ? " entity" : "", verdict,
                (s != nullptr && s_scan_rules != nullptr) ? s_scan_rules->where(s->verdict.rule) : std::string{"?"});
        }
        (void)DMK::log().try_log(DMK::LogLevel::Trace, "RenderOcclusion: around the camera: {}",
                                 n_near == 0 ? std::string{"nothing"} : text);
    }

    int occluders_on_sightlines(const Sightline *lines, int n_lines, std::span<void *const> skip,
                                SightlineOccluder *out, int cap) noexcept
    {
        DMK_PROFILE_SCOPE("render.occluder_sightlines");
        if (!s_scan_valid || s_scan_count == 0 || lines == nullptr || n_lines <= 0 || out == nullptr || cap <= 0)
        {
            return 0;
        }
        const int n = sightline_nodes(lines, std::min(n_lines, 4), skip, out, cap);
        try
        {
            log_nodes_at_camera(lines[0].from, 0.4f, out, n);
        }
        catch (...)
        {
            // A trace line that cannot be built is skipped.
        }
        return n;
    }

    int trees_in_window(const Vector3 (&near_end)[4], const Vector3 (&far_end)[4], void **out, int cap) noexcept
    {
        if (!s_scan_valid || s_scan_count == 0 || out == nullptr || cap <= 0)
        {
            return 0;
        }
        const auto dot = [](const Vector3 &a, const Vector3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
        // The solid's own box, and a point inside it (the mean of its eight corners), which tells each plane below
        // which way is out.
        float solid[6] = {near_end[0].x, near_end[0].y, near_end[0].z, near_end[0].x, near_end[0].y, near_end[0].z};
        Vector3 inside{};
        for (int i = 0; i < 4; ++i)
        {
            for (const Vector3 &c : {near_end[i], far_end[i]})
            {
                solid[0] = std::min(solid[0], c.x);
                solid[1] = std::min(solid[1], c.y);
                solid[2] = std::min(solid[2], c.z);
                solid[3] = std::max(solid[3], c.x);
                solid[4] = std::max(solid[4], c.y);
                solid[5] = std::max(solid[5], c.z);
                inside = inside + c;
            }
        }
        inside = inside / 8.0f;
        // The four sides and the two ends, each as a unit normal pointing out of the solid and the plane's offset along
        // it. A face too thin to have a normal (the near end of a pyramid) separates nothing and is left out.
        Vector3 normals[6];
        float offsets[6] = {};
        int n_planes = 0;
        const auto add_plane = [&](const Vector3 &a, const Vector3 &b, const Vector3 &c)
        {
            Vector3 normal = (b - a).cross(c - a);
            if (normal.magnitude_squared() < 1e-10f)
            {
                return;
            }
            normal = normal.normalized();
            if (dot(normal, inside - a) > 0.0f)
            {
                normal = normal * -1.0f;
            }
            normals[n_planes] = normal;
            offsets[n_planes++] = dot(normal, a);
        };
        for (int i = 0; i < 4; ++i)
        {
            // A side through the near corner, the far corner and the next far corner (or the next near corner when
            // the far ones coincide, which they never do for a window).
            add_plane(near_end[i], far_end[i], far_end[(i + 1) % 4]);
        }
        add_plane(far_end[0], far_end[1], far_end[2]);
        add_plane(near_end[0], near_end[1], near_end[2]);
        const auto meets_solid = [&](const float *b)
        {
            if (b[3] < solid[0] || b[0] > solid[3] || b[4] < solid[1] || b[1] > solid[4] || b[5] < solid[2] ||
                b[2] > solid[5])
            {
                return false;
            }
            for (int p = 0; p < n_planes; ++p)
            {
                // The box corner deepest into the solid along this plane's normal: when even that one is out, the
                // whole box is.
                const Vector3 &normal = normals[p];
                const Vector3 deepest{normal.x >= 0.0f ? b[0] : b[3], normal.y >= 0.0f ? b[1] : b[4],
                                      normal.z >= 0.0f ? b[2] : b[5]};
                if (dot(normal, deepest) > offsets[p])
                {
                    return false;
                }
            }
            return true;
        };
        // The centre line from the near end to the window. The plants whose box it passes through stand straight
        // between the camera and the character, so they are listed first and kept when the list is full.
        const Vector3 centre_from = (near_end[0] + near_end[1] + near_end[2] + near_end[3]) / 4.0f;
        const Vector3 centre_to = (far_end[0] + far_end[1] + far_end[2] + far_end[3]) / 4.0f;

        int n = 0;
        int left_out = 0;
        for (int pass = 0; pass < 2; ++pass)
        {
            for (int i = 0; i < s_scan_count; ++i)
            {
                const ScannedNode &s = s_scan[i];
                if ((s.cls != OccluderClass::Tree && s.cls != OccluderClass::Vegetation &&
                     s.cls != OccluderClass::Grass) ||
                    !meets_solid(s.bbox) || segment_hits_box(centre_from, centre_to, s.bbox) != (pass == 0) ||
                    !node_alive(s))
                {
                    continue;
                }
                if (n < cap)
                {
                    out[n++] = s.node;
                }
                else
                {
                    ++left_out;
                }
            }
        }
        // Logged once each time the window starts holding more plants than the list takes.
        static bool s_logged_full = false;
        if ((left_out > 0) != s_logged_full)
        {
            s_logged_full = left_out > 0;
            if (s_logged_full)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "RenderOcclusion: {} plant(s) meeting the see-through window left out (the list "
                    "takes {}, the ones on the line from the camera to the character first)",
                    left_out, cap);
            }
        }
        return n;
    }

    float structure_on_arm(QueryLedger::Tag tag, const Sightline *lines, int n_lines, int min_lines, float max_fraction,
                           void **node_out, bool trace) noexcept
    {
        DMK_PROFILE_SCOPE("render.structure_on_arm");
        if (node_out != nullptr)
        {
            *node_out = nullptr;
        }
        float best = 2.0f;
        if (!s_scan_valid || s_scan_count == 0 || lines == nullptr || n_lines <= 0)
        {
            return best;
        }
        const int n = std::min(n_lines, k_max_lines);
        const int need = std::clamp(min_lines, 1, n);
        int budget = Constants::OCCLUDER_TRIANGLE_BUDGET;
        // The structures tested and the budget each took, for the trace below.
        constexpr int k_trace_max = 8;
        void *tested[k_trace_max];
        int spent[k_trace_max];
        int n_tested = 0;
        for (int i = 0; i < s_scan_count && budget > 0; ++i)
        {
            const ScannedNode &s = s_scan[i];
            if (s.cls != OccluderClass::Structure || !s.verdict.mesh)
            {
                continue;
            }
            // Only a structure whose box at least min_lines of the lines enter, the first of them short of
            // max_fraction: each crossing lies inside the box, so one that fewer lines enter cannot count, and one
            // entered only past max_fraction could stop the camera no nearer than it already stops. Neither end of
            // the lines has to be inside the box: a cloth canopy with no physics hangs between the character and a
            // camera looking down on him from above it.
            int entering = 0;
            float first_entry = 2.0f;
            for (int l = 0; l < n; ++l)
            {
                const float entry = segment_box_entry(lines[l].from, lines[l].to, s.bbox);
                if (entry >= 0.0f)
                {
                    ++entering;
                    first_entry = std::min(first_entry, entry);
                }
            }
            if (entering < need || first_entry > max_fraction || !node_alive(s))
            {
                continue;
            }
            float nearest[k_max_lines];
            std::fill_n(nearest, k_max_lines, 2.0f);
            const int budget_before = budget;
            const int crossed = node_crosses_guarded(tag, s.node, lines, n, 0.0f, budget, nearest);
            if (n_tested < k_trace_max)
            {
                tested[n_tested] = s.node;
                spent[n_tested++] = budget_before - budget;
            }
            if (crossed != 1)
            {
                continue;
            }
            // One that counts stops the camera where the min_lines-th line meets it, the line that made it count: as
            // the arm swings into a sloped cloth or roof that point moves smoothly in from the far end, where taking
            // the first line's crossing would jump in by however deep that line already was. For the arm's centre and
            // four offset lines that is about where the centre meets a flat surface.
            std::sort(nearest, nearest + n);
            if (nearest[need - 1] > 1.0f)
            {
                continue;
            }
            const float blocked_at = nearest[need - 1];
            if (blocked_at < best)
            {
                best = blocked_at;
                if (node_out != nullptr)
                {
                    *node_out = s.node;
                }
            }
        }
        // Trace the structures the arm was tested against when that set changes, with the triangle budget each took,
        // so the cost of the test is attributable from the log.
        static void *s_last_tested[k_trace_max] = {};
        static int s_last_n_tested = 0;
        if (trace && DMK::log().is_enabled(DMK::LogLevel::Trace) &&
            (n_tested != s_last_n_tested || !std::equal(tested, tested + n_tested, s_last_tested)))
        {
            std::copy_n(tested, n_tested, s_last_tested);
            s_last_n_tested = n_tested;
            try
            {
                std::string text;
                for (int i = 0; i < n_tested; ++i)
                {
                    char name[160];
                    name_of_node_guarded(tested[i], name, static_cast<int>(sizeof(name)));
                    text += std::format("{}{} ({})", i == 0 ? "" : ", ", name, spent[i]);
                }
                (void)DMK::log().try_log(DMK::LogLevel::Trace, "RenderOcclusion: arm tested against: {}",
                                         n_tested == 0 ? std::string{"nothing"} : text);
            }
            catch (...)
            {
                // A trace line that cannot be built is skipped; the camera result stands.
            }
        }
        return best;
    }

    /**
     * @brief Measures @p rmesh when it fits the run cache and is not in it. POD body (runs in the caller's SEH frame).
     * @return 1 when it was measured, 0 when it needs no measuring, -1 when it cannot be read.
     */
    static int measure_mesh(void *statobj, void *rmesh, uintptr_t mod_lo, uintptr_t mod_hi) noexcept
    {
        const int n_verts =
            *reinterpret_cast<int *>(reinterpret_cast<std::byte *>(rmesh) + Constants::RENDERMESH_NVERTS_OFFSET);
        const int n_indices =
            *reinterpret_cast<int *>(reinterpret_cast<std::byte *>(rmesh) + Constants::RENDERMESH_NINDICES_OFFSET);
        if (!mesh_counts_valid(n_verts, n_indices))
        {
            return -1;
        }
        if (!fits_run_cache(n_indices))
        {
            return 0;
        }
        MeshReader reader{};
        if (!read_mesh(rmesh, n_indices, mod_lo, mod_hi, reader))
        {
            return -1;
        }
        if (run_cache_find(statobj, rmesh, reader.indices, n_verts, n_indices) != nullptr)
        {
            return 0;
        }
        int stride = 0;
        const std::uint8_t *pos = decode_positions(rmesh, reader.get_pos, stride);
        if (pos == nullptr)
        {
            return -1;
        }
        return run_cache_build(statobj, rmesh, pos, stride, reader.indices, n_verts, n_indices) != nullptr ? 1 : -1;
    }

    /**
     * @brief Measures the first render mesh of @p node not measured yet (its root mesh, or a sub-mesh of a compound
     *        statobj), one per call, under its own SEH frame. POD-only body.
     * @return 1 when a mesh was measured, 0 when every mesh is measured or needs none, -1 when none can be read, -2
     *         when reading the node faults.
     */
    static int measure_statobj(void *node, void *statobj) noexcept
    {
        auto *sb = reinterpret_cast<std::byte *>(statobj);
        void *rmesh = *reinterpret_cast<void **>(sb + Constants::STATOBJ_RENDERMESH_OFFSET);
        if (rmesh != nullptr)
        {
            return measure_mesh(statobj, rmesh, s_mod_lo, s_mod_hi);
        }
        const uintptr_t self_vt = *reinterpret_cast<uintptr_t *>(sb);
        const uintptr_t begin = *reinterpret_cast<uintptr_t *>(sb + Constants::STATOBJ_SUBOBJ_BEGIN_OFFSET);
        const uintptr_t end = *reinterpret_cast<uintptr_t *>(sb + Constants::STATOBJ_SUBOBJ_END_OFFSET);
        if (begin == 0 || end <= begin || (end - begin) % Constants::SUBOBJ_STRIDE != 0)
        {
            return -1;
        }
        const int count = subobj_count(begin, end);
        const HideMask mask = hide_mask_of(node);
        int result = -1;
        for (int i = 0; i < count; ++i)
        {
            auto *so = reinterpret_cast<std::byte *>(begin + static_cast<uintptr_t>(i) * Constants::SUBOBJ_STRIDE);
            void *child = *reinterpret_cast<void **>(so + Constants::SUBOBJ_PSTATOBJ_OFFSET);
            if (child == nullptr || *reinterpret_cast<uintptr_t *>(child) != self_vt ||
                !subobj_drawn(so, child, i, mask))
            {
                continue;
            }
            void *child_rmesh = *reinterpret_cast<void **>(reinterpret_cast<std::byte *>(child) +
                                                           Constants::STATOBJ_RENDERMESH_OFFSET);
            if (child_rmesh == nullptr)
            {
                continue;
            }
            const int r = measure_mesh(child, child_rmesh, s_mod_lo, s_mod_hi);
            if (r == 1)
            {
                return 1;
            }
            result = std::max(result, r);
        }
        return result;
    }

    /**
     * @brief Measures the first unmeasured render mesh of any visible slot of @p node, one per call, under its own SEH
     *        frame. POD-only body.
     * @return As measure_statobj(): 1 measured, 0 nothing to measure, -1 nothing readable, -2 a fault.
     */
    static int measure_node_guarded(void *node) noexcept
    {
        __try
        {
            const int slots = visible_slot_count(node);
            int result = -1;
            for (int slot = 0; slot < slots; ++slot)
            {
                void *statobj = visible_statobj(node, static_cast<unsigned int>(slot));
                if (statobj == nullptr)
                {
                    continue;
                }
                const int measured = measure_statobj(node, statobj);
                if (measured == 1)
                {
                    return 1; // Preserve the single mesh-build allowance across all slots.
                }
                result = std::max(result, measured);
            }
            return result;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -2;
        }
    }

    void prefetch_occluder_meshes(const Sightline *lines, int n_lines) noexcept
    {
        DMK_PROFILE_SCOPE("render.mesh_prefetch");
        // The nodes of this scan with every mesh measured (or none readable), so they are not walked again. A mesh the
        // cache later drops is measured again by the test that needs it.
        static void *s_done[Constants::OCCLUDER_MESH_CACHE_ENTRIES] = {};
        static int s_done_count = 0;
        static std::uint64_t s_done_scan = 0;
        static std::uint64_t s_builds_seen = 0;
        const bool measured_since = s_mesh_builds != s_builds_seen;
        s_builds_seen = s_mesh_builds;
        if (s_done_scan != s_scan_stamp_ms)
        {
            s_done_scan = s_scan_stamp_ms;
            s_done_count = 0;
        }
        if (measured_since || !s_scan_valid || lines == nullptr || n_lines <= 0)
        {
            return;
        }
        const int n = std::min(n_lines, k_max_lines);
        const float m = Constants::OCCLUDER_MESH_PREFETCH_MARGIN;
        void *best = nullptr;
        float best_entry = 2.0f;
        for (int i = 0; i < s_scan_count; ++i)
        {
            const ScannedNode &s = s_scan[i];
            // A fence's wall check reads only measured meshes (measure_wall_row_guarded). A pending fence holds its
            // rule's outcome, and no camera test reads the mesh of some outcomes (a collide without mesh), so a fence
            // is measured here whatever class it holds.
            if (!((s.cls == OccluderClass::Structure && s.verdict.mesh) || s.cls == OccluderClass::Prop ||
                  s.cls == OccluderClass::Frame || s.verdict.check == Rules::Check::Wall) ||
                std::find(s_done, s_done + s_done_count, s.node) != s_done + s_done_count)
            {
                continue;
            }
            const float grown[6] = {s.bbox[0] - m, s.bbox[1] - m, s.bbox[2] - m,
                                    s.bbox[3] + m, s.bbox[4] + m, s.bbox[5] + m};
            for (int l = 0; l < n; ++l)
            {
                const float entry = segment_box_entry(lines[l].from, lines[l].to, grown);
                if (entry >= 0.0f && entry < best_entry && node_alive(s))
                {
                    best_entry = entry;
                    best = s.node;
                }
            }
        }
        if (best == nullptr)
        {
            return;
        }
        const std::int64_t ledger_started = QueryLedger::begin();
        const int measured = measure_node_guarded(best);
        QueryLedger::end(QueryLedger::Tag::Prefetch, QueryLedger::Kind::Mesh, ledger_started);
        if (measured != 1 && s_done_count < static_cast<int>(std::size(s_done)))
        {
            s_done[s_done_count++] = best;
        }
        s_builds_seen = s_mesh_builds;
    }

    static void copy_cstr(char *dst, int dst_sz, const char *src) noexcept;

    /**
     * @brief Names the current visible animated model, including non-actor objects such as doors.
     * @details The getters read only the current slot's character and its file path. No animation update, mesh
     *          decode or physics query is needed. Classification resolves actor identity separately.
     */
    static void animated_model_name_guarded(void *node, char *out, int cap) noexcept
    {
        if (out == nullptr || cap <= 0)
        {
            return;
        }
        out[0] = '\0';
        __try
        {
            void **vt = *reinterpret_cast<void ***>(node);
            if (!vtable_is(GameClass::RenderProxy, reinterpret_cast<uintptr_t>(vt)))
            {
                return;
            }
            using GetCharacterFn = void *(__fastcall *)(void *, unsigned int, float *, bool);
            const auto get_character =
                reinterpret_cast<GetCharacterFn>(vt[Constants::RENDERNODE_VTABLE_GETENTITYCHARACTER_OFFSET / 8]);
            const auto character_fn = reinterpret_cast<uintptr_t>(get_character);
            if (character_fn < s_mod_lo || character_fn >= s_mod_hi)
            {
                return;
            }
            void *character = nullptr;
            const int count = visible_slot_count(node);
            for (int slot = 0; slot < count && character == nullptr; ++slot)
            {
                character = get_character(node, static_cast<unsigned int>(slot), nullptr, true);
            }
            if (character == nullptr)
            {
                return;
            }
            void **character_vt = *reinterpret_cast<void ***>(character);
            if (!vtable_is(GameClass::CharInstance, reinterpret_cast<uintptr_t>(character_vt)))
            {
                return;
            }
            using GetPathFn = const char *(__fastcall *)(void *);
            const auto get_path =
                reinterpret_cast<GetPathFn>(character_vt[Constants::CHAR_INSTANCE_VTABLE_GETFILEPATH_OFFSET / 8]);
            const auto path_fn = reinterpret_cast<uintptr_t>(get_path);
            if (path_fn < s_mod_lo || path_fn >= s_mod_hi)
            {
                return;
            }
            const char *path = get_path(character);
            if (path != nullptr)
            {
                copy_cstr(out, cap, path);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out[0] = '\0';
        }
    }

    /** @brief Copies the first current visible static or animated model path. POD-only body. */
    static void name_of_node_guarded(void *node, char *out, int cap) noexcept
    {
        void *statobj = nullptr;
        __try
        {
            void **vt = *reinterpret_cast<void ***>(node);
            const auto get_stat_obj =
                reinterpret_cast<GetEntityStatObjFn>(vt[Constants::RENDERNODE_VTABLE_GETENTITYSTATOBJ_OFFSET / 8]);
            const auto fn_addr = reinterpret_cast<uintptr_t>(get_stat_obj);
            if (fn_addr >= s_mod_lo && fn_addr < s_mod_hi)
            {
                const int slots = visible_slot_count(node);
                for (int slot = 0; slot < slots && statobj == nullptr; ++slot)
                {
                    statobj = visible_statobj(node, static_cast<unsigned int>(slot));
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            statobj = nullptr;
        }
        copy_brush_name(statobj, out, cap);
        if (statobj == nullptr)
        {
            animated_model_name_guarded(node, out, cap);
        }
    }

    void render_node_name(void *node, char *out, int cap) noexcept
    {
        if (cap > 0)
        {
            out[0] = '\0';
        }
        if (node == nullptr)
        {
            return;
        }
        const auto vt = DMK::memory::read<uintptr_t>(DMK::Address{reinterpret_cast<uintptr_t>(node)});
        if (!vt || *vt < s_mod_lo || *vt >= s_mod_hi)
        {
            return;
        }
        name_of_node_guarded(node, out, cap);
    }

    // Bounded C-string copy into a fixed buffer (null-terminates, no <cstring> dependency). A free function, not a
    // lambda, so it is safe to call inside render_hit_info's structured-exception (__try) frame.
    static void copy_cstr(char *dst, int dst_sz, const char *src) noexcept
    {
        if (dst == nullptr || dst_sz <= 0)
        {
            return;
        }
        int k = 0;
        for (; k < dst_sz - 1 && src[k] != '\0'; ++k)
        {
            dst[k] = src[k];
        }
        dst[k] = '\0';
    }

    /** @brief render_hit_info()'s body, with the rule set in force (or null) for telling excluded brushes apart. */
    static bool render_hit_info_guarded(const Vector3 &hit_point, void *node, char *out_name, int out_sz, float ext[3],
                                        int *out_kind, const Rules::RuleSet *rules) noexcept
    {
        if (out_name != nullptr && out_sz > 0)
        {
            out_name[0] = '\0';
        }
        ext[0] = ext[1] = ext[2] = 0.0f;
        if (out_kind != nullptr)
        {
            *out_kind = 0; // 0 none, 1 foreign-linked, 2 prop (mesh at hit), 3 solid (bbox only), 4 excluded
        }
        // Resolve the render-engine pointer up front (memory::read is itself guarded); keeping its Result local
        // out of the structured-exception frame below avoids object-unwinding (MSVC C2712) in the __try.
        void *p3d_ptr = nullptr;
        if (s_get_objects_in_box != nullptr && s_p3d_engine_slot_addr != 0)
        {
            const auto p3d = DMK::memory::read<uintptr_t>(DMK::Address{s_p3d_engine_slot_addr});
            if (p3d && *p3d != 0 && DMK::memory::is_plausible_ptr(DMK::Address{*p3d}))
            {
                const auto vtable = DMK::memory::read<uintptr_t>(DMK::Address{*p3d});
                if (vtable && s_game_module.contains(DMK::Address{*vtable}))
                {
                    refresh_brush_query(*vtable);
                    p3d_ptr = reinterpret_cast<void *>(*p3d);
                }
            }
        }
        __try
        {
            // Case 1: the collider carried a foreign render-node link (resolved by the caller). Read it directly.
            if (node != nullptr)
            {
                void **vt = *reinterpret_cast<void ***>(node);
                const auto vtaddr = reinterpret_cast<uintptr_t>(vt);
                if (vtaddr >= s_mod_lo && vtaddr < s_mod_hi)
                {
                    // Any node type: a runtime-physicalized brush, or a tree whose trunk collider links its
                    // CVegetation node.
                    const auto get_bbox =
                        reinterpret_cast<GetBBoxFn>(vt[Constants::RENDERNODE_VTABLE_GETBBOX_OFFSET / 8]);
                    float aabb[6] = {};
                    const float *b = reinterpret_cast<const float *>(get_bbox(node, aabb));
                    if (b != nullptr)
                    {
                        ext[0] = b[3] - b[0];
                        ext[1] = b[4] - b[1];
                        ext[2] = b[5] - b[2];
                    }
                    name_of_node_guarded(node, out_name, out_sz);
                    if (out_kind != nullptr)
                    {
                        *out_kind = 1;
                    }
                    return true;
                }
            }
            // Case 2: no foreign link (baked level physics, e.g. the bird_feeder / a monument). Identify the compact
            // brush at the hit point: prefer the brush whose mesh is AT the hit, else the smallest brush that contains
            // it (a compound building, reported so it is identifiable).
            if (p3d_ptr == nullptr)
            {
                return false;
            }
            constexpr float m = 0.25f;
            float bbox[6] = {hit_point.x - m, hit_point.y - m, hit_point.z - m,
                             hit_point.x + m, hit_point.y + m, hit_point.z + m};
            void *nodes[Constants::RENDER_OCCLUSION_MAX_NODES];
            const std::uint32_t count = query_brush_nodes(QueryLedger::Tag::Diagnostic, p3d_ptr, bbox, nodes);
            if (count == 0)
            {
                return false;
            }
            // Three tiers, most specific wins: prop (a brush whose MESH is at the hit = the real thin occluder) >
            // solid (a brush the rules keep that merely contains the hit = a compound building wall, still named) >
            // excluded (a brush the rules exclude, such as a coarse HLOD block, reported only if nothing better
            // contains the hit so the log never mislabels a wall as the giant level chunk). Stop early once a prop is
            // found.
            char prop_name[160] = {};
            float prop_ext[3] = {};
            bool have_prop = false;
            char solid_name[160] = {};
            float solid_ext[3] = {};
            float solid_min = 1e30f;
            bool have_solid = false;
            char excluded_name[160] = {};
            float excluded_ext[3] = {};
            bool have_excluded = false;
            for (std::uint32_t i = 0; i < count && !have_prop; ++i)
            {
                void *n = nodes[i];
                if (n == nullptr)
                {
                    continue;
                }
                const unsigned int rnd_flags = *reinterpret_cast<unsigned int *>(
                    reinterpret_cast<std::byte *>(n) + Constants::RENDERNODE_RNDFLAGS_OFFSET);
                if (rnd_flags & Constants::ERF_HIDDEN)
                {
                    continue;
                }
                void **vt = *reinterpret_cast<void ***>(n);
                const auto get_type =
                    reinterpret_cast<GetRenderNodeTypeFn>(vt[Constants::RENDERNODE_VTABLE_GETTYPE_OFFSET / 8]);
                if (get_type(n) != Constants::EERTYPE_BRUSH)
                {
                    continue;
                }
                const auto get_bbox =
                    reinterpret_cast<GetBBoxFn>(vt[Constants::RENDERNODE_VTABLE_GETBBOX_OFFSET / 8]);
                float aabb[6] = {};
                const float *b = reinterpret_cast<const float *>(get_bbox(n, aabb));
                if (b == nullptr)
                {
                    continue;
                }
                if (hit_point.x < b[0] || hit_point.x > b[3] || hit_point.y < b[1] || hit_point.y > b[4] ||
                    hit_point.z < b[2] || hit_point.z > b[5])
                {
                    continue; // bbox does not contain the hit
                }
                const float ex = b[3] - b[0], ey = b[4] - b[1], ez = b[5] - b[2];
                void *so = *reinterpret_cast<void **>(reinterpret_cast<std::byte *>(n) +
                                                      Constants::CBRUSH_STATOBJ_OFFSET);
                char nm[160];
                copy_brush_name(so, nm, static_cast<int>(sizeof(nm)));
                const Rules::State state = brush_invisible_guarded(n) ? Rules::State::NoDraw : Rules::State::None;
                if (rules != nullptr &&
                    rules->classify(nm, Rules::Kind::Brush, state).behaviour == Rules::Behaviour::Exclude)
                {
                    if (!have_excluded)
                    {
                        copy_cstr(excluded_name, static_cast<int>(sizeof(excluded_name)), nm);
                        excluded_ext[0] = ex;
                        excluded_ext[1] = ey;
                        excluded_ext[2] = ez;
                        have_excluded = true;
                    }
                    continue;
                }
                if (mesh_near_point(n, hit_point, Constants::HIT_MESH_EPS, s_mod_lo, s_mod_hi))
                {
                    copy_cstr(prop_name, static_cast<int>(sizeof(prop_name)), nm); // mesh at the hit: real occluder
                    prop_ext[0] = ex;
                    prop_ext[1] = ey;
                    prop_ext[2] = ez;
                    have_prop = true;
                    continue;
                }
                const float fp = (ex > ey) ? ((ex > ez) ? ex : ez) : ((ey > ez) ? ey : ez);
                if (!have_solid || fp < solid_min)
                {
                    copy_cstr(solid_name, static_cast<int>(sizeof(solid_name)), nm);
                    solid_ext[0] = ex;
                    solid_ext[1] = ey;
                    solid_ext[2] = ez;
                    solid_min = fp;
                    have_solid = true;
                }
            }
            const char *chosen = nullptr;
            const float *chosen_ext = nullptr;
            int chosen_kind = 0;
            if (have_prop)
            {
                chosen = prop_name;
                chosen_ext = prop_ext;
                chosen_kind = 2;
            }
            else if (have_solid)
            {
                chosen = solid_name;
                chosen_ext = solid_ext;
                chosen_kind = 3;
            }
            else if (have_excluded)
            {
                chosen = excluded_name;
                chosen_ext = excluded_ext;
                chosen_kind = 4;
            }
            if (chosen != nullptr)
            {
                copy_cstr(out_name, out_sz, chosen);
                ext[0] = chosen_ext[0];
                ext[1] = chosen_ext[1];
                ext[2] = chosen_ext[2];
                if (out_kind != nullptr)
                {
                    *out_kind = chosen_kind;
                }
                return true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return false;
    }

    bool render_hit_info(const Vector3 &hit_point, void *node, char *out_name, int out_sz, float ext[3],
                         int *out_kind) noexcept
    {
        const std::shared_ptr<const Rules::RuleSet> rules = Rules::current();
        return render_hit_info_guarded(hit_point, node, out_name, out_sz, ext, out_kind, rules.get());
    }

} // namespace TPVCamera
