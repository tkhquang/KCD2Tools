# Loot Beacon - Never Miss a Drop

## Overview

**Loot Beacon** is a Lua mod for Kingdom Come: Deliverance II that helps players spot dropped items in the game world. Activate the highlight feature with a keypress and watch as nearby pickable items are highlighted with a colorful particle effect.

## Features

- Instantly highlight nearby pickable items with a glowing beacon (default: F4 key)
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
4. **Launch the game** and press F4 to highlight nearby items

## Configuration

Edit the `mod.cfg` file in the LootBeacon folder to customize the mod:

```
# Detection radius in meters
loot_beacon_set_detection_radius 15.0

# Highlight duration in seconds
loot_beacon_set_highlight_duration 5.0

# Show on-screen messages (1=on, 0=off)
loot_beacon_set_show_message 1

# Particle effect color (available: red, green, blue)
# Options: loot_beacon.pillar_red, loot_beacon.pillar_green, loot_beacon.pillar_blue
loot_beacon_set_particle_effect_path "loot_beacon.pillar_red"

# Key binding for highlight activation (F4)
bind f4 loot_beacon_activate
```

## Usage

1. Press F4 (or your configured key) in-game
2. All pickable items within your configured radius will be highlighted
3. The highlight effect lasts for 5 seconds (or your configured duration)
4. Pressing F4 again while active will restart the highlight duration
5. A notification will show how many items were found

## Known Issues and Limitations

- The "PickableItem" class doesn't contain all pickable items in the game (for example, torches may not be highlighted)
- Some items might be highlighted but still can't be picked up, and some pickable items might not be highlighted
- This is an early version that needs further testing - please report any issues you encounter

## Note

I don't have experience with creating particle effects, so the current highlight beacons are adapted from existing game resources with some tweaks. If anyone has better particle effect ideas or expertise with creating custom particles for KCD II, please contact me - I'd be happy to collaborate on improving the visual effects in this mod!

## Credits

- **[7H3LaughingMan](https://next.nexusmods.com/profile/7H3LaughingMan?gameId=7286)** - For [KCD2 PAK](https://www.nexusmods.com/kingdomcomedeliverance2/mods/1482), which helps create pak files with ease
- **[yobson](https://next.nexusmods.com/profile/yobson?gameId=7286)** - For [VS Code Lua Runner](https://www.nexusmods.com/kingdomcomedeliverance2/mods/459), which helped testing lua scripts faster
- **Warhorse Studios** - For creating Kingdom Come: Deliverance II
