# Changelog

All notable changes to the TPVCamera mod will be documented in this file.

## [1.3.0] - [Title for next release]

- The ASI loader is no longer bundled: install Ultimate ASI Loader yourself once (direct download links are in the readme and on the mod page), which keeps the download lean and always up to date
- Free-look look sensitivity can now be set separately for horizontal and vertical, for both mouse and gamepad, and either axis can be inverted
- Removed the gamepad free-look minimum-speed limit, so its speed can be set to any value (including inverted)
- Added a Shared tickbox in the preset manager: turn it on for a setting to apply that value to every preset at once
- Fixed the preset manager showing unsaved changes when you only switched presets or set a value back to what it already was
- Added an optional see-through camera collision (Use Coverage Collision, off by default): when turned on, the view pulls in only when something actually hides your character, so thin posts, rails and fences you can see past no longer jerk the camera, and it keeps out of corners
- The camera no longer buries itself in tent, awning or canopy cloth on a steep look-down (on by default, with its own separate switch)
- Updated the bundled modding toolkit (DetourModKit) to the latest version (v3.8.2)
- Fixed the preset overlay looking blurry or misaligned on displays scaled above 100%
- Saved camera presets can no longer be lost if the game closes while they are being written
- The third-person camera now automatically adapts to many game updates that move data around internally, so a common patch no longer breaks it before the mod is updated
- Stays safe to load alongside other mods, and falls back to its previous behaviour if a game update changes too much
- Various under-the-hood stability and reliability fixes
- Standardised the source-code formatting and added automatic style enforcement for future development
- Cleaned up developer comments and documentation for consistency, with no change to in-game behaviour

## [1.2.0] - GOG Support and Fixes

- Fixed the controller zoom (hold LB + D-pad up/down) sometimes opening the inventory or map when you finish zooming
- Updated the bundled modding toolkit to the latest version
- Added support for the GOG version of the game
- Made the camera zoom in and out in bigger steps so it moves faster per press

## [1.1.0] - Fixes and Improvements

- Added an optional hold-to-look key for the free-look orbit: hold it to swing the camera around, release to snap back to the aiming view
- Fixed a startup issue that could stop the third-person camera from loading when other mods or graphics overlays were running

## [1.0.2] - Fixes and Improvements

- Free-look orbit no longer turns off during the hole-digging minigame
- The third-person camera no longer slips through fabric tent, awning, and stall roofs
- Made the mod more likely to keep working after a game update

## [1.0.1] - Free-Look Orbit Fixes

- Fixed the free-look orbit camera sometimes spinning on its own and not stopping until you opened a menu
- Fixed your character sometimes turning on its own after combat while free-look was on

## [1.0.0] - Initial Release

- First release of TPVCamera, a third-person camera mod for Kingdom Come: Deliverance II
- Switch between first and third person with F3 (or hold LB + RB); third person is on by default, or set AutoEnableTPV = false in the INI to start in first person
- Zoom in and out with LShift+PageUp / PageDown (or hold LB + D-pad up/down); the overlay shows the live follow distance with a reset button
- The camera automatically pulls in so it does not clip through walls
- Optional auto aim-focus eases the crosshair onto your target's real distance, and camera-space interaction lets you loot and use whatever the crosshair is on at any range, even point-blank
- Basic free-look orbit (F4, or hold LB + click the left stick) to look around your character; while it is on, movement follows the camera in every direction
- Optionally pause free-look in chosen situations such as dialogue or minigames, and you can still turn it on by hand even where it normally turns off
- Optionally switch the view automatically for combat, aiming, dialogue, minigames, riding, or menus, then restore it afterwards
- In-game preset manager (press Home): edit, add, and save camera presets and watch the changes live
- Built-in presets for normal play, combat, aiming, horseback, sneaking, and special poses apply automatically, and you can bind your own to several states at once (the most specific match wins)
- Third person stays out of every minigame (lockpicking, reading, alchemy, pickpocketing, and more), and you can give any minigame its own framing
- Configurable hotkeys, with most changes applying while you play

[1.3.0]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVCamera-v1.3.0
[1.2.0]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVCamera-v1.2.0
[1.1.0]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVCamera-v1.1.0
[1.0.2]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVCamera-v1.0.2
[1.0.1]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVCamera-v1.0.1
[1.0.0]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVCamera-v1.0.0
