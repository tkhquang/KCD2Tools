/**
 * @file hooks/character_fade.cpp
 * @brief Implementation of the character fade (see character_fade.hpp).
 *
 * @details The camera frame on the main thread publishes the player's CEntity, the dissolve byte, a stamp and the
 *          carried items' render proxies through relaxed atomics. The detour runs on the render job threads, once per
 *          render proxy per pass, and only reads them: lock-free, no allocation, no logging. Each value is read on its
 *          own, so a detour that reads the entity of one frame and the byte of the next misjudges one draw at most.
 */

#include "character_fade.hpp"
#include "aob_resolver.hpp"
#include "character_attachments.hpp"
#include "constants.hpp"
#include "dissolve.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <span>
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
        // The render proxy the detour last saw draw that entity in the main view (character_render_node()).
        std::atomic<std::uintptr_t> s_character_node{0};
        // The render proxies of what the character carries (what is bound to its attachments), faded with it. The
        // camera frame writes the entries, then the class bits (bit i set: entry i draws a character model, a bow or a
        // crossbow; clear: a static mesh), then the count, which the detour reads first. A detour that reads the count
        // of one frame and an entry of the next misjudges at most one draw.
        constexpr int k_max_carried = Constants::CHARACTER_FADE_MAX_CARRIED;
        std::array<std::atomic<std::uintptr_t>, k_max_carried> s_carried_proxies{};
        std::atomic<std::uint32_t> s_carried_character_mask{0};
        std::atomic<int> s_carried_count{0};
        // Main-view draws the detour raised, by kind of node, for the once-a-second debug line.
        std::atomic<std::uint32_t> s_applies{0};
        std::atomic<std::uint32_t> s_carried_character_applies{0};
        std::atomic<std::uint32_t> s_carried_static_applies{0};

        /** @brief What a drawn render proxy is to the character fade. */
        enum class ProxyKind : std::uint8_t
        {
            None,
            Character,
            CarriedCharacter,
            CarriedStatic,
        };

        /** @brief Whether @p proxy renders the character, a character model it carries, or a static mesh it carries. */
        [[nodiscard]] ProxyKind character_proxy_kind(const void *proxy) noexcept
        {
            const auto address = reinterpret_cast<std::uintptr_t>(proxy);
            if (*reinterpret_cast<const std::uintptr_t *>(address + Constants::RENDER_PROXY_ENTITY_OFFSET) ==
                s_entity.load(std::memory_order_relaxed))
            {
                return ProxyKind::Character;
            }
            const int count = std::clamp(s_carried_count.load(std::memory_order_acquire), 0, k_max_carried);
            for (int i = 0; i < count; ++i)
            {
                if (s_carried_proxies[static_cast<std::size_t>(i)].load(std::memory_order_relaxed) == address)
                {
                    return ((s_carried_character_mask.load(std::memory_order_relaxed) >> i) & 1u) != 0
                               ? ProxyKind::CarriedCharacter
                               : ProxyKind::CarriedStatic;
                }
            }
            return ProxyKind::None;
        }

        /** @brief Counts one raised main-view draw of a node of @p kind, for the once-a-second debug line. */
        void count_apply(ProxyKind kind) noexcept
        {
            switch (kind)
            {
            case ProxyKind::Character:
                s_applies.fetch_add(1, std::memory_order_relaxed);
                break;
            case ProxyKind::CarriedCharacter:
                s_carried_character_applies.fetch_add(1, std::memory_order_relaxed);
                break;
            case ProxyKind::CarriedStatic:
                s_carried_static_applies.fetch_add(1, std::memory_order_relaxed);
                break;
            case ProxyKind::None:
                break;
            }
        }

        /**
         * @brief CRenderProxy::Render detour: fades the player's character, and what it carries, in the main view.
         * @details The proxy copies @p params into every slot's render parameters (1.5.6 sub_1804A0084 at the top of
         *          sub_18049FD10), and the character slot hands the CLodValue dissolve byte (SRendParams +
         *          SRENDPARAMS_LOD_OFFSET + CLODVALUE_DISSOLVE_OFFSET) to its per-frame render objects with
         *          FOB_DISSOLVE (sub_18049E210 from the slot renderer sub_18049F08C). A static slot hands the same copy
         *          to CStatObj::Render (sub_18049E258), which passes its CLodValue to
         *          AddOrCreatePersistentRenderObject, so a carried static mesh fades through the same byte. The byte is
         *          raised for the call and restored after it, so the caller's parameters are left as they were. Shadow
         *          passes are left alone, so the character keeps its shadow. Each main-view draw of the character
         *          publishes its proxy (character_render_node()).
         * @note Raw arbitrary-signature detour (DMK [B-84]): it must not throw, and every call in it is noexcept.
         */
        void __fastcall proxy_render_detour(void *proxy, std::uint8_t *params, const std::uint8_t *pass) noexcept
        {
            const DetourScope in_flight;
            const ProxyRenderFn original = s_original.load(std::memory_order_acquire);
            const std::uintptr_t character = s_entity.load(std::memory_order_relaxed);
            if (character != 0 && proxy != nullptr && pass != nullptr && main_view_pass(pass))
            {
                const auto address = reinterpret_cast<std::uintptr_t>(proxy);
                if (*reinterpret_cast<const std::uintptr_t *>(address + Constants::RENDER_PROXY_ENTITY_OFFSET) ==
                        character &&
                    s_character_node.load(std::memory_order_relaxed) != address)
                {
                    s_character_node.store(address, std::memory_order_relaxed);
                }
            }
            const auto ours = static_cast<std::uint8_t>(s_byte.load(std::memory_order_relaxed));
            if (ours != 0 && proxy != nullptr && params != nullptr && pass != nullptr && main_view_pass(pass))
            {
                if (const ProxyKind kind = character_proxy_kind(proxy);
                    kind != ProxyKind::None &&
                    GetTickCount64() - s_stamp_ms.load(std::memory_order_relaxed) <= Constants::FADE_STALE_MS)
                {
                    count_apply(kind);
                    std::uint8_t *lod = params + Constants::SRENDPARAMS_LOD_OFFSET;
                    const std::uint8_t saved = lod[Constants::CLODVALUE_DISSOLVE_OFFSET];
                    raise_dissolve(lod, ours);
                    original(proxy, params, pass);
                    lod[Constants::CLODVALUE_DISSOLVE_OFFSET] = saved;
                    return;
                }
            }
            original(proxy, params, pass);
        }

        /**
         * @brief The close-up fade's debug log, from set_character_fade once per camera frame: the amount as it crosses
         *        each quarter (so a test at a partial fade can be found in the log), and once a second while the
         *        character fades, the amount, the carried set's size and how often it changed, the attachment walk's
         *        time, and the main-view draws the detour raised per fading frame for each kind of node. A carried set
         *        that keeps changing while the character stands still is an item dropping in and out of the fade, and a
         *        fading character with no raised draws means the detour never matched the character's render proxy.
         * @note Main thread only.
         */
        void log_character_fade(float amount, std::span<const std::uintptr_t> carried, float walk_us) noexcept
        {
            static std::array<std::uintptr_t, k_max_carried> s_last{};
            static std::size_t s_last_count = 0;
            static int s_quarter = 0;
            static int s_fading_frames = 0;
            static int s_walks = 0;
            static int s_set_changes = 0;
            static float s_walk_sum_us = 0.0f;
            static float s_walk_max_us = 0.0f;
            static std::uint64_t s_window_start_ms = 0;

            if (carried.size() != s_last_count || !std::equal(carried.begin(), carried.end(), s_last.begin()))
            {
                ++s_set_changes;
                std::copy(carried.begin(), carried.end(), s_last.begin());
                s_last_count = carried.size();
            }
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
            if (walk_us > 0.0f)
            {
                ++s_walks;
                s_walk_sum_us += walk_us;
                s_walk_max_us = std::max(s_walk_max_us, walk_us);
            }

            const std::uint64_t now_ms = GetTickCount64();
            if (now_ms - s_window_start_ms < 1000)
            {
                return;
            }
            const std::uint32_t character = s_applies.exchange(0, std::memory_order_relaxed);
            const std::uint32_t carried_character = s_carried_character_applies.exchange(0, std::memory_order_relaxed);
            const std::uint32_t carried_static = s_carried_static_applies.exchange(0, std::memory_order_relaxed);
            if (s_fading_frames > 0)
            {
                const auto frames = static_cast<float>(s_fading_frames);
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "CloseUpFade: amount {:.2f}, carried {} ({} set change(s) in the last second), walk {:.1f} us "
                    "average, {:.1f} us max, raised draws per fading frame: character {:.1f}, carried character "
                    "{:.1f}, carried static {:.1f}",
                    amount, carried.size(), s_set_changes,
                    (s_walks > 0) ? s_walk_sum_us / static_cast<float>(s_walks) : 0.0f, s_walk_max_us,
                    static_cast<float>(character) / frames, static_cast<float>(carried_character) / frames,
                    static_cast<float>(carried_static) / frames);
            }
            s_fading_frames = 0;
            s_walks = 0;
            s_set_changes = 0;
            s_walk_sum_us = 0.0f;
            s_walk_max_us = 0.0f;
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

    void set_character_fade(std::uintptr_t entity, float amount, const CharacterNodes *nodes) noexcept
    {
        const int byte = static_cast<int>(
            std::lround(std::clamp(amount, 0.0f, 1.0f) * static_cast<float>(Constants::DISSOLVE_MAX_BYTE)));
        const std::span<const std::uintptr_t> carried =
            (nodes != nullptr) ? nodes->carried() : std::span<const std::uintptr_t>{};
        const int count = std::min(static_cast<int>(carried.size()), k_max_carried);
        for (int i = 0; i < count; ++i)
        {
            s_carried_proxies[static_cast<std::size_t>(i)].store(carried[static_cast<std::size_t>(i)],
                                                                 std::memory_order_relaxed);
        }
        s_carried_character_mask.store((nodes != nullptr) ? nodes->character_mask : 0u, std::memory_order_relaxed);
        s_carried_count.store(count, std::memory_order_release);
        log_character_fade(amount, carried.first(static_cast<std::size_t>(count)),
                           (nodes != nullptr) ? nodes->walk_us : 0.0f);
        // Another entity (none, a reload, another player body): its proxy is learned when it draws.
        if (s_entity.exchange(entity, std::memory_order_relaxed) != entity)
        {
            s_character_node.store(0, std::memory_order_relaxed);
        }
        s_stamp_ms.store(GetTickCount64(), std::memory_order_relaxed);
        s_byte.store(entity != 0 ? static_cast<std::uint32_t>(byte) : 0u, std::memory_order_relaxed);
    }

    std::uintptr_t character_render_node() noexcept
    {
        return s_character_node.load(std::memory_order_relaxed);
    }

} // namespace TPVCamera
