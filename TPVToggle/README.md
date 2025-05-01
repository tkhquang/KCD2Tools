# Kingdom Come: Deliverance II - Third Person View Toggle

## Overview

**TPVToggle** is an ASI plugin for Kingdom Come: Deliverance II that enables players to toggle between first-person and third-person camera views using customizable hotkeys.

## Features

- Toggle between first-person and third-person views with a keypress (default: F3)
- Dedicated keys for forcing first-person or third-person view
- Automatic switching to first-person view when menus/dialogs open
- Custom FOV (Field of View) setting for third-person mode
- Fully customizable settings via INI configuration

## Installation

1. Download the latest release from [Nexus Mods](https://www.nexusmods.com/kingdomcomedeliverance2/mods/1550) or the [GitHub Releases page](https://github.com/tkhquang/KCD2Tools/releases)
2. Extract all files to your game directory:

   ```
   <KC:D 2 installation folder>/Bin/Win64MasterMasterSteamPGO/
   ```

3. Launch the game and use the configured hotkey (default: F3) to toggle the camera view.

> **Note:** This mod was developed and tested on the Steam version of Kingdom Come: Deliverance II. Other versions (Epic, GOG, etc.) may not be compatible.

## How It Works

This mod uses advanced techniques to integrate with the game:

1. **AOB Pattern Scanning** – Dynamically scans the game's memory for specific byte patterns to locate camera functions and UI module addresses
2. **Memory Hooking** – Uses MinHook to intercept game functions for overlay detection and event handling
3. **Key Monitoring** – Spawns background threads to listen for configured hotkeys and process input events

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

; Enable/disable overlay detection and automatic camera switching
; true = enabled, false = disabled
EnableOverlayFeature = true

; Custom FOV for third-person view in degrees
; Valid range: 0-180, or empty to use default
; Example: TpvFovDegrees = 75.0
; Leave empty to disable FOV modification
TpvFovDegrees = 68.75
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
3. After a game update, the mod may stop working due to memory layout changes.
   The AOB patterns in the code will need to be updated for the new game version.

Common issues:

- **Mod doesn't load** – Ensure the files are in the correct location and ASI Loader is installed
- **Toggle doesn't work** – Check log file for AOB pattern not found errors
- **Game crashes** – Check log file for errors; try updating to the latest version
- **Controller doesn't work** – Ensure JoyToKey or Steam Input is set up properly
- **Scrolling still works in menus** – Check if overlay detection is working, verify MouseHook functionality in logs

> **Still stuck?** [Open a GitHub issue](https://github.com/tkhquang/KCD2Tools/issues/new?assignees=&labels=bug&template=bug_report.yaml) and include your INI config, log output, and game version.

## Known Issues and Limitations

### Camera and View Limitations

- Camera may clip through objects in third-person view (no collision detection)
- Some game events or menus may temporarily be buggy in third-person view (menus, map, dialog...)
  - **Workaround**: Use the default FPV keys (M, P, I, J, N) to automatically switch to first-person view when using these features

### Rare Camera Behavior Issue in Specific Scene

**Specific Scenario**: During the scene where Hans carries Henry (likely a story-critical moment from the game's opening), switching between first-person and third-person views can cause unexpected camera and character model behavior.

**Impact**: This issue appears to be unique to this specific scripted sequence where the character positioning is tightly controlled by the game.

**Recommended Approach**:
- Keep the game in first-person view during this specific scene
- Avoid toggling camera views until the scene completes
- If you accidentally switch views, you may need to reload the previous save
- **Temporary Solution**: Simply rename `KCD2_TPVToggle.asi` to `KCD2_TPVToggle.bak` or remove it from your game directory

### General Limitations

- The third-person camera uses the game's experimental (debug) implementation and may not be perfect
- Currently only tested with the Steam version of the game

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for a detailed history of updates.

## Dependencies

This mod requires:
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by [**ThirteenAG**](https://github.com/ThirteenAG)
- [MinHook](https://github.com/TsudaKageyu/minhook) hooking library

> **Note:** `dinput8.dll` (ASI Loader) is bundled in the ZIP file. The mod will not work without it.

## Building from Source

### Prerequisites

- [MinGW-w64](https://www.mingw-w64.org/) (GCC/G++)
- Windows SDK headers (for WinAPI access)
- Git (to fetch submodules)

### Building with Makefile

```bash
# Fetch dependencies
git submodule update --init --recursive

# Build
make

# Create distribution package
make install
```

This will output:
```
build/
├── KCD2_TPVToggle.asi     # The mod itself
├── KCD2_TPVToggle.ini     # Configuration file
├── dinput8.dll            # ASI Loader
├── README_MOD.md          # Documentation
└── THIRD-PARTY-LICENSES.txt
```

### Manual Compilation

If make is not available:

```bash
# Fetch dependencies first
git submodule update --init --recursive

# Configure C++ compiler flags
CXXFLAGS="-std=c++17 -m64 -Os -Wall -Wextra \
          -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 \
          -I./external/minhook/include \
          -I./external/simpleini \
          -I./src"

# Compile MinHook
g++ $CXXFLAGS -c external/minhook/src/*.c -o obj/*.o

# Compile the mod
g++ $CXXFLAGS -static -shared \
    src/*.cpp obj/*.o external/minhook/src/hde/*.c \
    -lpsapi -luser32 -lkernel32 \
    -o build/KCD2_TPVToggle.asi

# Assemble the assembly file
g++ $CXXFLAGS -c src/asm/overlay_hook.S -o obj/overlay_hook.o
```

## Credits

- [ThirteenAG](https://github.com/ThirteenAG) – for the Ultimate ASI Loader
- [TsudaKageyu](https://github.com/TsudaKageyu) - for MinHook
- [Brodie Thiesfield](https://github.com/brofield) - for SimpleIni
- [Frans 'Otis_Inf' Bouma](https://opm.fransbouma.com/intro.htm) – for his camera tools and inspiration
- Warhorse Studios – for Kingdom Come: Deliverance II

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
