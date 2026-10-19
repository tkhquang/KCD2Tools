# Making a Sound Pack

A sound pack is an ordinary KC:D 2 mod folder with a `SoundFramework` folder inside. Every `.ini` file in that folder is a pack. A pack can:

- add new sounds and play them from the console or in place of the game's own sounds;
- add voice lines that NPCs speak with subtitles, chat bubbles, lip movement and a facial mood;
- replace the game's own voice lines with yours;
- define moods (facial expressions) that any pack can use.

The framework reads the packs at startup and reads a pack again within a second of you saving it. Problems, such as a missing file or a misspelled key, are written to `KCD2_SoundFramework.log` with the INI file and line number.

[Stop Right There, Criminal Scum](../../CriminalScum/) is a complete example pack: custom guard voice lines that replace the arrest dialog.

## Folder layout

```
Mods/
\-- my_mod/
    |-- mod.manifest                    (needed only when the mod has paks)
    |-- SoundFramework/
    |   |-- my_mod_sounds.ini           (a pack; any name ending in .ini)
    |   \-- bell_1.ogg                  (sound files, relative to the INI)
    \-- Localization/
        |-- english.pak                 (voice line recordings, see Voice lines)
        \-- English_xml.pak             (voice line text)
```

Sounds that are only played by the framework need no paks: their files sit loose beside the INI. Voice lines go through the game's own speech system, which only reads recordings from the localization paks, so a pack with voice lines needs a `mod.manifest` and paks like any other mod. Build paks with a packer that writes plain zip entries without timestamp fields, such as [kcd-pak-action](https://github.com/tkhquang/kcd-pak-action).

## Names and load order

Section names are case-insensitive and may contain letters, digits, `_`, `.` and `-`. Start them with your mod's name (`[my_mod_bell]`) so they don't clash with other packs.

Packs load in the order of `Mods/mod_order.txt`, then the remaining mods alphabetically, and within a mod by file name. When two packs define the same name, the later one wins and the log notes the override. The default moods (`KCD2_SoundFramework_moods.ini`, beside the framework) load before every pack.

## Sounds

```ini
[my_mod_bell]
Files = bell_1.ogg, bell_2.ogg          ; one is picked at random per play, never the same twice in a row
Bus = world
Space = 3d
Volume = 0.8
VolumeVariation = 0.1
PitchVariation = 0.05
MaxDistance = 60
```

| Key | Default | Meaning |
|---|---|---|
| `Files` (or `File`) | required | Comma-separated audio files, relative to the INI. WAV, OGG, MP3 and FLAC all work. |
| `Bus` | `world` | The mixer bus, which decides the volume slider and whether the sound pauses with the game. An alias below, or a raw FMOD path starting with `bus:/`. |
| `Space` | `3d` | `3d` plays at a position in the world; `2d` plays flat, like interface sounds. |
| `Volume` | `1` | 0 to 4. |
| `VolumeVariation` | `0` | 0 to 1: each play scales the volume by a random factor between 1 minus this and 1. |
| `Pitch` | `1` | 0.25 to 4. |
| `PitchVariation` | `0` | 0 to 0.75: each play scales the pitch by a random factor within plus or minus this. |
| `MinDistance` | `2` | Metres within which the sound plays at full volume. |
| `MaxDistance` | `40` | Metres beyond which the sound is silent. |
| `Loop` | `false` | Loops until stopped. |
| `Stream` | `false` | Decodes while playing instead of loading the whole file first. Use it for long music or ambience. |
| `MaxInstances` | `8` | 1 to 64 copies may play at once; a new play stops the oldest one beyond this. |
| `Cooldown` | `0` | Seconds; plays requested sooner after the last one are dropped. |
| `Replaces` | | Game sound events this sound plays instead of (see below). |
| `ReplacesFor` | | Only replace the events when they start at one of these entities. |

Bus aliases:

| Alias | FMOD bus | Use |
|---|---|---|
| `world` | `bus:/dieg/w_obj/obj` | Objects in the world (the default). |
| `world_loud` | `bus:/dieg/w_obj/obj_loud` | Loud world objects. |
| `player` | `bus:/dieg/w_obj/player` | Sounds Henry makes. |
| `voice` | `bus:/dieg/dialog` | Speech. |
| `barks` | `bus:/dieg/w_obj/barks` | Shouts and short calls. |
| `ambience` | `bus:/dieg/w_beds` | Ambient beds. |
| `world_music` | `bus:/dieg/w_obj/music` | Music played in the world, such as a bard. |
| `music` | `bus:/music` | The soundtrack, under the music slider. |

