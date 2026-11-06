/**
 * @file hooks/anim_patch.hpp
 * @brief Conflict-free animation modding: mods ship deltas instead of whole Mannequin files and clip indexes, and the
 *        deltas merge into what the game loads.
 *
 * A patch for `<dir>/<stem>.<ext>` under Animations/Mannequin/ADB is `<dir>/<stem>__<modid>.<ext>`, the convention
 * the game's own table patches use. It holds only what it adds, replaces or removes:
 *   - .adb, FragmentList/<Fragment name>/<Fragment Tags FragTags>: the options with the same tags (compared as sets)
 *     are replaced in place. Mode="append" adds the options after them, Mode="remove" removes them.
 *   - .adb, FragmentBlendList/<Blend from to>/<Variant from to>: the variant is replaced, or removed with
 *     Mode="remove".
 *   - .adb, SubADBs/<SubADB File>: added unless the database already lists that file.
 *   - Tag and fragment-ID definitions (TagDefinition): new tags, groups, fragment IDs and imports are appended, so
 *     a mod can add a fragment of its own and play it (`player.human:PlayAnim(name, tags)`).
 *   - Controller definitions (`*_controllerdefs.xml`, ControllerDef): FragmentDefs/<Fragment scopes flags> set a
 *     fragment's scopes and flags (exactly with Mode="replace"), and its <Override tags fragTags scopes> replace the
 *     stock one with the same tag sets or are removed with Mode="remove". ScopeDefs/<Scope layer numLayers context
 *     Tags> set or add a scope. Scope contexts and fragment definition imports are added.
 *   - Animation event lists (`<stem>__<modid>.animevents` beside the stock one, any folder): <animation name> (the
 *     clip path, case-insensitive) replaces that clip's events, Mode="append" adds to them, Mode="remove" drops them.
 *   - Animation action tables (Libs/Tables anim_fragment.xml and actor_anim_action.xml): rows from
 *     `Animations/Mannequin/Tables/<table>__<modid>.xml` replace the row with the same identity or are appended.
 * `clipswaps__<modid>.xml` (`<ClipSwaps><Swap from to/>`) renames clips in every database fragment. A key two patches
 * set is logged with the one that wins. Mods that ship whole copies of one database, controller definition, tag
 * definition, event list or action table are turned into deltas against the game's copy and merged, so none of them
 * overwrites the others. Skeletons (.chr) two mods ship whole merge bone by bone. Every other animation file two mods
 * ship whole is logged with the mod whose copy loads. Clips: every .caf under `animations/` that a mod pak carries
 * registers in the clip index, a clip the game already has (loose or packed in a .dba) is replaced, and whole
 * `Animations.img` copies from several mods are merged. Patches come from the game's paks in mod load order (the
 * last-loaded mod's patch merges last and wins), then from loose files and paks in
 * `<game bin>/KCD2_AnimationFramework_Dev/`, so they can be edited without a repack and reloaded with `mn_reload`
 * (databases) or `animfw_reload_clips` (clips: it also packs the DEV folder's loose `.caf`, `.bspace` and `.comb`
 * files under `Animations/` into a pak of the framework's own and mounts it).
 */
#ifndef ANIMFW_HOOKS_ANIM_PATCH_HPP
#define ANIMFW_HOOKS_ANIM_PATCH_HPP

#include "anchors.hpp"
#include "config.hpp"
#include "core/anim_catalog.hpp"
#include "core/mod_tools.hpp"

#include <DetourModKit/hook.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace AnimationFramework
{
    /**
     * @brief Hooks the database loader (Settings::animation_patches), the skeleton reads (Settings::skeleton_merge)
     *        and the clip index loaders, turns the .dba loader's mismatch check off (Settings::mod_clips), and arms
     *        the late clip registration.
     * @note Setup only. Best-effort: a part whose anchors did not resolve stays off and logs why.
     */
    void install_anim_patch_hooks(const Anchors &anchors, const Settings &settings, DMK::hook::HookStack &hooks);

    /**
     * @brief The per-frame work: logs the load time the patches added once loading goes quiet, adds the
     *        `animfw_reload_clips` command once the console is up, runs a requested clip reload, and runs the late
     *        clip registration.
     * @details The late clip registration is for a generation that starts after the game loaded its clip index (a
     *          DEV hot reload): it mounts the DEV folder's paks, appends every `animations/animations__*.img` and
     *          mods' new clips to the global clip index through the engine's own loader, reloads each animation set's
     *          CHRPARAMS so the new clips play, and reloads replaced clips in place. It runs once. Until the clip
     *          index loads, each frame checks for it again. When the facial index hook appended everything inside the
     *          clip index init, the late registration has nothing to do.
     * @note Main thread only (the frame tick calls it).
     */
    void run_anim_patch_frame_work() noexcept;

    /**
     * @brief Removes the `animfw_reload_clips` command the frame work added (it packs the DEV folder's loose clips,
     *        mounts its new paks, adds new clips and reloads changed ones without a restart).
     * @note Main thread, teardown.
     */
    void remove_anim_patch_commands() noexcept;

    /** @brief Asks for what `animfw_reload_clips` does, on the next frame. */
    void request_clip_reload() noexcept;

    /**
     * @brief The animation catalog's view of the game's files: the copy of a file the game loads, from the newest
     *        mounted pak that holds it, and its patches in the order they merge, DEV folder ones last.
     * @note Main thread, once the game has mounted its paks.
     */
    [[nodiscard]] AnimCatalogFiles animation_catalog_files();

    /** @brief The running game as the clip check and the mod maker read it. Main thread. */
    [[nodiscard]] ModToolGame running_game_tools();

    /** @brief The DEV folder of loose patches, loose clips and test paks: `<game bin>/KCD2_AnimationFramework_Dev`. */
    [[nodiscard]] std::filesystem::path animation_dev_folder();

    /**
     * @brief The folder (lowercase, forward slashes, with its trailing slash) of the game file named @p file_name
     *        (lowercase), from the game's own paks, or nothing.
     */
    [[nodiscard]] std::optional<std::string> game_file_folder(std::string_view file_name);
} // namespace AnimationFramework

#endif // ANIMFW_HOOKS_ANIM_PATCH_HPP
