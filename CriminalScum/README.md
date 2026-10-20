# Kingdom Come: Deliverance II - Stop Right There, Criminal Scum

Town guards arrest you with a certain famous line.

## Overview

Get caught stealing, trespassing or brawling and the guard who stops you has only one thing to say: *"Stop right there, criminal scum! You violated the law. Pay the court a fine or serve your sentence. Your stolen goods are now forfeit."*

It is a brief showcase for [Sound Framework](../SoundFramework/), not a mod meant for regular play: one pack INI, three voice takes and a text table, with no code of its own. It shows how a pack can replace the game's own voice lines and give them subtitles, a speaker name, an angry face and lip sync.

[![Showcase video on YouTube](https://img.youtube.com/vi/oVVImFIX-uI/maxresdefault.jpg)](https://youtu.be/oVVImFIX-uI)

## Features

- Every crime accusation in the arrest dialog (theft, lockpicking, pickpocketing, trespass, assault, murder, poaching and the rest) becomes the line, in one of three takes
- Bottom subtitles with the speaker name "Guard", an angry face and lip movement
- The rest of the arrest dialog, the chase and the fight are untouched

## Requirements

- [Sound Framework](../SoundFramework/), ASI or KCSE build
- English voice and text language

## Installation

Install Sound Framework first. Then install this mod with your mod manager, or extract the archive into `<KC:D 2 installation folder>/Mods/` (create the folder if it is missing):

```
Mods/
\-- criminal_scum/
    |-- mod.manifest
    |-- Localization/
    |   |-- english.pak           (voice takes)
    |   \-- English_xml.pak       (subtitle text)
    \-- SoundFramework/
        \-- criminal_scum.ini     (the pack)
```

To check it, open `KCD2_SoundFramework.log` after loading a save: it lists `criminal_scum.ini`. Then get caught.

To uninstall, delete `Mods/criminal_scum`.

## Make Your Own

[`criminal_scum.ini`](src/SoundFramework/criminal_scum.ini) is a complete, commented example of voice replacement. The [Sound Framework modding guide](../SoundFramework/docs/MODDING.md) covers every key and how to find the line keys of other conversations with the `SpeakLog` debug option.

## AI Disclosure

The voices are AI-generated: they were made with the [Maya1](https://huggingface.co/maya-research/maya1) text-to-speech model by Maya Research (Apache 2.0), from a written description of a pompous town guard. No voice actor was recorded and no audio from any Bethesda game is used.

The line is a nod to *The Elder Scrolls IV: Oblivion*. This mod is not affiliated with or endorsed by Bethesda Softworks or Warhorse Studios.

## License

MIT (see [LICENSE](LICENSE)).

## Credits

- [Maya Research](https://huggingface.co/maya-research/maya1) - Maya1 text-to-speech
- Warhorse Studios - Kingdom Come: Deliverance II
