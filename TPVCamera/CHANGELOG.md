# Changelog

All notable changes to the TPVCamera mod will be documented in this file.

## [3.0.0] - Fading Obstacles, See-Through Trees, Camera Rules and Combat Lock-On

- Added the close-up fade: with no room behind Henry, he fades out with his gear, eyes, hair and beard instead of the view jumping to first person (CloseUpFade, CloseUpFadeMinOpacity).
- Added the occluder fade: posts, fences, open carts, lamps, furniture and hanging decorations between the camera and Henry fade out instead of pulling the camera in (UseOccluderFade, OccluderFadeMinOpacity, OccluderFadeBodyWidth, OccluderFadeBodyMargin).
- Added the tree see-through: trees, bushes and tall grass in front of Henry open a dithered window around him, from his hips up (UseTreeSeeThrough and the TreeSeeThrough options).
- Added camera rules: choose by folder or by model which objects stop the camera, fade, open in the window or are left alone, other mods' models included, in KCD2_TPVCamera_rules_User.txt or a mod's own rules file, applied while the game runs.
- Added UseBeamGate (off by default): an obstacle that matches nothing drawn, such as an invisible collision box, stops the camera only when the whole collision probe is blocked, so a thin one with open space beside it no longer pulls the camera in.
- Added Gamepad Orbit Curve (2.0) and Gamepad Orbit Smoothing (0.1 s) to every preset: free-look on a gamepad turns slower for a small right-stick push, for fine aim, still at full speed at full stick, and follows the stick smoothly. 1.0 and 0 give the previous linear, unsmoothed stick.
- Added Orbit Follow Speed to every preset (off by default): riding a horse or a cart with free-look on, the camera turns back behind the mount as it moves, faster the faster it goes; moving free-look stops it for 2 seconds.
- Added NpcLensFade (off by default) and NpcLensFadeMinOpacity: people and animals that come right up to the camera fade with what they carry, eyes and hair included, instead of the camera's near cut slicing them open.
- Added Arm Follow Up and Lens Drop Up to every preset (1 and 0, the camera as before): looking up, the camera swings down around Henry less, or not at all, instead of diving toward the ground behind him, and can be lowered a little instead. Looking level or down is unchanged.
- Added Shoulder Collision to every preset (off by default): a wall beside the shoulder the camera sits over moves the camera toward the middle instead of pulling it in closer, so it keeps its distance.
- Added SwapShoulderKey (unbound by default): moves the camera over the other shoulder, sliding across in 0.3 s, for every preset until you press it again.
- Added ExportRules to write the built-in camera rules to KCD2_TPVCamera_rules.default.txt for reference.
- Added Blend In Seconds to every preset: how long the camera takes to settle into it (aiming 0.3 s, mount and cart 1 s, the rest 0.52 s); presets you already have keep the feel PresetBlendSpeed gave them.
- Added the Indoors state for your own presets: bind one to Indoors, alone or with another state such as Crouch, to use it inside houses, sheds, barns and cellars.
- Added CollisionPullInSeconds (0.12 by default): when something comes between the camera and Henry, the camera eases in over that long instead of jumping in one frame, moving only through open air, so it never passes into a wall; 0 brings back the jump.
- Added lock framing for the game's combat lock-on: the camera swings toward your shoulder side and keeps the locked enemy in view beside Henry instead of behind him (Lock Framing Angle, Lock Focus Height and Lock Focus Offset in every preset).
- Added AutoSaveCyclePick and AutoSaveShoulder (on by default): the preset you cycle to and the shoulder you swap to are kept for your next session; off keeps them for this session only, unless you press Save.
- Added the mod_camera_view console variable: 1 while the camera shows Henry from outside, 0 in first person, so other mods (such as animations meant for third person only) can follow the view.
- Changed the camera to follow Henry's upper body during his own animations (praying, eating, sitting down, standing up, bending): it lowers and leans with him smoothly instead of staying put until his head drops far and then dropping in one go, ignores head bob and nods, and stays put after he turns in place, standing or crouched.
- Changed Eye Height 0 (COMBAT, AIMING, SITTING, KNEEL, LYING, CART) to follow Henry's upper body instead of his bobbing first-person eye, so switching between those presets and the others no longer moves the camera in one jump, and Henry's turn-in-place animations now play with it too.
- Changed the camera to keep its distance and pass through bushes and tree trunks, while buildings, walls, tall fences, plank decks and plank walls, high-sided wagons, tall shelves, haystacks, rocks and the ground still stop it.
- Changed leaves, bare tent beams and prop corners near the lens to thin away instead of filling the view (UseLensCut, on by default).
- Changed the shaders behind the fades and the window to be prepared at loading screens and pauses, never while you play.
- Changed the camera to cost less each frame in busy towns, and to spread its look at the objects around it over several frames, so it no longer adds an occasional hitch of 1 to 2 ms.
- Changed the shaders to be prepared only while UseTreeSeeThrough, UseLensCut or CloseUpFade is on: with UseOccluderFade off too and CollisionPullInSeconds at 0, the camera behaves as in 2.0.0, with this version's fixes, and costs about what it did.
- Changed the aim preview to show for scatter shot too: its ring marks the middle of the spread.
- Changed the INFO log to be quieter; diagnostics moved to DEBUG and TRACE.
- Changed the camera to switch to a situation's preset the moment it starts, such as drawing a bow; only leaving one waits, for StateExitHoldSeconds (was StateSwitchHoldSeconds).
- Changed HeadClearance to only set where the close-up fade happens: HeadClearance 0 no longer turns CloseUpFade off.
- Changed MaxAimCorrection to default to 90, so shots at the ground at Henry's feet and other close targets land on the crosshair (an existing INI keeps its value).
- Changed the first-person/third-person switch with ViewTransitionDuration above 0: the camera now swings out around Henry instead of sliding straight from the eye, stops at walls along the way, keeps his head hidden while it passes through it, eases the FOV with it, and no longer flips halfway when free-look has the camera facing him.
- Changed the mod to build on DetourModKit 4.4.0.
- Changed the combat lock-on in third person to move to the next enemy your camera points at after a kill, only one who is fighting you, instead of ending the fight stance while other enemies are near.
- Changed free-look to come back the moment your opponent dies, and the camera with free-look off to stay put instead of following the finishing animation.
- Changed the overlay's Save button to light up for every unsaved change, UI Scale and Compact precision included.
- Changed COMBAT to look down at Henry a little from straight behind him (Follow Pitch 5, Offset Right 0) and to frame a locked enemy wider (Lock Framing Angle 30); press Reset to factory on COMBAT to get it in an existing setup.
- Changed the overlay to keep the whole panel on screen: the preset settings scroll in their own box, which you can make taller or shorter by dragging its bottom edge, and the Overlay settings come first.
- Removed UseCoverageCollision, CoverageThreshold, CameraProbeSize and UseRenderOcclusion, which the fade replaces.
- Removed PresetBlendSpeed, replaced by each preset's Blend In Seconds.
- Removed UseSphereCollision: the camera's rays alone decide where it stops (the sphere moved the stop by under 6 cm when it changed anything), which saves an engine query on most frames next to a wall.
- Fixed the camera swinging a metre or more in one frame when the game turns Henry's view at the start or end of an action (sitting down, standing up, eating): it now eases after the game's turn, even while you move the mouse, and your own mouse and stick turns stay as direct as before.
- Fixed scatter shot in third person spreading around a spot beside the crosshair; with AimAtCrosshair it now spreads around the crosshair point, as in first person.
- Fixed the inventory showing the sky or a blank view instead of Henry when ViewTransitionDuration is above 0: the switch to first person now runs on real time, not on the game clock the inventory slows down.
- Fixed the camera jiggling when swung up and down next to a market stall, a roof edge or a counter.
- Fixed the camera stopping at the small invisible collision boxes around buildings and props.
- Fixed closed animated doors letting the camera through and hiding Henry behind them.
- Fixed the camera jumping out from a wall when a preset with Enable Collision off becomes active; it now eases out, and only once the switch into that preset has settled.
- Fixed Henry popping between solid and faded in one frame when the camera jumps in or out at a wall; the close-up fade now eases like the obstacle fade.
- Fixed parts of the mod staying active when it cannot start, for example after a game update; it now switches itself fully off.
- Fixed free-look fighting the game's combat lock-on: Henry now turns to the enemy the lock-on picked, so the lock holds instead of dropping.
- Fixed the lock-on switching back to the first enemy after you block an attack from a second one.
- Fixed Henry and the camera jiggling when you turn and press a movement key during a stealth kill.
- Fixed the camera sinking to Henry's feet and looking up (or rising over his head) in a fight after a drop off a raised spot: while the lock-on holds an enemy, the camera's height and tilt now come from where the enemy stands.
- Fixed the overlay showing an ImGui "conflicting ID" error when hovering a preset that is in a cycle group.
- Fixed cutscenes playing in third person: the view now switches to first person as soon as a cutscene starts.