The `dieg` buses pause with the game and sit under the Sound volume slider.

### Replacing the game's sounds

`Replaces` takes one or more game event paths. Each time the game starts one of those events, it is muted and your sound plays at its position instead, following it while it lasts:

```ini
[my_mod_dog]
Files = woof_1.ogg, woof_2.ogg
Replaces = event:/animals/dog/dog_bark
ReplacesFor = tvez_vorech               ; optional: only this one dog
```

To find event paths, set `EventLog` under `[Debug]` in `KCD2_SoundFramework.ini` to part of a path (`dog`) or `*` for everything. The log then lists every matching event as it starts, with its world position.

`ReplacesFor` takes entity names from the game's entity system, such as `tvez_vorech`. A Lua console or mod reads an entity's name with `entity:GetName()`. An event is replaced when it starts within 2 metres of one of the named entities.

## Voice lines

A section with a `Voice` key is a voice line. NPCs speak it through the game's own speech system, so it plays on the dialog bus with the NPC's voice settings, and the framework adds the text, lip movement and mood.

```ini
[my_mod_guard_hello]
Voice = Localization/dialog/my_mod/mymd_hello_01, Localization/dialog/my_mod/mymd_hello_02
TextKey = mymd_hello_01, mymd_hello_02
Speaker = soul_ui_name_guard
Display = subtitle
Cooldown = 30
Mood = angry
MoodStrength = 0.8
```

