/**
 * @file hooks/haptic_distance.cpp
 * @brief The hooks that measure the haptic presets' range from the eye (see haptic_distance.hpp).
 */

#include "hooks/haptic_distance.hpp"
#include "aob_resolver.hpp"
#include "hooks/frame_publication.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace TPVCamera
{
    namespace
    {
        /// The haptic preset play: the result is the voice index or -1, and the first parameter is not read.
        using HapticPlayFn = std::int32_t (*)(void *unused, const void *preset, float distance);

        // The eye in FMOD coordinates (the world's x, z, y), the space of the event attributes the start measures.
        static_assert(std::atomic<float>::is_always_lock_free, "the mid hook reads the eye without a lock");
        alignas(64) std::array<std::atomic<float>, 3> s_eye{};
        // Without a live eye the play keeps the listener's distance.
        FramePublication s_eye_publication;

        // The eye distance the call site's mid hook measured, for the play the same thread enters next, so two
        // threads that start events at once never trade distances. A slot packs the thread id in its high half and
        // the distance's bits in its low half, and 0 is empty because no thread has id 0. Thread ids are multiples
        // of 4, so the slot index drops the two low bits.
        constexpr std::size_t k_slot_count = 8;
        alignas(64) std::array<std::atomic<std::uint64_t>, k_slot_count> s_slots{};

        // The return address of the start's call of the play: only a play entered from there takes a slot. The mid
        // hook sits before the call (HapticPlayArgs), so the call keeps its place and this address.
        std::atomic<std::uintptr_t> s_call_return{0};
        std::atomic<HapticPlayFn> s_play_original{nullptr};

        [[nodiscard]] std::atomic<std::uint64_t> &slot_for(std::uint32_t thread) noexcept
        {
            return s_slots[(thread >> 2) % k_slot_count];
        }

        /**
         * @brief Mid hook on the argument setup of the start's call of the play: rbx holds the event's FMOD 3D
         *        attributes, whose position the start measured to the listener.
         * @details Stores the distance from the published eye to that position for this thread. The play's entry hook
         *          takes it. No-throw, no allocation.
         */
        void detour_haptic_play_args(DMK::hook::MidContext &ctx) noexcept
        {
            if (!s_eye_publication.live())
            {
                return;
            }
            const auto position =
                DMK::memory::read<std::array<float, 3>>(DMK::Address{DMK::hook::gpr(ctx, DMK::hook::Gpr::Rbx)});
            if (!position)
            {
                return;
            }
            const float dx = (*position)[0] - s_eye[0].load(std::memory_order_relaxed);
            const float dy = (*position)[1] - s_eye[1].load(std::memory_order_relaxed);
            const float dz = (*position)[2] - s_eye[2].load(std::memory_order_relaxed);
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (!std::isfinite(distance))
            {
                return;
            }
            const auto thread = static_cast<std::uint32_t>(GetCurrentThreadId());
            slot_for(thread).store((static_cast<std::uint64_t>(thread) << 32) | std::bit_cast<std::uint32_t>(distance),
                                   std::memory_order_relaxed);
        }

        /**
         * @brief Detour of the haptic preset play: a play the start's call entered on this thread gets the distance
         *        the mid hook measured from the eye.
         * @details Any other play, the console's among them, keeps its distance. No-throw, no allocation.
         */
        std::int32_t detour_haptic_play(DMK::hook::CallerFrame frame, void *unused, const void *preset,
                                        float distance) noexcept
        {
            const HapticPlayFn original = s_play_original.load(std::memory_order_acquire);
            if (reinterpret_cast<std::uintptr_t>(frame.return_address()) !=
                s_call_return.load(std::memory_order_relaxed))
            {
                return original(unused, preset, distance);
            }
            const auto thread = static_cast<std::uint32_t>(GetCurrentThreadId());
            std::atomic<std::uint64_t> &slot = slot_for(thread);
            std::uint64_t packed = slot.load(std::memory_order_relaxed);
            if (static_cast<std::uint32_t>(packed >> 32) != thread ||
                !slot.compare_exchange_strong(packed, 0, std::memory_order_relaxed))
            {
                return original(unused, preset, distance);
            }
            const float eye_distance = std::bit_cast<float>(static_cast<std::uint32_t>(packed));
            const std::int32_t voice = original(unused, preset, eye_distance);
            // A voice of -1 means that the play dropped the preset (out of range or unknown), so the pad stays still.
            (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                     "Haptics: a preset measured {:.2f} m from the eye, {:.2f} m from the camera "
                                     "(voice {})",
                                     eye_distance, distance, voice);
            return voice;
        }
    } // namespace

    void install_haptic_distance_hooks(DMK::hook::HookStack &hooks)
    {
        DMK::Logger &logger = DMK::log();
        const std::uintptr_t args = gated_anchor_address(Feature::HapticsFromEye, AnchorId::HapticPlayArgs);
        const std::uintptr_t play = gated_anchor_address(Feature::HapticsFromEye, AnchorId::HapticPlay);
        if (args == 0 || play == 0)
        {
            logger.warning("Haptics: the vibration range test not found; in third person the range stays measured "
                           "from the camera");
            return;
        }
        // The setup's rungs end in the play's `call rel32`. It must reach this play, or the slots would feed the
        // distance to another function.
        constexpr std::uint8_t k_call_rel32 = 0xE8;
        constexpr std::size_t k_call_length = 5;
        const std::uintptr_t call = args + Aob::k_hapticPlayCallOffset;
        const auto opcode = DMK::memory::read<std::uint8_t>(DMK::Address{call});
        const auto callee = DMK::scan::resolve_rip_relative(DMK::Address{call}, 1, k_call_length);
        if (!opcode || *opcode != k_call_rel32 || !callee || callee->raw() != play)
        {
            logger.warning("Haptics: the start's call at {} does not reach the preset play at {}; in third person the "
                           "range stays measured from the camera",
                           DMK::format::format_address(call), DMK::format::format_address(play));
            return;
        }

        // The play's hook first: it only takes slots, so it changes nothing until the setup's hook stores one.
        s_call_return.store(call + k_call_length, std::memory_order_relaxed);
        const auto play_installed = hooks.install<&detour_haptic_play>(DMK::hook::InlineInstallRequest{
            .name = "HapticPlay",
            .target = DMK::Address{play},
            .original = &s_play_original,
        });
        if (!play_installed.has_value())
        {
            logger.warning("Haptics: preset play hook failed ({}); in third person the range stays measured from the "
                           "camera",
                           play_installed.error().message());
            return;
        }
        const auto args_installed = hooks.install(
            DMK::hook::MidRequest{
                .name = "HapticPlayArgs",
                .target = DMK::Address{args},
            },
            detour_haptic_play_args);
        if (!args_installed.has_value())
        {
            logger.warning("Haptics: event start hook failed ({}); in third person the range stays measured from the "
                           "camera",
                           args_installed.error().message());
            return;
        }
        logger.info("Haptics: in third person the controller vibration range is measured from the eye (play at {}, "
                    "call at {})",
                    DMK::format::format_address(play), DMK::format::format_address(call));
    }

    void publish_haptic_eye(float x, float y, float z) noexcept
    {
        s_eye[0].store(x, std::memory_order_relaxed);
        s_eye[1].store(z, std::memory_order_relaxed);
        s_eye[2].store(y, std::memory_order_relaxed);
        s_eye_publication.renew();
    }

    void withdraw_haptic_eye() noexcept
    {
        s_eye_publication.withdraw();
    }
} // namespace TPVCamera
