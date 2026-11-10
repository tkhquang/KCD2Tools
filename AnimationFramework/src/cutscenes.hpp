/**
 * @file cutscenes.hpp
 * @brief Mods' in-game cutscenes: sequence loading, the game's cutscene player, and runtime scene layers and profiles.
 *
 * `animfw_load_sequence` loads TrackView sequences from mods' XML files into the game's movie system. A sequence file
 * holds one `<Sequence>` (the element a level's moviedata.xml or a cutscene layer holds), or a root
 * whose `<Sequence>` children each load. Loading a sequence replaces the one of the same name. The engine binds an
 * entity node by its EntityGUID only, and an entity spawned at run time has a new GUID each time, so the loader writes
 * the GUID of the live entity named by the node's Name into the node first. A script spawns the cast, then loads the
 * file.
 *
 * `animfw_play_cutscene <holder>` starts a cutscene through the game's own cutscene player, with the calls the quest
 * CutsceneHandler node makes: the game then runs everything a cutscene does. No level file changes.
 * The read-only `animfw_cutscene_playing` variable lists the holders whose cutscenes still run; a holder leaves it
 * when its cutscene ends or the player skips it, so a script polls it to remove the scene's entities.
 *
 * The player reaches a holder's data entity through a link, and links need a WUID, which the game gives only the
 * entities its level lists. `animfw_make_linkable <entity>...` gives spawned entities one, with the call the game's
 * spawn listener makes, so that Lua's XGenAIModule.AddLink can link them.
 *
 * `animfw_scene_layer` adds a streamed entity layer at run time and `animfw_scene_profile` a game profile over it, as
 * the level loader adds its own, so a cutscene's IngameCutsceneData switches the scene's layer on. The Lua table
 * AnimationFramework.Cutscene (cutscene_lua.hpp) runs these commands in order for one scene.
 */
#ifndef ANIMFW_CUTSCENES_HPP
#define ANIMFW_CUTSCENES_HPP

#include "anchors.hpp"

#include <DetourModKit/hook.hpp>

namespace AnimationFramework
{
    /**
     * @brief Binds to the movie system and the engine's XML loader. Without them no command is added.
     * @note Setup only.
     */
    void bind_cutscenes(const Anchors &anchors) noexcept;

    /** @brief The per-frame work: adds the commands once the console is up. Main thread only. */
    void run_cutscene_frame_work() noexcept;

    /**
     * @brief Hooks CAnimSequence::TriggerTrackEvent so Event-node keys of the framework's cutscenes reach Lua
     *        (def.onEvent, def.signals). Logs and returns on any failure.
     */
    void install_scene_event_hook(const Anchors &anchors, DMK::hook::HookStack &hooks) noexcept;

    /**
     * @brief Hooks CEntityLoadManager::ReserveEntityIds: a streamed layer animfw_scene_layer added numbers its
     *        entities 1..N (as the Sandbox converter writes them), and each load moves them, with every reference to
     *        them in the file, to free ids before the game reserves and spawns them. Logs and returns on any failure.
     */
    void install_scene_layer_id_hook(const Anchors &anchors, DMK::hook::HookStack &hooks) noexcept;

    /** @brief Removes the commands and drops the framework's references to started cutscenes. Main thread, teardown. */
    void remove_cutscene_console() noexcept;
} // namespace AnimationFramework

#endif // ANIMFW_CUTSCENES_HPP
