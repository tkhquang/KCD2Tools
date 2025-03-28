# Kingdom Come: Deliverance II - Third Person View Toggle

## Overview

TPVToggle is an ASI plugin for Kingdom Come: Deliverance II that enables players to toggle between first-person and third-person camera views with customizable hotkeys.

## Features

- Toggle between first-person and third-person views with configurable hotkeys (default: F3)
- Minimal performance impact through efficient memory scanning
- Detailed logging for troubleshooting
- Flexible configuration file search to work with different game setups

## Installation

1. Download the latest release from [Nexus Mods](https://www.nexusmods.com/kingdomcomedeliverance2/mods/1550) or the [Releases page](https://github.com/tkhquang/KDC2Tools/releases)
2. Extract all files to your game directory:

   ```
   <KC:D 2 installation folder>/Bin/Win64MasterMasterSteamPGO/
   ```

3. Launch the game and use the configured hotkey (default: F3) to toggle the camera view

> **Note:** This mod was developed and tested on the Steam version of Kingdom Come: Deliverance II. Other versions (Epic, GOG, etc.) may not work with this mod.

## How It Works

This mod uses this approach to enable third-person view:

1. **Memory Scanning**: Scans the game's memory for a specific byte pattern that accesses the camera view state
2. **Exception Handling**: Uses a minimal INT3 hook to capture the register (r9) that contains the pointer to the view state
3. **Key Monitoring**: Creates a separate thread to monitor for configured hotkeys
4. **View Toggling**: Safely toggles the camera view byte between 0 (first-person) and 1 (third-person)

This implementation minimizes modifications to the game code, improving stability and compatibility with game updates.

## Configuration

The mod can be configured by editing the `KCD2_TPVToggle.ini` file:

```ini
[Settings]
; Keys that toggle the third-person view (comma-separated, in hex)
; F3 = 0x72, F4 = 0x73, E = 0x45, etc.
; If set to empty (ToggleKey = ), no toggle keys will be monitored.
ToggleKey = 0x72

; First-person view keys (comma-separated, in hex)
; Will always switch to first-person view when pressed
; Default keys are game menu keys that benefit from first-person view
; If set to empty (FPVKey = ), no first-person view keys will be monitored.
; 0x4D,0x50,0x49,0x4A,0x4E = M, P, I, J, N
FPVKey = 0x4D,0x50,0x49,0x4A,0x4E

; Third-person view keys (comma-separated, in hex)
; Will always switch to third-person view when pressed
; If set to empty (TPVKey = ), no third-person view keys will be monitored.
TPVKey =

; Logging level: DEBUG, INFO, WARNING, ERROR
LogLevel = INFO

; AOB Pattern (advanced users only - update if mod stops working after game patches)
AOBPattern = 48 8B 8F 58 0A 00 00
```

The mod will search for the INI file in several locations:

- The game's executable directory (`Win64MasterMasterSteamPGO`)
- The base game directory
- The current working directory

### View Control Keys

The mod supports three types of key bindings:

1. **Toggle Keys** (`ToggleKey`): Switches between first-person and third-person views each time pressed
2. **First-Person Keys** (`FPVKey`): Always switches to first-person view when pressed
3. **Third-Person Keys** (`TPVKey`): Always switches to third-person view when pressed

#### Default FPV Keys Explained

The default FPV keys (M, P, I, J, N) were specifically chosen because they correspond to important in-game UI interactions. These keys automatically switch the view to first-person when pressed because many game UI elements and menus can appear buggy or non-functional when accessed in third-person view. This provides a seamless experience where pressing any UI key will automatically ensure you're in the correct camera mode for that interface.

If you've remapped these keys in your game settings, you may want to update the FPV key settings to match your custom keybinds.

#### Empty Key Settings

For each key type, you can leave the setting empty to disable that functionality:

- Setting `ToggleKey =` will disable the toggle feature (no keys will toggle the view)
- Setting `FPVKey =` will disable automatic switching to first-person view
- Setting `TPVKey =` will disable explicit switching to third-person view

If all three key settings are empty, the mod will still initialize but won't monitor any keys (operating in a noop state).

### Key Codes

Common key codes:

- F1-F12: 0x70-0x7B
- 1-9: 0x31-0x39
- A-Z: 0x41-0x5A

For a complete list, see [Microsoft's Virtual Key Codes](https://docs.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes).

## Troubleshooting

If you encounter issues:

1. Set `LogLevel = DEBUG` in the INI file
2. Check the log file: `<KC:D 2 installation folder>/Bin/Win64MasterMasterSteamPGO/KCD2_TPVToggle.log`
3. After a game update, you may need to update the `AOBPattern` in the INI file

Common issues:

- **Mod doesn't load**: Ensure the files are in the correct location
- **Toggle doesn't work**: The game update may have changed the memory layout, requiring an AOB pattern update
- **Game crashes**: Check the log file for details; consider updating to the latest mod version

## Known Issues and Limitations

- Camera may clip through objects in third-person view (no collision detection)
- Some game events or menus may temporarily be buggy in third-person view (menus, map, dialog...)
  - **Workaround**: Use the default FPV keys (M, P, I, J, N) to automatically switch to first-person view when using these features. This is why these keys are configured by default.
- The third-person camera uses the game's experimental implementation and may not be perfect
- Currently only tested with the Steam version of the game

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for a detailed history of changes.

## Dependencies

This mod relies critically on the [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by ThirteenAG. This essential component:

- Allows the mod to be injected into the game as an ASI plugin
- Handles the loading and execution of the mod when the game starts
- Provides a standardized way to modify the game without changing game files
- Makes installation simple by just placing files in the game directory

**Note:** The Ultimate ASI Loader (`dinput8.dll`) is included in the download package. This is a crucial component and the mod will not function without it.

## Building from Source

### Prerequisites

- GCC/MinGW compiler
- Windows SDK (for WinAPI functions)

### Compilation

```bash
g++ -shared -o build/KCD2_TPVToggle.asi dllmain.cpp logger.cpp config.cpp toggle_thread.cpp aob_scanner.cpp exception_handler.cpp version.cpp -static -lpsapi "-Wl,--add-stdcall-alias" -O2 -Wall -Wextra
```

## Credits

- [ThirteenAG](https://github.com/ThirteenAG) for the Ultimate ASI Loader, without which this mod would not be possible
- [Frans 'Otis_Inf' Bouma](https://opm.fransbouma.com/intro.htm) for his camera tools and inspiration
- Warhorse Studios for Kingdom Come: Deliverance II

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
