/**
 * @file switch_log.hpp
 * @brief A ring of the last camera switches: preset changes, view policy edges and cuts.
 *
 * @details The ring keeps the newest SWITCH_RING_SIZE events and is never cleared, so the events before a cut stay
 *          readable after it. Each event is also logged at Debug when it is recorded. Main thread only: the camera
 *          detour records and reads it.
 */
#ifndef TPVCAMERA_PRESETS_SWITCH_LOG_HPP
#define TPVCAMERA_PRESETS_SWITCH_LOG_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace TPVCamera::Presets
{
    /** @brief What changed: the target preset, a view policy (forced view, free-look exclusion) or a camera cut. */
    enum class SwitchKind : std::uint8_t
    {
        Preset,
        Policy,
        Cut,
    };

    /** @brief One switch, with fixed-size text so recording it never allocates. */
    struct SwitchEvent
    {
        SwitchKind kind = SwitchKind::Preset;
        std::array<char, 48> from{};
        std::array<char, 48> to{};
        /// The bind mask that selected the new preset, or the state bits that raised the policy edge.
        std::uint32_t mask = 0;
        std::array<char, 24> reason{};
        /// Seconds the state change that caused the switch waited in the game-state debounce.
        float hold_s = 0.0f;
        /// Seconds the preset blend needs to close 95% of a change at its current speed (0 when it snaps).
        float expected_settle_s = 0.0f;
        /// Seconds the preset blend took to close 95% of the framing change, negative until it has.
        float settle_s = -1.0f;
        /// Camera frames counted by count_switch_frame() when the event was recorded.
        std::uint64_t frame = 0;
        /// Seconds since the module loaded when the event was recorded.
        double wall_s = 0.0;
    };

    constexpr std::size_t SWITCH_RING_SIZE = 16;

    /** @brief Copies @p text into @p out, truncated, always null-terminated. */
    template <std::size_t N> void copy_switch_text(std::array<char, N> &out, std::string_view text) noexcept
    {
        const std::size_t n = text.size() < N - 1 ? text.size() : N - 1;
        for (std::size_t i = 0; i < n; ++i)
        {
            out[i] = text[i];
        }
        out[n] = '\0';
    }

    /**
     * @brief Advances the camera frame count the events are stamped with.
     * @note Main thread only, once per camera frame.
     */
    void count_switch_frame() noexcept;

    /**
     * @brief Stamps @p event with the frame count and the time, records it as the newest entry and logs it at Debug.
     * @note Main thread only.
     */
    void record_switch(SwitchEvent event) noexcept;

    /**
     * @brief Sets the settle time of the newest Preset event and logs it at Debug.
     * @note Main thread only.
     */
    void note_switch_settled(float seconds) noexcept;

    /**
     * @brief Copies the newest events into @p out, newest first.
     * @return The number of events copied.
     * @note Main thread only.
     */
    [[nodiscard]] std::size_t recent_switches(std::span<SwitchEvent> out) noexcept;

    /** @brief The log name of @p kind. */
    [[nodiscard]] constexpr const char *switch_kind_name(SwitchKind kind) noexcept
    {
        switch (kind)
        {
        case SwitchKind::Preset:
            return "preset";
        case SwitchKind::Policy:
            return "policy";
        case SwitchKind::Cut:
            return "cut";
        }
        return "?";
    }
} // namespace TPVCamera::Presets

#endif // TPVCAMERA_PRESETS_SWITCH_LOG_HPP
