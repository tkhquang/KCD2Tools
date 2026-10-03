/**
 * @file engine/octree_query.cpp
 * @brief Render-octree queries: the render nodes in a box, and a node's world bounds.
 */

#include "engine/octree_query.hpp"
#include "aob_resolver.hpp"
#include "constants.hpp"
#include "rtti_types.hpp"
#include "engine/engine_env.hpp"
#include "engine/seh.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace HenrySenses
{
    namespace
    {
        using GetObjectsInBoxFn = std::uint32_t(__fastcall *)(std::uintptr_t engine, const float *box, void **out);
        using GetObjectsByTypeInBoxFn = std::uint32_t(__fastcall *)(
            std::uintptr_t engine,
            std::uint32_t type,
            const float *box,
            void **out,
            std::uint64_t rnd_flags_mask
        );
        using GetBBoxFn = void *(__fastcall *)(std::uintptr_t node, float *out);
        using GetTypeFn = std::uint32_t(__fastcall *)(std::uintptr_t node);

        constexpr std::uint32_t QUERY_FAULT = 0xFFFFFFFFu;
        // Slack over the counted size, in case the octree grows between the counting call and the copying call.
        constexpr std::size_t NODE_SLACK = 256;

        // The query query_render_nodes_of_types() ran last (0 none yet, 1 typed, 2 untyped), so a change is logged
        // once. Main thread only.
        int s_query_path = 0;

        [[nodiscard]] std::uint32_t
        call_get_objects_in_box(std::uintptr_t fn, std::uintptr_t engine, const float *box, void **out) noexcept
        {
            __try
            {
                return reinterpret_cast<GetObjectsInBoxFn>(fn)(engine, box, out);
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return QUERY_FAULT;
            }
        }

        [[nodiscard]] std::uint32_t call_get_objects_by_type_in_box(
            std::uintptr_t fn,
            std::uintptr_t engine,
            std::uint32_t type,
            const float *box,
            void **out,
            std::uint64_t rnd_flags_mask
        ) noexcept
        {
            __try
            {
                return reinterpret_cast<GetObjectsByTypeInBoxFn>(fn)(engine, type, box, out, rnd_flags_mask);
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return QUERY_FAULT;
            }
        }

        [[nodiscard]] std::uint32_t call_get_type(std::uintptr_t fn, std::uintptr_t node) noexcept
        {
            __try
            {
                return reinterpret_cast<GetTypeFn>(fn)(node);
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return 0;
            }
        }

        /**
         * @brief C3DEngine::GetObjectsByTypeInBox, read from the engine's vtable once the slot after it is confirmed
         *        to hold the GetObjectsInBox anchor.
         * @return The function, or 0 when the vtable does not have the expected layout.
         */
        [[nodiscard]] std::uintptr_t typed_query_function(std::uintptr_t engine) noexcept
        {
            if (validated_vtable_slot(
                    engine,
                    constants::ENGINE3D_VTABLE_GET_OBJECTS_IN_BOX_OFFSET,
                    AnchorId::GetObjectsInBox
                ) == 0)
            {
                return 0;
            }
            return read_vtable_slot(engine, constants::ENGINE3D_VTABLE_GET_OBJECTS_BY_TYPE_IN_BOX_OFFSET);
        }

        void log_query_path(bool typed, std::uintptr_t fn)
        {
            const int path = typed ? 1 : 2;
            if (s_query_path == path)
            {
                return;
            }
            s_query_path = path;
            if (typed)
            {
                (void)DMK::log()
                    .try_log(DMK::LogLevel::Info, "Octree: typed queries through GetObjectsByTypeInBox 0x{:016X}", fn);
            }
            else
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "Octree: GetObjectsByTypeInBox is not next to GetObjectsInBox in the engine vtable; typed "
                    "queries filter the full query instead"
                );
            }
        }

        [[nodiscard]] bool call_get_bbox(std::uintptr_t fn, std::uintptr_t node, float *out) noexcept
        {
            __try
            {
                reinterpret_cast<GetBBoxFn>(fn)(node, out);
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }
    } // namespace

    OctreeQueryResult
    query_render_nodes(const game_structures::Aabb &box, std::vector<std::uintptr_t> &out, std::uint32_t cap)
    {
        out.clear();
        const std::uintptr_t fn = gated_anchor_address(Feature::Octree, AnchorId::GetObjectsInBox);
        const std::uintptr_t engine = genv_interface(constants::GENV_3DENGINE_OFFSET);
        if (fn == 0 || engine == 0 || !object_is(GameClass::ThreeDEngine, engine))
        {
            return OctreeQueryResult::Unavailable;
        }
        const std::array<float, 6> raw{box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z};
        const std::uint32_t counted = call_get_objects_in_box(fn, engine, raw.data(), nullptr);
        if (counted == QUERY_FAULT)
        {
            return OctreeQueryResult::Faulted;
        }
        if (counted > cap)
        {
            return OctreeQueryResult::TooMany;
        }
        if (counted == 0)
        {
            return OctreeQueryResult::Ok;
        }
        std::vector<void *> nodes(static_cast<std::size_t>(counted) + NODE_SLACK, nullptr);
        const std::uint32_t copied = call_get_objects_in_box(fn, engine, raw.data(), nodes.data());
        if (copied == QUERY_FAULT || copied > nodes.size())
        {
            return OctreeQueryResult::Faulted;
        }
        out.reserve(copied);
        for (std::uint32_t i = 0; i < copied; ++i)
        {
            if (nodes[i] != nullptr)
            {
                out.push_back(reinterpret_cast<std::uintptr_t>(nodes[i]));
            }
        }
        return OctreeQueryResult::Ok;
    }

    OctreeQueryResult query_render_nodes_of_types(
        const game_structures::Aabb &box,
        std::span<const std::uint32_t> types,
        std::vector<std::uintptr_t> &out,
        std::uint32_t cap,
        std::uint64_t rnd_flags_mask
    )
    {
        out.clear();
        const std::uintptr_t anchor = gated_anchor_address(Feature::Octree, AnchorId::GetObjectsInBox);
        const std::uintptr_t engine = genv_interface(constants::GENV_3DENGINE_OFFSET);
        if (anchor == 0 || engine == 0 || !object_is(GameClass::ThreeDEngine, engine))
        {
            return OctreeQueryResult::Unavailable;
        }
        const std::uintptr_t fn = typed_query_function(engine);
        log_query_path(fn != 0, fn);
        if (fn == 0)
        {
            // The untyped query keeps every node, so the type and the flag mask are applied here, in the typed
            // walk's order (the type first, then the flags).
            const OctreeQueryResult result = query_render_nodes(box, out, cap);
            std::erase_if(
                out,
                [types, rnd_flags_mask](std::uintptr_t node)
                {
                    if (std::find(types.begin(), types.end(), render_node_type(node)) == types.end())
                    {
                        return true;
                    }
                    if (rnd_flags_mask == constants::ENGINE3D_QUERY_ANY_RNDFLAGS)
                    {
                        return false;
                    }
                    const auto flags =
                        DMK::memory::read<std::uint64_t>(DMK::Address{node + constants::RENDERNODE_RNDFLAGS_OFFSET});
                    return !flags || (*flags & rnd_flags_mask) == 0;
                }
            );
            return result;
        }
        const std::array<float, 6> raw{box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z};
        std::vector<void *> nodes;
        std::size_t total = 0;
        for (const std::uint32_t type : types)
        {
            const std::uint32_t counted =
                call_get_objects_by_type_in_box(fn, engine, type, raw.data(), nullptr, rnd_flags_mask);
            if (counted == QUERY_FAULT)
            {
                out.clear();
                return OctreeQueryResult::Faulted;
            }
            total += counted;
            if (total > cap)
            {
                out.clear();
                return OctreeQueryResult::TooMany;
            }
            if (counted == 0)
            {
                continue;
            }
            nodes.assign(static_cast<std::size_t>(counted) + NODE_SLACK, nullptr);
            const std::uint32_t copied =
                call_get_objects_by_type_in_box(fn, engine, type, raw.data(), nodes.data(), rnd_flags_mask);
            if (copied == QUERY_FAULT || copied > nodes.size())
            {
                out.clear();
                return OctreeQueryResult::Faulted;
            }
            for (std::uint32_t i = 0; i < copied; ++i)
            {
                if (nodes[i] != nullptr)
                {
                    out.push_back(reinterpret_cast<std::uintptr_t>(nodes[i]));
                }
            }
        }
        return OctreeQueryResult::Ok;
    }

    std::uint32_t render_node_type(std::uintptr_t node) noexcept
    {
        const std::uintptr_t fn = read_vtable_slot(node, constants::RENDERNODE_VTABLE_GET_TYPE_OFFSET);
        return fn != 0 ? call_get_type(fn, node) : 0;
    }

    std::optional<game_structures::Aabb> render_node_bounds(std::uintptr_t node) noexcept
    {
        if (node == 0)
        {
            return std::nullopt;
        }
        const std::uintptr_t fn = read_vtable_slot(node, constants::RENDERNODE_VTABLE_GET_BBOX_OFFSET);
        if (fn == 0)
        {
            return std::nullopt;
        }
        std::array<float, 6> raw{};
        if (!call_get_bbox(fn, node, raw.data()))
        {
            return std::nullopt;
        }
        const game_structures::Aabb bounds{{raw[0], raw[1], raw[2]}, {raw[3], raw[4], raw[5]}};
        for (const float v : raw)
        {
            if (!std::isfinite(v))
            {
                return std::nullopt;
            }
        }
        if (bounds.min.x > bounds.max.x || bounds.min.y > bounds.max.y || bounds.min.z > bounds.max.z)
        {
            return std::nullopt;
        }
        return bounds;
    }

} // namespace HenrySenses
