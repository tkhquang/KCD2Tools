/**
 * @file audio/replacements.hpp
 * @brief Game-sound replacement and the event log, both decided when the game starts an FMOD Studio event.
 *
 * A pack sound that names game events in `Replaces` takes their place: each start of such an event is muted (the
 * event still runs, so the game's logic around it is untouched) and the pack sound plays at the event's position,
 * following it while it lives. With `PlayAt` the pack sound waits after the muted start until the game sets the named
 * parameter on that event, so it lands on a later stage of the event, such as a bow's release. With `KeepOriginal`
 * the event is never muted, so the pack sound plays on top of it.
 */
#ifndef SNDFW_AUDIO_REPLACEMENTS_HPP
#define SNDFW_AUDIO_REPLACEMENTS_HPP

#include "packs/catalog.hpp"

#include <string>
#include <string_view>

namespace SoundFramework::audio::replacements
{
    /** @brief Rebuilds the event-to-sound table from @p catalog; a later sound wins an event two sounds claim. */
    void publish(const packs::Catalog &catalog);

    /** @brief Drops the table; later event starts pass through untouched. */
    void clear() noexcept;

    /**
     * @brief Sets which started events go to the log: a substring of the event path, "*" for all, empty for none.
     * @note Setup/control-plane only.
     */
    void set_event_log_filter(std::string_view filter);

    /**
     * @brief Handles one event start: logs it when it matches the filter, and mutes it and queues its replacement
     *        when a pack replaces it, or holds the replacement back until its PlayAt parameter is set.
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
