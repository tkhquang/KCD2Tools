# Kingdom Come: Deliverance II - True Light - Real Torch Shadows

The torch in Henry's hand lights the world from its flame and casts real shadows, his own included.

## Overview

The game doesn't light the scene from the torch. It uses an invisible light just above Henry's head, made for first person: its shadows only fall in a cone in front of the camera, and Henry casts none. In third person or photo mode that shows as shadows with straight edges, and weapon shadows floating with no body. True Light switches that light off and lights the scene from the flame.

## What to Expect

- **Torch or lantern in hand:** light from the flame, shadows in every direction, Henry's included. It follows the flame through any animation, animation mods too.
- **Crouching, mounting, dismounting:** the game hides the torch model. The light stays on from just above Henry's head, with full shadows, and moves back to the flame once the torch is raised again.
- **Torch put away, dialogues, cutscenes:** no torch light, as in the game.
- NPC torches and every other light are untouched. No measurable frame-rate cost.

## Installation

Install with your mod manager, or extract into `<KC:D 2 installation folder>/Mods/`:

```
Mods/
\-- true_light/
    |-- mod.manifest
    |-- mod.cfg              (settings)
    \-- Data/
        \-- true_light.pak
```

`kcd.log` shows a `[TrueLight] loaded` line once it runs. To uninstall, delete `Mods/true_light`.

## Settings

`Mods/true_light/mod.cfg`, read when a save loads. `1` = real torch light, `0` = the game's own.

| Setting | Default | View |
|---|---|---|
| `true_light_third_person` | `1` | Third person (TPVCamera or any camera mod) |
| `true_light_photo_mode` | `1` | Photo mode |
| `true_light_first_person` | `0` | First person |

The same names work as console commands. `true_light_reload` reads the file again.

### Lights From Other Mods

Torches and lanterns from mods (class `Lamp`) are built in. Add another mod's handheld light with:

```
true_light_held_light <class | name:prefix> [helper] [state]
```

`name:` matches the start of the entity name instead of the class, `helper` is the flame helper on the model, and `state` is a script state it must be in to count as lit; `-` skips one. Without a value the command lists the lights. The item must get the game's torch light when held, as torches and lanterns do.

## Compatibility

- [TPVCamera](../TPVCamera/): recommended, not required. Its view signal also counts its close-up shots as third person.
- Other camera mods: detected from the camera's distance to Henry's head.

## Known Limitations

- In first person (off by default) the torch arm can shadow the view.

## License

MIT (see [LICENSE](LICENSE)).

## Credits

- Warhorse Studios - Kingdom Come: Deliverance II
