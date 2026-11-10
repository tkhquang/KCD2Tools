## In-Game Cutscenes

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
