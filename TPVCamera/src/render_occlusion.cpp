/**
 * @file render_occlusion.cpp
 * @brief Implementation of the render-node overhead camera clamp (see render_occlusion.hpp).
 */

#include "render_occlusion.hpp"
#include "aob_resolver.hpp"
#include "constants.hpp"
#include "global_state.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace TPVCamera
{

    // I3DEngine::GetObjectsInBox(this, const AABB* bbox, IRenderNode** p_out) -> count (see constants.hpp).
    using GetObjectsInBoxFn = std::uint32_t(__fastcall *)(void *p3d_engine, const float *bbox, void **out_list);
    // IRenderNode vtable methods (fork-specific slots, verified live; see constants.hpp). GetBBox fills the
    // caller buffer and returns a pointer to {Vec3 min @0, Vec3 max @0xC}.
    using GetBBoxFn = void *(__fastcall *)(void *node, void *out_aabb);
    using GetRenderNodeTypeFn = int(__fastcall *)(void *node);
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
    static DMK::Memory::ModuleRange s_game_module{};

    bool initialize_render_occlusion(uintptr_t module_base, size_t module_size, uintptr_t g_env)
    {
        DMK::Logger &logger = DMK::Logger::get_instance();

        const uintptr_t fn = anchor_address(AnchorId::GetObjectsInBox);
        if (fn == 0)
        {
            logger.warning(
                "RenderOcclusion: GetObjectsInBox not found (game patched?); render occlusion unavailable");
            return false;
        }

        s_get_objects_in_box = reinterpret_cast<GetObjectsInBoxFn>(fn);
        s_p3d_engine_slot_addr = g_env + Constants::GENV_3DENGINE_OFFSET;
        s_game_module = {module_base, module_base + module_size};
        s_mod_lo = module_base;
        s_mod_hi = module_base + module_size;

        logger.info("RenderOcclusion: GetObjectsInBox at {}, p3DEngine slot at {}", DMK::Format::format_address(fn),
                    DMK::Format::format_address(s_p3d_engine_slot_addr));
        return true;
    }

    // Sentinel returned by the cloth sampler when the brush's mesh is unavailable or has no vertices in the
    // camera column; the caller then falls back to the coarse world-AABB bottom.
    static constexpr float k_cloth_unavailable = 1e9f;

    /**
     * @brief Furthest the camera may sit from the pivot before this brush's cloth occludes the sightline.
     * @details node is the CBrush. Walks node -> IStatObj -> IRenderMesh, reads the engine-decoded float3 CPU
     *          position cache (GetPosPtr / FSL_READ), transforms each vertex to world by the brush Matrix34, and
     *          returns the SMALLEST distance d along the pivot->camera arm at which a cloth vertex comes within
     *          RENDER_OCCLUSION_COLUMN_RADIUS of the sightline -- i.e. the camera at distance d would just begin to
     *          see cloth between itself and the character. This is an analytic ray-march of the view ray against
     *          the cloth point cloud (each vertex stands in for the surface; the tube radius covers the gaps
     *          between vertices and doubles as the standoff). It naturally ignores the tent's vertical walls /
     *          skirts: once the camera is below the canopy the short remaining sightline to the central character
     *          clears them, so they never drag the camera down. @p dir is the unit pivot->camera direction and
     *          @p arm_len its length. Returns k_cloth_unavailable when the mesh is unreadable or fewer than
     *          RENDER_OCCLUSION_MIN_COLUMN_VERTS vertices lie on the sightline. POD-only (runs in the caller's
     *          SEH frame).
     */
    static float cloth_sightline_block_distance(void *node, Vector3 pivot, Vector3 dir, float arm_len,
                                                uintptr_t mod_lo, uintptr_t mod_hi)
    {
        auto *bytes = reinterpret_cast<std::byte *>(node);
        void *statobj = *reinterpret_cast<void **>(bytes + Constants::CBRUSH_STATOBJ_OFFSET);
        if (statobj == nullptr)
        {
            return k_cloth_unavailable;
        }
        void *rmesh =
            *reinterpret_cast<void **>(reinterpret_cast<std::byte *>(statobj) + Constants::STATOBJ_RENDERMESH_OFFSET);
        if (rmesh == nullptr)
        {
            return k_cloth_unavailable;
        }
        const int n_verts =
            *reinterpret_cast<int *>(reinterpret_cast<std::byte *>(rmesh) + Constants::RENDERMESH_NVERTS_OFFSET);
        if (n_verts <= 0 || n_verts > Constants::RENDER_OCCLUSION_VERT_MAX)
        {
            return k_cloth_unavailable;
        }

        void **rm_vt = *reinterpret_cast<void ***>(rmesh);
        const auto get_pos = reinterpret_cast<GetPosPtrFn>(rm_vt[Constants::RENDERMESH_VTABLE_GETPOSPTR_OFFSET / 8]);
        const auto get_pos_addr = reinterpret_cast<uintptr_t>(get_pos);
        if (get_pos_addr < mod_lo || get_pos_addr >= mod_hi)
        {
            return k_cloth_unavailable; // vtable slot does not point into the game image (patched / wrong layout)
        }

        int stride = 0;
        const std::uint8_t *pos = get_pos(rmesh, &stride, Constants::IRENDERMESH_FSL_READ, 0);
        if (pos == nullptr)
        {
            return k_cloth_unavailable;
        }
        if (stride <= 0)
        {
            stride = 12; // decoded position cache is tightly packed float3
        }

        // Brush world matrix (row-major Matrix34: row r = M[4r..4r+3], translation in column 3).
        const float *M = reinterpret_cast<const float *>(bytes + Constants::CBRUSH_MATRIX_OFFSET);

        const float colr = Constants::RENDER_OCCLUSION_COLUMN_RADIUS;
        const float colr2 = colr * colr;
        float best = k_cloth_unavailable;
        int hits = 0;
        for (int v = 0; v < n_verts; ++v)
        {
            const auto *lp = reinterpret_cast<const float *>(pos + static_cast<size_t>(v) * stride);
            const float lx = lp[0], ly = lp[1], lz = lp[2];
            const float wx = M[0] * lx + M[1] * ly + M[2] * lz + M[3];
            const float wy = M[4] * lx + M[5] * ly + M[6] * lz + M[7];
            const float wz = M[8] * lx + M[9] * ly + M[10] * lz + M[11];
            // Project the vertex onto the arm: tpar = signed distance from the pivot along dir; the leftover is
            // the perpendicular (off-axis) distance. Only vertices BETWEEN the character and the camera matter.
            const float rx = wx - pivot.x, ry = wy - pivot.y, rz = wz - pivot.z;
            const float tpar = rx * dir.x + ry * dir.y + rz * dir.z;
            if (tpar <= 0.0f || tpar > arm_len)
            {
                continue;
            }
            const float ex = rx - tpar * dir.x, ey = ry - tpar * dir.y, ez = rz - tpar * dir.z;
            const float perp2 = ex * ex + ey * ey + ez * ez;
            if (perp2 > colr2)
            {
                continue; // cloth is too far off the sightline to occlude it
            }
            // Distance along the arm at which this vertex first enters the colr tube of the growing sightline:
            // the camera must stop short of it. (At this distance the vertex is exactly colr away = the standoff.)
            const float d = tpar - std::sqrt(colr2 - perp2);
            if (d <= 0.0f)
            {
                // The cloth is within colr of the pivot itself: the CHARACTER is in / touching it (e.g. standing
                // amid hanging laundry), not occluded by it from a distance. No camera pull-in helps, so it is
                // NOT an occluder -- otherwise the camera would collapse onto the character (the blockDist=0 bug).
                continue;
            }
            ++hits;
            if (d < best)
            {
                best = d;
            }
        }
        return (hits >= Constants::RENDER_OCCLUSION_MIN_COLUMN_VERTS) ? best : k_cloth_unavailable;
    }

    /**
     * @brief Fraction (0..1) of the character's on-screen silhouette THIS brush occludes from the camera.
     * @details Distance-aware SCREEN coverage by TRIANGLE rasterization: the brush's triangles that lie between
     *          the camera and the character are rasterized (by screen bbox) into a per-band column grid over the
     *          character body box, then the filled-column fraction of each band is head-weighted. Using the
     *          actual SURFACE (triangles, via the index buffer) makes this independent of vertex density and,
     *          crucially, gap-aware: a contiguous cloth canopy fills every column it spans (high -> clamp), while
     *          a scattered prop the character stands amid (a clothes line, hanging laundry) leaves the
     *          character's own columns EMPTY where the gap is (low -> ignore) -- which a min/max-extent test
     *          reads as fully covered. Distance-aware (a near object spans more columns). The brush is
     *          non-physical so physics rays cannot measure it; this uses its own render mesh. POD body.
     */
    // Sentinel: the brush has no readable render mesh (a compound / multi-submesh statobj whose ROOT IRenderMesh
    // is null, or a layout drift), so its visible coverage cannot be MEASURED. This is DISTINCT from a real 0.0
    // (mesh read, covers nothing): a caller treats < 0 as "no opinion, defer to the physics-ray coverage", so a
    // solid wall whose root mesh we cannot read still blocks instead of being silently dropped (the bug this
    // sentinel fixes: workshop_a.cgf has a null root mesh, so 0.0 wrongly suppressed the physics fallback).
    static constexpr float k_coverage_unavailable = -1.0f;

    // Probe that the whole uint16 index buffer is actually mapped. Some static meshes return a NON-null but FREED
    // index pointer (the mesh streams its index stream out while keeping positions, so GetIndexPtr != null yet
    // reading it faults) -- live: bird_feeder.cgf flickers this frame to frame, faulting the triangle raster on
    // some frames and rasterizing fine on others (the in/out camera jump). Page mapping is 4 KB-granular, so one
    // touch per 4 KB page (2048 uint16) plus the last index proves every page the buffer spans is readable; if all
    // succeed the raster below cannot fault. POD-only body so its __try carries no object unwinding. False on a
    // fault -> the caller drops to the vertex-occupancy path (which reads only the still-mapped position stream).
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

    // Compose two row-major Matrix34 affine transforms (each 12 floats, [R|t] with the translation in column 3):
    // out applies b then a, so out * v == a * (b * v). Folds a compound statobj's sub-object local transform
    // into the parent brush world transform before a sub-mesh's vertices are projected.
    static void mat34_compose(const float *a, const float *b, float *out)
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

    // Shared projection + character screen box for the coverage raster, built once per (pivot, camera) and reused
    // for every mesh measured (a simple brush has one render mesh, a compound statobj several sub-meshes). Screen
    // coords are angular: lateral / vertical offset per unit depth from the camera. valid is false on a degenerate
    // setup (camera on the character, or a zero-area box) so the caller returns "unmeasurable".
    struct CoverageProjection
    {
        Vector3 camera{};
        float vdx = 0.0f, vdy = 0.0f, vdz = 0.0f; // view dir (camera -> character), unit
        float rx = 0.0f, ry = 0.0f;               // world-horizontal right (rz == 0)
        float ux = 0.0f, uy = 0.0f, uz = 0.0f;    // camera up
        float chmin = 0.0f, chmax = 0.0f, cvmin = 0.0f, cvmax = 0.0f;
        float band_h = 0.0f, col_w = 0.0f;
        float dchar = 0.0f; // camera -> character distance
        bool valid = false;
    };

    static CoverageProjection make_coverage_projection(Vector3 pivot, Vector3 camera)
    {
        CoverageProjection P{};
        P.camera = camera;
        float vdx = pivot.x - camera.x, vdy = pivot.y - camera.y, vdz = pivot.z - camera.z;
        const float dchar = std::sqrt(vdx * vdx + vdy * vdy + vdz * vdz);
        if (dchar < 1e-3f)
        {
            return P; // camera sits on the character: cannot project, defer to physics
        }
        vdx /= dchar;
        vdy /= dchar;
        vdz /= dchar;
        P.vdx = vdx;
        P.vdy = vdy;
        P.vdz = vdz;
        P.dchar = dchar;
        float rx = vdy, ry = -vdx; // view x world-up, projected to XY (horizontal)
        const float rl = std::sqrt(rx * rx + ry * ry);
        if (rl > 1e-4f)
        {
            rx /= rl;
            ry /= rl;
        }
        else
        {
            rx = 1.0f;
            ry = 0.0f;
        }
        P.rx = rx;
        P.ry = ry;
        // up = right x view (rz == 0)
        P.ux = ry * vdz;
        P.uy = -rx * vdz;
        P.uz = rx * vdy - ry * vdx;

        // Character box on screen. Preferred source: the REAL posed player world AABB, modeled as a vertical
        // CYLINDER (see the projection below), so the box tracks where the player actually is on screen and
        // shrinks / re-anchors with the pose (crouch / lying / mount). Fallback when the live AABB is
        // unavailable: the historical fixed synthetic box around the pivot (lateral +/-0.25 along right,
        // world-z +0.10 head .. -1.55 shins), which this reproduces EXACTLY so behaviour is unchanged when the
        // AABB cannot be resolved.
        float chmin = 1e9f, chmax = -1e9f, cvmin = 1e9f, cvmax = -1e9f;
        const auto add_corner = [&](float wx, float wy, float wz) {
            const float ex = wx - camera.x, ey = wy - camera.y, ez = wz - camera.z;
            const float d = std::sqrt(ex * ex + ey * ey + ez * ez);
            if (d < 1e-3f)
            {
                return;
            }
            const float h = (ex * rx + ey * ry) / d; // rz == 0
            const float vv = (ex * P.ux + ey * P.uy + ez * P.uz) / d;
            chmin = std::min(chmin, h);
            chmax = std::max(chmax, h);
            cvmin = std::min(cvmin, vv);
            cvmax = std::max(cvmax, vv);
        };
        const PlayerScreenBounds &pb = player_screen_bounds();
        if (pb.valid)
        {
            // Model the body as a vertical CYLINDER: lateral half-width = half the NARROWER horizontal AABB
            // dim, centered on the AABB centre, spanning the real z-range. Projecting all 8 AABB corners
            // instead captured the box DIAGONAL, so the on-screen width ballooned ~0.77 -> ~1.12 m as the
            // player turned (a box looks widest corner-on) -- which a roughly-cylindrical body never does. That
            // made coverage facing-dependent: a genuine occluder read ~0.84 and stepped across the threshold as
            // the camera rotated (flaky clamp/release). The cylinder width is facing-stable, so coverage is too.
            const float cx = 0.5f * (pb.min_x + pb.max_x), cy = 0.5f * (pb.min_y + pb.max_y);
            float r = 0.5f * std::min(pb.max_x - pb.min_x, pb.max_y - pb.min_y);
            r = std::clamp(r, 0.20f, 0.50f); // keep to a human on-screen radius
            const float zs[2] = {pb.min_z, pb.max_z};
            for (int c = 0; c < 2; ++c)
            {
                add_corner(cx + rx * r, cy + ry * r, zs[c]);
                add_corner(cx - rx * r, cy - ry * r, zs[c]);
            }
        }
        else
        {
            const float hl[2] = {-0.25f, 0.25f};
            const float vl[2] = {0.10f, -1.55f};
            for (int a = 0; a < 2; ++a)
            {
                for (int c = 0; c < 2; ++c)
                {
                    add_corner(pivot.x + rx * hl[a], pivot.y + ry * hl[a], pivot.z + vl[c]);
                }
            }
        }
        const float chw = chmax - chmin, cvh = cvmax - cvmin;
        if (chw < 1e-6f || cvh < 1e-6f)
        {
            return P; // degenerate character box
        }
        P.chmin = chmin;
        P.chmax = chmax;
        P.cvmin = cvmin;
        P.cvmax = cvmax;
        P.band_h = cvh * 0.25f;
        P.col_w = chw / static_cast<float>(Constants::RENDER_COVERAGE_COLUMNS);
        P.valid = true;
        return P;
    }

    // Rasterizes ONE render mesh (its local verts transformed to world by the row-major Matrix34 @p M) into the
    // shared coverage @p cell grid using the precomputed projection @p P. Triangle raster via the index buffer is
    // the default; if the index stream is unreadable (freed after GPU upload) a vertex-occupancy fallback marks
    // the verts instead. Cells already set by a previous mesh stay set, so a compound statobj's sub-meshes
    // accumulate into one silhouette. Returns true if a readable mesh was rasterized, so the caller can tell
    // "measured, covers nothing" from "no measurable mesh". POD body (runs in the caller's SEH frame).
    static bool rasterize_mesh(void *rmesh, const float *M, const CoverageProjection &P,
                               bool cell[4][Constants::RENDER_COVERAGE_COLUMNS], uintptr_t mod_lo, uintptr_t mod_hi)
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
        const int n_indices =
            *reinterpret_cast<int *>(reinterpret_cast<std::byte *>(rmesh) + Constants::RENDERMESH_NINDICES_OFFSET);
        const std::uint16_t *indices = nullptr;
        bool have_tris = (n_indices >= 3 && n_indices <= Constants::RENDER_OCCLUSION_INDEX_MAX && n_verts <= 65535);
        if (have_tris)
        {
            const auto get_idx =
                reinterpret_cast<GetIndexPtrFn>(rm_vt[Constants::RENDERMESH_VTABLE_GETINDEXPTR_OFFSET / 8]);
            const auto get_idx_addr = reinterpret_cast<uintptr_t>(get_idx);
            if (get_idx_addr < mod_lo || get_idx_addr >= mod_hi)
            {
                have_tris = false;
            }
            else
            {
                indices = get_idx(rmesh, Constants::IRENDERMESH_FSL_READ, 0);
                if (indices == nullptr || !indices_readable(indices, n_indices))
                {
                    have_tris = false;
                }
            }
        }

        // Project a world point to screen angular (h, v) = lateral / vertical offset per unit depth.
        const auto project = [&](float wx, float wy, float wz, float &h, float &vv) -> float
        {
            const float ex = wx - P.camera.x, ey = wy - P.camera.y, ez = wz - P.camera.z;
            const float d = std::sqrt(ex * ex + ey * ey + ez * ez);
            if (d < 1e-3f)
            {
                h = 0.0f;
                vv = 0.0f;
                return 0.0f;
            }
            h = (ex * P.rx + ey * P.ry) / d; // rz == 0
            vv = (ex * P.ux + ey * P.uy + ez * P.uz) / d;
            return ex * P.vdx + ey * P.vdy + ez * P.vdz; // depth along the view
        };
        const auto vtx_screen = [&](int idx, float &h, float &vv) -> float
        {
            const auto *lp = reinterpret_cast<const float *>(pos + static_cast<size_t>(idx) * stride);
            const float lx = lp[0], ly = lp[1], lz = lp[2];
            const float wx = M[0] * lx + M[1] * ly + M[2] * lz + M[3];
            const float wy = M[4] * lx + M[5] * ly + M[6] * lz + M[7];
            const float wz = M[8] * lx + M[9] * ly + M[10] * lz + M[11];
            return project(wx, wy, wz, h, vv);
        };

        constexpr int k_cols = Constants::RENDER_COVERAGE_COLUMNS;
        constexpr int k_total_cells = 4 * k_cols;
        // Running count of filled cells, seeded from cells a previous sub-mesh already set, so the raster can stop
        // the moment the character silhouette is fully covered: a solid occluder (a wall, a closed door / gate)
        // fills the 4x8 grid in a handful of triangles instead of rasterizing the whole mesh. This is the main
        // saving in dense / indoor scenes, where a compound wall carries thousands of triangles across sub-meshes.
        int filled = 0;
        for (int b = 0; b < 4; ++b)
        {
            for (int c = 0; c < k_cols; ++c)
            {
                if (cell[b][c])
                {
                    ++filled;
                }
            }
        }
        const int n_tris = have_tris ? (n_indices / 3) : 0; // 0 when indices unreadable -> vertex fallback below
        for (int t = 0; t < n_tris && filled < k_total_cells; ++t)
        {
            const int i0 = indices[t * 3 + 0], i1 = indices[t * 3 + 1], i2 = indices[t * 3 + 2];
            if (i0 >= n_verts || i1 >= n_verts || i2 >= n_verts)
            {
                continue;
            }
            float h0, v0, h1, v1, h2, v2;
            const float t0 = vtx_screen(i0, h0, v0);
            const float t1 = vtx_screen(i1, h1, v1);
            const float t2 = vtx_screen(i2, h2, v2);
            const float tmin = std::min(t0, std::min(t1, t2));
            if (tmin <= 0.1f || tmin > P.dchar + 0.2f)
            {
                continue; // triangle not between the camera and the character
            }
            // Triangle screen bbox, clamped to the character box.
            const float hmn = std::max(P.chmin, std::min(h0, std::min(h1, h2)));
            const float hmx = std::min(P.chmax, std::max(h0, std::max(h1, h2)));
            const float vmn = std::max(P.cvmin, std::min(v0, std::min(v1, v2)));
            const float vmx = std::min(P.cvmax, std::max(v0, std::max(v1, v2)));
            if (hmx < hmn || vmx < vmn)
            {
                continue; // does not overlap the character box
            }
            int rb0 = static_cast<int>((P.cvmax - vmx) / P.band_h); // top band (cvmax is the head)
            int rb1 = static_cast<int>((P.cvmax - vmn) / P.band_h); // bottom band
            int cc0 = static_cast<int>((hmn - P.chmin) / P.col_w);
            int cc1 = static_cast<int>((hmx - P.chmin) / P.col_w);
            rb0 = (rb0 < 0) ? 0 : rb0;
            rb1 = (rb1 > 3) ? 3 : rb1;
            cc0 = (cc0 < 0) ? 0 : cc0;
            cc1 = (cc1 > k_cols - 1) ? k_cols - 1 : cc1;
            // Mark only cells whose CENTRE is inside the triangle, not its whole bbox. A thin DIAGONAL stick
            // (tripod / lean-to pole) has a large axis-aligned bbox but the triangle is a sliver, so bbox-marking
            // would falsely fill the character's columns. Edge-function sign test (a point on an edge counts in).
            for (int b = rb0; b <= rb1; ++b)
            {
                const float pcv = P.cvmax - (static_cast<float>(b) + 0.5f) * P.band_h;
                for (int c = cc0; c <= cc1; ++c)
                {
                    if (cell[b][c])
                    {
                        continue;
                    }
                    const float pch = P.chmin + (static_cast<float>(c) + 0.5f) * P.col_w;
                    const float e0 = (pch - h0) * (v1 - v0) - (pcv - v0) * (h1 - h0);
                    const float e1 = (pch - h1) * (v2 - v1) - (pcv - v1) * (h2 - h1);
                    const float e2 = (pch - h2) * (v0 - v2) - (pcv - v2) * (h0 - h2);
                    const bool has_neg = (e0 < 0.0f) || (e1 < 0.0f) || (e2 < 0.0f);
                    const bool has_pos = (e0 > 0.0f) || (e1 > 0.0f) || (e2 > 0.0f);
                    if (!(has_neg && has_pos))
                    {
                        cell[b][c] = true;
                        ++filled;
                    }
                }
            }
        }

        // Vertex-occupancy fallback, only when the index list was unreadable (see have_tris): mark the band/column
        // cell each in-front vertex projects into. Less precise than the triangle raster (vertex-density dependent)
        // but it NEVER runs for a mesh whose triangles are readable, so canopies / walls are unaffected.
        if (!have_tris)
        {
            for (int v = 0; v < n_verts && filled < k_total_cells; ++v)
            {
                float vh = 0.0f, vv = 0.0f;
                const float vd = vtx_screen(v, vh, vv);
                if (vd <= 0.1f || vd > P.dchar + 0.2f)
                {
                    continue; // vertex not between the camera and the character
                }
                if (vh < P.chmin || vh > P.chmax || vv < P.cvmin || vv > P.cvmax)
                {
                    continue; // projects outside the character box
                }
                int b = static_cast<int>((P.cvmax - vv) / P.band_h);
                int c = static_cast<int>((vh - P.chmin) / P.col_w);
                b = (b < 0) ? 0 : (b > 3 ? 3 : b);
                c = (c < 0) ? 0 : (c > k_cols - 1 ? k_cols - 1 : c);
                if (!cell[b][c])
                {
                    cell[b][c] = true;
                    ++filled;
                }
            }
        }
        return true;
    }

    // Walks a compound statobj's sub-object vector and rasterizes each child sub-mesh into @p cell (transformed by
    // the parent brush matrix folded with each sub-object's local transform). Returns true if at least one
    // sub-mesh was measured. A compound .cgf (fence / gate / drying rack) has a null ROOT render mesh and its
    // geometry split across child IStatObjs, so without this the raster would see nothing and the caller would
    // fall back to the coarser physics-ray occlusion. Child sub-objects share the parent CStatObj vtable, checked
    // before any read so a non-compound statobj with unrelated data at the vector offset is rejected. POD body.
    // True once every coverage cell is set: a fully-covered silhouette cannot read higher, so the compound walk
    // stops issuing further sub-mesh reads (each is an engine GetPosPtr call) the moment the grid is full.
    static bool grid_full(const bool cell[4][Constants::RENDER_COVERAGE_COLUMNS])
    {
        for (int b = 0; b < 4; ++b)
        {
            for (int c = 0; c < Constants::RENDER_COVERAGE_COLUMNS; ++c)
            {
                if (!cell[b][c])
                {
                    return false;
                }
            }
        }
        return true;
    }

    static bool rasterize_compound(void *statobj, const float *brush_m, const CoverageProjection &P,
                                   bool cell[4][Constants::RENDER_COVERAGE_COLUMNS], uintptr_t mod_lo,
                                   uintptr_t mod_hi)
    {
        auto *sb = reinterpret_cast<std::byte *>(statobj);
        const uintptr_t self_vt = *reinterpret_cast<uintptr_t *>(sb);
        const uintptr_t begin = *reinterpret_cast<uintptr_t *>(sb + Constants::STATOBJ_SUBOBJ_BEGIN_OFFSET);
        const uintptr_t end = *reinterpret_cast<uintptr_t *>(sb + Constants::STATOBJ_SUBOBJ_END_OFFSET);
        if (begin == 0 || end <= begin || !DMK::Memory::plausible_userspace_ptr(begin) ||
            (end - begin) % Constants::SUBOBJ_STRIDE != 0)
        {
            return false; // not a populated sub-object vector (drift / simple statobj streamed out)
        }
        int count = static_cast<int>((end - begin) / Constants::SUBOBJ_STRIDE);
        if (count > Constants::SUBOBJ_MAX)
        {
            count = Constants::SUBOBJ_MAX;
        }
        bool measured = false;
        for (int i = 0; i < count; ++i)
        {
            if (grid_full(cell))
            {
                break; // silhouette already fully covered -> remaining sub-meshes cannot change the result
            }
            auto *so = reinterpret_cast<std::byte *>(begin + static_cast<uintptr_t>(i) * Constants::SUBOBJ_STRIDE);
            void *child = *reinterpret_cast<void **>(so + Constants::SUBOBJ_PSTATOBJ_OFFSET);
            if (child == nullptr || !DMK::Memory::plausible_userspace_ptr(reinterpret_cast<uintptr_t>(child)) ||
                *reinterpret_cast<uintptr_t *>(child) != self_vt)
            {
                continue; // not a sibling CStatObj -> skip (also rejects garbage at a non-compound vector offset)
            }
            void *child_rmesh = *reinterpret_cast<void **>(reinterpret_cast<std::byte *>(child) +
                                                           Constants::STATOBJ_RENDERMESH_OFFSET);
            if (child_rmesh == nullptr)
            {
                continue; // a nested-compound child is not recursed (one level covers these props)
            }
            float world_m[12];
            mat34_compose(brush_m, reinterpret_cast<const float *>(so + Constants::SUBOBJ_TM_OFFSET), world_m);
            measured = rasterize_mesh(child_rmesh, world_m, P, cell, mod_lo, mod_hi) || measured;
        }
        return measured;
    }

    static float brush_char_coverage(void *node, Vector3 pivot, Vector3 camera, uintptr_t mod_lo, uintptr_t mod_hi,
                                     float *out_head_fill = nullptr)
    {
        if (out_head_fill != nullptr)
        {
            *out_head_fill = -1.0f; // unmeasurable until a readable mesh is rasterized
        }
        auto *bytes = reinterpret_cast<std::byte *>(node);
        void *statobj = *reinterpret_cast<void **>(bytes + Constants::CBRUSH_STATOBJ_OFFSET);
        if (statobj == nullptr)
        {
            return k_coverage_unavailable;
        }
        const CoverageProjection P = make_coverage_projection(pivot, camera);
        if (!P.valid)
        {
            return k_coverage_unavailable; // camera on the character / degenerate box: defer to physics
        }
        const float *brush_m = reinterpret_cast<const float *>(bytes + Constants::CBRUSH_MATRIX_OFFSET);
        bool cell[4][Constants::RENDER_COVERAGE_COLUMNS] = {};

        void *rmesh =
            *reinterpret_cast<void **>(reinterpret_cast<std::byte *>(statobj) + Constants::STATOBJ_RENDERMESH_OFFSET);
        bool measured = false;
        if (rmesh != nullptr)
        {
            measured = rasterize_mesh(rmesh, brush_m, P, cell, mod_lo, mod_hi);
        }
        else
        {
            // Compound statobj (root mesh null): a fence / gate / drying-rack .cgf whose rails live in child
            // sub-meshes. Rasterize them so the open GAPS between rails register, instead of bailing to the
            // physics-ray fallback that reads a see-through structure's collision proxy as near-solid.
            measured = rasterize_compound(statobj, brush_m, P, cell, mod_lo, mod_hi);
        }
        if (!measured)
        {
            return k_coverage_unavailable; // no readable mesh (terrain / pure-physics / freed): defer to physics
        }

        // Head-weighted coverage: each band's filled-column fraction, weighted head->feet (a real TPV camera
        // protects the head / look-at socket, not the legs). A wide obstruction over the upper body collides; a
        // thin pole, a scattered prop, or an open-rail fence the body shows through does not.
        static const float k_char_level_weight[4] = {1.0f, 0.8f, 0.45f, 0.2f}; // head, chest, hips, shins
        float wsum = 0.0f, wov = 0.0f;
        for (int b = 0; b < 4; ++b)
        {
            int filled = 0;
            for (int c = 0; c < Constants::RENDER_COVERAGE_COLUMNS; ++c)
            {
                if (cell[b][c])
                {
                    ++filled;
                }
            }
            const float band_fill = static_cast<float>(filled) / static_cast<float>(Constants::RENDER_COVERAGE_COLUMNS);
            if (b == 0 && out_head_fill != nullptr)
            {
                *out_head_fill = band_fill; // top band = head silhouette fill, for the head-visible gate
            }
            wov += k_char_level_weight[b] * band_fill;
            wsum += k_char_level_weight[b];
        }
        return (wsum > 0.0f) ? std::min(1.0f, wov / wsum) : 0.0f;
    }

    // Identity of the overhead roof the clamp selected, captured for trace logging so false positives (a
    // sign / pole / wall chunk that passed the filters instead of a real roof) can be identified. POD, so it
    // is filled inside the SEH frame and read / logged outside it.
    struct RoofHitInfo
    {
        void *node = nullptr;
        void *statobj = nullptr;
        float roof_z = 0.0f;
        float min_x = 0.0f, min_y = 0.0f, min_z = 0.0f;
        float max_x = 0.0f, max_y = 0.0f, max_z = 0.0f;
        float coverage = -1.0f; // measured player-silhouette coverage of the binding brush (-1 = not measured)
    };

    /**
     * @brief SEH-isolated octree query plus sightline ray-march over the candidate brushes. POD-only body so
     *        the structured handler shares no frame with C++ unwinding; any fault becomes "no occluder". Returns
     *        the SMALLEST clear distance along the pivot->camera arm over every brush whose cloth lies on the
     *        sightline (k_cloth_unavailable when none do). Fills @p out_hit with the binding brush's identity.
     */
    static float nearest_sightline_block_guarded(void *p3d, GetObjectsInBoxFn query, const float *bbox, Vector3 pivot,
                                                 Vector3 camera, uintptr_t mod_lo, uintptr_t mod_hi, float cov_thresh,
                                                 RoofHitInfo *out_hit) noexcept
    {
        float best_dist = k_cloth_unavailable;
        __try
        {
            Vector3 dir = camera - pivot;
            const float arm_len = dir.magnitude();
            if (arm_len < 1e-3f)
            {
                return k_cloth_unavailable;
            }
            dir.x /= arm_len;
            dir.y /= arm_len;
            dir.z /= arm_len;

            const std::uint32_t count = query(p3d, bbox, nullptr);
            if (count == 0 || count > static_cast<std::uint32_t>(Constants::RENDER_OCCLUSION_MAX_NODES))
            {
                return k_cloth_unavailable;
            }

            void *nodes[Constants::RENDER_OCCLUSION_MAX_NODES];
            query(p3d, bbox, nodes);

            for (std::uint32_t i = 0; i < count; ++i)
            {
                void *node = nodes[i];
                if (node == nullptr)
                {
                    continue;
                }
                // Skip nodes that are in the octree but NOT drawn: GetObjectsInBox is a spatial query, not a
                // visibility query, so it returns ERF_HIDDEN placements (e.g. conditional laundry / rag
                // decorations). Invisible geometry cannot occlude the view -- colliding with it would pull the
                // camera onto something the player never sees.
                const unsigned int rnd_flags =
                    *reinterpret_cast<unsigned int *>(reinterpret_cast<std::byte *>(node) +
                                                      Constants::RENDERNODE_RNDFLAGS_OFFSET);
                if (rnd_flags & Constants::ERF_HIDDEN)
                {
                    continue;
                }

                void **vt = *reinterpret_cast<void ***>(node);
                const auto get_type =
                    reinterpret_cast<GetRenderNodeTypeFn>(vt[Constants::RENDERNODE_VTABLE_GETTYPE_OFFSET / 8]);
                if (get_type(node) != Constants::EERTYPE_BRUSH)
                {
                    continue; // only solid static brushes are roofs (skip lights / particles / fog / decals)
                }

                const auto get_bbox =
                    reinterpret_cast<GetBBoxFn>(vt[Constants::RENDERNODE_VTABLE_GETBBOX_OFFSET / 8]);
                float aabb[6] = {};
                const float *b = reinterpret_cast<const float *>(get_bbox(node, aabb));
                if (b == nullptr)
                {
                    continue;
                }
                const float min_x = b[0], min_y = b[1], min_z = b[2];
                const float max_x = b[3], max_y = b[4], max_z = b[5];

                // Skip world / terrain (merged static cells, building shells): only compact props are roofs.
                if ((max_x - min_x) > Constants::RENDER_OCCLUSION_MAX_BRUSH_SIZE ||
                    (max_y - min_y) > Constants::RENDER_OCCLUSION_MAX_BRUSH_SIZE ||
                    (max_z - min_z) > Constants::RENDER_OCCLUSION_MAX_BRUSH_SIZE)
                {
                    continue;
                }
                // NOTE: no brush-level "overhead" (min_z > pivot.z) gate here. Tent / awning brushes whose
                // posts and skirts reach the ground have a bbox bottom BELOW the pivot, yet their canopy cloth
                // hangs above it -- a coarse bbox test wrongly rejected exactly the brushes that cover the view.
                // Occlusion is decided per VERTEX in cloth_sightline_block_distance (only cloth on the
                // pivot->camera sightline counts), so the ground under the character is naturally excluded.

                // Cheap reject: the brush must overlap the sightline footprint in XY (pivot->camera AABB,
                // expanded by the tube radius). A camera parked at a tent edge still has a sightline back to
                // the character that passes under the canopy, so this is a sightline test, not "camera under it".
                const float colr = Constants::RENDER_OCCLUSION_COLUMN_RADIUS;
                const float los_min_x = (pivot.x < camera.x ? pivot.x : camera.x) - colr;
                const float los_max_x = (pivot.x > camera.x ? pivot.x : camera.x) + colr;
                const float los_min_y = (pivot.y < camera.y ? pivot.y : camera.y) - colr;
                const float los_max_y = (pivot.y > camera.y ? pivot.y : camera.y) + colr;
                if (max_x < los_min_x || min_x > los_max_x || max_y < los_min_y || min_y > los_max_y)
                {
                    continue;
                }

                // Ray-march this brush's cloth: the furthest arm distance still clear of it. Only ACTUAL cloth
                // vertices on the sightline count (no AABB fallback); a brush the sightline misses returns
                // k_cloth_unavailable and is skipped. The nearest occluder across all brushes wins.
                const float d = cloth_sightline_block_distance(node, pivot, dir, arm_len, mod_lo, mod_hi);
                if (d >= k_cloth_unavailable)
                {
                    continue;
                }
                if (d < best_dist)
                {
                    // Coverage gate (render-side use of CoverageCollision): when enabled (cov_thresh > 0) AND the
                    // live player AABB is available, DROP a brush that hides less than cov_thresh of the player's
                    // REAL on-screen silhouette -- a thin prop the body is plainly visible past (a pole, a bird
                    // feeder), not a view-burying canopy. Keyed on the real projected player extent (not the old
                    // synthetic box, which sat below an overhead canopy and read ~0, the reason the gate used to
                    // be omitted here): a canopy on a look-down covers the player on screen and survives, while a
                    // thin prop does not. Only a MEASURED low coverage skips -- an unreadable mesh (cloth often
                    // is) returns < 0 and keeps the clamp, so canopies are never dropped by a failed measurement.
                    float cov = -1.0f;
                    if (cov_thresh > 0.0f && player_screen_bounds().valid)
                    {
                        cov = brush_char_coverage(node, pivot, camera, mod_lo, mod_hi);
                        if (cov >= 0.0f && cov < cov_thresh)
                        {
                            continue; // thin prop -> body visible past it -> no render-occlusion clamp
                        }
                    }
                    best_dist = d;
                    out_hit->node = node;
                    out_hit->statobj = *reinterpret_cast<void **>(reinterpret_cast<std::byte *>(node) +
                                                                  Constants::CBRUSH_STATOBJ_OFFSET);
                    out_hit->roof_z = pivot.z + d * dir.z; // world-Z of the block point on the sightline
                    out_hit->min_x = min_x;
                    out_hit->min_y = min_y;
                    out_hit->min_z = min_z;
                    out_hit->max_x = max_x;
                    out_hit->max_y = max_y;
                    out_hit->max_z = max_z;
                    out_hit->coverage = cov;
                }
            }

            // The body-silhouette coverage gate above is keyed on the REAL projected player extent
            // (PlayerScreenBounds), so unlike the historical synthetic box -- which sat below an overhead canopy
            // and read ~0 -- it keeps canopies (high coverage on a look-down) and drops only thin props the body
            // is visible past. It runs only when cov_thresh > 0 and the live AABB is valid; otherwise the
            // sightline vertex-count test (RENDER_OCCLUSION_MIN_COLUMN_VERTS) is the sole filter, as before.
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return k_cloth_unavailable;
        }
        return best_dist;
    }

    // Best-effort, SEH-guarded copy of a brush's full .cgf path for trace logging only. The field at
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

    // True if the .cgf path marks an HLOD merged proxy ("hlod" anywhere in the path, e.g. ".../hlods/..."). An
    // HLOD is a coarse, building-scale stand-in that coexists in the octree with the real structure, so it is
    // never a thin prop; keeping it out of the visible-coverage measurement stops its low-detail mesh from
    // reporting a misleadingly low coverage and demoting a genuine wall. Case-insensitive, bounded read.
    static bool name_is_hlod_proxy(const char *s) noexcept
    {
        if (s == nullptr)
        {
            return false;
        }
        for (int i = 0; i < 156 && s[i] != '\0'; ++i)
        {
            if ((s[i] | 0x20) == 'h' && (s[i + 1] | 0x20) == 'l' && (s[i + 2] | 0x20) == 'o' &&
                (s[i + 3] | 0x20) == 'd')
            {
                return true;
            }
        }
        return false;
    }

    // True if the .cgf path is a WALL-OPENING element -- a window or hole frame embedded in a building wall
    // (e.g. objects/intermediates/elements/window_timber_a.cgf, hole_timber_*). These are thin decorations ON a
    // solid wall, NOT standalone props: from inside, the body stays visible THROUGH the opening so the frame
    // reads as low coverage, which would let the camera pass through the wall into the building. Excluding them
    // leaves the solid (compound) wall behind as the occluder, so the camera collides and stays out. DOORWAYS are
    // deliberately NOT matched -- a door is passable, the camera must follow the body through it.
    static bool name_is_wall_opening(const char *s) noexcept
    {
        if (s == nullptr)
        {
            return false;
        }
        for (int i = 0; i < 150 && s[i] != '\0'; ++i)
        {
            const char a = s[i] | 0x20;
            if (a == 'w' && (s[i + 1] | 0x20) == 'i' && (s[i + 2] | 0x20) == 'n' && (s[i + 3] | 0x20) == 'd' &&
                (s[i + 4] | 0x20) == 'o' && (s[i + 5] | 0x20) == 'w')
            {
                return true; // "window"
            }
            if (a == 'h' && (s[i + 1] | 0x20) == 'o' && (s[i + 2] | 0x20) == 'l' && (s[i + 3] | 0x20) == 'e')
            {
                return true; // "hole" (hole_timber wall opening)
            }
        }
        return false;
    }

    // True if brush @p node's render mesh has a vertex within @p eps of the world-space @p hit. Confirms a
    // candidate brush is the one a physics ray ACTUALLY hit (its mesh is there), not merely a neighbour whose
    // bbox contains the point. Reads the same pos buffer brush_char_coverage uses (GetPosPtr slot, FSL_READ),
    // transforms by the CBrush world matrix, scans EVERY vertex with an early-out (sampling missed a thin pole's
    // nearest vertex when it fell between sampled indices; the reads are raw, so a full scan is cheap).
    static bool mesh_has_vertex_near(void *rmesh, const float *M, Vector3 hit, float eps2, uintptr_t mod_lo,
                                     uintptr_t mod_hi)
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
    // rasterize_compound). POD body (runs in the caller's SEH frame).
    static bool mesh_near_point(void *node, Vector3 hit, float eps, uintptr_t mod_lo, uintptr_t mod_hi)
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
        // Compound statobj: scan the child sub-meshes (same vector layout the coverage raster walks).
        auto *sb = reinterpret_cast<std::byte *>(statobj);
        const uintptr_t self_vt = *reinterpret_cast<uintptr_t *>(sb);
        const uintptr_t begin = *reinterpret_cast<uintptr_t *>(sb + Constants::STATOBJ_SUBOBJ_BEGIN_OFFSET);
        const uintptr_t end = *reinterpret_cast<uintptr_t *>(sb + Constants::STATOBJ_SUBOBJ_END_OFFSET);
        if (begin == 0 || end <= begin || !DMK::Memory::plausible_userspace_ptr(begin) ||
            (end - begin) % Constants::SUBOBJ_STRIDE != 0)
        {
            return false;
        }
        int count = static_cast<int>((end - begin) / Constants::SUBOBJ_STRIDE);
        if (count > Constants::SUBOBJ_MAX)
        {
            count = Constants::SUBOBJ_MAX;
        }
        for (int i = 0; i < count; ++i)
        {
            auto *so = reinterpret_cast<std::byte *>(begin + static_cast<uintptr_t>(i) * Constants::SUBOBJ_STRIDE);
            void *child = *reinterpret_cast<void **>(so + Constants::SUBOBJ_PSTATOBJ_OFFSET);
            if (child == nullptr || !DMK::Memory::plausible_userspace_ptr(reinterpret_cast<uintptr_t>(child)) ||
                *reinterpret_cast<uintptr_t *>(child) != self_vt)
            {
                continue;
            }
            void *child_rmesh = *reinterpret_cast<void **>(reinterpret_cast<std::byte *>(child) +
                                                           Constants::STATOBJ_RENDERMESH_OFFSET);
            float world_m[12];
            mat34_compose(brush_m, reinterpret_cast<const float *>(so + Constants::SUBOBJ_TM_OFFSET), world_m);
            if (mesh_has_vertex_near(child_rmesh, world_m, hit, eps2, mod_lo, mod_hi))
            {
                return true;
            }
        }
        return false;
    }

    // Character coverage of ONE candidate render node at @p hit, under its OWN structured-exception frame. Returns
    // the brush's triangle coverage, or k_coverage_unavailable (< 0) when it is not a measurable thin prop (hidden /
    // non-brush / large / does-not-contain-the-hit / HLOD / wall-opening / mesh-not-at-the-hit) OR when reading it
    // FAULTS. The per-node guard is the point: an unreadable NEIGHBOUR brush (a freed mesh that faults mid-raster)
    // must not abort the whole query and discard a VALID measurement of another brush -- the bug where the
    // bird_feeder rasterized to 0.45 but a later node at the same hit faulted, so the query returned -1 and the
    // walk treated the feeder as a solid. POD-only body (no lambdas), so the __try carries no object unwinding.
    static float measure_node_at_hit(void *node, Vector3 hit, Vector3 pivot, Vector3 camera, uintptr_t mod_lo,
                                     uintptr_t mod_hi, float *out_head) noexcept
    {
        if (out_head != nullptr)
        {
            *out_head = -1.0f; // unmeasurable unless this node rasters
        }
        __try
        {
            const unsigned int rnd_flags = *reinterpret_cast<unsigned int *>(reinterpret_cast<std::byte *>(node) +
                                                                             Constants::RENDERNODE_RNDFLAGS_OFFSET);
            if (rnd_flags & Constants::ERF_HIDDEN)
            {
                return k_coverage_unavailable;
            }
            void **vt = *reinterpret_cast<void ***>(node);
            const auto get_type =
                reinterpret_cast<GetRenderNodeTypeFn>(vt[Constants::RENDERNODE_VTABLE_GETTYPE_OFFSET / 8]);
            if (get_type(node) != Constants::EERTYPE_BRUSH)
            {
                return k_coverage_unavailable;
            }
            const auto get_bbox = reinterpret_cast<GetBBoxFn>(vt[Constants::RENDERNODE_VTABLE_GETBBOX_OFFSET / 8]);
            float aabb[6] = {};
            const float *b = reinterpret_cast<const float *>(get_bbox(node, aabb));
            if (b == nullptr)
            {
                return k_coverage_unavailable;
            }
            const float min_x = b[0], min_y = b[1], min_z = b[2];
            const float max_x = b[3], max_y = b[4], max_z = b[5];
            if ((max_x - min_x) > Constants::RENDER_OCCLUSION_MAX_BRUSH_SIZE ||
                (max_y - min_y) > Constants::RENDER_OCCLUSION_MAX_BRUSH_SIZE ||
                (max_z - min_z) > Constants::RENDER_OCCLUSION_MAX_BRUSH_SIZE)
            {
                return k_coverage_unavailable; // large geometry (wall / building): let physics decide
            }
            if (hit.x < min_x || hit.x > max_x || hit.y < min_y || hit.y > max_y || hit.z < min_z || hit.z > max_z)
            {
                return k_coverage_unavailable; // bbox does not contain the physics hit point
            }
            void *brush_statobj = *reinterpret_cast<void **>(reinterpret_cast<std::byte *>(node) +
                                                             Constants::CBRUSH_STATOBJ_OFFSET);
            char brush_name[160];
            copy_brush_name(brush_statobj, brush_name, static_cast<int>(sizeof(brush_name)));
            if (name_is_hlod_proxy(brush_name))
            {
                return k_coverage_unavailable; // coarse building-scale LOD proxy: leave it to the physics fan
            }
            if (name_is_wall_opening(brush_name))
            {
                return k_coverage_unavailable; // window / hole frame on a wall: the solid wall behind is the occluder
            }
            // The bbox containing the hit is NOT enough: a nearby prop's bbox can enclose a point on a DIFFERENT
            // surface the ray truly hit. Require this brush's MESH to be at the hit (a transformed vertex within
            // COVERAGE_HIT_EPS); a compound shed / house roof (no readable mesh at the hit) thus falls through to
            // "solid" (physics decides -> collide), while a thin monument the ray actually hit is measured as thin.
            if (!mesh_near_point(node, hit, Constants::COVERAGE_HIT_EPS, mod_lo, mod_hi))
            {
                return k_coverage_unavailable;
            }
            return brush_char_coverage(node, pivot, camera, mod_lo, mod_hi, out_head);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return k_coverage_unavailable; // this node faulted -> skip it; other candidates are unaffected
        }
    }

    // SEH-isolated: the MAX character coverage over compact, VISIBLE (not ERF_HIDDEN), non-HLOD CBrushes whose
    // world AABB contains @p hit AND whose render mesh is readable. The brush at a physics hit point is the
    // visible object the camera collided with; its triangle coverage is the true occlusion. The physics collision
    // PROXY (what the ray-based gate measures) can be far broader than the thin visible mesh (a clothes-line /
    // tripod hull), so this corrects it. Returns -1 ("no opinion, defer to physics") when no such brush yields a
    // measurement: no compact brush contains the point (terrain / pure-physics body), every candidate is an HLOD
    // proxy, or the hit brush is a compound building whose ROOT mesh is null. The caller then falls back to the
    // physics-ray coverage, so large solids still block. POD-only body.
    static float coverage_at_point_guarded(void *p3d, GetObjectsInBoxFn query, const float *bbox, Vector3 hit,
                                           Vector3 pivot, Vector3 camera, uintptr_t mod_lo, uintptr_t mod_hi,
                                           void **out_node, float *out_head) noexcept
    {
        // Define both out-params up front so every no-measurement return path (count 0 / overflow, octree
        // fault, or no candidate beating best) leaves them well-defined without relying on the caller.
        if (out_node != nullptr)
        {
            *out_node = nullptr;
        }
        if (out_head != nullptr)
        {
            *out_head = -1.0f;
        }
        // The OCTREE query is guarded on its own; each candidate node is then measured under measure_node_at_hit's
        // own SEH frame, so a single unreadable neighbour can no longer discard the whole result. `nodes` lives
        // outside the query __try so the post-query loop (which has no SEH of its own) can read it.
        std::uint32_t count = 0;
        void *nodes[Constants::RENDER_OCCLUSION_MAX_NODES];
        __try
        {
            count = query(p3d, bbox, nullptr);
            if (count == 0 || count > static_cast<std::uint32_t>(Constants::RENDER_OCCLUSION_MAX_NODES))
            {
                return -1.0f;
            }
            query(p3d, bbox, nodes);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1.0f;
        }
        float best = -1.0f;
        float best_head = -1.0f;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            if (nodes[i] == nullptr)
            {
                continue;
            }
            float node_head = -1.0f;
            const float cov = measure_node_at_hit(nodes[i], hit, pivot, camera, mod_lo, mod_hi, &node_head);
            if (cov > best)
            {
                best = cov;
                best_head = node_head;
                if (out_node != nullptr)
                {
                    // the dominant occluder; the caller caches it to re-raster without re-querying the octree
                    *out_node = nodes[i];
                }
            }
        }
        if (out_head != nullptr)
        {
            *out_head = best_head;
        }
        return best;
    }

    std::optional<float> render_occlusion_limit(const Vector3 &pivot, const Vector3 &to_camera, float radius,
                                                float cov_thresh)
    {
        if (s_get_objects_in_box == nullptr || s_p3d_engine_slot_addr == 0)
        {
            return std::nullopt;
        }

        const float desired = to_camera.magnitude();
        if (desired < 1e-3f)
        {
            return std::nullopt;
        }
        const Vector3 camera = pivot + to_camera;

        // Overhead-only fast reject: a render-only roof can occlude the sightline only when the camera sits ABOVE
        // the pivot (a look-down raises it over the character). When the camera is at or below the pivot the
        // pivot->camera sightline only descends, so no overhead brush can lie on it -- skip the whole octree query.
        // This removes the render-octree cost from level and upward looks, which is most of normal play.
        if (camera.z <= pivot.z)
        {
            return std::nullopt;
        }

        // Throttle: the cloth is a STATIC brush, so the octree query + sightline ray-march is re-run only when
        // the camera has moved more than RENDER_OCCLUSION_REQUERY_DIST from the last query; otherwise the cached
        // clear distance is reused (the collision easing still runs smoothly). This collapses the per-frame
        // cost to ~nothing while standing still. Single render-thread caller, so plain statics are race-free.
        static bool s_cache_valid = false;
        static Vector3 s_cache_cam{};
        static float s_cache_block = k_cloth_unavailable;
        static float s_cache_cov = -1.0f; // cov_thresh the cache was computed with (toggling it forces a requery)

        const float requery_d2 = Constants::RENDER_OCCLUSION_REQUERY_DIST * Constants::RENDER_OCCLUSION_REQUERY_DIST;
        if (!s_cache_valid || s_cache_cov != cov_thresh || (camera - s_cache_cam).magnitude_squared() > requery_d2)
        {
            float block = k_cloth_unavailable;
            RoofHitInfo hit{};

            // Resolve p3DEngine fresh and screen it (set once the 3DEngine exists; must carry an in-image vtable).
            const auto p3d = DMK::Memory::seh_read<uintptr_t>(s_p3d_engine_slot_addr);
            if (p3d && *p3d != 0 && DMK::Memory::plausible_userspace_ptr(*p3d))
            {
                const auto vtable = DMK::Memory::seh_read<uintptr_t>(*p3d);
                if (vtable && DMK::Memory::plausible_userspace_ptr(*vtable) &&
                    DMK::Memory::contains(s_game_module, *vtable))
                {
                    // Query box bounding the pivot->camera arm, expanded by the standoff.
                    const float margin = radius + 0.05f;
                    float bbox[6];
                    bbox[0] = std::min(pivot.x, camera.x) - margin;
                    bbox[1] = std::min(pivot.y, camera.y) - margin;
                    bbox[2] = std::min(pivot.z, camera.z) - margin;
                    bbox[3] = std::max(pivot.x, camera.x) + margin;
                    bbox[4] = std::max(pivot.y, camera.y) + margin;
                    bbox[5] = std::max(pivot.z, camera.z) + margin;
                    block = nearest_sightline_block_guarded(reinterpret_cast<void *>(*p3d), s_get_objects_in_box,
                                                            bbox, pivot, camera, s_mod_lo, s_mod_hi, cov_thresh, &hit);
                }
            }

            s_cache_block = block;
            s_cache_cam = camera;
            s_cache_cov = cov_thresh;
            s_cache_valid = true;

            // Trace WHAT the clamp latched onto, so a false positive (a prop / wall chunk on the sightline
            // instead of a real canopy) is identifiable by name + size + position. Logged only on a re-query
            // that produced a REAL clamp (block < desired), so it neither spams every frame nor logs misses.
            if (block < desired)
            {
                char name[256];
                copy_brush_name(hit.statobj, name, static_cast<int>(sizeof(name)));
                DMK::Logger::get_instance().trace(
                    "RenderOcclusion HIT: node={} statobj={} name=\"{}\" blockDist={} of {} cov={} covThresh={} "
                    "blockZ={} sizeXYZ=({}, {}, {}) bboxMin=({}, {}, {}) cam=({}, {}, {}) pivot=({}, {}, {})",
                    DMK::Format::format_address(reinterpret_cast<uintptr_t>(hit.node)),
                    DMK::Format::format_address(reinterpret_cast<uintptr_t>(hit.statobj)), name, block, desired,
                    hit.coverage, cov_thresh, hit.roof_z, hit.max_x - hit.min_x, hit.max_y - hit.min_y,
                    hit.max_z - hit.min_z, hit.min_x, hit.min_y, hit.min_z, camera.x, camera.y, camera.z, pivot.x,
                    pivot.y, pivot.z);
            }
        }

        // The cached clear distance IS the allowed camera distance from the pivot: cloth occludes beyond it.
        if (s_cache_block >= desired)
        {
            return std::nullopt; // no cloth on the sightline within reach
        }
        return s_cache_block;
    }

    float render_coverage_at(const Vector3 &hit_point, const Vector3 &pivot, const Vector3 &to_camera, void **out_node,
                             float *out_head)
    {
        if (out_node != nullptr)
        {
            *out_node = nullptr;
        }
        if (out_head != nullptr)
        {
            *out_head = -1.0f;
        }
        if (s_get_objects_in_box == nullptr || s_p3d_engine_slot_addr == 0)
        {
            return -1.0f;
        }
        const auto p3d = DMK::Memory::seh_read<uintptr_t>(s_p3d_engine_slot_addr);
        if (!p3d || *p3d == 0 || !DMK::Memory::plausible_userspace_ptr(*p3d))
        {
            return -1.0f;
        }
        const auto vtable = DMK::Memory::seh_read<uintptr_t>(*p3d);
        if (!vtable || !DMK::Memory::plausible_userspace_ptr(*vtable) ||
            !DMK::Memory::contains(s_game_module, *vtable))
        {
            return -1.0f;
        }
        const Vector3 camera = pivot + to_camera;
        // Small octree box around the hit; the exact bbox-contains test inside the guard selects the brush.
        constexpr float m = 0.25f;
        float bbox[6];
        bbox[0] = hit_point.x - m;
        bbox[1] = hit_point.y - m;
        bbox[2] = hit_point.z - m;
        bbox[3] = hit_point.x + m;
        bbox[4] = hit_point.y + m;
        bbox[5] = hit_point.z + m;
        return coverage_at_point_guarded(reinterpret_cast<void *>(*p3d), s_get_objects_in_box, bbox, hit_point, pivot,
                                         camera, s_mod_lo, s_mod_hi, out_node, out_head);
    }

    float render_coverage_of_brush(void *node, const Vector3 &pivot, const Vector3 &camera, float *out_head) noexcept
    {
        if (out_head != nullptr)
        {
            *out_head = -1.0f;
        }
        if (node == nullptr)
        {
            return -1.0f;
        }
        __try
        {
            void **vt = *reinterpret_cast<void ***>(node);
            const auto vtaddr = reinterpret_cast<uintptr_t>(vt);
            if (vtaddr < s_mod_lo || vtaddr >= s_mod_hi)
            {
                return -1.0f; // vtable not in the game image -> not a render node we can trust
            }
            const auto get_type =
                reinterpret_cast<GetRenderNodeTypeFn>(vt[Constants::RENDERNODE_VTABLE_GETTYPE_OFFSET / 8]);
            if (get_type(node) != Constants::EERTYPE_BRUSH)
            {
                return -1.0f; // not a CBrush (vegetation / decal / a non-static owner) -> cannot rasterize
            }
            void *statobj =
                *reinterpret_cast<void **>(reinterpret_cast<std::byte *>(node) + Constants::CBRUSH_STATOBJ_OFFSET);
            char nm[160];
            copy_brush_name(statobj, nm, static_cast<int>(sizeof(nm)));
            if (name_is_hlod_proxy(nm))
            {
                return -1.0f; // building-scale HLOD proxy -> not a thin prop, leave it to the caller's solid path
            }
            return brush_char_coverage(node, pivot, camera, s_mod_lo, s_mod_hi, out_head);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1.0f;
        }
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

    bool render_hit_info(const Vector3 &hit_point, void *node, char *out_name, int out_sz, float ext[3],
                         int *out_kind) noexcept
    {
        if (out_name != nullptr && out_sz > 0)
        {
            out_name[0] = '\0';
        }
        ext[0] = ext[1] = ext[2] = 0.0f;
        if (out_kind != nullptr)
        {
            *out_kind = 0; // 0 none, 1 foreign-linked, 2 prop (mesh at hit), 3 solid (bbox only), 4 hlod
        }
        // Resolve the render-engine pointer up front (seh_read is itself guarded); keeping its std::optional local
        // out of the structured-exception frame below avoids object-unwinding (MSVC C2712) in the __try.
        void *p3d_ptr = nullptr;
        if (s_get_objects_in_box != nullptr && s_p3d_engine_slot_addr != 0)
        {
            const auto p3d = DMK::Memory::seh_read<uintptr_t>(s_p3d_engine_slot_addr);
            if (p3d && *p3d != 0 && DMK::Memory::plausible_userspace_ptr(*p3d))
            {
                p3d_ptr = reinterpret_cast<void *>(*p3d);
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
                    const auto get_type =
                        reinterpret_cast<GetRenderNodeTypeFn>(vt[Constants::RENDERNODE_VTABLE_GETTYPE_OFFSET / 8]);
                    if (get_type(node) == Constants::EERTYPE_BRUSH)
                    {
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
                        void *so = *reinterpret_cast<void **>(reinterpret_cast<std::byte *>(node) +
                                                              Constants::CBRUSH_STATOBJ_OFFSET);
                        copy_brush_name(so, out_name, out_sz);
                        if (out_kind != nullptr)
                        {
                            *out_kind = 1;
                        }
                        return true;
                    }
                }
            }
            // Case 2: foreign-null collider (merged / proxy, e.g. the bird_feeder / a monument). Identify the compact
            // brush at the hit point -- the same selection the coverage gate measures: prefer the brush whose mesh is
            // AT the hit, else the first brush that contains it (a compound building, reported so it is identifiable).
            if (p3d_ptr == nullptr)
            {
                return false;
            }
            constexpr float m = 0.25f;
            float bbox[6] = {hit_point.x - m, hit_point.y - m, hit_point.z - m,
                             hit_point.x + m, hit_point.y + m, hit_point.z + m};
            const std::uint32_t count = s_get_objects_in_box(p3d_ptr, bbox, nullptr);
            if (count == 0 || count > static_cast<std::uint32_t>(Constants::RENDER_OCCLUSION_MAX_NODES))
            {
                return false;
            }
            void *nodes[Constants::RENDER_OCCLUSION_MAX_NODES];
            s_get_objects_in_box(p3d_ptr, bbox, nodes);
            // Three tiers, most specific wins: prop (a brush whose MESH is at the hit = the real thin occluder) >
            // solid (a non-HLOD brush that merely contains the hit = a compound building wall, still named) > hlod
            // (a coarse level / building proxy, reported only if nothing better contains the hit so the log never
            // mislabels a wall as the giant level chunk). Stop early once a prop is found.
            char prop_name[160] = {};
            float prop_ext[3] = {};
            bool have_prop = false;
            char solid_name[160] = {};
            float solid_ext[3] = {};
            float solid_min = 1e30f;
            bool have_solid = false;
            char hlod_name[160] = {};
            float hlod_ext[3] = {};
            bool have_hlod = false;
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
                if (name_is_hlod_proxy(nm))
                {
                    if (!have_hlod)
                    {
                        copy_cstr(hlod_name, static_cast<int>(sizeof(hlod_name)), nm);
                        hlod_ext[0] = ex;
                        hlod_ext[1] = ey;
                        hlod_ext[2] = ez;
                        have_hlod = true;
                    }
                    continue;
                }
                if (mesh_near_point(n, hit_point, Constants::COVERAGE_HIT_EPS, s_mod_lo, s_mod_hi))
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
            else if (have_hlod)
            {
                chosen = hlod_name;
                chosen_ext = hlod_ext;
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

} // namespace TPVCamera
