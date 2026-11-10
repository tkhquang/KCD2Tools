# Cutscene Examples

Three cutscenes made with the [Animation Framework](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3981) and played through the game's own cutscene player. Talk to Magdalena at the forge in Kutna Hora and pick one.

> A showcase and a set of examples for modders who want to make their own cutscenes. It adds no gameplay.

- **A dance battle:** Henry challenges Magdalena. Both dance, and he wins or loses at random, in front of a crowd that cheers, claps and laughs.
- **A game cutscene, recast:** the fencers' bout from Kutna Hora, in its arena, with Henry, Jobst von Bergow and Hans in other people's places.
- **The same cutscene, moved:** the bout in an open field as a duel between Henry and Hans, with new camera work, grass footsteps and its music.
- **Real voices:** Henry and Magdalena speak with their own recorded lines from the game.
- **Made to learn from:** every file is open, and the [devlog](docs/DEVLOG.md) shows how each scene was made.
- Open source, built on the [Animation Framework](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3981) (built with [DetourModKit](https://github.com/tkhquang/DetourModKit)).

> [!CAUTION]
> **Spoiler: both fight cutscenes come from the side quest Ars Dimicatoria (the Kuttenberg swordfighters). If you haven't finished it, consider not watching them. It's just the fight, with the fighters' models swapped, but now you know.**

## Requirements

- [Animation Framework](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3981) 1.2.0 or newer.
- The Legacy of the Forge DLC (Magdalena is from it).
- Optional: [Sound Framework](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3860) 1.3.0 or newer, for the dialog voices. Without it the lines show as subtitles only.

## Installation

Install with your mod manager, or extract into `<game>/Mods/`. To uninstall, delete `Mods/cutscene_examples`.

## Playing the examples

Talk to Magdalena and pick a topic. The cutscene starts when the conversation closes. Press Space twice to skip it.

| Topic | Plays |
|---|---|
| "I challenge you to a (dance) duel!" | The dance battle in the forge yard. |
| "Did I ever tell you how I beat von Bergow at fencing?" | The fencers' bout where the game plays it. |
| "Hans and I had a friendly bout." | The bout in an open field west of Kutna Hora. |

> [!CAUTION]
> **The last two are from the side quest Ars Dimicatoria. See the spoiler note above.**

## For modders

- [Devlog](docs/DEVLOG.md): how each scene was made, and what went wrong.
- The framework's [cutscene guide](https://github.com/tkhquang/KCD2Tools/blob/main/AnimationFramework/docs/CUTSCENES.md): setup, Lua, quests and the rules a scene must follow.

## Credits

- The dance animations were made with [Meshy](https://www.meshy.ai/) (AI) and retargeted with the Animation Framework.
- The crowd, voices, music and sounds are the game's own, played from the installed game.

## License

MIT for the mod's own files. The copied cutscenes are the game's data and are not covered by it: see [LICENSE](LICENSE).
