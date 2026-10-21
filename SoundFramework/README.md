# Kingdom Come: Deliverance II - Sound Framework

Custom sounds and voices with lip sync for Kingdom Come: Deliverance II mods.

## Overview

Sound Framework lets mods add sounds and voice lines to the game without writing code. A mod drops an INI file into its own `SoundFramework` folder; the framework plays its sounds on the game's own audio mixer, makes NPCs speak its voice lines with subtitles and lip movement, and can replace the game's sounds and voice lines with the mod's.

On its own the framework changes nothing you can hear. Players install it for the mods that need it; [Stop Right There, Criminal Scum](../CriminalScum/) and [Chiuuu! Mouth-Sound Bows](../MouthSoundBows/) are two, and the [modding guide](docs/MODDING.md) shows how to make your own.

Questions, packs to share or ideas? Join the [KCD Modding Community](https://discord.gg/wzhhsQRnkZ) on Discord.

[![Showcase video on YouTube](https://img.youtube.com/vi/oTJTcMXMzU0/maxresdefault.jpg)](https://youtu.be/oTJTcMXMzU0)

> [!NOTE]
> This mod is in early development. If something doesn't work properly, please [report it](https://github.com/tkhquang/KCD2Tools/issues) with your game/mod versions, settings and log.

## Features

- **Sounds:** play files from a pack on the game's buses (world, voice, music and others), in 3D or flat, with volume and pitch variation, loops, cooldowns and instance limits
- **Sound replacement:** play a pack sound in place of a game sound event or on top of it, everywhere or for chosen entities, at its start or at a later moment such as a bow's release
- **Voice lines:** NPCs speak a pack's lines through the game's own speech system, with a bottom subtitle and speaker name, a chat bubble, or both
- **Voice replacement:** play a pack line instead of a game voice line, by exact key or by pattern
- **Lip sync:** a pack's lines move the speaker's mouth, either with a small model that learned from the game's own animation (ADVANCED) or from the loudness of the voice (BASIC). A voice dub without mouth animation of its own gets it on every game line too, cutscenes included
- **Moods:** facial expressions held while a line plays, with eleven defaults that packs can change or add to
- **Live reload:** saving a pack INI or the settings INI applies it while you play
- Diagnostics for pack authors: every problem in a pack is logged with its file and line, and debug options log the game's sound events and voice lines

## Installation

Built around **KCD2 Steam 1.5.6**. Its game-code signatures also match the GOG and Game Pass builds. The framework comes in two builds; install **one**:

- **ASI build** (`KCD2_SoundFramework_vX.Y.Z.zip`): needs an ASI loader, which is not bundled.
- **KCSE build** (`KCD2_SoundFramework_KCSE_vX.Y.Z.zip`): needs [Kingdom Come Script Extender (KCSE)](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3332) and installs into the `Mods` folder like a regular mod.

If both are installed, the second one to load stays off and says so in its log.

### ASI build

1. Open your KC:D 2 installation folder, then the `Bin/Win64MasterMaster...` subfolder that contains `WHGame.dll`. On Steam this is `<KC:D 2 installation folder>/Bin/Win64MasterMasterSteamPGO/`.
2. If you don't have an ASI loader yet, download **one x64** variant of [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) and extract its DLL beside `WHGame.dll`:
   - [dinput8.dll](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/dinput8-x64.zip): the usual choice if the name is unused
   - [version.dll](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/version-x64.zip): when `dinput8.dll` is already used
   - [winmm.dll](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/winmm-x64.zip): another alternative

   KCSE also loads `.asi` files beside its `dinput8.dll`, so KCSE users need no separate loader.
3. Extract the archive into the same folder, beside `WHGame.dll`, without an extra archive-named subfolder:

```
Bin/Win64MasterMasterSteamPGO/
|-- WHGame.dll                             (game file, already present)
|-- dinput8.dll                            (ASI loader)
|-- KCD2_SoundFramework.asi                (this mod)
|-- KCD2_SoundFramework.ini                (settings)
|-- KCD2_SoundFramework_moods.ini          (default facial moods)
|-- KCD2_SoundFramework_lipmodel.bin       (advanced lip sync model)
|-- KCD2_SoundFramework_cutscenes.txt      (who speaks in each cutscene)
\-- KCD2_SoundFramework.log                (created when the mod loads)
```

On Wine/Proton, add an override for the loader in Steam's **Properties > Launch Options**: `WINEDLLOVERRIDES="dinput8=n,b" %command%` (or `version=n,b`, `winmm=n,b`).

### KCSE build

Install KCSE, then extract the archive into `<KC:D 2 installation folder>/Mods/` (create the folder if it is missing), or install it with a mod manager:

```
Mods/
\-- soundframework/
    |-- mod.manifest
    \-- KCSE/
        \-- Plugins/
            |-- KCD2_SoundFramework.dll
            |-- KCD2_SoundFramework.ini
            |-- KCD2_SoundFramework_moods.ini
            |-- KCD2_SoundFramework_lipmodel.bin
            |-- KCD2_SoundFramework_cutscenes.txt
            \-- KCD2_SoundFramework.log    (created when the mod loads)
```

### Verify

After loading a save, open `KCD2_SoundFramework.log` beside the mod's DLL or ASI. It lists every pack the framework found and any problem in them.

## Configuration

`KCD2_SoundFramework.ini` sits beside the mod. Save it to apply changes while playing; only `[LipSync] Enabled` needs a restart.

| Setting | Default | Meaning |
|---|---|---|
| `[Settings] LogLevel` | `INFO` | `TRACE`, `DEBUG`, `INFO`, `WARNING` or `ERROR`. |
| `[LipSync] Enabled` | `true` | Lip movement for voice lines without the game's own lip-sync animation. Off means the face hook is not installed at all. |
| `[LipSync] Mode` | `ADVANCED` | `ADVANCED`: a small machine-learned model moves the whole lower face (jaw, lips, mouth corners, smile lines). `BASIC`: jaw and lip shapes follow the loudness and spectrum of the voice. Advanced falls back to basic when the model file is missing. |
| `[LipSync] OffsetMs` | `0` | Moves the mouth earlier (positive) or later (negative) than the sound. |
| `[LipSync] DubbedVoices` | `AUTO` | Voice dubs: the game shows English mouth animation for a voice language without its own. `AUTO`: the lips then follow each line's own voice, in cutscenes too, and the animation's smiles and frowns stay. `ON`: always. `OFF`: never. |
| `[Debug] EventLog` | empty | Logs every game sound event whose path contains this text (`*` for all), and the parameters the game sets on it. |
| `[Debug] SpeakLog` | empty | Logs every voice line the game speaks whose key or file contains this text (`*` for all). |

`KCD2_SoundFramework_moods.ini` holds the default moods. It is replaced on update; change a mood by defining it again in your own pack.

## Console Commands

Open the game console with `~`.

| Command | What it does |
|---|---|
| `sfw_play <sound> [x y z]` | Plays a pack sound at a world position, or at the camera. |
| `sfw_stop <sound>` / `sfw_stop_all` | Stops a sound / every framework sound. |
| `sfw_say <entity> <line or voice path> ...` | An NPC says a pack voice line, or speaks a recording. |
| `sfw_lipsync_offset <ms>` | Moves the mouth earlier or later. |
| `sfw_mood_override <mood> [pose:weight ...]` / `sfw_mood_dump <mood>` | Tries mood values live / writes a mood to the log. |
| `sfw_reload` / `sfw_list` | Reads every pack again / writes the catalog to the log. |

### Quick test

No pack needed. Load a save, open the console and make Henry say other characters' lines in different moods:

```
sfw_say Dude Localization/dialog/trosecko/konecZabomysichValek/jmil_comb_bojovnici_ty_odporny_RkVq mood:angry strength:2 You disgusting, slimy, rotten piece of shit!
sfw_say Dude Localization/dialog/open_world/lazne_a_lazebnice/aals_obje_lazebnice_sss__jeste_AjPM mood:happy Wonderful! You'll see, you'll be happy to come back here.
sfw_say Dude Localization/dialog/open_world/crime_reaction_barks/mrtvola/mbis_npc__npc_vidi__promin_str_0DUg mood:sad I'm sorry, but I can't stand to be here any longer.
sfw_say Dude Localization/dialog/kutnohorsko/pogrom/tsho_zena_pogrom_ob_na_pomoc_S0g7 mood:scared Somebody help us!
sfw_say Dude Localization/dialog/open_world/lazne_a_lazebnice/aals_obje_lazebnice_ale_ty_uz__wAcW mood:surprised You're already undressed? That's a new one!
sfw_say Dude Localization/dialog/trosecko/konecZabomysichValek/jmil_comb_bojovnici_olbramovi__xiB6 mood:disgusted Olbram's breath smells like a bull's arse!
sfw_say Dude Localization/dialog/utils/roomrenting/lcar_zena_hospodska_to_pujde_z_8Lbz mood:smug Very well, but you'll have to pay in advance.
```

- `Dude` is Henry. A third-person camera such as [TPVCamera](../TPVCamera/) shows his face.
- `strength` scales the mood from 0 to 2 (default 1). The text at the end is the subtitle.
- None of these recordings has the game's lip-sync animation, and the voices are other characters', so the mouth movement is the framework's lip sync and the mood moves the whole face.

## Making Sound Packs

See the [modding guide](docs/MODDING.md) for the pack format: sounds, voice lines, text, moods, replacing the game's sounds and voice lines, and how to find the event paths and line keys to replace. Share your packs and ask for help in the [KCD Modding Community](https://discord.gg/wzhhsQRnkZ) Discord.

## How It Works

The framework plays sounds through the game's own FMOD Studio system, so they follow the game's volume sliders, pause with the game and use its 3D listener. Voice lines go through the game's speech system, the same path its dialog takes, so the subtitles and bubbles are the game's own. Game code is found by byte signatures and type information rather than fixed addresses, so small game updates are less likely to break it.

Advanced lip sync runs a 680,000-parameter convolutional network on a worker thread. It turns each voice line into 33 face pose channels in about 7 ms per second of audio, and the mouth starts moving within a frame or two of the line.

## Lip Sync Model and License

Advanced lip sync is machine-learned: the network was trained on Kingdom Come: Deliverance II's own voice lines and facial animation, and generates facial animation only (no audio, text or images). Set `[LipSync] Mode = BASIC` to use the mouth without machine learning.

The code is MIT licensed (see [LICENSE](LICENSE)). `KCD2_SoundFramework_lipmodel.bin` and `KCD2_SoundFramework_cutscenes.txt` are derived from Kingdom Come: Deliverance II game data owned by Warhorse Studios and PLAION. The MIT license does not cover them: use them only with KC:D 2 through this mod, and not in other games, other products or commercially.

## Troubleshooting

- Set `LogLevel = DEBUG` under `[Settings]`, save the INI and reproduce the problem.
- Check `KCD2_SoundFramework.log` beside the mod. Pack problems are listed with their file and line.
- If a mod's sounds don't play, check that the mod has a `SoundFramework` folder with its INI and that the log lists the pack.
- Include the log, INI, game/mod versions and reproduction steps in your report.

## Building from Source

### Prerequisites

- Visual Studio 2022 with the C++ workload
- CMake 3.28 or newer
- Git (to fetch submodules)
- Python 3 (for version updates)

### Release Build

From the repository root in PowerShell:

```powershell
git submodule update --init --recursive -- SoundFramework/external/DetourModKit
Set-Location SoundFramework
cmake --preset msvc-release
cmake --build --preset msvc-release
```

This builds both release outputs, `build/release-msvc/KCD2_SoundFramework.asi` and `build/release-msvc/KCSE/KCD2_SoundFramework.dll`, plus the unit tests (`build/release-msvc/Release/sndfw_*_tests.exe`). The runtime files to ship beside them are in `build/template/`.

### Dev Build (Hot-Reload)

The dev build produces a resident loader (`KCD2_SoundFramework.asi`) and a logic DLL (`KCD2_SoundFramework.logic.dll`), so you can rebuild and reload the mod code without restarting the game.

From the `SoundFramework` directory, with the game closed for the initial build:

```powershell
cmake --preset msvc-dev -DSNDFW_GAME_DIR="<game>/Bin/Win64MasterMasterSteamPGO"
cmake --build --preset msvc-dev
```

The build deploys the loader into the game folder, replacing the release ASI, and copies the logic DLL and PDB into `staging/`. After changing the mod code, rebuild and press **Numpad 6** while the game window has focus to load the staged build. Loader changes need a game restart.

### Code style

The C++23 sources follow a hard 120-column baseline and DetourModKit's coding conventions ([AGENTS.md](https://github.com/tkhquang/DetourModKit/blob/main/AGENTS.md)), with `.clang-format` (clang-format 20 or newer). Format changed files before submitting changes and check them with `clang-format --dry-run --Werror <files>`.

## Credits

- [DetourModKit](https://github.com/tkhquang/DetourModKit) - core modding framework
- [ThirteenAG](https://github.com/ThirteenAG) - Ultimate ASI Loader
- [cursey](https://github.com/cursey) - SafetyHook
- [Florian Bernd and Joel Hoener](https://github.com/zyantific) - Zydis and Zycore, the disassembly used by SafetyHook
- [Brodie Thiesfield](https://github.com/brofield) - SimpleIni
- Warhorse Studios - Kingdom Come: Deliverance II
