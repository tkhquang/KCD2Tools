# Kingdom Come: Deliverance II - HUD Clock - Show the Time of Day

Shows the game time on the HUD.

## Features

- Shows with the HUD and in minigames (alchemy, smithing, dice, reading), optionally in photo mode. Hides in menus, dialogue and cutscenes.
- Fully customizable: 24 or 12 hours, part of the day, day count, real time, sun and moon dial, fonts, colors, position, and when it shows. See `KCD2_HUDClock.ini` for more.
- Replaces no game files and binds no keys by default.

## Requirements

- Kingdom Come: Deliverance II 1.5.6 on Steam.
- [Kingdom Come Script Extender (KCSE)](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3332), or an ASI loader with the ASI add-on.

## Installation

Install `KCD2_HUDClock_v*.zip` with your mod manager, or extract it into `<KC:D 2 folder>/Mods/`. With KCSE, that is all.

```
Mods/
\-- hud_clock/
    |-- mod.manifest
    |-- Data/
    |   \-- hud_clock.pak
    \-- KCSE/
        \-- Plugins/
            |-- KCD2_HUDClock.dll
            |-- KCD2_HUDClock.ini
            \-- KCD2_HUDClock_License.txt
```

<details>
<summary>Without KCSE: ASI add-on</summary>

1. Install the main file as above.
2. Put an x64 [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) `dinput8.dll` beside `WHGame.dll`.
3. Extract `KCD2_HUDClock_ASI_v*.zip` into the game folder. The ASI reads the `KCD2_HUDClock.ini` beside it.
4. On Linux or Steam Deck, add `WINEDLLOVERRIDES="dinput8=n,b" %command%` to the Steam launch options.

```
<KC:D 2 folder>/
\-- Bin/
    \-- Win64MasterMasterSteamPGO/
        |-- dinput8.dll
        |-- KCD2_HUDClock.asi
        |-- KCD2_HUDClock.ini
        \-- KCD2_HUDClock_License.txt
```

With both KCSE and the add-on installed, only one copy runs.

</details>

## Troubleshooting

Set `LogLevel=DEBUG`, reproduce the problem, and [open an issue](https://github.com/tkhquang/KCD2Tools/issues) with `KCD2_HUDClock.log` and your INI. The log is next to the INI.

## Building from Source

<details>
<summary>Release and dev builds</summary>

Needs Visual Studio 2022, CMake 3.28+, Git and Python 3 with [kcd-pak-action](https://github.com/tkhquang/kcd-pak-action).

```powershell
git submodule update --init --recursive -- HUDClock/external/DetourModKit
Set-Location HUDClock
cmake --preset msvc-release
cmake --build --preset msvc-release
```

Output: `build/release-msvc/KCD2_HUDClock.asi`, `build/release-msvc/KCSE/KCD2_HUDClock.dll`, and the INI in `build/template/`. Pack `pack/` with kcd-pak-action's `scripts/pak_kcd_mod.py`. `ui/KCD2_HUDClock.as` is the source of the prebuilt movie.

Dev build with in-game reload:

```powershell
cmake --preset msvc-dev -DHUDCLOCK_GAME_DIR="<game>/Bin/Win64MasterMasterSteamPGO"
cmake --build --preset msvc-dev
```

Rebuild `KCD2_HUDClock-logic` and press **Numpad 5** in game.

</details>

## Credits

- [DetourModKit](https://github.com/tkhquang/DetourModKit), with [SafetyHook](https://github.com/cursey/safetyhook), [Zydis](https://github.com/zyantific/zydis) and [SimpleIni](https://github.com/brofield/simpleini)
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by ThirteenAG
- [JPEXS FFDec](https://github.com/jindrapetrik/jpexs-decompiler), the movie compiler
- [Show Time II](https://www.nexusmods.com/kingdomcomedeliverance2/mods/636) by LoriAwesome, for the names of the parts of the day
- [A Matter of Time - A HUD clock widget](https://www.nexusmods.com/skyrimspecialedition/mods/12937) by SkyAmigo, for the timed and periodic display ideas
- Warhorse Studios, for Kingdom Come: Deliverance II
