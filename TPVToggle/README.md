# Kingdom Come: Deliverance II - TPV Toggle & Camera Utils

## Overview

**TPVToggle** is an ASI plugin for Kingdom Come: Deliverance II that enables players to:
- Toggle between first-person and third-person views using hotkeys.
- Adjust various third-person camera parameters (distance, height, pitch, etc.) using hotkeys.

## Features

- Toggle between first-person (FPV) and third-person (TPV) views (default: F3)
- Force FPV or TPV using dedicated keys
- Adjust TPV Camera:
    - Distance (Zoom In/Out)
    - Height (Up/Down)
    - Pitch (Tilt Up/Down)
    * Side Offset (Shift Left/Right)
    * FOV (Field of View) - *Experimental*
    * Z/X Offset - *Experimental*
- Reset camera offsets to defaults
- Automatic switch to FPV when game menus/overlays are active (prevents UI bugs)
- Fully customizable key bindings and adjustment steps via INI configuration
- Open-source with MinGW compatibility

## Installation

1.  **Install ASI Loader:** If you haven't already, download and install [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases). Place its `dinput8.dll` (or `wininet.dll`) into your game's main executable directory:
    ```
    <KC:D 2 installation folder>/Bin/Win64MasterMasterSteamPGO/
    ```
2.  **Download TPVToggle:** Download the latest release from [Nexus Mods](https://www.nexusmods.com/kingdomcomedeliverance2/mods/1550) or the [GitHub Releases page](https://github.com/tkhquang/KDC2Tools/releases).
3.  **Extract Files:** Extract all files from the TPVToggle release archive (`KCD2_TPVToggle.asi`, `KCD2_TPVToggle.ini`, `README.txt`, etc.) into the *same game directory* where you placed the ASI Loader:
    ```
    <KC:D 2 installation folder>/Bin/Win64MasterMasterSteamPGO/
    ```
4.  **Configure (Optional):** Edit `KCD2_TPVToggle.ini` to customize hotkeys and camera settings (see Configuration section).
5.  **Launch Game:** Start the game. The ASI Loader will automatically load the mod. Use the configured hotkeys to control the camera.

> **Note:** Developed and tested on the Steam version. Other versions (Epic, GOG) may work but are not guaranteed.

## How It Works

1.  **Memory Scanning:** Finds memory patterns (AOBs) related to the view state flag and camera parameter modification code.
2.  **Hooking:** Uses MinHook to place small assembly detours at specific game code locations.
    *   One hook captures the `r9` register needed to access the FPV/TPV flag byte.
    *   Another hook captures the `rbx` register used by the game to detect overlays (menus, map, dialogue, etc.).
    *   A third hook captures the `rbx` register when the game writes camera parameters (like distance), providing a base pointer for modifications.
3.  **Key Monitoring:** A background thread monitors keyboard input for configured hotkeys.
4.  **Interaction:** Based on key presses and game state (overlay active/inactive), the thread:
    *   Toggles the view state flag (byte at `[r9+0x38]`).
    *   Forces FPV when an overlay is detected. Restores previous view (TPV/FPV) when overlay closes.
    *   Adjusts camera parameters (floats at `[rbx+Offset]`, e.g., `[rbx+0x20]` for distance) based on camera hotkey presses.

This approach uses minimal, targeted hooks and assembly detours that execute original game code bytes, significantly improving stability compared to older methods.

## Configuration (`KCD2_TPVToggle.ini`)

```ini
[Settings]
; --- Basic View Control ---
; ToggleKey: Keys to toggle FPV/TPV. Comma-separated hex VK codes. 0x optional.
ToggleKey = 0x72 ; F3

; FPVKey: Keys to FORCE first-person view. Useful for menu keys. Comma-separated hex.
; Default game menus: M=4D, P=50, I=49, J=4A, N=4E. Update if you rebind!
FPVKey = 0x4D, 0x50, 0x49, 0x4A, 0x4E

; TPVKey: Keys to FORCE third-person view. Comma-separated hex.
TPVKey =

; --- Logging & Advanced ---
LogLevel = INFO ; DEBUG, INFO, WARNING, ERROR
AOBPattern = 48 8B 8F 58 ... ; Pattern for TPV flag code. Don't change unless needed.

; =============================================================
;                   Camera Adjustment Hotkeys
; =============================================================
; Keys to adjust camera settings IN THIRD-PERSON VIEW ONLY. Ignored in FPV or menus.
[CameraHotkeys]
; --- Camera Distance (Zoom) --- (Default: -2.5) +Val = Farther
IncreaseDistance = ADD     ; Numpad +
DecreaseDistance = SUBTRACT ; Numpad -
; --- Camera Height --- (Default: -0.1) -Val = Higher
IncreaseHeight = NEXT   ; Page Down (Increases offset value -> lowers camera)
DecreaseHeight = PRIOR  ; Page Up   (Decreases offset value -> raises camera)
; --- Camera Pitch (Vertical Angle) --- (Default: ~0.95) +Val = Tilt Up
IncreasePitch = NUMPAD8
DecreasePitch = NUMPAD2
; --- Camera Side Offset (Horizontal Position) --- (Default: 0.0) +Val = Right, -Val = Left
IncreaseSideOffset = NUMPAD6
DecreaseSideOffset = NUMPAD4
; --- Camera Forward/Backward/Height? (Z/X Combined Offset) --- (Default: 0.0) *Experimental*
IncreaseZXOffset =
DecreaseZXOffset =
; --- Camera Field of View (FOV) --- (Default: ~60?) *Experimental*
IncreaseFOV =
DecreaseFOV =
; --- Reset ---
ResetOffsets = END ; Key to reset ALL above camera offsets to game defaults.

; =============================================================
;                    Camera Adjustment Settings
; =============================================================
; How much each parameter changes per key press. MUST be positive numbers.
[CameraSettings]
DistanceStep = 0.25
HeightStep = 0.05
PitchStep = 0.05
SideOffsetStep = 0.05
ZXOffsetStep = 0.05
FovStep = 1.0
```

### Key Binding Notes

-   **Empty Lists:** Leave a key setting empty (e.g., `ToggleKey =`) to disable those specific keys. If all `ToggleKey`, `FPVKey`, `TPVKey`, and all `[CameraHotkeys]` are empty, hotkey monitoring is fully disabled.
-   **Virtual Key Codes:** Use hexadecimal VK codes. Common codes:
    -   F1-F12: `0x70`-`0x7B`
    -   0-9: `0x30`-`0x39`
    -   A-Z: `0x41`-`0x5A`
    -   Numpad 0-9: `0x60`-`0x69`
    -   Numpad /*-+.: `0x6A`-`0x6E`
    -   Arrows: Left=`0x25`, Up=`0x26`, Right=`0x27`, Down=`0x28`
    -   PageUp=`0x21` (PRIOR), PageDown=`0x22` (NEXT), End=`0x23`, Home=`0x24`
    -   Shift=`0x10`, Ctrl=`0x11`, Alt=`0x12`
    -   See full list: [Microsoft Virtual Key Codes](https://docs.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes)

## Using with Controllers

This mod listens for keyboard input. Use tools like **JoyToKey**, **reWASD**, or **Steam Input** to map controller buttons to the keyboard keys configured in the INI file.

## Troubleshooting

1.  **Mod Not Loading?** Ensure `dinput8.dll` (or other ASI loader) is installed correctly in `Bin/Win64MasterMasterSteamPGO/` along with `KCD2_TPVToggle.asi` and `.ini`. Check the ASI loader's log file if it creates one.
2.  **Keys Not Working?**
    *   Verify keys/codes in the INI file (`[Settings]`, `[CameraHotkeys]`). Check `KCD2_TPVToggle.log` (set `LogLevel = DEBUG` in INI first).
    *   Remember camera adjustment keys only work in TPV and when *not* in a menu/overlay.
    *   If using controller mapping software, ensure it's running and correctly mapping buttons to keys.
3.  **Toggle Fails After Game Update?** The `AOBPattern` in the INI likely needs updating. Check mod pages/forums for updated patterns or learn to find them using Cheat Engine. The Overlay and Camera parameter patterns are currently hardcoded but might also need updates in the future (requiring a mod update).
4.  **Crashes?** Check `KCD2_TPVToggle.log` for ERROR messages. Ensure you have the latest version of the mod. Report issues on GitHub with log file contents.

> **Still stuck?** [Open a GitHub issue](https://github.com/tkhquang/KDC2Tools/issues/new?assignees=&labels=bug&template=bug_report.yaml) including INI, log output, game version, and detailed steps.

## Known Issues and Limitations

-   **Clipping:** Standard TPV clipping issues (camera going through walls) may occur. The mod doesn't add custom collision.
-   **Menus/Overlays:** Opening menus (Inv, Map, Dialogues etc.) automatically forces FPV to prevent UI bugs. Your previous view state (TPV/FPV) is restored when the menu closes. ***Note:*** Camera parameter *adjustments* might be reset or behave unexpectedly when returning from menus; manual readjustment might be needed. The previous automatic distance restoration was removed due to instability.
-   **Scripted Scenes:** Certain highly scripted scenes (like the opening) may have camera issues if you toggle views. It's best to stay in the intended view (usually FPV) during such sequences.
-   **Experimental Parameters:** FOV and Z/X offset adjustments are less tested; their exact effects and limits might vary.

## Changelog

See [CHANGELOG.md](CHANGELOG.md).

## Dependencies

-   [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by ThirteenAG (included/required).
-   [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu (included, built from source).

## Building from Source

(Build instructions remain largely the same as provided previously - using MinGW and make)

## Credits

-   [ThirteenAG](https://github.com/ThirteenAG) (ASI Loader)
-   [Tsuda Kageyu](https://github.com/TsudaKageyu) (MinHook)
-   [Frans 'Otis_Inf' Bouma](https://opm.fransbouma.com/intro.htm) (Inspiration)
-   [Warhorse Studios](https://warhorsestudios.com/) (The Game!)

## License

MIT License - see [LICENSE](LICENSE).
