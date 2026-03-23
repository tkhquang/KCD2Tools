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

### Linux / Steam Deck (Wine/Proton)

Since this mod uses `dinput8.dll` as the ASI loader, you need to tell Wine/Proton to use the bundled DLL instead of its built-in version. Set the following DLL override:

- **Steam:** Go to the game's **Properties → Launch Options** and add:
  `WINEDLLOVERRIDES="dinput8=n,b" %command%`
- **Command line:** Prepend your launch command with:
  `WINEDLLOVERRIDES="dinput8=n,b"`

## How It Works

This mod uses advanced techniques to integrate with the game:

1. **AOB Pattern Scanning** – Dynamically scans the game's memory for specific byte patterns to locate camera functions and UI module addresses
2. **Memory Hooking** – Uses [SafetyHook](https://github.com/cursey/safetyhook) (via [DetourModKit](https://github.com/tkhquang/DetourModKit)) to intercept game functions for overlay detection and event handling
3. **Input Management** – Uses DetourModKit's input system to handle keyboard, mouse, and gamepad input with support for modifier key combos

## Configuration

The mod is configured via the `KCD2_TPVToggle.ini` file:

```ini
[Settings]
; Toggle between first-person and third-person views
; Supports named keys, modifiers, and combos (e.g., F3, Ctrl+F3, F3,F4)
; See https://github.com/tkhquang/DetourModKit?tab=readme-ov-file#supported-input-names
ToggleKey = F3

; Keys that always switch to first-person view
FPVKey =

; Keys that always switch to third-person view
TPVKey =

; Logging level: DEBUG, INFO, WARNING, ERROR
LogLevel = INFO

; Enable/disable overlay detection and automatic camera switching
EnableOverlayFeature = true

; Custom FOV for third-person view in degrees (empty to disable)
TpvFovDegrees = 68.75
```

The mod looks for the INI file in the following locations:

- The game's executable directory (`Win64MasterMasterSteamPGO`)
- The base game directory
- The current working directory

## Hold-to-Scroll Feature

A new `HoldKeyToScroll` configuration option has been added to provide more granular control over mouse wheel scrolling in third-person view.

### How It Works
- When a specific key is configured, mouse wheel scrolling is disabled by default
- Scrolling is ONLY enabled while holding down the specified key
- If no key is specified, the default overlay detection behavior is used (scrolling disabled during overlays)

### Configuration
In the `KCD2_TPVToggle.ini` file, you can set the hold-to-scroll key using the virtual key code:

```ini
; Hold a key to enable camera distance scrolling
; Examples: Shift, LShift, Ctrl, Alt, Space
HoldKeyToScroll = Shift
```

### Benefits
- Prevents accidental camera distance changes
- Gives you precise control over when scrolling is allowed
- Works seamlessly with existing overlay detection system

## Using with Controllers

DetourModKit v2 supports gamepad input natively via the **XInput** API. You can use gamepad button names directly in the INI file:

```ini
; Example: Toggle view with gamepad Y button
ToggleKey = Gamepad_Y

; Example: Use a modifier combo
ToggleKey = Gamepad_LB+Gamepad_Y

; Example: Multiple independent combos (comma = OR between combos)
; F3 alone OR (hold LB + press Y) — use keyboard and gamepad interchangeably
ToggleKey = F3,Gamepad_LB+Gamepad_Y
```

Supported gamepad inputs include `Gamepad_A`, `Gamepad_B`, `Gamepad_X`, `Gamepad_Y`, `Gamepad_LB`, `Gamepad_RB`, `Gamepad_LT`, `Gamepad_RT`, `Gamepad_Start`, `Gamepad_Back`, `Gamepad_LS`, `Gamepad_RS`, and D-pad directions.

See the full list at the [Supported Input Names](https://github.com/tkhquang/DetourModKit?tab=readme-ov-file#supported-input-names) reference.

> **XInput only:** Xbox controllers work natively. For PS4/PS5/Switch controllers, use [DS4Windows](https://github.com/Ryochan7/DS4Windows), [DualSenseX](https://github.com/Jehan-HENRY/DualSenseX), [BetterJoy](https://github.com/Davidobot/BetterJoy), or Steam Input to present your controller as XInput. See [Gamepad Compatibility](https://github.com/tkhquang/DetourModKit?tab=readme-ov-file#gamepad-compatibility) for details.

Alternatively, you can still use external tools like [JoyToKey](https://joytokey.net/en/) or [Steam Input](https://store.steampowered.com/controller) to map controller buttons to keyboard keys.

## View Control Keys

The mod supports three types of key bindings:

1. **Toggle Keys (`ToggleKey`)** – Switch between first-person and third-person views when pressed
2. **First-Person Keys (`FPVKey`)** – Forces first-person view
3. **Third-Person Keys (`TPVKey`)** – Forces third-person view

### FPV Keys

You can configure FPV keys that correspond to in-game UI interactions (e.g., `M,P,I,J,N` for Map, Perks, Inventory, Journal, Nobility). These automatically switch the view to first-person to avoid UI bugs in third-person view.

> If you use this feature, update the `FPVKey` list to match your in-game key bindings.

### Empty Key Settings

You can leave any key list empty to disable its feature:

- `ToggleKey =` → disables toggle behavior
- `FPVKey =` → disables forced first-person mode
- `TPVKey =` → disables forced third-person mode

If all are empty, the mod will initialize but not monitor any keys (noop mode).

### Key Names

Keys are specified using human-readable names. Common examples:

- Function keys: `F1`–`F24`
- Letters: `A`–`Z`
- Digits: `0`–`9`
- Modifiers: `Ctrl`, `Shift`, `Alt` (or `LCtrl`, `RCtrl`, etc.)
- Numpad: `Numpad0`–`Numpad9`, `NumpadAdd`, `NumpadSubtract`
- Mouse: `Mouse1`, `Mouse2`, `Mouse3`, `Mouse4`, `Mouse5`
- Gamepad: `Gamepad_A`, `Gamepad_B`, `Gamepad_X`, `Gamepad_Y`, etc.

Hex VK codes (e.g., `0x72`) are still supported. See the full list: [Supported Input Names](https://github.com/tkhquang/DetourModKit?tab=readme-ov-file#supported-input-names)

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
- Camera distance may shift unexpectedly in certain game situations. This behavior is inherent to the experimental (debug) third person mode the game provides
- Camera appears slightly tilted when riding a horse

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
- [DetourModKit](https://github.com/tkhquang/DetourModKit) – A lightweight C++ toolkit for game modding (provides SafetyHook, AOB scanning, logging, and configuration management)
- [nlohmann/json](https://github.com/nlohmann/json) – JSON library for modern C++

> **Note:** `dinput8.dll` (ASI Loader) is bundled in the ZIP file. The mod will not work without it.

## Building from Source

### Prerequisites

- [Visual Studio 2022](https://visualstudio.microsoft.com/) (MSVC with C++23 support) or [MinGW-w64](https://www.mingw-w64.org/) (GCC 12+)
- [CMake](https://cmake.org/) (3.25 or newer)
- Windows SDK headers (for WinAPI access)
- Git (to fetch submodules)

### Building with CMake (MSVC - Recommended)

```bash
# Fetch dependencies (including DetourModKit and its submodules)
git submodule update --init --recursive

# Configure and build
cd TPVToggle
cmake -S . -B build/msvc -G "Visual Studio 17 2022" -A x64
cmake --build build/msvc --config Release --parallel
```

### Building with MinGW

```bash
git submodule update --init --recursive
cd TPVToggle
cmake -S . -B build/mingw -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/mingw --config Release --parallel
```

The output binary (`KCD2_TPVToggle.asi`) will be placed in the respective build directory.

## Architecture

This mod is built on top of **DetourModKit**, which provides:
- **SafetyHook** – Safe, modern hooking library for intercepting game functions
- **AOB Scanner** – Pattern scanning with wildcard support for dynamic address resolution
- **Configuration System** – INI file parsing with automatic value assignment
- **Logger** – Thread-safe synchronous and async logging with configurable log levels
- **Memory Utilities** – Safe memory access and manipulation helpers

## Credits

- [ThirteenAG](https://github.com/ThirteenAG) – for the Ultimate ASI Loader
- [cursey](https://github.com/cursey) – for SafetyHook
- [Brodie Thiesfield](https://github.com/brofield) – for SimpleIni
- [nlohmann](https://github.com/nlohmann) – for the JSON library
- [Frans 'Otis_Inf' Bouma](https://opm.fransbouma.com/intro.htm) – for his camera tools and inspiration
- Warhorse Studios – for Kingdom Come: Deliverance II

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
