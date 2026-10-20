# Kingdom Come: Deliverance II - Chiuuu! Mouth-Sound Bows

Every bow and crossbow shot goes "Chiuuu!".

## Overview

Draw, aim, release, and instead of the bowstring you hear a grown man going *"Chiuuu!"* with his mouth. Crossbows too, a little deeper. Henry, bandits, hunters: every shooter in Bohemia now makes their own sound effects.

It is a small joke mod and a showcase for [Sound Framework](../SoundFramework/): it is made of one pack INI and two sound files, with no code and no paks. It shows how a pack can replace the game's own sounds with brand-new ones, timed to the moment the arrow leaves the bow.

## Features

- Every bow shot plays one of two mouth-made "Chiuuu!" takes, right as the arrow leaves the bow
- Crossbow shots get the same takes, a little deeper
- Each shot varies a little in pitch and volume, so no two sound the same
- Works for every shooter, Henry and NPCs alike
- Only the shot itself changes: nocking, reloading and hits keep the game's own sounds
- Optional: keep the game's own shot under the Chiuuu (see Options)
- Works with any game language

## Requirements

- [Sound Framework](../SoundFramework/), ASI or KCSE build

## Installation

Install Sound Framework first. Then install this mod with your mod manager, or extract the archive into `<KC:D 2 installation folder>/Mods/` (create the folder if it is missing):

```
Mods/
\-- mouth_sound_bows/
    |-- mod.manifest
    \-- SoundFramework/
        |-- mouth_sound_bows.ini       (the pack)
        |-- mouth_sound_bows_01.ogg    (long take)
        \-- mouth_sound_bows_02.ogg    (short take)
```

To check it, open `KCD2_SoundFramework.log` after loading a save: it lists `mouth_sound_bows.ini`. Then shoot.

To uninstall, delete `Mods/mouth_sound_bows`.

## Options

To hear the game's own bowstring and crossbow under the Chiuuu, open `SoundFramework/mouth_sound_bows.ini` and set `KeepOriginal = true` in the section you want (`[mouth_sound_bows_shot]` for bows, `[mouth_sound_bows_crossbow]` for crossbows). Saving the file applies it while you play.

## Make Your Own

[`mouth_sound_bows.ini`](src/SoundFramework/mouth_sound_bows.ini) is a complete, commented example of sound replacement: swap the files for your own takes, or point `Replaces` at other game sounds. The [Sound Framework modding guide](../SoundFramework/docs/MODDING.md) covers every key and how to find the game's sound events with the `EventLog` debug option.

## License

The pack and its documentation are MIT licensed (see [LICENSE](LICENSE)). The two sounds are public domain (CC0 1.0) recordings by monosfera.

## Credits

- [monosfera](https://freesound.org/people/monosfera/) - the mouth sounds ([lorenc_fx21_laser](https://freesound.org/people/monosfera/sounds/572298/) and [lorenc_fx23_laser](https://freesound.org/people/monosfera/sounds/572296/), CC0) on Freesound
- Warhorse Studios - Kingdom Come: Deliverance II
