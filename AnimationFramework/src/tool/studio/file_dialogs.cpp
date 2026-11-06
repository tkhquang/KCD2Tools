/**
 * @file tool/studio/file_dialogs.cpp
 * @brief Windows file and folder pickers (see file_dialogs.hpp).
 */

#include "tool/studio/file_dialogs.hpp"

#include <windows.h>
#include <shobjidl.h>

#include <filesystem>
#include <iterator>

namespace AnimationFramework::studio
{
    namespace
    {
        /** @brief Runs the common item dialog; @p folders picks a folder instead of a file of @p kind (@p pattern). */
        [[nodiscard]] std::optional<std::string> pick(
            bool folders,
            const wchar_t *title,
            const wchar_t *kind = L"Compiled clips (*.caf)",
            const wchar_t *pattern = L"*.caf"
        )
        {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            std::optional<std::string> chosen;
            IFileOpenDialog *dialog = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            {
                FILEOPENDIALOGOPTIONS options{};
                dialog->GetOptions(&options);
                dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (folders ? FOS_PICKFOLDERS : 0));
                dialog->SetTitle(title);
                if (!folders)
                {
                    const COMDLG_FILTERSPEC filters[] = {{kind, pattern}, {L"All files", L"*.*"}};
                    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
                }
                IShellItem *item = nullptr;
                if (SUCCEEDED(dialog->Show(nullptr)) && SUCCEEDED(dialog->GetResult(&item)))
                {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                    {
                        chosen = std::filesystem::path{path}.string();
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
                dialog->Release();
            }
            if (SUCCEEDED(com))
            {
                CoUninitialize();
            }
            return chosen;
        }
    } // namespace

    std::optional<std::string> pick_clip_file()
    {
        return pick(false, L"Pick a clip");
    }

    std::optional<std::string> pick_mocap_file()
    {
        return pick(
            false,
            L"Pick an animation to retarget",
            L"Motion capture and DCC animations (*.bvh, *.fbx)",
            L"*.bvh;*.fbx"
        );
    }

    std::optional<std::string> pick_folder(std::string_view title)
    {
        const std::wstring wide = std::filesystem::path{title}.wstring();
        return pick(true, wide.c_str());
    }

    std::optional<std::string> windows_font(std::string_view file_name)
    {
        wchar_t windows[MAX_PATH]{};
        const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
        {
            return std::nullopt;
        }
        const std::filesystem::path font = std::filesystem::path{windows} / "Fonts" / file_name;
        std::error_code error;
        return std::filesystem::exists(font, error) ? std::optional{font.string()} : std::nullopt;
    }
} // namespace AnimationFramework::studio
