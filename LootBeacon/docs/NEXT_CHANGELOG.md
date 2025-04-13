# Changelog

## Version 1.2.0

- Added Italian translation (thanks to Nevyn77)
- Fixed Vortex installation and mod.cfg parsing issues (thanks to c0rish)
- Improved configuration parsing with better error handling
- Enhanced logging system with configurable verbosity levels
- Fixed issue with some pickable items not being properly detected
- Improved randomized particle angles for better visual distinction
- Optimized code for better performance

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