| Key | Default | Meaning |
|---|---|---|
| `Voice` | required | Comma-separated takes: paths of recordings in your localization pak, without the extension. Each say picks one, never the same twice in a row. |
| `TextKey` | | Text table keys: one shared by every take, or one per take. |
| `Speaker` | | Text key of the name shown above the bottom subtitle, such as the game's `soul_ui_name_guard`, or your own key. |
| `Display` | `bubble` with text, `none` without | Where the text shows: `subtitle` (bottom, dialog style), `bubble` (above the speaker's head), `both` or `none`. |
| `Cooldown` | `0` | Seconds; says requested sooner after the last one are dropped. |
| `Mood` | | A mood the speaker's face holds while speaking (see Moods). |
| `MoodStrength` | `1` | 0 to 2; scales every weight of the mood. Below 1 tones it down, above 1 exaggerates it, up to the mood's `MaxStrength`. |
| `MoodPoses` | | Extra pose weights, or ones that override the mood's. Without `Mood`, a mood of the line's own. |
| `ReplacesVoice` | | Game voice lines this line plays instead of (see below). |

### Recordings and text

The game reads voice recordings only from localization paks. Put them in your mod's `Localization/english/` folder before packing, for example `Localization/english/dialog/my_mod/mymd_hello_01.ogg`, and refer to them as `Localization/dialog/my_mod/mymd_hello_01` (no language, no extension). Ogg Vorbis is the format the game uses. Record at 16 kHz or more, or advanced lip sync falls back to the basic mouth.

Text goes in a table in `Localization/English_xml/`, in a file named after your mod (such as `text__my_mod.xml`). The game merges every table file it finds:

```xml
<Table>
<Row><Cell>mymd_hello_01</Cell><Cell>Halt, traveler.</Cell><Cell>Halt, traveler.</Cell></Row>
<Row><Cell>mymd_speaker_watchman</Cell><Cell>Watchman</Cell><Cell>Watchman</Cell></Row>
</Table>
```

Each row is the key, the source text and the translated text. Add the same files for other languages (`Localization/german/`, `Localization/German_xml/`) to translate a pack.

### Saying a line

`sfw_say <entity name> <line name>` makes an NPC say a line from the console. The entity name is the NPC's name in the game's entity system, such as `kmis_man_22` (`entity:GetName()` in Lua).

`sfw_say <entity name> <voice path> [speaker:<name key>] [bubble:<text key>] [mood:<mood>] [strength:<0..2>] [subtitle text]` speaks a single recording without a pack line, for trying things out.

### Replacing the game's voice lines

`ReplacesVoice` takes game line keys. Each time an NPC is about to say one of those lines, your line plays instead, with your recording, text and mood:

```ini
[my_mod_guard_arrest]
Voice = Localization/dialog/my_mod/mymd_arrest_01
TextKey = mymd_arrest_01
Speaker = soul_ui_name_guard
Display = subtitle
ReplacesVoice = stra_strazny_z_se_zlodeje_xNNL, stra_strazny_z_prisel_cas_ySQ2
ReplacesVoice = zaty_strazny_z_*
```

- Keys are case-insensitive. `*` matches any run of characters, so `zaty_strazny_z_*` covers every line whose key starts that way.
- An exact key always wins over a pattern, and a longer pattern wins over a shorter one, so you can catch a whole group with a pattern and give a few lines their own replacement.
- A line may repeat `ReplacesVoice` to keep long lists readable.

To find line keys, set `SpeakLog` under `[Debug]` in `KCD2_SoundFramework.ini` to part of a key or path, or `*` for everything. The log then shows every line the game speaks, with its key and its recording. The game's dialog files (`Scripts.pak`, `Quests/`) list the keys of whole conversations.

## Moods

A mood is a set of face pose weights that the speaker holds while a line plays:

```ini
[my_mod_furious]
Poses = L_browsLowerer:0.9, R_browsLowerer:0.9, L_noseWrinkler:0.5, R_noseWrinkler:0.5
```

Pose names are the face rig's poses without the `FA_POSE_` prefix: `C_` for the centre of the face, `L_` and `R_` for each side, for example `L_browsLowerer`, `R_cheekRaiser`, `C_chinRaise`. Weights run from 0 to 2; the game's own facial animation goes up to about 1.6. An optional `MaxStrength = 0..2` caps how far any line scales the mood: the defaults set it to 1 for `happy`, `smug` and `crying`, and to 1.5 for `sad` and `disgusted`, where the face pulls out of shape beyond that. `KCD2_SoundFramework_moods.ini` beside the framework lists the default moods (`angry`, `happy`, `crying`, `sad`, `scared`, `surprised`, `disgusted`, `smug`, `pain`, `worried`, `tired`) with every pose they use.

Any pack can use a mood defined in any other pack. Define a mood again under the same name in your pack to change it; packs load after the defaults, so yours wins.

On game lines that have Warhorse's own lip-sync animation, a mood moves only the upper face (brows, eyes, cheeks, nose) and the mouth stays Warhorse's. On your own lines the mood moves the whole face, with the lip movement on top.

Tune moods live: `sfw_mood_override <mood> pose:weight ...` changes a mood until the next reload (`sfw_mood_override <mood>` with no poses resets it), and `sfw_mood_dump <mood>` writes its current poses to the log, ready to paste into a pack.

## Lip movement

Your voice lines move the speaker's mouth automatically; there is nothing to set up. Players choose how under `[LipSync]` in `KCD2_SoundFramework.ini`:

- `ADVANCED`: a small model that learned from the game's own voice lines and facial animation moves the whole lower face.
- `BASIC`: jaw and lip shapes follow the loudness and spectrum of the voice.

`OffsetMs` (or `sfw_lipsync_offset <ms>` in the console) moves the mouth earlier or later than the sound.

## Console commands

| Command | What it does |
|---|---|
| `sfw_play <sound> [x y z]` | Plays a sound at a world position, or at the camera without one. |
| `sfw_stop <sound>` | Stops every playing copy of a sound. |
| `sfw_stop_all` | Stops every framework sound. |
| `sfw_say <entity> <line or voice path> ...` | An NPC says a voice line (see Saying a line). |
| `sfw_mood_override <mood> [pose:weight ...]` | Changes a mood until the next reload. |
| `sfw_mood_dump <mood>` | Writes a mood's poses to the log. |
| `sfw_lipsync_offset <ms>` | Moves the mouth earlier (positive) or later (negative). |
| `sfw_reload` | Stops every framework sound and reads every pack again. Use it after adding a new pack file. |
| `sfw_list` | Writes every sound, voice line and mood to the log. |

## How voice lines share a speaker

A character says one line at a time, and the game respects that for your lines as it does for its own:

- **Your line waits for the character's line.** A pack line asked for while the character is speaking starts when that line ends, so the game's own line and its subtitle finish normally. A line still waiting after 10 seconds is dropped.
- **The game waits for your line.** Greetings, barks and conversation lines for a character who is speaking one of your lines start only after it ends. A minute-long line on a character the player is about to talk to delays the conversation by that minute, so keep long lines for characters the player does not talk to, or keep them short.
- **The face follows the sound.** The mouth, the mood and the subtitle start when the line's sound actually starts and stop when it ends, also when the game cuts it short (for example when the character dies or a conversation ends). They pause with the game in the pause menu and the inventory.
- **Length:** the game stops any line 100 seconds after its sound starts, so keep each recording under 100 seconds.
- **Loading a save** stops every pack line and pack sound that is still playing.

## Reloading

- **Pack INIs:** saving one reloads every pack within a second. A new pack file is picked up by `sfw_reload`.
- **Loose sound files:** changes are picked up on the next reload.
- **Recordings and text in paks:** the game reads its paks only at startup, so these need a restart.
