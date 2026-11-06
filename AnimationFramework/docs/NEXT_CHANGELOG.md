## Initial Release

- Animation mods work together: each mod ships only what it changes, and mods that ship whole copies of the same files (databases, tags, sounds, skeletons, clip indexes) are merged instead of overwriting each other.
- Mods can add new animation clips or replace the game's own, without shipping the game's whole clip index.
- The log names the mod that wins when two mods change the same thing, and lists what cannot be merged.
- Scripts can play animations on NPCs as well as Henry, over what a character is doing, pick an exact clip, hold the last pose, queue several and stop them at once.
- Animations can play only in third person (with a camera mod), or only while a mod's own state, such as tired or wet, is on.
- Scripts can search every animation Henry can play, and keep their mod's settings in a file.
- Tools for mod authors, in game, on the command line, in a window and in Blender: check clips, compile Blender exports, retarget motion capture onto Henry, make and pack a mod, and play it in the running game in seconds.
- Installs as a KCSE plugin with a mod manager, or as an ASI for players without KCSE.
- Each part can be switched off in the INI.
