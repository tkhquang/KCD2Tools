/**
 * @file game/speak_hook.cpp
 * @brief The speak manager's Speak hook.
 *
 * The dialog system shows a line's subtitle and then hands the line to Speak, as do barks and the game's
 * wh_snd_playfile command, so this one hook sees every voice line with its file path and text key.
 */

#include "game/speak_hook.hpp"
#include "game/main_thread.hpp"
#include "game/speech.hpp"

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace SoundFramework::game::speak_hook
{
    namespace
    {
        /// Returns the whole register: callers test only its low byte, and the hook passes the full value through.
        using SpeakFn = std::uintptr_t(__fastcall *)(void *speak_man, void *request);
        using AssignFn = void *(__fastcall *)(void *string, const char *text);

        /// Request layout: CryString file path (no extension) at +0x00, text key at +0x08, volume text at +0x10.
        constexpr std::size_t REQUEST_PATH = 0x00;
        constexpr std::size_t REQUEST_TEXT_KEY = 0x08;
        constexpr std::size_t REQUEST_VOLUME = 0x10;

        SpeakFn s_original = nullptr;
        std::uintptr_t s_assign = 0;
        /// Where the dialog line player's call into Speak returns to, or 0.
        std::uintptr_t s_dialog_return = 0;

        std::mutex s_filter_mutex;
        std::string s_filter;
        std::atomic<bool> s_logging{false};

        [[nodiscard]] char lower(char c) noexcept
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        [[nodiscard]] std::string_view string_at(const void *request, std::size_t offset) noexcept
        {
            const char *text =
                *reinterpret_cast<const char *const *>(static_cast<const std::uint8_t *>(request) + offset);
            return text == nullptr ? std::string_view{} : std::string_view{text};
        }

        void log_request(const void *speak_man, const void *request)
        {
            const std::string_view path = string_at(request, REQUEST_PATH);
            const std::string_view key = string_at(request, REQUEST_TEXT_KEY);
            std::string haystack(path);
            haystack.append(" ").append(key);
            std::ranges::transform(haystack, haystack.begin(), lower);
            {
                const std::lock_guard lock(s_filter_mutex);
                if (s_filter != "*" && haystack.find(s_filter) == std::string::npos)
                {
                    return;
                }
            }
            DMK::log().info(
                "SpeakLog: {} key '{}' volume '{}' (speaker {}{})",
                path,
                key,
                string_at(request, REQUEST_VOLUME),
                DMK::format::format_address(reinterpret_cast<std::uintptr_t>(speak_man)),
                main_thread::is_main_thread() ? "" : ", worker thread"
            );
        }

        std::uintptr_t __fastcall speak_detour(void *speak_man, void *request) noexcept
        {
            if (s_logging.load(std::memory_order_acquire) && request != nullptr)
            {
                try
                {
                    log_request(speak_man, request);
                }
                catch (...)
                {
                    (void)DMK::log().log_noexcept(DMK::LogLevel::Warning, "SpeakLog: a request was not logged");
                }
            }
            std::optional<speech::Swap> swap;
            // The game's own line whose mouth the framework drives, or empty.
            std::string game_line;
            if (request != nullptr && speak_man != nullptr && main_thread::is_main_thread())
            {
                // A conversation line takes the speaker over from a framework line playing or waiting on it.
                if (s_dialog_return != 0 && reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == s_dialog_return)
                {
                    speech::yield_to_dialog(speak_man);
                }
                try
                {
                    swap = speech::prepare_swap(string_at(request, REQUEST_TEXT_KEY));
                }
                catch (...)
                {
                    swap.reset();
                    (void)DMK::log().log_noexcept(
                        DMK::LogLevel::Error,
                        "Speech: the voice replacement failed; the game line plays unchanged"
                    );
                }
                try
                {
                    if (!swap.has_value() && speech::drives_game_lines())
                    {
                        game_line = string_at(request, REQUEST_PATH);
                    }
                }
                catch (...)
                {
                    game_line.clear();
                    (void)DMK::log().log_noexcept(
                        DMK::LogLevel::Warning,
                        "Speech: the voice language check failed; the line keeps the game's face"
                    );
                }
            }
            if (swap.has_value())
            {
                // The request's own game-allocated strings take the pack line's file and text key.
                auto *const bytes = static_cast<std::uint8_t *>(request);
                const auto assign = reinterpret_cast<AssignFn>(s_assign);
                assign(bytes + REQUEST_PATH, swap->voice.c_str());
                assign(bytes + REQUEST_TEXT_KEY, swap->bubble ? swap->text_key.c_str() : "");
            }
            const std::uintptr_t result = s_original(speak_man, request);
            if (swap.has_value())
            {
                try
                {
                    speech::finish_swap(speak_man, *swap);
                }
                catch (...)
                {
                    (void)DMK::log().log_noexcept(
                        DMK::LogLevel::Error,
                        "Speech: the replaced line plays without its face and subtitle"
                    );
                }
            }
            if (!game_line.empty())
            {
                try
                {
                    speech::follow_game_line(speak_man, game_line);
                }
                catch (...)
                {
                    (void)DMK::log().log_noexcept(
                        DMK::LogLevel::Warning,
                        "Speech: a game line's mouth failed; it keeps the game's face"
                    );
                }
            }
            return result;
        }
    } // namespace

    DMK::Result<void> install(const Anchors &anchors, DMK::hook::HookStack &hooks)
    {
        s_assign = anchors.string_assign;
        s_dialog_return = anchors.dialog_handover ? anchors.dialog_speak_return : 0;
        if (!anchors.speech)
        {
            DMK::log().warning("SpeakHook: the Speech feature is off; game voice lines are not seen");
            return {};
        }
        DMK_TRY(
            created,
            DMK::hook::inline_at<&speak_detour>(DMK::hook::InlineRequest{
                .name = "ActorSpeak",
                .target = DMK::Address{anchors.speak},
            })
        );
        s_original = created.original<SpeakFn>();
        DMK::hook::Hook &stored = hooks.push(std::move(created));
        DMK_TRY_VOID(stored.enable());
        DMK::log().info("SpeakHook: Speak hooked at {}", DMK::format::format_address(anchors.speak));
        return {};
    }

    void set_log_filter(std::string_view filter)
    {
        std::string lowered(filter);
        std::ranges::transform(lowered, lowered.begin(), lower);
        const std::lock_guard lock(s_filter_mutex);
        s_filter = std::move(lowered);
        s_logging.store(!s_filter.empty(), std::memory_order_release);
    }
} // namespace SoundFramework::game::speak_hook
