# Changelog

All notable changes to the Animation Framework will be documented in this file.

## [1.2.0] - In-Game Cutscenes

- Mods can play their own in-game cutscenes through the game's cutscene player, with letterbox, fades, skipping, voices, subtitles and Henry in his current armour.
- A script starts a cutscene with one Lua line, and quests can start one too through the game's "run console command" quest node, which now works.
- A cutscene can tell a script or a quest when it reaches a marked moment and when it ends.
- Cutscenes keep the game's crowd, fire and other sound effects, and can show text on a black screen like the game's "Some time later...".
- With Sound Framework installed, actors' mouths move with a mod's own voice lines.
- New tool command `animfw clone-cutscene` copies one of the game's cutscenes into a mod, and can play it somewhere else.
- New tool commands `animfw sandbox-layer` and `animfw sandbox-setup` turn a scene made in the game's Sandbox editor into a mod's cutscene, with a one-click export button.
- `animfw pack` also packs a mod's level files and dialogue text.
- `animfw retarget --skeleton female` makes animations for women.
- New INI option Cutscenes switches all of this off.
- New [cutscene guide](https://github.com/tkhquang/KCD2Tools/blob/main/AnimationFramework/docs/CUTSCENES.md) and a new example mod, [Cutscene Examples](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3996).

## [1.1.0] - Play in Game Without the Lua Runner

- The Blender add-on's Play in game and Stop work with only the Animation Framework installed, no KCD2 Lua runner needed.
- Play in game tells you at once when no save is loaded.
- Play in game works when the game runs as administrator and Blender does not.
- The new INI option BlenderBridge switches this off. The add-on then uses the KCD2 Lua runner.

## [1.0.0] - Initial Release

- Animation mods work together: each mod ships only what it changes, and mods that ship whole copies of the same files (databases, tags, sounds, skeletons, clip indexes) are merged instead of overwriting each other.
- Mods can add new animation clips or replace the game's own, without shipping the game's whole clip index.
- The log names the mod that wins when two mods change the same thing, and lists what cannot be merged.
- Scripts can play animations on NPCs as well as Henry, over what a character is doing, pick an exact clip, hold the last pose, queue several and stop them at once.
- Animations can play only in third person (with a camera mod), or only while a mod's own state, such as tired or wet, is on.
- Scripts can search every animation Henry can play, and keep their mod's settings in a file.
- Tools for mod authors, in game, on the command line, in a window and in Blender: check clips, compile Blender exports, retarget motion capture onto Henry, make and pack a mod, and play it in the running game in seconds.
- Installs as a KCSE plugin with a mod manager, or as an ASI for players without KCSE.
- Each part can be switched off in the INI.

[1.2.0]: https://github.com/tkhquang/KCD2Tools/releases/tag/AnimationFramework-v1.2.0
[1.1.0]: https://github.com/tkhquang/KCD2Tools/releases/tag/AnimationFramework-v1.1.0
[1.0.0]: https://github.com/tkhquang/KCD2Tools/releases/tag/AnimationFramework-v1.0.0
