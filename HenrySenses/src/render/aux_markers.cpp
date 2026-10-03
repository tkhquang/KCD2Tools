/**
 * @file render/aux_markers.cpp
 * @brief Corner-bracket markers drawn through the main thread's aux-geom command buffer.
 */

#include "render/aux_markers.hpp"
#include "aob_resolver.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "rtti_types.hpp"
#include "engine/engine_env.hpp"
#include "engine/seh.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace HenrySenses
{
    namespace
    {
        /** @brief Engine ColorB. */
        struct AuxColor
        {
            std::uint8_t r;
            std::uint8_t g;
            std::uint8_t b;
            std::uint8_t a;
        };

        /** @brief Engine SAuxGeomRenderFlags (a packed u32). */
        struct AuxFlags
        {
            std::uint32_t value;
        };

        using GetAuxFn = std::uintptr_t(__fastcall *)(std::uintptr_t renderer);
        using SetFlagsFn = AuxFlags *(__fastcall *)(std::uintptr_t aux, AuxFlags *previous, const AuxFlags *flags);
        using GetFlagsFn = AuxFlags *(__fastcall *)(std::uintptr_t aux, AuxFlags *out);
        using DrawLinesFn = void(__fastcall *)(
            std::uintptr_t aux,
            const game_structures::Vec3f *vertices,
            std::uint32_t count,
            const AuxColor *color,
            float thickness
        );

        constexpr std::size_t BRACKET_VERTICES = 48; // 8 corners x 3 edges x 2 endpoints
        constexpr float LINE_THICKNESS = 2.0f;
        constexpr float BRACKET_FRACTION = 0.25f;
        constexpr float BRACKET_MIN = 0.05f;
        constexpr float BRACKET_MAX = 0.5f;
        constexpr float MARKER_ALPHA = 224.0f;

        std::atomic<bool> s_available{false};
        std::atomic<bool> s_faulted{false};

        [[nodiscard]] std::uintptr_t call_get_aux(std::uintptr_t fn, std::uintptr_t renderer) noexcept
        {
            __try
            {
                return reinterpret_cast<GetAuxFn>(fn)(renderer);
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return 0;
            }
        }

        [[nodiscard]] bool call_swap_flags(
            std::uintptr_t get_fn,
            std::uintptr_t set_fn,
            std::uintptr_t aux,
            std::uint32_t flags,
            std::uint32_t *previous
        ) noexcept
        {
            __try
            {
                AuxFlags old{};
                reinterpret_cast<GetFlagsFn>(get_fn)(aux, &old);
                const AuxFlags wanted{flags};
                AuxFlags ignored{};
                reinterpret_cast<SetFlagsFn>(set_fn)(aux, &ignored, &wanted);
                *previous = old.value;
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        [[nodiscard]] bool call_set_flags(std::uintptr_t set_fn, std::uintptr_t aux, std::uint32_t flags) noexcept
        {
            __try
            {
                const AuxFlags wanted{flags};
                AuxFlags ignored{};
                reinterpret_cast<SetFlagsFn>(set_fn)(aux, &ignored, &wanted);
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        [[nodiscard]] bool call_draw_lines(
            std::uintptr_t fn,
            std::uintptr_t aux,
            const game_structures::Vec3f *vertices,
            std::uint32_t count,
            const AuxColor *color
        ) noexcept
        {
            __try
            {
                reinterpret_cast<DrawLinesFn>(fn)(aux, vertices, count, color, LINE_THICKNESS);
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        /**
         * @brief Builds the 24 corner-bracket segments of a box as a line list.
         * @param box World-space bounds.
         * @param out Receives 48 vertices.
         */
        void build_brackets(const game_structures::Aabb &box, std::array<game_structures::Vec3f, BRACKET_VERTICES> &out)
        {
            auto arm = [](float extent) { return std::clamp(extent * BRACKET_FRACTION, BRACKET_MIN, BRACKET_MAX); };
            const float ax = arm(box.max.x - box.min.x);
            const float ay = arm(box.max.y - box.min.y);
            const float az = arm(box.max.z - box.min.z);
            std::size_t n = 0;
            for (int corner = 0; corner < 8; ++corner)
            {
                const bool hx = (corner & 1) != 0;
                const bool hy = (corner & 2) != 0;
                const bool hz = (corner & 4) != 0;
                const game_structures::Vec3f p{
                    hx ? box.max.x : box.min.x,
                    hy ? box.max.y : box.min.y,
                    hz ? box.max.z : box.min.z
                };
                const float sx = hx ? -ax : ax;
                const float sy = hy ? -ay : ay;
                const float sz = hz ? -az : az;
                out[n++] = p;
                out[n++] = game_structures::Vec3f{p.x + sx, p.y, p.z};
                out[n++] = p;
                out[n++] = game_structures::Vec3f{p.x, p.y + sy, p.z};
                out[n++] = p;
                out[n++] = game_structures::Vec3f{p.x, p.y, p.z + sz};
            }
        }
    } // namespace

    DMK::Result<void> initialize_aux_markers()
    {
        if (!feature_ready(Feature::AuxMarkers))
        {
            DMK::log().warning("AuxMarkers: the aux-geometry gate failed; markers are off");
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "aux_markers/gate"});
        }
        s_faulted.store(false, std::memory_order_relaxed);
        s_available.store(true, std::memory_order_release);
        DMK::log().info(
            "AuxMarkers: ready (GetIRenderAuxGeom {}, DrawLines {}, marker flags {:#x})",
            DMK::format::format_address(anchor_address(AnchorId::GetAuxGeom)),
            DMK::format::format_address(anchor_address(AnchorId::AuxDrawLines)),
            constants::AUX_MARKER_RENDER_FLAGS
        );
        return {};
    }

    void shutdown_aux_markers() noexcept
    {
        s_available.store(false, std::memory_order_release);
    }

    bool aux_markers_available() noexcept
    {
        return s_available.load(std::memory_order_acquire) && !s_faulted.load(std::memory_order_relaxed);
    }

    std::size_t draw_markers(std::span<const MarkerRequest> markers, float opacity) noexcept
    {
        if (markers.empty() || !aux_markers_available() || opacity <= 0.0f)
        {
            return 0;
        }
        const std::uintptr_t renderer = genv_interface(constants::GENV_RENDERER_OFFSET);
        if (renderer == 0 || !object_is(GameClass::Renderer, renderer))
        {
            return 0;
        }
        const std::uintptr_t get_aux =
            validated_vtable_slot(renderer, constants::RENDERER_VTABLE_GET_AUX_GEOM_OFFSET, AnchorId::GetAuxGeom);
        if (get_aux == 0)
        {
            return 0;
        }
        const std::uintptr_t aux = call_get_aux(get_aux, renderer);
        if (aux == 0 || !object_is(GameClass::AuxGeom, aux))
        {
            // A CAuxGeomCB_Null (aux geometry compiled out or r_enableAuxGeom 0) is not an error.
            return 0;
        }
        const std::uintptr_t set_flags =
            validated_vtable_slot(aux, constants::AUX_VTABLE_SET_RENDER_FLAGS_OFFSET, AnchorId::AuxSetFlags);
        const std::uintptr_t get_flags =
            validated_vtable_slot(aux, constants::AUX_VTABLE_GET_RENDER_FLAGS_OFFSET, AnchorId::AuxGetFlags);
        const std::uintptr_t draw_lines =
            validated_vtable_slot(aux, constants::AUX_VTABLE_DRAW_LINES_OFFSET, AnchorId::AuxDrawLines);
        if (set_flags == 0 || get_flags == 0 || draw_lines == 0)
        {
            return 0;
        }

        // SeeThrough = false: the scene depth hides the markers as it hides the silhouettes.
        const std::uint32_t render_flags = settings().see_through.load(std::memory_order_relaxed)
                                               ? constants::AUX_MARKER_RENDER_FLAGS
                                               : constants::AUX_MARKER_RENDER_FLAGS & ~constants::AUX_DEPTH_TEST_OFF;
        std::uint32_t previous = 0;
        if (!call_swap_flags(get_flags, set_flags, aux, render_flags, &previous))
        {
            s_faulted.store(true, std::memory_order_relaxed);
            return 0;
        }
        const auto alpha = static_cast<std::uint8_t>(std::lround(MARKER_ALPHA * std::clamp(opacity, 0.0f, 1.0f)));
        std::size_t drawn = 0;
        std::array<game_structures::Vec3f, BRACKET_VERTICES> vertices{};
        for (const MarkerRequest &marker : markers)
        {
            build_brackets(marker.bounds, vertices);
            const AuxColor color{
                static_cast<std::uint8_t>(marker.color_word >> 24),
                static_cast<std::uint8_t>(marker.color_word >> 16),
                static_cast<std::uint8_t>(marker.color_word >> 8),
                alpha
            };
            if (!call_draw_lines(draw_lines, aux, vertices.data(), static_cast<std::uint32_t>(vertices.size()), &color))
            {
                s_faulted.store(true, std::memory_order_relaxed);
                break;
            }
            ++drawn;
        }
        (void)call_set_flags(set_flags, aux, previous);
        if (s_faulted.load(std::memory_order_relaxed))
        {
            (void)DMK::log().log_noexcept(
                DMK::LogLevel::Error,
                "AuxMarkers: a draw call faulted; markers are disabled for this session"
            );
        }
        return drawn;
    }

} // namespace HenrySenses
