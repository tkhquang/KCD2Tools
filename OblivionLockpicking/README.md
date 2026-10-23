# Kingdom Come: Deliverance II - Oblivion-Style Lockpicking - Tumbler Lock

My shaky hands never got along with the vanilla lockpicking, so I set out to replace it. I have to confess I spent more than a year on this mod and on [Sound Framework](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3860), which I built for it.

This mod replaces the lockpicking minigame with a tumbler lock in the style of The Elder Scrolls IV: Oblivion. The mechanics are recreated from observing how Oblivion's lockpicking plays. It is not a port and uses no Oblivion code, art or sound: the lock art is made for this mod, and the sounds are the game's own.

> [!NOTE]
> This mod is in early development. [Troubleshooting](#troubleshooting) explains how to report a problem.

## How It Works

- A lock has one to five tumblers, more on harder locks.
- Push a tumbler up with the pick. It rises at one of three speeds, pauses at the top, then falls back.
- Set it while it pauses. A set at any other moment is a wrong set: the game rolls whether the pick breaks, and the controls pause for a moment.
- A wrong set keeps some of your set tumblers, more with a higher Thievery level. Set them all and the lock opens.
- Thievery, perks and buffs slow the tumblers down. Experience, noise, crime and broken picks stay the game's own.

<details>
<summary>Looks, sounds and settings</summary>

- The lock is 15th-century Gothic ironwork, rusty on common locks and gilt on fancy ones, and richer on harder locks. The pick is shaped like the game's lockpick.
- Every push, click, fall and set has its own sound through Sound Framework.
- Keyboard, mouse and controller, with Oblivion Remastered's controls by default and every key rebindable. All texts are in the game's 16 languages.
- Settings apply while the game runs, and `Enabled=false` brings the vanilla minigame back.

</details>

## Requirements

- Kingdom Come: Deliverance II 1.5.6 on Steam. The GOG and Game Pass builds pass the same signature checks.
- [Kingdom Come Script Extender (KCSE)](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3332) for the KCSE build, or an ASI loader for the ASI build
- [Sound Framework](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3860)

## Installation

Install **one** of the two builds, not both.

**KCSE build:** install KCSE, then install `KCD2_OblivionLockpicking_KCSE_v*.zip` with your mod manager, or extract it into `<KC:D 2 folder>/Mods/`.

<details>
<summary>ASI build</summary>

1. Without an ASI loader, extract one **x64** [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) DLL beside `WHGame.dll` in `Bin/Win64MasterMasterSteamPGO/`. Other stores use their own `Bin/Win64MasterMaster...` folder. Use [dinput8-x64.zip](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/dinput8-x64.zip), or [version-x64.zip](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/version-x64.zip) when another mod already uses `dinput8.dll`. KCSE also loads ASI files, so KCSE users skip this step.
2. Extract `KCD2_OblivionLockpicking_v*.zip` into the game folder, so its `Bin` and `Mods` folders merge with the game's. Without `Mods/oblivion_lockpicking/`, the game keeps its vanilla lockpicking.
3. On Linux or Steam Deck, add `WINEDLLOVERRIDES="dinput8=n,b" %command%` to the game's Steam launch options (`version=n,b` for the version loader).

</details>

Recommended: [FPS FIX (Memory Leak)](https://www.nexusmods.com/kingdomcomedeliverance2/mods/654) and [Map Close Crash Bugfix](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3462), which fix crashes in the game's own memory handling that a UI-heavy mod can also run into.

## Controls

| Action | Keyboard and mouse | Controller |
| --- | --- | --- |
| Move the pick | `A` / `D`, or the mouse | Left stick |
| Push the tumbler | `W`, or move the mouse up | Left stick up |
| Set the tumbler at the top | `Space` or left click | `A` / `Cross` |

Hold the push to keep a tumbler bouncing. `KCD2_OblivionLockpicking.ini` documents every setting, and `[Controls]` changes any key.

## Troubleshooting

Set `LogLevel=DEBUG` in the INI, reproduce the problem, and [report it](https://github.com/tkhquang/KCD2Tools/issues) with `KCD2_OblivionLockpicking.log`, your INI, and your game and mod versions. The log is beside the ASI, or in `Mods/oblivion_lockpicking/KCSE/Plugins/` for the KCSE build (with MO2, in its overwrite folder).

<details>
<summary>Known limitations</summary>

- The help screen's key icons show the game's default movement, jump and use keys, not your own bindings.
- PlayStation button names are not tested on a real controller.

</details>

## Building from Source

<details>
<summary>Release and dev builds</summary>

Needs Visual Studio 2022 with the C++ workload, CMake 3.28 or newer, Git and Python 3 with [kcd-pak-action](https://github.com/tkhquang/kcd-pak-action). From the repository root in PowerShell:

```powershell
git submodule update --init --recursive
Set-Location OblivionLockpicking
cmake --preset msvc-release
cmake --build --preset msvc-release
```

The build writes `build/release-msvc/KCD2_OblivionLockpicking.asi` and the KCSE plugin `build/release-msvc/KCSE/KCD2_OblivionLockpicking.dll`. The INI is in `build/template/`. `pack/` is the `Mods` folder part: pack it with kcd-pak-action's `scripts/pak_kcd_mod.py`. Its art and Scaleform movie are committed prebuilt, and `ui/KCD2_OblivionLockpicking.as` is the movie's source.

The dev build adds a resident loader and a logic DLL that reloads in game:

```powershell
cmake --preset msvc-dev -DOBLIVIONLP_GAME_DIR="<game>/Bin/Win64MasterMasterSteamPGO"
cmake --build --preset msvc-dev
```

Rebuild the target `KCD2_OblivionLockpicking-logic` and press **Numpad 8** in game to load it.

</details>

## Credits

<details>
<summary>Libraries, tools and assets</summary>

- [DetourModKit](https://github.com/tkhquang/DetourModKit): the modding framework
- [ThirteenAG](https://github.com/ThirteenAG): Ultimate ASI Loader
- [cursey](https://github.com/cursey): SafetyHook, and [zyantific](https://github.com/zyantific): Zydis and Zycore
- [Brodie Thiesfield](https://github.com/brofield): SimpleIni
- [Poly Haven](https://polyhaven.com): the CC0 textures Rusty Metal 04 and Rust Coarse 01 and the Studio Small 09 HDRI used to render the lock
- [JPEXS FFDec](https://github.com/jindrapetrik/jpexs-decompiler): the movie compiler
- [Caseter](https://github.com/Caseter/KCD2-Blender-Toolkit) and [Lune](https://github.com/LuneMods/KCD2-Blender-Toolkit): KCD2 Blender Toolkit
- Bethesda Game Studios: the lockpicking design of The Elder Scrolls IV: Oblivion, which inspired this mod. This mod is not affiliated with or endorsed by Bethesda Softworks or Warhorse Studios.
- Warhorse Studios: Kingdom Come: Deliverance II

</details>
