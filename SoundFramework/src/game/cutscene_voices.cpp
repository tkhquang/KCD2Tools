/**
 * @file game/cutscene_voices.cpp
 * @brief The cutscene voice event queue, the speaker table lookup and the running cutscene lines.
 *
 * The FMOD start hook sees every game sound start on the game's audio thread, so it only compares the event's GUID and
 * copies the voice file out of the event's user data; the table lookup and the faces run on the main thread.
 */

#include "game/cutscene_voices.hpp"
#include "constants.hpp"
#include "fmod/fmod_api.hpp"
#include "game/entity_tracker.hpp"
#include "game/speech.hpp"
#include "packs/cutscene_table.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace SoundFramework::game::cutscene_voices
{
    namespace
    {
        /** @brief A cutscene voice event that started, and its voice file. */
        struct Started
        {
            void *instance = nullptr;
            /// Localization path without extension, with forward slashes.
            std::string voice_path;
        };

        /** @brief A cutscene line whose faces move, watched until its event stops. */
        struct Running
        {
            void *instance = nullptr;
            std::uint64_t id = 0;
        };

        /// How often the GUID lookup retries while the Studio strings bank does not know the event path yet.
        constexpr std::uint64_t LOOKUP_RETRY_MS = 1000;

        std::atomic<std::shared_ptr<const packs::CutsceneTable>> s_table;
        /// Anchors::event_instance_vtable; written before s_table is published.
        std::uintptr_t s_event_instance_vtable = 0;
        /// The cutscene voice event's GUID; written before s_cinematic_known is set.
        fmod::Guid s_cinematic_id{};
        std::atomic<bool> s_cinematic_known{false};
        /// Audio-thread state: when on_studio_system() may look the GUID up again.
        std::uint64_t s_next_lookup_ms = 0;
        std::mutex s_started_mutex;
        std::vector<Started> s_started;
        /// Main-thread state.
        std::vector<Running> s_running;
        std::atomic_flag s_reported_unreadable;

        /**
         * @brief The voice file in @p user_data, the game's audio event object, or nothing when the object is not one
         *        or its string does not check out. Every read is guarded, since the object belongs to the game.
         */
        [[nodiscard]] std::optional<std::string> voice_file_of(const void *user_data) noexcept
        {
            const auto object = reinterpret_cast<std::uintptr_t>(user_data);
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{object});
            if (!vtable || *vtable != s_event_instance_vtable)
            {
                return std::nullopt;
            }
            const std::uintptr_t file = object + constants::EVENT_VOICE_FILE;
            const auto size = DMK::memory::read<std::uint64_t>(DMK::Address{file + constants::STD_STRING_SIZE});
            const auto capacity = DMK::memory::read<std::uint64_t>(DMK::Address{file + constants::STD_STRING_CAPACITY});
            if (!size || !capacity || *size == 0 || *size > constants::MAX_VOICE_FILE_LENGTH || *capacity < *size)
            {
                return std::nullopt;
            }
            std::uintptr_t text = file;
            if (*capacity > constants::STD_STRING_INLINE_CAPACITY)
            {
                const auto heap = DMK::memory::read<std::uintptr_t>(DMK::Address{file});
                if (!heap)
                {
                    return std::nullopt;
                }
                text = *heap;
            }
            std::array<std::byte, constants::MAX_VOICE_FILE_LENGTH> bytes{};
            const std::span<std::byte> used = std::span(bytes).first(static_cast<std::size_t>(*size));
            if (!DMK::memory::read_into(DMK::Address{text}, used))
            {
                return std::nullopt;
            }
            std::string path(reinterpret_cast<const char *>(used.data()), used.size());
            if (std::ranges::any_of(path, [](char character) { return static_cast<unsigned char>(character) < 0x20; }))
            {
                return std::nullopt;
            }
            std::ranges::replace(path, '\\', '/');
            return path;
        }

        /**
         * @brief The faces @p entries move, each entity once. A speaker row wins over a faces row for the same actor.
         */
        [[nodiscard]] std::vector<speech::CutsceneFace> faces_of(const std::vector<packs::CutsceneEntry> &entries)
        {
            std::vector<speech::CutsceneFace> faces;
            const auto add = [&](const std::string &name, bool follow_motion)
            {
                const std::uint32_t id = entities::find_id(name);
                if (id == 0)
                {
                    return;
                }
                const auto known = std::ranges::find(faces, id, &speech::CutsceneFace::entity);
                if (known == faces.end())
                {
                    faces.push_back(
                        speech::CutsceneFace{
                            .entity = id,
                            .follow_motion = follow_motion,
                        }
                    );
                }
                else
                {
                    known->follow_motion = known->follow_motion && follow_motion;
                }
            };
            for (const packs::CutsceneEntry &entry : entries)
            {
                if (entry.mode == packs::CutsceneMode::Player)
                {
                    add(constants::PLAYER_ENTITY_NAME, false);
                }
                for (const std::string &actor : entry.actors)
                {
                    add(actor, entry.mode == packs::CutsceneMode::Faces);
                }
            }
            return faces;
        }

        /** @brief Starts the faces of the cutscene line @p started. */
        void follow(const packs::CutsceneTable &table, const Started &started)
        {
            const std::string line = packs::cutscene_line_id(started.voice_path);
            const auto entries = table.find(line);
            if (entries == table.end())
            {
                DMK::log().debug("Cutscene: {} has no known face; the cutscene's faces stay", line);
                return;
            }
            const std::vector<speech::CutsceneFace> faces = faces_of(entries->second);
            const std::uint64_t id =
                faces.empty() ? 0 : speech::follow_cutscene_line(started.voice_path + ".ogg", faces);
            if (id == 0)
            {
                DMK::log().debug("Cutscene: {} names no face present now; the cutscene's faces stay", line);
                return;
            }
            DMK::log().debug(
                "Cutscene: {} drives {} face(s), {} of them by their own mouth motion",
                line,
                faces.size(),
                std::ranges::count_if(faces, &speech::CutsceneFace::follow_motion)
            );
            s_running.push_back(
                Running{
                    .instance = started.instance,
                    .id = id,
                }
            );
        }

        /**
         * @brief Ends the running lines whose event stopped or was released: a released instance handle fails FMOD's
         *        handle check, so its state query returns an error instead of touching freed memory.
         */
        void watch()
        {
            const fmod::Api &api = fmod::api();
            std::erase_if(
                s_running,
                [&](const Running &line)
                {
                    int state = fmod::PLAYBACK_STOPPED;
                    if (api.event_get_playback_state(line.instance, &state) == fmod::OK &&
                        state != fmod::PLAYBACK_STOPPED)
                    {
                        return false;
                    }
                    speech::end_cutscene_line(line.id);
                    return true;
                }
            );
        }
    } // namespace

    void configure(const Anchors &anchors, const std::filesystem::path &table_file) noexcept
    {
        if (!anchors.cutscene_voices)
        {
            return;
        }
        try
        {
            std::ifstream in(table_file, std::ios::binary);
            if (!in)
            {
                DMK::log().warning(
                    "Cutscene: {} not found; cutscenes keep the game's faces",
                    table_file.filename().string()
                );
                return;
            }
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::size_t rejected = 0;
            auto table = std::make_shared<const packs::CutsceneTable>(packs::parse_cutscene_table(text, rejected));
            DMK::log().info(
                "Cutscene: {} voice line(s) with known faces in {}{}",
                table->size(),
                table_file.filename().string(),
                rejected == 0 ? std::string{} : ", " + std::to_string(rejected) + " malformed row(s) skipped"
            );
            s_event_instance_vtable = anchors.event_instance_vtable;
            s_table.store(std::move(table), std::memory_order_release);
        }
        catch (...)
        {
            (void)DMK::log().log_noexcept(
                DMK::LogLevel::Error,
                "Cutscene: the speaker table failed to load; cutscenes keep the game's faces"
            );
        }
    }

    void on_studio_system(void *system) noexcept
    {
        if (s_cinematic_known.load(std::memory_order_acquire) || s_table.load(std::memory_order_acquire) == nullptr)
        {
            return;
        }
        const std::uint64_t now = GetTickCount64();
        if (now < s_next_lookup_ms)
        {
            return;
        }
        s_next_lookup_ms = now + LOOKUP_RETRY_MS;
        fmod::Guid id{};
        if (fmod::api().studio_lookup_id(system, constants::CINEMATIC_VOICE_EVENT, &id) != fmod::OK)
        {
            return;
        }
        s_cinematic_id = id;
        s_cinematic_known.store(true, std::memory_order_release);
        (void)DMK::log().try_log(DMK::LogLevel::Debug, "Cutscene: {} resolved", constants::CINEMATIC_VOICE_EVENT);
    }

    void on_event_start(void *instance) noexcept
    {
        try
        {
            if (instance == nullptr || !s_cinematic_known.load(std::memory_order_acquire))
            {
                return;
            }
            const fmod::Api &api = fmod::api();
            void *description = nullptr;
            fmod::Guid id{};
            if (api.event_get_description(instance, &description) != fmod::OK || description == nullptr ||
                api.description_get_id(description, &id) != fmod::OK || id != s_cinematic_id)
            {
                return;
            }
            void *user_data = nullptr;
            std::optional<std::string> voice_path =
                api.event_get_user_data(instance, &user_data) == fmod::OK && user_data != nullptr
                    ? voice_file_of(user_data)
                    : std::nullopt;
            if (!voice_path.has_value())
            {
                if (!s_reported_unreadable.test_and_set(std::memory_order_relaxed))
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Warning,
                        "Cutscene: a cutscene voice event carries no readable voice file; cutscenes keep the game's "
                        "faces"
                    );
                }
                return;
            }
            const std::lock_guard lock(s_started_mutex);
            s_started.push_back(
                Started{
                    .instance = instance,
                    .voice_path = std::move(*voice_path),
                }
            );
        }
        catch (...)
        {
            // The game's audio thread must never see an exception; the line keeps the game's faces.
        }
    }

    void on_frame() noexcept
    {
        try
        {
            std::vector<Started> started;
            {
                const std::lock_guard lock(s_started_mutex);
                started.swap(s_started);
            }
            if (!s_running.empty())
            {
                watch();
            }
            const std::shared_ptr<const packs::CutsceneTable> table = s_table.load(std::memory_order_acquire);
            if (started.empty() || table == nullptr || !speech::drives_game_lines())
            {
                return;
            }
            for (const Started &line : started)
            {
                follow(*table, line);
            }
        }
        catch (...)
        {
            (void)DMK::log().log_noexcept(
                DMK::LogLevel::Error,
                "Cutscene: a cutscene line failed to start; its faces stay the game's"
            );
        }
    }

    void shutdown() noexcept
    {
        s_cinematic_known.store(false, std::memory_order_release);
        s_table.store(nullptr, std::memory_order_release);
        {
            const std::lock_guard lock(s_started_mutex);
            s_started.clear();
        }
        s_running.clear();
    }
} // namespace SoundFramework::game::cutscene_voices
