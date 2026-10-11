/**
 * @file hooks/character_fade.cpp
 * @brief Implementation of the character fade (see character_fade.hpp).
 *
 * @details The camera frame on the main thread publishes the player's CEntity, the dissolve byte and a stamp through
 *          relaxed atomics. The detour runs on the render job threads, once per render proxy per pass, and only reads
 *          them: lock-free, no allocation, no logging. Each value is read on its own, so a detour that reads the
 *          entity of one frame and the byte of the next misjudges one draw at most.
 */

#include "character_fade.hpp"
#include "aob_resolver.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace TPVCamera
{

    namespace
    {
        // CRenderProxy::Render(this, const SRendParams& params, const SRenderingPassInfo& pass).
        using ProxyRenderFn = void(__fastcall *)(void *proxy, std::uint8_t *params, const std::uint8_t *pass);

        std::atomic<ProxyRenderFn> s_original{nullptr};
        std::atomic<bool> s_available{false};
        // The player's CEntity, the dissolve byte its render proxy gets, and when the camera frame last set them.
        std::atomic<std::uintptr_t> s_entity{0};
        std::atomic<std::uint32_t> s_byte{0};
        std::atomic<std::uint64_t> s_stamp_ms{0};
        // Main-view draws of the character the detour raised, for the once-a-second debug line.
        std::atomic<std::uint32_t> s_applies{0};

        /** @brief True for the main view: not a recursive (mirror), shadow-map or aux-window pass. */
        [[nodiscard]] bool main_view_pass(const std::uint8_t *pass) noexcept
        {
            return pass[Constants::PASS_INFO_RECURSION_OFFSET] == 0 && pass[Constants::PASS_INFO_SHADOW_OFFSET] == 0 &&
                   pass[Constants::PASS_INFO_AUX_WINDOW_OFFSET] == 0;
        }

        /**
         * @brief Combines @p ours with the engine's own LOD-crossfade value in the CLodValue at @p lod.
         * @details With LodA set (the "out" half, FOB_DISSOLVE_OUT) a higher byte hides more, so the larger wins; for
         *          the LOD fading in (LodA -1, LodB set, no OUT) a lower byte hides more, so the byte is lowered.
         */
        void raise_dissolve(std::uint8_t *lod, std::uint8_t ours) noexcept
        {
            const auto lod_a = *reinterpret_cast<const std::int16_t *>(lod + Constants::CLODVALUE_LOD_A_OFFSET);
            const auto lod_b = *reinterpret_cast<const std::int16_t *>(lod + Constants::CLODVALUE_LOD_B_OFFSET);
            std::uint8_t &ref = lod[Constants::CLODVALUE_DISSOLVE_OFFSET];
            if (lod_a >= 0)
            {
                ref = std::max(ref, ours);
            }
            else if (lod_b >= 0 && ref != 0)
            {
                ref = std::max<std::uint8_t>(1, std::min<std::uint8_t>(ref, static_cast<std::uint8_t>(255 - ours)));
            }
        }

        /**
         * @brief CRenderProxy::Render detour: fades the player's character in the main view.
         * @details The proxy copies @p params into every slot's render parameters (1.5.6 sub_1804A0084 at the top of
         *          sub_18049FD10), and the character slot hands the CLodValue dissolve byte (SRendParams +
         *          SRENDPARAMS_LOD_OFFSET + CLODVALUE_DISSOLVE_OFFSET) to its per-frame render objects with
         *          FOB_DISSOLVE (sub_18049E210 from the slot renderer sub_18049F08C). The byte is raised for the call
         *          and restored after it, so the caller's parameters are left as they were. Shadow passes are left
         *          alone, so the character keeps its shadow.
         * @note Raw arbitrary-signature detour (DMK [B-84]): it must not throw, and every call in it is noexcept.
         */
        void __fastcall proxy_render_detour(void *proxy, std::uint8_t *params, const std::uint8_t *pass) noexcept
        {
            const DetourScope in_flight;
            const ProxyRenderFn original = s_original.load(std::memory_order_acquire);
            const auto ours = static_cast<std::uint8_t>(s_byte.load(std::memory_order_relaxed));
            const std::uintptr_t character = s_entity.load(std::memory_order_relaxed);
            if (ours != 0 && character != 0 && proxy != nullptr && params != nullptr && pass != nullptr &&
                main_view_pass(pass) &&
                *reinterpret_cast<const std::uintptr_t *>(reinterpret_cast<std::uintptr_t>(proxy) +
                                                          Constants::RENDER_PROXY_ENTITY_OFFSET) == character &&
                GetTickCount64() - s_stamp_ms.load(std::memory_order_relaxed) <= Constants::FADE_STALE_MS)
            {
                s_applies.fetch_add(1, std::memory_order_relaxed);
                std::uint8_t *lod = params + Constants::SRENDPARAMS_LOD_OFFSET;
                const std::uint8_t saved = lod[Constants::CLODVALUE_DISSOLVE_OFFSET];
                raise_dissolve(lod, ours);
                original(proxy, params, pass);
                lod[Constants::CLODVALUE_DISSOLVE_OFFSET] = saved;
                return;
            }
            original(proxy, params, pass);
        }

        /**
         * @brief The close-up fade's debug log, from set_character_fade once per camera frame: the amount as it crosses
         *        each quarter (so a test at a partial fade can be found in the log), and once a second while the
         *        character fades, the amount and the main-view draws the detour raised per fading frame. A fading
         *        character with no raised draws means the detour never matched the character's render proxy.
         * @note Main thread only.
         */
        void log_character_fade(float amount) noexcept
        {
            static int s_quarter = 0;
            static int s_fading_frames = 0;
            static std::uint64_t s_window_start_ms = 0;

            // Rising, the highest mark passed; falling, the lowest mark gone under.
            const int quarter = static_cast<int>(std::floor(std::clamp(amount, 0.0f, 1.0f) * 4.0f));
            if (quarter != s_quarter)
            {
                const bool rising = quarter > s_quarter;
                (void)DMK::log().try_log(DMK::LogLevel::Debug, "CloseUpFade: close amount {:.2f} {} {:.2f}", amount,
                                         rising ? "reached" : "fell below",
                                         0.25f * static_cast<float>(rising ? quarter : quarter + 1));
                s_quarter = quarter;
            }
            if (amount > 0.0f)
            {
                ++s_fading_frames;
            }

            const std::uint64_t now_ms = GetTickCount64();
            if (now_ms - s_window_start_ms < 1000)
            {
                return;
            }
            const std::uint32_t applies = s_applies.exchange(0, std::memory_order_relaxed);
            if (s_fading_frames > 0)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "CloseUpFade: amount {:.2f}, raised draws per fading frame {:.1f}", amount,
                                         static_cast<float>(applies) / static_cast<float>(s_fading_frames));
            }
            s_fading_frames = 0;
            s_window_start_ms = now_ms;
        }

    } // namespace

    DMK::Result<void> initialize_character_fade(HookSet &hooks)
    {
        const std::uintptr_t target = gated_anchor_address(Feature::CharacterFade, AnchorId::ProxyRender);
        if (target == 0)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "character_fade/proxy_render_anchor"});
        }
        auto result = DMK::hook::inline_at(
            DMK::hook::InlineRequest{
                .name = "CRenderProxyRender",
                .target = DMK::Address{target},
            },
            proxy_render_detour);
        if (!result.has_value())
        {
            return std::unexpected(result.error());
        }
        s_original.store(result->original<ProxyRenderFn>(), std::memory_order_release);
        if (auto armed = hooks.push(std::move(*result)).enable(); !armed.has_value())
        {
            return std::unexpected(armed.error());
        }
        s_available.store(true, std::memory_order_release);
        DMK::log().info("CharacterFade: hook on CRenderProxy::Render at {}", DMK::format::format_address(target));
        return {};
    }

    bool character_fade_available() noexcept
    {
        return s_available.load(std::memory_order_acquire);
    }

    void set_character_fade(std::uintptr_t entity, float amount) noexcept
    {
        const int byte = static_cast<int>(
            std::lround(std::clamp(amount, 0.0f, 1.0f) * static_cast<float>(Constants::DISSOLVE_MAX_BYTE)));
        log_character_fade(amount);
        s_entity.store(entity, std::memory_order_relaxed);
        s_stamp_ms.store(GetTickCount64(), std::memory_order_relaxed);
        s_byte.store(entity != 0 ? static_cast<std::uint32_t>(byte) : 0u, std::memory_order_relaxed);
    }

} // namespace TPVCamera
