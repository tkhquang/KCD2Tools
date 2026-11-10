# Devlog

How the three examples were made, and what went wrong. Read the framework's [cutscene guide](https://github.com/tkhquang/KCD2Tools/blob/main/AnimationFramework/docs/CUTSCENES.md) first.

There are two ways to get a cutscene: make one from scratch (the dance battle), or copy one of the game's and change it (the fencers' bout, twice).

## The mod around the scenes

- **Files carry the mod id.** The game reads the Lua, quest, cutscene table, role, Storm and text files only when their names carry the id of the mod's folder (`cutscene__cutscene_examples.xml`, and so on). Test the mod installed as its own mod, never inside another.
- **Topics on Magdalena.** A Storm rule (`hasName kkut_magdalena`) gives her the mod's own role, so no game file is replaced. The quest's dialog has one sequence per topic.
- **Start the scene after the talk.** Each topic sets a quest State. When the conversation closes (`AfterPlay`), an If runs `#CutsceneExamples.Play('dance')` with the game's "run console command" node. Started from the topic itself, the scene would play under the dialog camera.
- **Real voices on new lines.** The lines use the mod's own text keys, so they need `SelectedSouls`, `VoiceoverFallbackLevel` and a `ReferenceLength`. The Sound Framework pack `SoundFramework/cutscene_examples.ini` plays the game's recordings on those keys (`ReplacesVoice`). Copy each line's length, mood and gestures from the game's own dialog too.
- **Pick lines that fit the topic.** The first duel lines ("Fancy a duel?", "Good luck to you!") sounded like Henry challenging Magdalena. Lines that tell a story ("We had a friendly bout…", "Christ, you men…") fit.
- **One Lua call per scene:** `AnimationFramework.Cutscene.Play{ name, row, layer, sequence, soundBank }`.

## The dance battle, from scratch

1. **Animations.** Dances from Meshy's library (motion capture), retargeted with `animfw retarget <file.fbx> <clip.caf> --take <take> --no-loop --root in-place` (`--skeleton female` for Magdalena). Keep clips in place, or the next one starts at its own origin and the dancer pops back.
2. **Stage.** One level layer with:
   - a `SequenceArea` over the yard;
   - a `CameraSource` per shot;
   - the doubles: `AnimChar`s with body presets (Henry's preset wears what he wears; the crowd uses the game's own spectators);
   - `MusicCutscene` sound sources (music, ambience, crowd, footsteps);
   - one `SequenceObject` per outcome.
3. **Clear the yard.** List the `SequenceArea` in `CleanupAreas`: the game hides the people there while the scene plays.
4. **Dancers.** Animation keys, Rotation keys and `LookAt` keys. Overlap clips by about 0.8 s: two keys blend only where they overlap.
5. **Crowd.** An idle loop for everyone, plus reactions on each beat (cheer, clap, laugh, point). Start them at slightly different times, or the crowd moves as one.
6. **Cameras.** One per shot, switched by the Director track. Key the depth of field on every camera key.
7. **Sound.** Crowd and cheers come from other cutscenes' sound banks: list those cutscenes in `soundBank`. Footsteps go where a foot lands.
8. **Fades and end card.** A `ScreenFader` fades in and out; the text on black is a `CustomText` event in the scene's row.
9. **Music.** A game music file on a `Voice` key with `IsMusic="1"`.

## The fencers' bout, recast

1. **Copy:** `animfw clone-cutscene src cin_s3970k_sermiri__fencers_fight ce_bergow_bout`. It writes a new layer, a row and the `Register` line.
2. **Recast.** A double's `guidBodyPresetId` is a soul: face plus a default outfit. The winner gets Henry's soul, the loser Jobst von Bergow's, the fencing master Hans's. Von Bergow also needs his costume, `guidClothingPresetId` `UC_Bergov`, and `esClothingConfig="male"` for his long coat.
3. **End early.** `EndTime` 51.4 s, when the loser is down and before the master speaks. Move the last fade to black to 0.6 s before the end.

## The field duel, the same bout moved

