/**
 * @file switch_log.cpp
 * @brief The camera switch ring (see switch_log.hpp).
 */

#include "switch_log.hpp"

#include <DetourModKit.hpp>

#include <chrono>

namespace TPVCamera::Presets
{
    namespace
    {
        // Main thread only: written by count_switch_frame(), record_switch() and note_switch_settled().
        std::array<SwitchEvent, SWITCH_RING_SIZE> s_ring{};
        std::size_t s_written = 0;
        std::uint64_t s_frame = 0;
        const std::chrono::steady_clock::time_point s_loaded = std::chrono::steady_clock::now();
    } // namespace

    void count_switch_frame() noexcept
    {
        ++s_frame;
    }

    void record_switch(SwitchEvent event) noexcept
    {
        event.frame = s_frame;
        event.wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_loaded).count();
        s_ring[s_written % SWITCH_RING_SIZE] = event;
        ++s_written;
        (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                 "Switch [{}] {} -> {}: mask {:#x}, cause {}, held {:.2f} s, settle t95 {:.2f} s, "
                                 "frame {}, at {:.3f} s",
                                 switch_kind_name(event.kind), event.from.data(), event.to.data(), event.mask,
                                 event.reason.data(), event.hold_s, event.expected_settle_s, event.frame, event.wall_s);
    }

    void note_switch_settled(float seconds) noexcept
    {
        for (std::size_t back = 1; back <= SWITCH_RING_SIZE && back <= s_written; ++back)
        {
            SwitchEvent &event = s_ring[(s_written - back) % SWITCH_RING_SIZE];
            if (event.kind == SwitchKind::Preset)
            {
                event.settle_s = seconds;
                (void)DMK::log().try_log(DMK::LogLevel::Debug, "Switch [preset] {} -> {} settled 95% in {:.3f} s",
                                         event.from.data(), event.to.data(), seconds);
                return;
            }
        }
    }

    std::size_t recent_switches(std::span<SwitchEvent> out) noexcept
    {
        std::size_t n = 0;
        for (std::size_t back = 1; back <= SWITCH_RING_SIZE && back <= s_written && n < out.size(); ++back)
        {
            out[n++] = s_ring[(s_written - back) % SWITCH_RING_SIZE];
        }
        return n;
    }
} // namespace TPVCamera::Presets
