## [Title for next release]

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
