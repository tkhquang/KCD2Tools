# Kingdom Come: Deliverance II - Mods

A collection of mods for Kingdom Come: Deliverance II that enhance gameplay and add new features to the game.

## Lua Scriptbind Documentation from WH

[2025_01_14](https://tkhquang.github.io/KCD2Tools/script_bind/script_bind_2025_01_14/)

## Available Mods

### [Proper Third Person View (TPV Camera)](TPVCamera/)
A standalone third-person camera that renders behind the player while aiming, looting, and combat keep working off the first-person frame. Includes free-look orbit, zoom, camera collision, and crosshair convergence.

### [Henry's Senses - Loot and Object Highlighting](HenrySenses/)
A loot and object highlighting mod for Kingdom Come: Deliverance II. The native (ASI) successor to LootBeacon.

### [Sound Framework - Custom Sounds and Voices](SoundFramework/)
A framework that lets mods add sounds and voice lines without code: new sounds on the game's mixer, NPC voice lines with subtitles, lip sync and facial moods, and replacements for the game's own sounds and voice lines.

### [Engine Tweaks (More Performance)](EngineTweaks/)
Removes engine freezes that grow with the number of items Henry carries: looking at a bed, and opening or closing the inventory, a loot or trade screen, the map or the journal, and transferring all items to or from a container. Made for hoarders like me, and for anyone who uses a mod that raises or removes Henry's carry weight limit.

### [Animation Framework](AnimationFramework/)
Lets animation mods work together instead of overwriting each other: mods that ship the same animation files are merged, and mods add or replace clips without shipping the game's whole clip index. Comes with tools for mod authors: a clip compiler, a checker, an action maker, motion capture retargeting and a Blender add-on.

## Installation

Each mod has its own installation instructions. See the respective mod's README for details.

## Development

Mods in this repository are developed with various technologies and approaches:

- Some mods are built with C++ and compiled to ASI plugins (TPVToggle uses [DetourModKit](https://github.com/tkhquang/DetourModKit) as its foundation)
- Other mods may use different languages or techniques like Lua scripts

The repository follows a modular structure where each mod is contained in its own directory.

## Contributing

Contributions, bug reports, and feature requests are welcome! Feel free to open an issue or submit a pull request.

## License

This project is licensed under the MIT License - see the LICENSE file for details.