## [2.0.0] - Third-Person Aiming, Switchable Presets and Steadier Free-Look

- Added crosshair aiming in third person: arrows, bolts, handgonne shots and distraction stones land where they would in first person.
- Added an optional aim preview, an optional shot trail, and options for how much each weapon's shots drop.
- Added keys to switch between presets for the same situation (NextPresetKey and PreviousPresetKey, unbound by default).
- Added two presets to switch to: Close Shoulder for everyday play and Aiming Wide for aiming.
- Added a Reset all button to the preset manager that brings back the shipped presets, the new ones included, and keeps your own.
- Improved the preset manager to show which presets share a situation and to let your own preset be an everyday (Default) camera.
- Changed the camera to look the way Henry looks, so the crosshair stays on what you walk toward and zooming moves in a straight line.
- Changed the built-in presets to sit the camera a little closer to Henry's shoulder.
- Changed the aiming camera to wait until a crossbow is loaded (ReloadCountsAsAiming keeps the old timing).
- Changed free-look to turn Henry toward the camera when aiming or drawing a weapon turns it off.
- Changed MaxAimCorrection to default to 45, so shots at close targets are aimed too (an existing INI keeps its value).
- Changed preset switching and Reset zoom to ease the zoom out smoothly.
- Removed the Aim Focus Distance preset setting, no longer needed with crosshair aiming.
- Fixed free-look moving the camera when it turns on or comes back.
- Fixed free-look heading the wrong way on the first step, swinging on a quick tap, and not steering after aiming or crouching.
- Fixed the use prompt flickering between things side by side, and showing away from the crosshair for herbs in free-look.
- Fixed Reset to factory overwriting settings marked as Shared in every preset.
- Fixed the camera distance jumping when a preset's Min Follow Distance is above its Max.

