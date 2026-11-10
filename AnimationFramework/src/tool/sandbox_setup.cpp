/**
 * @file tool/sandbox_setup.cpp
 * @brief The Sandbox editor button's setup and note helper (see sandbox_setup.hpp).
 */

#include "tool/sandbox_setup.hpp"
#include "tool/offline_tools.hpp"

#include "sandbox_files.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace AnimationFramework
{
    namespace
    {
        constexpr std::string_view SCRIPT_FILE = "animfw_export.py";
        constexpr std::string_view SETTINGS_FILE = "animfw_export.cfg";
        constexpr std::string_view TOOLBAR_FILE = "AnimationFramework.xml";
        /// The editor keeps each dynamic toolbar's placement here, by the toolbar file's lowercase name.
        constexpr std::string_view TOOLBAR_PLACEMENT = "animationframework.xml";
        constexpr std::string_view TOOLBAR_SHOWN =
            "<?xml version=\"1.0\" encoding=\"us-ascii\"?>\n"
            "<ToolbarInfo visible=\"1\">\n"
            "\t<Position docked=\"4\" bottom=\"0\" top=\"0\" left=\"0\" right=\"0\" "
            "width=\"32767\" />\n"
            "</ToolbarInfo>\n";
        /// The button's settings, in the order animfw_export.py writes them.
        constexpr std::array<std::string_view, 4> SETTING_KEYS{
            "mod_folder",
            "animfw",
            "install",
            "name",
        };
        constexpr std::chrono::milliseconds NOTE_WAIT{8000};
        constexpr std::chrono::milliseconds NOTE_POLL{100};

        /** @brief This animfw.exe. */
        [[nodiscard]] fs::path own_exe()
        {
            std::wstring buffer(MAX_PATH, L'\0');
            for (;;)
            {
                const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
                if (length < buffer.size())
                {
                    buffer.resize(length);
                    return fs::path{buffer};
                }
                buffer.resize(buffer.size() * 2);
            }
        }

        /** @brief @p text without blanks (spaces, tabs, carriage returns) at either end. */
        [[nodiscard]] std::string_view trimmed(std::string_view text) noexcept
        {
            constexpr std::string_view blanks = " \t\r";
            const std::size_t start = text.find_first_not_of(blanks);
            if (start == std::string_view::npos)
            {
                return {};
            }
            return text.substr(start, text.find_last_not_of(blanks) - start + 1);
        }

        /** @brief The values of SETTING_KEYS, by key. */
        using Settings = std::vector<std::pair<std::string, std::string>>;

        /** @brief The value of @p key (one of SETTING_KEYS) in @p settings. */
        [[nodiscard]] std::string &setting(Settings &settings, std::string_view key)
        {
            return std::ranges::find(settings, key, &Settings::value_type::first)->second;
        }

        /**
         * @brief The settings in @p file, read as animfw_export.py reads them: `key=value` lines, blanks around the
         *        key and the value and quotes around the value dropped, `#` lines skipped.
         */
        [[nodiscard]] Settings read_settings(const fs::path &file)
        {
            Settings settings;
            for (const std::string_view key : SETTING_KEYS)
            {
                settings.emplace_back(std::string{key}, std::string{});
            }
            const std::optional<std::string> text = read_text(file);
            std::string_view rest = text ? std::string_view{*text} : std::string_view{};
            // A file saved by Notepad starts with a UTF-8 byte order mark, which would join the first key.
            constexpr std::string_view byte_order_mark = "\xEF\xBB\xBF";
            if (rest.starts_with(byte_order_mark))
            {
                rest.remove_prefix(byte_order_mark.size());
            }
            while (!rest.empty())
            {
                const std::size_t end = rest.find('\n');
                const std::string_view line = trimmed(rest.substr(0, end));
                rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
                const std::size_t equals = line.find('=');
                if (line.starts_with('#') || equals == std::string_view::npos)
                {
                    continue;
                }
                const std::string_view key = trimmed(line.substr(0, equals));
                std::string_view value = trimmed(line.substr(equals + 1));
                while (value.starts_with('"'))
                {
                    value.remove_prefix(1);
                }
                while (value.ends_with('"'))
                {
                    value.remove_suffix(1);
                }
                if (std::ranges::find(SETTING_KEYS, key) != SETTING_KEYS.end())
                {
                    setting(settings, key) = std::string{value};
                }
            }
            return settings;
        }

        [[nodiscard]] std::string write_settings(const Settings &settings)
        {
            std::string text = "# AnimationFramework export settings (see animfw_export.py)\n";
            for (const auto &[key, value] : settings)
            {
                text += std::format("{}={}\n", key, value);
            }
            return text;
        }

        [[nodiscard]] std::wstring window_text(HWND window)
        {
            std::array<wchar_t, 256> text{};
            GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
            return std::wstring{text.data()};
        }

        /** @brief What find_note looks for, and the OK button it found. */
        struct NoteSearch
        {
            DWORD pid = 0;
            HWND ok = nullptr;
        };

        /**
         * @brief EnumWindows callback: stores in the NoteSearch at @p context the OK button of the editor's "No
         *        modified layers found" note, when that note is all the box holds.
         * @return FALSE to stop the search once the button is found.
         */
        BOOL CALLBACK find_note(HWND window, LPARAM context)
        {
            auto *search = reinterpret_cast<NoteSearch *>(context);
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            if (pid != search->pid || !IsWindowVisible(window) || window_text(window) != L"Save layers")
            {
                return TRUE;
            }
            struct Children
            {
                bool note = false;
                HWND ok = nullptr;
                int buttons = 0;
            } children;
            EnumChildWindows(
                window,
                [](HWND child, LPARAM state) -> BOOL
                {
                    auto *found = reinterpret_cast<Children *>(state);
                    std::array<wchar_t, 32> type{};
                    GetClassNameW(child, type.data(), static_cast<int>(type.size()));
                    const std::wstring text = window_text(child);
                    found->note = found->note || text == L"No modified layers found";
                    if (std::wstring_view{type.data()} == L"Button")
                    {
                        ++found->buttons;
                        found->ok = text == L"OK" ? child : found->ok;
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&children)
            );
            if (children.note && children.ok != nullptr && children.buttons == 1)
            {
                search->ok = children.ok;
                return FALSE;
            }
            return TRUE;
        }
    } // namespace

    bool close_sandbox_note(unsigned long editor_pid)
    {
        const auto until = std::chrono::steady_clock::now() + NOTE_WAIT;
        while (std::chrono::steady_clock::now() < until)
        {
            NoteSearch search{
                .pid = editor_pid,
            };
            EnumWindows(&find_note, reinterpret_cast<LPARAM>(&search));
            if (search.ok != nullptr)
            {
                PostMessageW(search.ok, BM_CLICK, 0, 0);
                return true;
            }
            std::this_thread::sleep_for(NOTE_POLL);
        }
        return false;
    }

    std::expected<void, std::string> setup_sandbox(const SandboxSetupRequest &request, const ToolPrint &print)
    {
        std::error_code error;
        const std::optional<fs::path> root =
            request.editor_root ? std::optional{fs::absolute(*request.editor_root, error)} : find_steam_sandbox();
        if (!root)
        {
            return std::unexpected{std::string{
                "the Sandbox editor (KCD2Mod) was not found in Steam's libraries: install it from Steam (Kingdom Come: "
                "Deliverance II Mod Tools) or pass its folder with --editor <folder>"
            }};
        }
        if (!fs::is_directory(*root / "Editor" / "Scripts", error))
        {
            return std::unexpected{
                std::format("{} is not the Sandbox editor's folder (no Editor\\Scripts)", root->string())
            };
        }
        const fs::path scripts = *root / "Editor" / "Scripts";
        const fs::path script = scripts / SCRIPT_FILE;
        const fs::path settings_file = scripts / SETTINGS_FILE;
        const fs::path toolbar = *root / "Editor" / "DynamicToolbars" / TOOLBAR_FILE;
        const fs::path placement = *root / "user_editor" / "dynamictoolbars" / TOOLBAR_PLACEMENT;

        if (request.remove)
        {
            bool removed_all = true;
            for (const fs::path &file : {script, settings_file, toolbar, placement})
            {
                if (fs::remove(file, error))
                {
                    print(std::format("  removed {}", file.string()));
                }
                else if (error)
                {
                    print(std::format("  could not remove {}: {}", file.string(), error.message()));
                    removed_all = false;
                }
            }
            if (!removed_all)
            {
                return std::unexpected{std::string{"some of the setup could not be removed (see above)"}};
            }
            return {};
        }

        Settings settings = read_settings(settings_file);
        setting(settings, "animfw") = own_exe().string();
        if (request.mod_folder)
        {
            setting(settings, "mod_folder") = fs::absolute(*request.mod_folder, error).string();
        }
        if (!write_text(script, sandbox_files::EXPORT_SCRIPT) || !write_text(toolbar, sandbox_files::TOOLBAR) ||
            !write_text(settings_file, write_settings(settings)))
        {
            return std::unexpected{std::format("could not write into {}", (*root / "Editor").string())};
        }
        print(std::format("  button: {}", toolbar.string()));
        print(std::format("  script: {}", script.string()));
        // The editor writes the toolbar's placement when it closes, so only a first setup shows the toolbar.
        if (!fs::exists(placement, error) && write_text(placement, TOOLBAR_SHOWN))
        {
            print(std::format("  toolbar shown: {}", placement.string()));
        }
        const std::string &mod = setting(settings, "mod_folder");
        print(
            std::format(
                "  settings: {} (mod folder: {})",
                settings_file.string(),
                mod.empty() ? std::string{"asked on the first click"} : mod
            )
        );
        print("Restart the editor if it is open. In the editor: select an object of your scene, click AnimFW export.");
        print("The editor's skins stop the button: with View > Dark Skin or Light Skin on, every program the editor");
        print("starts crashes at once (any program, not only animfw). Choose View > No Skinning (no restart needed).");
        return {};
    }
} // namespace AnimationFramework
