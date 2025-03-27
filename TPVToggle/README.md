# Kingdom Come: Deliverance II - Third Person View Toggle

## Overview

TPVToggle (v0.1.0) is a lightweight ASI plugin for Kingdom Come: Deliverance II that enables players to toggle between first-person and third-person camera views with customizable hotkeys.

## Features

- Toggle between first-person and third-person views with configurable hotkeys (default: F3)
- Minimal performance impact through efficient memory scanning
- Detailed logging for troubleshooting

## Installation

1. Install [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader):
   - Download the latest release from the [Ultimate ASI Loader GitHub repository](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases)
   - Extract `dinput8.dll` to your game directory:

     ```
     <KC:D 2 installation folder>/Bin/Win64MasterMasterSteamPGO/
     ```

2. Install TPVToggle:
   - Download the latest release from [Nexus Mods](https://www.nexusmods.com/kingdomcomedeliverance2/mods/1550) or the [Releases page](https://github.com/tkhquang/KDC2Tools/releases)
   - Extract the following files to your game directory:

     ```
     <KC:D 2 installation folder>/Bin/Win64MasterMasterSteamPGO/KCD2_TPVToggle.asi
     <KC:D 2 installation folder>/Bin/Win64MasterMasterSteamPGO/KCD2_TPVToggle.ini
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
ToggleKey = 0x72

; Logging level: DEBUG, INFO, WARNING, ERROR
LogLevel = INFO

; AOB Pattern (advanced users only - update if mod stops working after game patches)
AOBPattern = 48 8B 8F 58 0A 00 00
```

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

- **Mod doesn't load**: Ensure the Ultimate ASI Loader is installed correctly
- **Toggle doesn't work**: The game update may have changed the memory layout, requiring an AOB pattern update
- **Game crashes**: Check the log file for details; consider updating to the latest mod version

## Known Issues and Limitations

- Camera may clip through objects in third-person view (no collision detection)
- Some game events or menus may temporarily be buggy
- The third-person camera uses the game's experimental implementation and may not be perfect

## Changelog

<details>
<summary>Click to expand</summary>

### v0.1.0 (Initial Release)

- Basic third-person view toggle functionality
- Configurable hotkeys through INI file
- Memory scanning to find the view state dynamically
- Exception-based hooking for minimal game modification
- Logging system for troubleshooting

</details>

## Building from Source

### Prerequisites

- GCC/MinGW compiler
- Windows SDK (for WinAPI functions)

### Compilation

```bash
g++ -shared -o build/KCD2_TPVToggle.asi dllmain.cpp logger.cpp config.cpp toggle_thread.cpp aob_scanner.cpp exception_handler.cpp -static-libgcc -static-libstdc++ -lpsapi -Wl,--add-stdcall-alias -O2 -Wall -Wextra
```

## Credits

- [ThirteenAG](https://github.com/ThirteenAG) for the Ultimate ASI Loader
- [Frans 'Otis_Inf' Bouma](https://opm.fransbouma.com/intro.htm) for his camera tools and inspiration - without his ideas, this mod wouldn't have been possible
- Warhorse Studios for Kingdom Come: Deliverance II

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
