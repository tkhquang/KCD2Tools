# Changelog

All notable changes to the TPVToggle mod will be documented in this file.

## [0.3.0] - Experimental Overlay Detection and Project Structure Improvements


### Added
- [EXPERIMENTAL] Automatic view management for game menus and overlays
- Automatically switch to first-person view when menus open
- Preserve camera state during menu interactions

### Project Structure
- Moved all source and header files to the `src/` folder
- Updated Makefile and GitHub Actions to use the new structure
- Adjusted version updater script to read `src/version.h`
- Updated README and build instructions

### Limitations
- Pre-release experimental feature
- May not work perfectly with all game interactions
- Relies on specific game memory patterns that could change with updates

## [0.2.3] - AOB Scanner and Workflow Improvements


- Added wildcard support in AOB scanning ("??" and "?" characters)
- Improved error handling and pattern matching for memory scanning
- Enhanced release workflow with automated changelog management

## [0.2.2] - Infrastructure Update

- Improved project structure and release workflow
  - Added centralized version management through version.h
  - Created separate CHANGELOG.md for better version history tracking
  - Implemented automated version bumping and changelog updates
  - Enhanced build and release process through GitHub Actions

## [0.2.1] - Stability Update

- Improved error handling for edge cases
  - Added proper handling for empty key configurations
  - Fixed potential crashes when no keys are defined
  - Enhanced memory safety for toggle operations

- Enhanced logging and diagnostics
  - Better error messaging for configuration issues
  - More detailed debugging information
  - Clearer logging for key monitoring

## [0.2.0] - Feature Update

- Added dedicated first-person view (FPV) keybindings
  - Added `FPVKey` option in the INI file to specify keys that always switch to first-person view
  - Default FPV keys: M, P, I, J, N (0x4D, 0x50, 0x49, 0x4A, 0x4E)
  - FPV keys will always switch to first-person view when pressed, regardless of current state

- Added dedicated third-person view (TPV) keybindings
  - Added `TPVKey` option in the INI file to specify keys that always switch to third-person view
  - No default TPV keys are assigned
  - TPV keys will always switch to third-person view when pressed, regardless of current state

- Improved logging for key monitoring
  - Added detailed logging of which keys are being monitored for each function
  - Added clearer action logging that distinguishes between toggle, FPV, and TPV key presses

- Code improvements
  - Added better error handling for key parsing
  - Improved configuration file loading with better fallbacks and validation
  - Enhanced memory safety when setting view states

## [0.1.2]

- Added support for multiple INI file locations - the mod now checks several paths for the configuration file, including the base game directory
- Included Ultimate ASI Loader (dinput8.dll) in the package for easier installation

## [0.1.1]

- Fixed missing dependency issues with static linking

## [0.1.0] - Initial Release

- Basic third-person view toggle functionality
- Configurable hotkeys through INI file
- Memory scanning to find the view state dynamically
- Exception-based hooking for minimal game modification
- Logging system for troubleshooting
KCD
[0.3.0]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVToggle-v0.3.0
[0.2.3]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVToggle-v0.2.3
[0.2.2]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVToggle-v0.2.2
[0.2.1]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVToggle-v0.2.1
[0.2.0]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVToggle-v0.2.0
[0.1.2]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVToggle-v0.1.2
[0.1.1]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVToggle-v0.1.1
[0.1.0]: https://github.com/tkhquang/KDC2Tools/releases/tag/TPVToggle-v0.1.0
