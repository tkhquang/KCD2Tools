# Changelog

## Version 1.2.1

- Added full configuration support for Steam Workshop version
- Users can now customize mod settings by creating a `mod.cfg` file in `C:\KCD2Mods\LootBeacon\` or `D:\KCD2Mods\LootBeacon\`
- Simplified mod installation and configuration instructions
- Removed version-specific restrictions on customization

## Version 1.2.0

- Added Italian translation (thanks to Nevyn77)
- Fixed Vortex installation and mod.cfg parsing issues (thanks to c0rish)
- Improved configuration parsing with better error handling

## Version 1.1.0

- Added detection and highlighting for human corpses and animal carcasses
- Implemented color-coded particles for different entity types:
  - Red: Pickable items
  - Green: Human corpses
  - Blue: Animal carcasses
- Added randomized particle angles for better visual distinction
- Added individual toggles for each entity type
- Updated notification system with separate messages by entity type
- Improved entity detection with direct actor property checks
- Optimized scanning with a single comprehensive entity pass

## Version 1.0.0 (Initial Release)

- Created main detection system for pickable items
- Implemented particle effect highlighting with three color options
- Added configurable detection radius and highlight duration
- Implemented auto-removal of highlights after set duration
- Added game pause handler for safety
- Created configuration system with `mod.cfg` support
- Implemented comprehensive error handling
