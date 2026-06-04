# Changelog

All notable changes to the TPVCamera mod will be documented in this file.

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

[1.0.1]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVCamera-v1.0.1
[1.0.0]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVCamera-v1.0.0
