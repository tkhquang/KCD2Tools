## Third-Person Aiming, Switchable Presets and Steadier Free-Look

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
