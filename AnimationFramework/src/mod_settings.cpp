/**
 * @file mod_settings.cpp
 * @brief Settings files in each mod's own folder (see mod_settings.hpp).
 */

#include "mod_settings.hpp"
#include "console_vars.hpp"
#include "core/text_util.hpp"

#include <DetourModKit.hpp>
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace AnimationFramework
{
    namespace
    {
        constexpr std::string_view TEXT_CVAR = "animfw_settings_text";
        constexpr std::string_view TEXT_CVAR_HELP = "The text of a mod's settings, on its way to or from its file.";
        constexpr char COMMAND_FAILED[] = "ModSettings: a settings command failed";
        constexpr const char *LOAD_COMMAND = "animfw_settings_load";
        constexpr const char *LOAD_COMMAND_HELP = "animfw_settings_load <mod id>: puts the mod's settings file in "
                                                  "animfw_settings_text";
        constexpr const char *SAVE_COMMAND = "animfw_settings_save";
        constexpr const char *SAVE_COMMAND_HELP = "animfw_settings_save <mod id>: writes animfw_settings_text to the "
                                                  "mod's settings file";
        constexpr std::wstring_view MODS_FOLDER = L"Mods";
        constexpr std::string_view FILE_EXTENSION = ".settings";
        constexpr std::size_t MAX_FILE_BYTES = 1024 * 1024;

        // Main thread only. s_failed latches a failed registration, so the frame tick does not retry it every frame.
        void *s_text_cvar = nullptr;
        bool s_added = false;
        bool s_failed = false;

        /** @brief The game's root: the exe sits in Bin/<platform>/. */
        [[nodiscard]] std::filesystem::path game_root()
        {
            std::wstring exe(MAX_PATH, L'\0');
            for (;;)
            {
                const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
                if (length < exe.size())
                {
                    exe.resize(length);
                    break;
                }
                exe.resize(exe.size() * 2);
            }
            return std::filesystem::path(exe).parent_path().parent_path().parent_path();
        }

        /** @brief Mods/<mod_id>/<mod_id>.settings, or nothing when no such mod is installed. */
        [[nodiscard]] std::optional<std::filesystem::path> settings_file(std::string_view mod_id)
        {
            const std::filesystem::path folder = game_root() / MODS_FOLDER / mod_id;
            std::error_code error;
            if (!std::filesystem::is_directory(folder, error))
            {
                return std::nullopt;
            }
            std::string name{mod_id};
            name += FILE_EXTENSION;
            return folder / name;
        }

        /** @brief The command's mod id, or nothing after logging why it is unusable. */
        [[nodiscard]] std::optional<std::string> mod_id_of(void *args, const char *command)
        {
            const std::string_view mod_id = command_argument_count(args) > 1 ? command_argument(args, 1) : "";
            if (!is_mod_id(mod_id))
            {
                DMK::log().warning(
                    "ModSettings: {} needs a mod id (lowercase letters and underscores), got '{}'",
                    command,
                    mod_id
                );
                return std::nullopt;
            }
            return std::string{mod_id};
        }

        /// The load command: the mod's settings file into the text variable, empty without one.
        void load(void *args)
        {
            const std::optional<std::string> mod_id = mod_id_of(args, LOAD_COMMAND);
            const std::optional<std::filesystem::path> file =
                mod_id.has_value() ? settings_file(*mod_id) : std::optional<std::filesystem::path>{};
            std::string text;
            std::error_code error;
            // A missing file fails the size query and loads as empty text.
            const std::uintmax_t size = file.has_value() ? std::filesystem::file_size(*file, error) : 0;
            if (file.has_value() && !error)
            {
                if (size > MAX_FILE_BYTES)
                {
                    // A script reads an empty text as "no settings yet", so the refusal goes to the log.
                    DMK::log().warning(
                        "ModSettings: the settings file of '{}' holds {} bytes, more than {}, so nothing is loaded",
                        *mod_id,
                        size,
                        MAX_FILE_BYTES
                    );
                }
                else
                {
                    std::ifstream stream(*file, std::ios::binary);
                    text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
                }
            }
            set_cvar_text(s_text_cvar, text);
        }

        /// The save command: the text variable into the mod's settings file.
        void save(void *args)
        {
            const std::optional<std::string> mod_id = mod_id_of(args, SAVE_COMMAND);
            if (!mod_id.has_value())
            {
                return;
            }
            const std::string_view text = cvar_text(s_text_cvar);
            const auto file = settings_file(*mod_id);
            if (!file.has_value() || text.size() > MAX_FILE_BYTES)
            {
                DMK::log()
                    .warning("ModSettings: nothing saved for '{}': no such mod, or {} bytes", *mod_id, text.size());
                return;
            }
            std::ofstream stream(*file, std::ios::binary | std::ios::trunc);
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!stream.good())
            {
                DMK::log().warning("ModSettings: could not write the settings file of '{}'", *mod_id);
            }
        }
    } // namespace

    void run_mod_settings_frame_work() noexcept
    {
        if (s_added || s_failed || !console_ready())
        {
            return;
        }
        bool registered = false;
        s_text_cvar = find_or_register_string_cvar(TEXT_CVAR, TEXT_CVAR_HELP, registered);
        if (s_text_cvar == nullptr)
        {
            s_failed = true;
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "ModSettings: {} could not be found or registered; the settings commands are off",
                TEXT_CVAR
            );
            return;
        }
        add_console_command(LOAD_COMMAND, &guarded_command<&load, COMMAND_FAILED>, LOAD_COMMAND_HELP);
        add_console_command(SAVE_COMMAND, &guarded_command<&save, COMMAND_FAILED>, SAVE_COMMAND_HELP);
        s_added = true;
    }

    void remove_mod_settings() noexcept
    {
        if (s_added)
        {
            remove_console_command(LOAD_COMMAND);
            remove_console_command(SAVE_COMMAND);
        }
        s_added = false;
        s_failed = false;
        s_text_cvar = nullptr;
    }
} // namespace AnimationFramework
