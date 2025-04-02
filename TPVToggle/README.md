# Kingdom Come: Deliverance II - Third Person View Toggle

## Overview

**TPVToggle** is an ASI plugin for Kingdom Come: Deliverance II that enables players to toggle between first-person and third-person camera views using customizable hotkeys.

## Features

- Toggle between first-person and third-person views with a keypress (default: F3)
- Dedicated keys for forcing first-person or third-person view
- Fully customizable key bindings via INI configuration
- Open-source with full transparency

## Installation

1. Download the latest release from [Nexus Mods](https://www.nexusmods.com/kingdomcomedeliverance2/mods/1550) or the [GitHub Releases page](https://github.com/tkhquang/KDC2Tools/releases)
2. Extract all files to your game directory:

   ```
   <KC:D 2 installation folder>/Bin/Win64MasterMasterSteamPGO/
   ```

3. Launch the game and use the configured hotkey (default: F3) to toggle the camera view.

> **Note:** This mod was developed and tested on the Steam version of Kingdom Come: Deliverance II. Other versions (Epic, GOG, etc.) may not be compatible.

## How It Works

This mod enables third-person view using the following approach:

1. **Memory Scanning** – Scans the game's memory for a specific byte pattern that accesses the camera view state.
2. **Exception Handling** – Uses a minimal INT3 hook to capture the `r9` register, which contains the pointer to the view state.
3. **Key Monitoring** – Spawns a separate thread to listen for configured hotkeys.
4. **View Toggling** – Safely toggles the camera view byte between `0` (first-person) and `1` (third-person).

This implementation minimizes code modification, improving stability and compatibility with game updates.

## Configuration

The mod is configured via the `KCD2_TPVToggle.ini` file:

```ini
[Settings]
; Keys that toggle the third-person view (comma-separated, in hex)
; F3 = 0x72, F4 = 0x73, E = 0x45, etc.
; If set to empty (ToggleKey = ), no toggle keys will be monitored.
ToggleKey = 0x72

; First-person view keys (comma-separated, in hex)
; Always switch to first-person view when pressed.
; Default keys are game menu keys that benefit from first-person view.
; If set to empty (FPVKey = ), no first-person view keys will be monitored.
; 0x4D,0x50,0x49,0x4A,0x4E = M, P, I, J, N
FPVKey = 0x4D,0x50,0x49,0x4A,0x4E

; Third-person view keys (comma-separated, in hex)
; Always switch to third-person view when pressed.
; If set to empty (TPVKey = ), no third-person view keys will be monitored.
TPVKey =

; Logging level: DEBUG, INFO, WARNING, ERROR
LogLevel = INFO

; AOB Pattern (advanced users only - update if mod stops working after game patches)
AOBPattern = 48 8B 8F 58 0A 00 00
```

The mod looks for the INI file in the following locations:

- The game's executable directory (`Win64MasterMasterSteamPGO`)
- The base game directory
- The current working directory

## Using with Controllers

This mod natively listens for keyboard input. To use it with a controller:

### Controller Support Options

- **[JoyToKey](https://joytokey.net/en/):** Map controller buttons to keys configured in the INI file.
- **[Steam Input](https://store.steampowered.com/controller):** Use Steam's controller configuration to map controller buttons to keys.

Both allow you to bind any controller button to F3 (or whichever key you've chosen to toggle the view).

## View Control Keys

The mod supports three types of key bindings:

1. **Toggle Keys (`ToggleKey`)** – Switch between first-person and third-person views when pressed
2. **First-Person Keys (`FPVKey`)** – Forces first-person view
3. **Third-Person Keys (`TPVKey`)** – Forces third-person view

### Default FPV Keys Explained

The default keys (M, P, I, J, N) correspond to important in-game UI interactions. These automatically switch the view to first-person to avoid UI bugs or broken menu displays in third-person view.

> If you've remapped these keys in your game settings, be sure to update the `FPVKey` list accordingly.

### Empty Key Settings

You can leave any key list empty to disable its feature:

- `ToggleKey =` → disables toggle behavior
- `FPVKey =` → disables forced first-person mode
- `TPVKey =` → disables forced third-person mode

If all are empty, the mod will initialize but not monitor any keys (noop mode).

### Key Codes

Some common virtual key codes:

- F1–F12: `0x70`–`0x7B`
- 1–9: `0x31`–`0x39`
- A–Z: `0x41`–`0x5A`

See the full list: [Microsoft Virtual Key Codes](https://docs.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes)

## Troubleshooting

If you encounter issues:

1. Set `LogLevel = DEBUG` in the INI file
2. Check the log file:
   `<KC:D 2 installation folder>/Bin/Win64MasterMasterSteamPGO/KCD2_TPVToggle.log`
3. After a game update, the mod may stop working. This usually means the AOB pattern no longer matches the updated binary.
   You will need to find the new `AOBPattern` for the latest version of the game.
   > If you know what you're doing, you can try finding the AOB manually using Cheat Engine or a disassembler.

Common issues:

- **Mod doesn't load** – Ensure the files are in the correct location
- **Toggle doesn't work** – The game update may have changed the memory layout, requiring an updated AOB pattern in the INI file
- **Game crashes** – Check the log file for errors; try updating to the latest version
- **Controller doesn’t work** – Ensure JoyToKey or Steam Input is set up properly

> **Still stuck?** [Open a GitHub issue](https://github.com/tkhquang/KDC2Tools/issues/new?assignees=&labels=bug&template=bug_report.yaml) and include your INI config, log output, and game version.

## Known Issues and Limitations

### Camera and View Limitations

- Camera may clip through objects in third-person view (no collision detection)
- Some game events or menus may temporarily be buggy in third-person view (menus, map, dialog...)
  - **Workaround**: Use the default FPV keys (M, P, I, J, N) to automatically switch to first-person view when using these features

### Rare Camera Behavior Issue in Specific Scene

#### Cinematic Sequence Camera Limitations

**Specific Scenario**: During the scene where Hans carries Henry (likely a story-critical moment from the game's opening), switching between first-person and third-person views can cause unexpected camera and character model behavior.

**Detailed Behavior**:

- The scene uses a forced camera perspective with specific positioning
- Switching to third-person view may rotate Henry's body incorrectly
- Returning to first-person view might not restore the original camera positioning

**Impact**: This issue appears to be unique to this specific scripted sequence where the character positioning is tightly controlled by the game.

**Recommended Approach**:

- Keep the game in first-person view during this specific scene
- Avoid toggling camera views until the scene completes
- If you accidentally switch views, you may need to reload the previous save
- **Temporary Solution**: Simply rename `KCD2_TPVToggle.asi` to `KCD2_TPVToggle.bak` or remove it from your game directory

**Note**: This behavior seems limited to this particular story moment and does not represent a widespread mod issue.

### General Limitations

- The third-person camera uses the game's experimental implementation and may not be perfect
- Currently only tested with the Steam version of the game

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for a detailed history of updates.

## Dependencies

This mod requires the [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by [**ThirteenAG**](https://github.com/ThirteenAG).

It enables `.asi` plugin support and loads the mod automatically at game startup.

> **Note:** `dinput8.dll` (ASI Loader) is bundled in the ZIP file. The mod will not work without it.

## Building from Source

### Prerequisites

- [MinGW-w64](https://www.mingw-w64.org/) (GCC/G++)
- Windows SDK headers (for WinAPI access)

### Using Makefile

If `make` is installed, simply run:

```bash
make
```

This will output:

```
build/KCD2_TPVToggle.asi
```

### Manual Compilation (without Makefile)

```bash
g++ -std=c++20 -m64 -O2 -Wall -Wextra \
    -static -static-libgcc -static-libstdc++ \
    -shared src/*.cpp \
    -o build/KCD2_TPVToggle.asi \
    -ldinput8 -luser32 -lkernel32 -lpsapi \
    -Wl,--add-stdcall-alias
```

Ensure that `dinput8.dll` (ASI Loader) and the resulting `.asi` file are placed in the correct game folder.

## Credits

- [ThirteenAG](https://github.com/ThirteenAG) – for the Ultimate ASI Loader
- [Frans 'Otis_Inf' Bouma](https://opm.fransbouma.com/intro.htm) – for his camera tools and inspiration
- Warhorse Studios – for Kingdom Come: Deliverance II

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
