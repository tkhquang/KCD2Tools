## Initial Release

- Mods can add sounds through an INI in their own SoundFramework folder and play them on the game's buses, in 3D or flat.
- Pack sounds can replace the game's sound events, everywhere or only for chosen entities.
- NPCs can speak a pack's voice lines with a subtitle, a chat bubble or both.
- Pack voice lines can replace the game's own voice lines, by exact key or by pattern.
- Voice lines move the speaker's mouth: ADVANCED uses a small model trained on the game's own animation, BASIC follows the voice's loudness.
- Voice lines can hold a facial mood; eleven default moods ship, and packs can change them or add their own.
- Moods can be pushed further for extreme expressions, up to a safe limit for each mood.
- The mouth, mood and subtitle start with the line's sound and stop when the line is cut short.
- Loading a save stops the framework's voice lines and sounds.
- A conversation line takes over the speaker at once from a pack line.
- A pack line waits for the character to finish speaking instead of cutting the game's line off.
- The mouth pauses with the game, so it stays in step with the voice after menus.
- Recognizes the Steam, GOG and Game Pass versions of the game.
- Saving a pack or the settings INI applies it while playing.
- Ships as an ASI and as a KCSE plugin.