## [1.5.2] - Smoother Turns and Better Game Update Support

- Fixed a brief pose glitch and a forward-and-back slide in crouched turns in third person.
- If a game update breaks one feature, only that feature turns off and the log names it.
- Small stability improvements to the camera hooks and the zoom keys.

## [1.5.1] - Camera Stability

- Updated the mod's support library for improved reliability.
- Improved startup diagnostics when camera recovery is unavailable.
- Updated shared build and editor guidance for working with the camera and highlighting mods.

## [1.5.0] - Turn-in-Place Animations and a Steadier Camera

- Henry now turns on the spot with the game's own turn animations in third person, and the camera stays still while he turns.
- The turn animation is on by default, adjustable in the INI, and switched off automatically in combat, aiming, riding, conversations and minigames.
- The camera no longer ends up inside Henry's head in tight spots such as low doorways, and switches to first person until there is room again.
- Fixed nearby people vanishing in third person, such as someone asleep in a house or standing inside a shop.
- Fixed the camera staying tilted to one side after getting on a horse.
- Smoother camera movement, most noticeable when turning slowly with a controller.
- Removed the short freeze when the world first appears after starting the game.
- Camera collision now costs less performance.
- Better resilience to game updates, with an updated modding toolkit.

## [1.4.0] - Steadier View and Patch Resilience

- Added two options to keep the view steady (Stable Aim Basis and Aim Basis Smoothing), both on by default and adjustable in the INI. These are a best-effort improvement and may still have quirks in some situations
- Strengthened the camera's ability to keep working after game updates
- Updated the bundled modding toolkit (DetourModKit) to v3.9.0 for extra stability and future compatibility

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

[3.0.0]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVCamera-v3.0.0
[2.0.0]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVCamera-v2.0.0
[1.5.2]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVCamera-v1.5.2
[1.5.1]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVCamera-v1.5.1
[1.5.0]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVCamera-v1.5.0
[1.4.0]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVCamera-v1.4.0
[1.3.0]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVCamera-v1.3.0
[1.2.0]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVCamera-v1.2.0
[1.1.0]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVCamera-v1.1.0
[1.0.2]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVCamera-v1.0.2
[1.0.1]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVCamera-v1.0.1
[1.0.0]: https://github.com/tkhquang/KCD2Tools/releases/tag/TPVCamera-v1.0.0
