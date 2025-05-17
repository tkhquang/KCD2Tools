## Improved UI Overlay Detection

- Replaced polling-based overlay detection with direct hooks into the game's UI functions
- Added direct hooks for UI overlay show/hide events
- Removed legacy overlay polling thread
- More responsive and reliable camera switching when menus appear
- UI Menu hooks to detect in-game menu (pause) state
- Automatic camera input suppression when game menu is active
