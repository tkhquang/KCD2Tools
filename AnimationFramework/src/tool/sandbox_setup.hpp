/**
 * @file tool/sandbox_setup.hpp
 * @brief Puts the "AnimFW export" button into the game's Sandbox editor (the KCD2Mod tools), and the helpers it runs.
 *
 * The button is an editor toolbar (`Editor/DynamicToolbars/AnimationFramework.xml`) whose element runs the editor's
 * Python file `Editor/Scripts/animfw_export.py`: it saves the layers, finds the selected scene's `.lyr` and runs
 * `animfw sandbox-layer` (sandbox_layer.hpp) for it. Both files are embedded in animfw.exe and written by the setup,
 * which also points `Editor/Scripts/animfw_export.cfg` at this animfw.exe (and the mod folder, when given). The setup
 * writes only these files and the toolbar's first placement. The editor's own files stay as they are.
 */
#ifndef ANIMFW_TOOL_SANDBOX_SETUP_HPP
#define ANIMFW_TOOL_SANDBOX_SETUP_HPP

#include "core/mod_tools.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace AnimationFramework
{
    /** @brief What the setup does. */
    struct SandboxSetupRequest
    {
        /// The editor's folder (KCD2Mod), or empty to find it in the Steam libraries.
        std::optional<std::filesystem::path> editor_root;
        /// The mod folder the button exports into, or empty to keep the one in the settings (or let the button ask).
        std::optional<std::filesystem::path> mod_folder;
        /// Removes everything the setup wrote instead.
        bool remove = false;
    };

    /** @brief Installs (or removes) the button, reporting each step to @p print. */
    [[nodiscard]] std::expected<void, std::string>
    setup_sandbox(const SandboxSetupRequest &request, const ToolPrint &print);

    /**
     * @brief Closes the editor's OK-only "No modified layers found" note for the editor process @p editor_pid, if it
     *        shows within a few seconds: the note the button's save pops when nothing changed. Nothing else is touched.
     * @return Whether a note was closed.
     */
    bool close_sandbox_note(unsigned long editor_pid);
} // namespace AnimationFramework

#endif // ANIMFW_TOOL_SANDBOX_SETUP_HPP