1. **Copy to the field:** the same command plus `--at 1458.6,956.1,125.38`. Cameras, lights and props move along.
2. **Leave the arena behind.** Remove what dressed the arena: onlookers, master, lights, fog, props, backdrop planes, crowd sounds, handclaps, their voice keys. Keep the two fighters (two doubles each), their swords, dust, blood, lines and foley.
3. **Recast.** Henry wins; Hans loses, in his armour (`UC_CaponArmor`, `esClothingConfig="male"`).
4. **Cut the crowd shots out.** From 33.5 to 38.1 s the arena showed its crowd, and the fighters stand frozen, then jump to new places. Remove that span, move every later key 4.6 s earlier, and put a camera cut on the join.
5. **End before the swap.** At 50.5 s the fallen loser's double is swapped for an injured one, and the bodies jump. The scene ends there (45.9 s after the cut), fading out over the last 0.6 s.
6. **New cameras for two people.** The arena's wide shots become a crane down, an over-the-shoulder shot each way, an orbit, a low shot on Henry and a high crane. The game's close-ups stay. Find where the fighters are from their animations, and key each camera every 0.5 s on a smoothed path. Give it the game's handheld animation (`CSshake_FK_standing_002`), depth of field on its subject, and a small shake on the orbit's sword hits.
7. **Time of day.** The row sets `Time="18h45m"` and clear weather. Remove the arena's `Environment` node.
8. **Footsteps on grass.** Remove the arena's sand steps. Find each foot landing in the animations and key a grass step there:

   | Camera distance | Sound |
   |---|---|
   | up to 4.5 m | `cin_fs_grassdry` |
   | up to 9 m | `cin_fs_grass_soft` |
   | further | none |

   The fall gets a grass thump and rustle.
9. **Music.** The bout's own fight music, from the start: it lasts 45.7 s, the scene 45.9 s.
10. **Air.** Drifting seeds, flies and crows, as in the game's open-field duels.

## What went wrong

| Problem | Cause | Fix |
|---|---|---|
| Doubles stood like statues | A double plays nothing between keys | An idle loop under every action |
| Dance to idle snapped | Keys only touched end to end | Overlap them |
| Henry popped back between dances | Clips travelled | Retarget in place |
| The crowd was silent | A copy doesn't load the original's sound bank | List it in `soundBank` |
| Magdalena glided in her sitting pose after a scene | Bound to a node to hide her | Hide her with the `SequenceArea` |
| Von Bergow looked like a stranger, no coat | Soul only; default cloth config `male2` | His clothing preset and `male` |
| Blurry push-in | One focus distance for a moving camera | Key focus on every camera key |
| The mod's Lua and quest were ignored | Test install inside another mod's folder | Install as its own mod |
| The field duel brought the arena along | The copy kept the arena's lights, fog, planes, props and claps | Remove everything that belonged to the arena |
| Added "sun" lights looked fake and too bright | Per-shot lights and haze pooled on the grass | The field's own daylight |
| Too much shake on every clash | A strong jolt on 18 hits | A small one, on one shot only |
| Frozen poses, then a jump | The arena showed the crowd there | Cut that span out |
| A glitch at the end | The loser's double swap | End before it |
| Camera stuttered | Key handles (`ds`, `dd`) longer than the gap to the next key make the curve run backwards | Handles a third of the gap |
| Per-shot volume did nothing | One-value tracks (RTPC) were written with subtracks | Keys go straight in the Track |
| Footsteps sounded like sand, some missing | The arena's steps, keyed only for its own shots | Grass steps on every landing |
| Footsteps as loud far away as close | Scene sounds play the same at any distance | Pick a softer step, or none, by distance |

**Picking a game scene to copy.** Avoid scenes that mostly talk, give away the story, or cut to pre-rendered video (a `Console` key running `wh_ui_PlayMovie`). Check who really fights: in the fencers' bout, Henry only watches. A scene that a quest plays directly has no row in the cutscene table: copy it with `--row`.

**Finding an open field.** Level ground is often a town square, a yard or a clearing. The game builds tree collision only near the player, so check each spot in place.

## Testing

- To play a scene without the dialog, run `CutsceneExamples.Play('duel')` (or `'dance'`, `'bergow'`) from a Lua console.
- A scene won't start while it, or another scene of its layer, still plays.
- `sys_SleepIfInactiveWH 0` keeps the game at full speed when its window is not focused.
