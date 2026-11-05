/**
 * @file audio/replacements.hpp
 * @brief Game-sound replacement and the event log, both decided when the game starts an FMOD Studio event.
 *
 * A pack sound that names game events in `Replaces` takes their place: each start of such an event is muted (the
 * event still runs, so the game's logic around it is untouched) and the pack sound plays at the event's position,
 * following it while it lives. With `PlayAt` the pack sound waits after the muted start until the game sets the named
 * parameter on that event, so it lands on a later stage of the event, such as a bow's release. With `KeepOriginal`
 * the event is never muted, so the pack sound plays on top of it.
 *
 * The same start sets the event's volume: the product of the matching `Adjusts` section's Volume and, for the game's
 * speech events, the [Volume] setting of that kind of speech. The game never sets an event instance's volume itself
 * (WHGame.dll imports no event volume setter), so the value holds for the whole play.
 */
#ifndef SNDFW_AUDIO_REPLACEMENTS_HPP
#define SNDFW_AUDIO_REPLACEMENTS_HPP

#include "packs/catalog.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace SoundFramework::audio::replacements
{
    /** @brief The game's speech, split the way the [Volume] settings split it. */
    enum class VoiceKind : std::uint8_t
    {
        Dialogue,
        Bark,
        Cutscene,
    };

    /**
     * @brief Rebuilds the event-to-sound table and the volume adjustments from @p catalog; a later sound wins an event
     *        two sounds claim, and a later adjustment wins an event two adjustments name.
     */
    void publish(const packs::Catalog &catalog);

    /**
     * @brief Sets the volume (linear, clamped to 0..MAX_EVENT_VOLUME) that every later start of @p kind's speech
     *        events plays at. A line already playing keeps the volume it started with.
     */
    void set_voice_volume(VoiceKind kind, float volume) noexcept;

    /** @brief Drops the table; later event starts pass through untouched. */
    void clear() noexcept;

    /**
     * @brief Sets which started events go to the log: a substring of the event path, "*" for all, empty for none.
     * @note Setup/control-plane only.
     */
    void set_event_log_filter(std::string_view filter);

    /**
     * @brief Handles one event start: sets its volume, logs it when it matches the filter, and mutes it and queues its
     *        replacement when a pack replaces it, or holds the replacement back until its PlayAt parameter is set.
     * @note Called from the start hook on the game's audio thread, before the original start.
     */
    void on_event_start(void *instance) noexcept;

    /**
     * @brief Handles one parameter the game sets on a running event: logs it when the event matches the filter, and
     *        queues the replacement held back for @p instance when @p name and @p value meet its PlayAt.
     * @note Called from the parameter hook on the calling game thread, before the original call.
     */
    void on_event_parameter(void *instance, const char *name, float value) noexcept;
} // namespace SoundFramework::audio::replacements

#endif // SNDFW_AUDIO_REPLACEMENTS_HPP
