/**
 * @file tool/studio/file_dialogs.hpp
 * @brief The Windows file and folder pickers for the Studio window. Kept apart from raylib, whose names (CloseWindow,
 *        Rectangle, DrawText) clash with windows.h.
 */

#ifndef ANIMFW_TOOL_STUDIO_FILE_DIALOGS_HPP
#define ANIMFW_TOOL_STUDIO_FILE_DIALOGS_HPP

#include <optional>
#include <string>
#include <string_view>

namespace AnimationFramework::studio
{
    /** @brief A `.caf` file the user picks, or nothing when they cancel. */
    [[nodiscard]] std::optional<std::string> pick_clip_file();

    /** @brief A `.bvh` or `.fbx` animation the user picks, or nothing when they cancel. */
    [[nodiscard]] std::optional<std::string> pick_mocap_file();

    /** @brief A folder the user picks, or nothing when they cancel. */
    [[nodiscard]] std::optional<std::string> pick_folder(std::string_view title);

    /** @brief A font of Windows' own Fonts folder by file name, when it exists. */
    [[nodiscard]] std::optional<std::string> windows_font(std::string_view file_name);
} // namespace AnimationFramework::studio

#endif // ANIMFW_TOOL_STUDIO_FILE_DIALOGS_HPP
