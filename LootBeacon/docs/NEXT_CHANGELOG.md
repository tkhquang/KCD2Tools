# Changelog

## Version 1.4.1

- Added expanded color palette for particle effects (12 colors total)
  - New colors: Yellow, Cyan, Magenta, Orange, Purple, White, Light Blue, Pink, Lime, Teal
  - Improves accessibility for users with color vision deficiencies
- Changed default color scheme:
  - Items: Orange (previously Red)
  - Human Corpses: Cyan (previously Green)
  - Animal Corpses: Blue (unchanged)
- Updated configuration file with new color options
- Added detailed color documentation to help files
- Adjust the particle effects: thinner, more consistent

## Version 1.4.0

- Added "Treat Unconscious as Dead" configuration option
- Enhanced Good Citizen Mode to exclude illegal corpses
- Improved entity detection and filtering logic
- Comprehensive codebase restructuring:
  - Improved module organization
  - Enhanced code modularity
  - Simplified inter-module communication

## Version 1.3.0

- Added Good Citizen Mode (will skip highlighting items that require stealing)
- Added German translation

## Version 1.2.2

- Added support for custom entity classes with configurable options:
  - Highlight bird nests, herbs, mushrooms, and other special entity types
  - Configure custom entity classes via comma-separated list in `mod.cfg`
  - Customize particle effect color for custom entities separately
  - Custom entities count toward item totals in notifications
- Added comprehensive debug logging for entity detection

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
