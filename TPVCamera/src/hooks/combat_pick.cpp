/**
 * @file hooks/combat_pick.cpp
 * @brief The mid hook that hands the combat target pick the camera's direction (see combat_pick.hpp).
 */

#include "hooks/combat_pick.hpp"
#include "aob_resolver.hpp"

#include <DetourModKit.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace TPVCamera
{
    namespace
    {
        // The part of a CCamera the pick reads through rax: the forward's x at +0x4 and y at +0x14 (its matrix's second
        // column, rows 0 and 1). The pick reads nothing else from it. The camera thread can rewrite the two lanes
        // during a pick: a pair read across a rewrite mixes two frames' directions, a few hundredths of a degree apart.
        constexpr std::size_t k_forward_x_lane = 1;
        constexpr std::size_t k_forward_y_lane = 5;
        static_assert(sizeof(std::atomic<float>) == sizeof(float) && std::atomic<float>::is_always_lock_free,
                      "the pick reads the lanes as plain floats");
        alignas(64) std::array<std::atomic<float>, 6> s_view{};

        // The steady clock (ns) the published forward lapses at, 0 for none.
        std::atomic<std::int64_t> s_valid_until{0};
        // A frame's forward stands this long, so the pick falls back to the rendered camera soon after the camera stops
        // publishing (a load, a rig that fails to build, first person).
        constexpr std::int64_t k_publish_lifetime_ns = 250'000'000;

        std::atomic<std::uint32_t> s_used{0};

        [[nodiscard]] std::int64_t steady_now_ns() noexcept
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        /**
         * @brief Mid hook at the pick's first forward read: rax holds the CCamera ISystem::GetViewCamera returned.
         * @details Points rax at s_view while a forward stands. The pick reads rax only at this instruction and the
         *          next one, and the following call replaces it. No-throw, no allocation.
         */
        void detour_pick_view_read(DMK::hook::MidContext &ctx) noexcept
        {
            if (steady_now_ns() >= s_valid_until.load(std::memory_order_acquire))
            {
                return;
            }
            DMK::hook::gpr(ctx, DMK::hook::Gpr::Rax) = reinterpret_cast<std::uintptr_t>(s_view.data());
            s_used.fetch_add(1, std::memory_order_relaxed);
        }
    } // namespace

    void install_combat_pick_forward_hook(DMK::hook::HookStack &hooks)
    {
        DMK::Logger &logger = DMK::log();
        const std::uintptr_t site = gated_anchor_address(Feature::CombatPickForward, AnchorId::CombatPickViewRead);
        if (site == 0)
        {
            logger.warning("Combat: the target pick's camera read not found; the lock-on picks along the rendered "
                           "camera");
            return;
        }
        auto result = DMK::hook::mid_at(
            DMK::hook::MidRequest{
                .name = "CombatPickViewRead",
                .target = DMK::Address{site},
            },
            detour_pick_view_read);
        if (!result.has_value())
        {
            logger.warning("Combat: target pick hook failed ({}); the lock-on picks along the rendered camera",
                           result.error().message());
            return;
        }
        if (auto armed = hooks.push(std::move(*result)).enable(); !armed.has_value())
        {
            logger.warning("Combat: target pick hook could not be armed ({}); the lock-on picks along the rendered "
                           "camera",
                           armed.error().message());
            return;
        }
        logger.info("Combat: the lock-on's target pick goes along the opponent the game steers to, or along the look "
                    "before the lock framing (camera read at {})",
                    DMK::format::format_address(site));
    }

    void publish_combat_pick_forward(float x, float y) noexcept
    {
        s_view[k_forward_x_lane].store(x, std::memory_order_relaxed);
        s_view[k_forward_y_lane].store(y, std::memory_order_relaxed);
        s_valid_until.store(steady_now_ns() + k_publish_lifetime_ns, std::memory_order_release);
    }

    void withdraw_combat_pick_forward() noexcept
    {
        s_valid_until.store(0, std::memory_order_release);
    }

    std::uint32_t take_combat_pick_forward_count() noexcept
    {
        return s_used.exchange(0, std::memory_order_acq_rel);
    }
} // namespace TPVCamera
