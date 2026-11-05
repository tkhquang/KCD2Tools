# Kingdom Come: Deliverance II - Engine Tweaks (More Performance)

> [!WARNING]
> I tested this mod on my own saves, but the game is huge and no test covers all of it. Use it at your own risk.

Removes engine freezes that grow with the number of items Henry carries: looking at a bed, and opening or closing the inventory, a loot or trade screen, the map or the journal, and transferring all items to or from a container. Made for hoarders like me, and for anyone who uses a mod that raises or removes Henry's carry weight limit.

## Fixes

Measured on a save with 2071 items. Each fix except the item lists has a switch in `KCD2_EngineTweaks.ini`.

| Switch | Change | Result |
| --- | --- | --- |
| `SleepPrediction` | The bed prompt simulates only the hours it reads, not a whole day. The sleep and wait sliders build the whole day once. | Bed check: 166-212 ms to 10-13 ms, with the next fix |
| `SleepPredictionWithoutItems` | That simulation leaves Henry's items out, most of its remaining cost. The prediction is identical for every hour. | |
| `MenuViewDistance` | Full-screen menus no longer change the view distance, which made the game rebuild the whole scene. | 370 ms on open and 450 ms on close removed |
| `LinkIndex` | Destroying an item finds its links through an index instead of a scan of every link in the game. | Menu copy of Henry: 650 ms to 110 ms, with the next two fixes |
| `InventoryRefresh` | Clearing an inventory refreshes its item flags once instead of once per item. | |
| `PerkCopy` | Copying perks runs the perk auto-unlock check once instead of once per perk. | |
| Item list movies | The game's own `ApseInventoryList.gfx` (inventory, trade) and `ItemTransfer.gfx` (loot) with a leaner script. The lists are identical. | Inventory list: 200 ms to 125 ms. Loot freezes: 1.2-1.5 s to 0.5 s at most. Transfer all: 47 s to 1 s |

The item list movies replace two game files, so remove `Data/engine_tweaks.pak` to get the stock lists back. Another mod that replaces them wins or loses by load order.

## Requirements

- Kingdom Come: Deliverance II 1.5.6 on Steam.
- [Kingdom Come Script Extender (KCSE)](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3332), or an ASI loader with the ASI add-on.

## Installation

**Main file:** install `KCD2_EngineTweaks_v*.zip` with your mod manager, or extract it into `<KC:D 2 folder>/Mods/`. With KCSE, that is all.

```
<KC:D 2 folder>/
|-- Bin/
|   \-- Win64MasterMasterSteamPGO/
|       |-- KingdomCome.exe              (game file)
|       \-- WHGame.dll                   (game file)
\-- Mods/
    \-- engine_tweaks/
        |-- mod.manifest
        |-- Data/
        |   \-- engine_tweaks.pak        (the item list movies)
        \-- KCSE/
            \-- Plugins/
                |-- KCD2_EngineTweaks.dll    (this mod)
                |-- KCD2_EngineTweaks.ini    (settings)
                |-- KCD2_EngineTweaks_License.txt
                \-- KCD2_EngineTweaks.log    (created when the mod loads)
```

<details>
<summary>Without KCSE: ASI add-on</summary>

1. Install the main file as above.
2. Without an ASI loader, extract one **x64** [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) DLL beside `WHGame.dll`.
3. Extract `KCD2_EngineTweaks_ASI_v*.zip` into the game folder, so its `Bin` folder merges with the game's. The ASI reads the `KCD2_EngineTweaks.ini` beside it.
4. On Linux or Steam Deck, add `WINEDLLOVERRIDES="dinput8=n,b" %command%` to the Steam launch options.

```
<KC:D 2 folder>/
|-- Bin/
|   \-- Win64MasterMasterSteamPGO/
|       |-- KingdomCome.exe              (game file)
|       |-- WHGame.dll                   (game file)
|       |-- dinput8.dll                  (ASI loader)
|       |-- KCD2_EngineTweaks.asi        (this mod)
|       |-- KCD2_EngineTweaks.ini        (settings)
|       |-- KCD2_EngineTweaks_License.txt
|       \-- KCD2_EngineTweaks.log        (created when the mod loads)
\-- Mods/
    \-- engine_tweaks/                   (main file)
```

With both KCSE and the add-on installed, only one copy runs.

</details>

## Troubleshooting

At `LogLevel = INFO` the log sums up each fix once a minute. `DEBUG` adds each notable event and `TRACE` each fixed call. To narrow a problem down, switch the fixes off one at a time, or remove `Data/engine_tweaks.pak` for the item lists. [Report it](https://github.com/tkhquang/KCD2Tools/issues) with `KCD2_EngineTweaks.log`, your INI, and your game and mod versions.

## Building from Source

<details>
<summary>Release and dev builds</summary>

Needs Visual Studio 2022 with the C++ workload, CMake 3.28 or newer and Git. From the repository root in PowerShell:

```powershell
git submodule update --init --recursive -- EngineTweaks/external/DetourModKit
Set-Location EngineTweaks
cmake --preset msvc-release
cmake --build --preset msvc-release
```

Pack `pack/` with [kcd-pak-action](https://github.com/tkhquang/kcd-pak-action)'s `scripts/pak_kcd_mod.py`. Its item list movies are committed prebuilt.

The dev preset builds a resident loader ASI and a logic DLL that Numpad 4 reloads in the running game:

```powershell
cmake --preset msvc-dev -DENGINETWEAKS_GAME_DIR="<KC:D 2 folder>/Bin/Win64MasterMasterSteamPGO"
cmake --build --preset msvc-dev
```

</details>
