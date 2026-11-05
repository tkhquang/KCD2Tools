# Kingdom Come: Deliverance II - HUD Clock - Show the Time of Day

Shows the game time on the HUD.

## Features

- Shows with the HUD and in minigames (alchemy, smithing, dice, reading), optionally in photo mode. Hides in menus, dialogue and cutscenes.
- Fully customizable: 24 or 12 hours, part of the day, day count, real time, sun and moon dial, fonts, colors, position, and when it shows. See `KCD2_HUDClock.ini` for more.
- Replaces no game files and binds no keys by default.

## Keys

`ToggleKey` and `ShowKey` in the INI are empty by default. They take named keys such as `F7`, `Numpad1`, `Mouse4` or `Gamepad_A`, combos such as `LShift+T`, and alternatives such as `F7,Gamepad_LB+Gamepad_Y`. See the [Supported Input Names](https://github.com/tkhquang/DetourModKit?tab=readme-ov-file#supported-input-names) reference. Controllers are XInput only, see [Gamepad Compatibility](https://github.com/tkhquang/DetourModKit?tab=readme-ov-file#gamepad-compatibility).

## Requirements

- Kingdom Come: Deliverance II 1.5.6 on Steam.
- [Kingdom Come Script Extender (KCSE)](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3332), or an ASI loader for the ASI add-on.

## Installation

**Main file:** install `KCD2_HUDClock_v*.zip` with your mod manager, or extract it into `<KC:D 2 folder>/Mods/`. With KCSE, that is all.

```
<KC:D 2 folder>/
|-- Bin/
|   \-- Win64MasterMasterSteamPGO/
|       |-- KingdomCome.exe              (game file)
|       \-- WHGame.dll                   (game file)
\-- Mods/
    \-- hud_clock/
        |-- mod.manifest
        |-- Data/
        |   \-- hud_clock.pak            (the clock's movie)
        \-- KCSE/
            \-- Plugins/
                |-- KCD2_HUDClock.dll    (this mod)
                |-- KCD2_HUDClock.ini    (settings)
                |-- KCD2_HUDClock_License.txt
                \-- KCD2_HUDClock.log    (created when the mod loads)
```

<details>
<summary>Without KCSE: ASI add-on</summary>

1. Install the main file as above.
2. Without an ASI loader, extract one **x64** [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) DLL beside `WHGame.dll`.
3. Extract `KCD2_HUDClock_ASI_v*.zip` into the game folder, so its `Bin` folder merges with the game's. The ASI reads the `KCD2_HUDClock.ini` beside it.
4. On Linux or Steam Deck, add `WINEDLLOVERRIDES="dinput8=n,b" %command%` to the Steam launch options.

```
<KC:D 2 folder>/
|-- Bin/
|   \-- Win64MasterMasterSteamPGO/
|       |-- KingdomCome.exe              (game file)
|       |-- WHGame.dll                   (game file)
|       |-- dinput8.dll                  (ASI loader)
|       |-- KCD2_HUDClock.asi            (this mod)
|       |-- KCD2_HUDClock.ini            (settings)
|       |-- KCD2_HUDClock_License.txt
|       \-- KCD2_HUDClock.log            (created when the mod loads)
\-- Mods/
    \-- hud_clock/                       (main file)
```

With both KCSE and the add-on installed, only one copy runs.

</details>

## Troubleshooting

Set `LogLevel = DEBUG` in the INI, reproduce the problem, and [report it](https://github.com/tkhquang/KCD2Tools/issues) with `KCD2_HUDClock.log`, your INI, and your game and mod versions. The log is next to the INI.

## Building from Source

<details>
<summary>Release and dev builds</summary>

Needs Visual Studio 2022 with the C++ workload, CMake 3.28 or newer and Git. From the repository root in PowerShell:

```powershell
git submodule update --init --recursive -- HUDClock/external/DetourModKit
Set-Location HUDClock
cmake --preset msvc-release
cmake --build --preset msvc-release
```

Pack `pack/` with [kcd-pak-action](https://github.com/tkhquang/kcd-pak-action)'s `scripts/pak_kcd_mod.py`. Its clock movie is committed prebuilt, and `ui/KCD2_HUDClock.as` is its source.

The dev preset builds a resident loader ASI and a logic DLL that Numpad 5 reloads in the running game:

```powershell
cmake --preset msvc-dev -DHUDCLOCK_GAME_DIR="<KC:D 2 folder>/Bin/Win64MasterMasterSteamPGO"
cmake --build --preset msvc-dev
```

</details>

## Credits

- [DetourModKit](https://github.com/tkhquang/DetourModKit), with [SafetyHook](https://github.com/cursey/safetyhook), [Zydis](https://github.com/zyantific/zydis) and [SimpleIni](https://github.com/brofield/simpleini)
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by ThirteenAG
- [JPEXS FFDec](https://github.com/jindrapetrik/jpexs-decompiler), the movie compiler
- [Show Time II](https://www.nexusmods.com/kingdomcomedeliverance2/mods/636) by LoriAwesome, for the names of the parts of the day
- [A Matter of Time - A HUD clock widget](https://www.nexusmods.com/skyrimspecialedition/mods/12937) by SkyAmigo, for the timed and periodic display ideas
- Warhorse Studios, for Kingdom Come: Deliverance II
