# Loot Beacon - Never Miss a Drop or Corpse

## Overview

**Loot Beacon** is a Lua mod for Kingdom Come: Deliverance II that helps players spot lootable objects in the game world. Activate the highlight feature with a keypress and watch as nearby items, human corpses, and animal carcasses are highlighted with colorful particle effects.

## Features

- Instantly highlight nearby lootable objects with color-coded beacons (default: F4 key)
  - **Red**: Pickable items and custom entity classes
  - **Green**: Human corpses
  - **Blue**: Animal carcasses
- Toggle highlighting for each object type individually
- Custom entity class support (highlight bird nests, herbs, and more)
- Configurable detection radius and highlight duration
- On-screen notifications showing what was found
- Full localization support (English and Italian)
- Fully customizable through configuration file

## Installation

1. **Download the mod** from [Nexus Mods](https://www.nexusmods.com/kingdomcomedeliverance2/mods/1722)
2. **Extract the contents** to your Kingdom Come: Deliverance 2 Mods folder:
   ```
   <KC:D 2 installation folder>/Mods/
   ```
   If the Mods folder doesn't exist, create it.
3. **Verify the folder structure** looks like this:
   ```
   KingdomComeDeliverance2/
   └── Mods/
       └── LootBeacon/
           ├── Data/
           ├── Localization/
           └── mod.cfg
           └── mod.manifest
   ```
4. **Launch the game** and press F4 to highlight nearby lootable objects

## Configuration

Edit the `mod.cfg` file in the LootBeacon folder to customize the mod:

```
-- Detection radius in meters
loot_beacon_set_detection_radius =15.0

-- Particle effect for items (available colors: red, green, blue)
loot_beacon_set_item_particle_effect_path ="loot_beacon.pillar_red"

-- Particle effect for human corpses
loot_beacon_set_human_corpse_particle_effect_path ="loot_beacon.pillar_green"

-- Particle effect for animal corpses
loot_beacon_set_animal_corpse_particle_effect_path ="loot_beacon.pillar_blue"

-- Custom entity classes to highlight (comma-separated, leave empty to disable)
-- Common entity classes in KC:D2:
-- - Nest: Bird nests
-- - PickableArea: Herbs, plants,...
loot_beacon_set_custom_entity_classes ="Nest"

-- Particle effect for custom entity classes
loot_beacon_set_custom_entity_particle_effect_path ="loot_beacon.pillar_red"

-- Highlight duration in seconds
loot_beacon_set_highlight_duration =5.0

-- Show on-screen messages (1=on, 0=off)
loot_beacon_set_show_message =1

-- Enable/disable highlighting for different types (1=on, 0=off)
loot_beacon_set_highlight_items =1      -- Pickable items
loot_beacon_set_highlight_corpses =1    -- Human corpses
loot_beacon_set_highlight_animals =1    -- Animal corpses

-- Key binding for highlight activation (F4 is default)
loot_beacon_set_key_binding =f4
```

## Usage

1. Press F4 (or your configured key) in-game
2. All enabled lootable objects within your configured radius will be highlighted:
   - Red beacons for pickable items and custom entities
   - Green beacons for human corpses
   - Blue beacons for animal carcasses
3. The highlight effect lasts for your configured duration (5 seconds by default)
4. Pressing F4 again while active will restart the highlight duration
5. A notification will show how many objects of each type were found

## Known Issues and Limitations

- The "PickableItem" class doesn't contain all pickable items in the game (for example, torches may not be highlighted)
- Some items might be highlighted but still can't be picked up, and some pickable items might not be highlighted
- Performance may decrease if highlighting many objects at once in dense areas
- This is an early version that needs further testing - please report any issues you encounter

## Localization Support

The mod currently supports the following languages:
- English
- Italian

## Credits

- **[Nevyn77](https://next.nexusmods.com/profile/Nevyn77?gameId=7286)** - For Italian translation
- **[c0rish](https://next.nexusmods.com/profile/c0rish?gameId=7286)** - For helping debug Vortex installation and mod.cfg errors
- **[7H3LaughingMan](https://next.nexusmods.com/profile/7H3LaughingMan?gameId=7286)** - For [KCD2 PAK](https://www.nexusmods.com/kingdomcomedeliverance2/mods/1482), which helps create pak files with ease
- **[yobson](https://next.nexusmods.com/profile/yobson?gameId=7286)** - For [VS Code Lua Runner](https://www.nexusmods.com/kingdomcomedeliverance2/mods/459), which helped testing lua scripts faster
- **Warhorse Studios** - For creating Kingdom Come: Deliverance II

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
