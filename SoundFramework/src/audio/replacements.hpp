/**
 * @file audio/replacements.hpp
 * @brief Game-sound replacement and the event log, both decided when the game starts an FMOD Studio event.
 *
 * A pack sound that names game events in `Replaces` takes their place: each start of such an event is muted (the
 * event still runs, so the game's logic around it is untouched) and the pack sound plays at the event's position,
 * following it while it lives.
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
     *        when a pack replaces it.
     * @note Called from the start hook on the game's audio thread, before the original start.
     */
    void on_event_start(void *instance) noexcept;
} // namespace SoundFramework::audio::replacements

#endif // SNDFW_AUDIO_REPLACEMENTS_HPP
