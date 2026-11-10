# Making an Animation Mod

An animation mod for the framework is an ordinary KC:D 2 mod: a folder under `Mods/` with a `mod.manifest` and a pak. The difference is inside the pak. The mod ships only what it changes, never a whole copy of a game file. The framework merges it into the game's file as the game loads it, so two mods that change different parts of one file both work.

A mod can:

- add, replace or remove fragment options, blends and sub-databases in a Mannequin database (`.adb`)
- add tags, change controller definitions and change a clip's animation events
- add new clips and replace the game's clips, without shipping the game's clip index
- move skeleton bones (such as where a weapon sits on the back) and keep the bones other mods move
- give Henry different animations in third person, or while a state of your own is on
- play your own in-game cutscenes through the game's cutscene player (see [Cutscenes](#cutscenes))

The framework writes what it merged, and every conflict it found, to `KCD2_AnimationFramework.log` beside `WHGame.dll`.

## Folder layout

```
Mods/
\-- my_mod/
    |-- mod.manifest
    \-- Data/
        \-- my_mod.pak
            |-- Animations/Mannequin/ADB/kcd_male_database__my_mod.adb    (a database patch)
            |-- Animations/humans/male/my_mod/my_mod_idle.caf            (a new clip)
            \-- Animations/humans/male/blendspace/my_mod/...             (your own blend spaces)
```

Build paks with a packer that writes plain zip entries (stored or deflate), such as [kcd-pak-action](https://github.com/tkhquang/kcd-pak-action).

## Names and load order

A patch sits in the same folder as the game file it patches and is named `<game file stem>__<modid>.<extension>`. For `Animations/Mannequin/ADB/kcd_male_database.adb`, that is `kcd_male_database__my_mod.adb`.

Use your mod's folder name as the `<modid>`. It takes lowercase letters and underscores only: the game skips a mod whose id holds a digit or a capital.

| Game file | Patch |
|---|---|
| `kcd_male_database.adb` (Henry and male NPCs), any other `.adb` | `kcd_male_database__my_mod.adb` |
| `kcd_male_tags.xml` | `kcd_male_tags__my_mod.xml` |
| `kcd_male_fragmentids.xml` | `kcd_male_fragmentids__my_mod.xml` |
| `kcd_male_controllerdefs.xml` | `kcd_male_controllerdefs__my_mod.xml` |
| any `.animevents`, such as `Animations/humans/male/male.animevents` | `male__my_mod.animevents` |
| clip renames in every database | `Animations/Mannequin/ADB/clipswaps__my_mod.xml` |
| rows of `Libs/Tables/animation/anim_fragment.xml`, `Libs/Tables/action/actor_anim_action.xml` | `Animations/Mannequin/Tables/anim_fragment__my_mod.xml`, `actor_anim_action__my_mod.xml` (see [Rows for the game's action system](#rows-for-the-games-action-system)) |

- Patches merge in the game's mod load order.
- A later patch wins a key an earlier one also set, and the log names both (`AnimPatch: ... my_mod overrides other_mod on 3 key(s), e.g. ...`).
- File names are not case-sensitive.
- Tag lists match in any order: `player+walk` is `walk+player`.

## Patching a database

A database patch is an `<AnimDB>` that holds only the parts you change, in the game file's own layout. Copy the elements you need from the game's file and leave out everything else.

```xml
<AnimDB>
	<FragmentList>
		<MotionIdle>
			<Fragment Tags="player">...</Fragment>                    <!-- replaces the game's player options -->
			<Fragment Tags="player+my_mod_tired" Mode="append">...</Fragment>
			<Fragment Tags="player+reading" Mode="remove"/>
		</MotionIdle>
	</FragmentList>
	<FragmentBlendList>
		<Blend from="MotionMovement" to="MotionMovement">
			<Variant from="run" to="sprint+player">...</Variant>      <!-- replaces this transition -->
		</Blend>
	</FragmentBlendList>
	<SubADBs>
		<SubADB File="Animations/Mannequin/ADB/my_mod_extra.adb"/>
	</SubADBs>
</AnimDB>
```

### Options

Options are the `<Fragment>` children of a fragment element (`<MotionIdle>`, `<MotionMovement>`, ...).

- An option is matched by its fragment, `Tags` and `FragTags`, never by its `GUID`.
- When you replace an option, keep the game option's `GUID`. Give a new option a new one. The game's action tables (`actor_anim_action`, behind NPC and quest actions) point at options by GUID and position.
- For the same reason, replace an option with the same number of options. More or fewer moves every later option to another position.

The first option of a key in your patch sets what happens to the game's options with that key:

| `Mode` | Effect |
|---|---|
| none (or `replace`) | The game's options with this key are dropped, and yours take the place of the first. A key the game lacks is added. |
| `append` | Yours go after the game's options with this key, which stay. |
| `remove` | The game's options with this key are dropped. |

Patches merge one after another, so "the game's options" includes what earlier patches added. Patches that each add options under the same tags, as random variants side by side, must use `Mode="append"`. This applies to several paks of one mod and to several mods. Without it, each patch replaces the options the one before it added, and the log warns `... overrides ... on N key(s)`.

### Blends and sub-databases

- A `<Blend>` is matched by its `from` and `to` fragments, and each `<Variant>` by its `from` and `to` tags.
- A variant replaces the game's variant with the same tags, or is added. `Mode="remove"` drops it.
- A `<Blend>` the game lacks is added whole.
- A sub-database is added unless the game already lists the same `File`.

### Adding a new fragment

A fragment is a named action (`Piss`, `DoorNervousKnockingIn`, ...). A new one takes three small patches: its ID, its definition and its options. Start its name with your mod's name.

```xml
<!-- kcd_male_fragmentids__my_mod.xml: the fragment's ID -->
<TagDefinition version="2">
	<Tags>
		<Tag name="MyMod_Pray"/>
	</Tags>
</TagDefinition>

<!-- kcd_male_controllerdefs__my_mod.xml: the body layers it takes over -->
<ControllerDef>
	<FragmentDefs>
		<MyMod_Pray scopes="FullBody+Looking"/>
	</FragmentDefs>
</ControllerDef>

<!-- kcd_male_database__my_mod.adb: its options -->
<AnimDB>
	<FragmentList>
		<MyMod_Pray>
			<Fragment BlendOutDuration="0.2" Tags="">...</Fragment>
		</MyMod_Pray>
	</FragmentList>
</AnimDB>
```

Start from a copy of a similar game option, with your clip in it. Game options for such actions carry procedural layers that:

- stop Henry from walking off (`MovementControlMethod`)
- set his collider
- move the first-person camera with the animation

To play the fragment, see [Playing an animation from a script](#playing-an-animation-from-a-script).

### Two rules that keep other mods working

- **Patch only the options you change.** Do not copy the game's other options to protect them: a copy replaces every other mod's change to them.
- **An option meant to stay on screen (an idle, a locomotion cycle) must be one looping clip.** A chain of clips holds the fragment until its last clip starts, so a change such as crouching waits for the whole chain. The log warns about such options (`... chains 2 clips where the game's options loop ...`).

## Tags, controller definitions, events and clip renames

### Tags and fragment IDs

`kcd_male_tags__my_mod.xml` and `kcd_male_fragmentids__my_mod.xml` each hold a `<TagDefinition>`.

- Its `<Tag>`s and `<Import>`s are added when the game's file lacks them.
- A `<Group>` merges into the game's group of the same name.
- Existing tags are never changed.

### Controller definitions

`kcd_male_controllerdefs__my_mod.xml` decides which body layers (scopes) a fragment takes over:

```xml
<ControllerDef>
	<FragmentDefs>
		<MotionIdle scopes="FullBody+HoldItem+LookPose+Looking" flags="AutoReinstall">
			<Override tags="player+my_mod_tired" scopes="FullBody"/>
			<Override tags="horse+player" Mode="remove"/>
		</MotionIdle>
	</FragmentDefs>
</ControllerDef>
```

- A fragment's `scopes` and `flags` change when you give them. `Mode="replace"` on the fragment sets both exactly, empty ones included.
- An `<Override>` is matched by its `tags` and `fragTags`. It replaces the game's override with the same tags, or is added. `Mode="remove"` drops it.
- New `<ScopeDefs>`, `<ScopeContextDefs>` and `<FragmentDefsImports>` entries are added.
- A change to an existing scope's `layer` or `numLayers` moves every option on it to other layers, and the log warns.

### Animation events

`male__my_mod.animevents` sets a clip's sounds, footsteps and effects.

- Each `<animation name="animations/.../clip.caf">` replaces the game's list for that clip.
- `Mode="append"` adds your events to the game's list, and `Mode="remove"` drops the list.

```xml
<anim_event_list>
	<animation name="animations/humans/male/my_mod/my_mod_idle.caf">...events...</animation>
	<animation name="animations/humans/male/locomotion/relaxed_run_fast_player.caf" Mode="append">...events...</animation>
</anim_event_list>
```

### Sounds come from the name an option plays

The game plays an option's `footstep`, `footstep_impulse`, `foley`, `audio_trigger`, `audio_rtpc` and `effect` events from the event list of each animation the option names: a clip's own list, or a blend space's or `.comb`'s own list. It never reads the lists of the clips inside a blend space for them.

So every new name an option plays needs a list of its own. Without one, the option is silent: no footsteps, no cloth, no torch. The game's `male.animevents` has lists for most of its blend spaces (the sprint's footsteps are on `3d_relaxed_sprint_turn_strafe.comb`, not only on its clips).

- **A game clip copied under a new name:** copy the game clip's `<animation>` to your clip's path.
- **A game blend space or `.comb` copied under a new name** (see [Blend spaces](#blend-spaces)): copy its list to your blend space's path (`animations/humans/male/blendspace/my_mod/2d_sprint_turn_player_my_mod.bspace`). Copy the lists of the clips inside it too, as the game does.
- **A blend space that plays a clip on the upper body:** give it the clip's events without `footstep` and `footstep_impulse`. The legs play their own.
- **A clip made from several game clips** (a baked loop): put each part's events at its place in the new clip. Event times are fractions of the clip, 0 to 1.

Compare by ear with the game's version: a copy that looks right can still be silent.

### Clip renames

`clipswaps__my_mod.xml` swaps one clip for another in every database, wherever an option names it:

```xml
<ClipSwaps>
	<Swap from="relaxed_idle_both_player" to="my_mod_idle"/>
</ClipSwaps>
```

## Clips

Every compiled `.caf` under `Animations/` in a mod pak registers in the game's clip index. You don't ship `Animations.img`.

- **A new clip** gets its own path, and an option plays it by its file name without `.caf`. Start the name with your mod's name so it can't collide with a game clip or another mod's: `my_mod_idle`, not `idle`. Clips for Henry and male NPCs go under `Animations/humans/male/`, in a folder of your own.
- **A replacement** sits at the game clip's exact path and replaces it everywhere, also in clips the game keeps packed in `.dba` files. Two mods that replace the same clip can't merge: the later one loads, and the log says so.
- Clips must be compiled (CryEngine `.caf` with a motion chunk). Anything else is skipped with a warning.
- Mods that ship whole `Animations.img` copies still work: the framework merges them.

### What a clip should key

A clip track overrides the skeleton for its bone, wherever the clip plays. Ship only the tracks you own:

- **Full-body clips** (locomotion, idles: the base layer) must key every body bone. A bone a base-layer clip leaves out falls to the skeleton's rest pose, not to the game's animation.
- **Overlay clips** (`HoldItem*` and other partial layers) key only the bones they animate. The game does not play an overlay in step with the locomotion under it, so its time drifts against the stride. An arm that swings with the legs belongs in the locomotion clip. Overlays suit held poses, such as a grip.
- **Never key the bones skeleton mods move** (`BackSlot_bow`, `BackSlot_crossbow`, `BackSlot_shield`, `BackSlot_cannon`, `RightWeaponRoot`), unless your mod moves that bone itself.
  - Mods such as Swords on the Back and Realistic Bow and Crossbow Position move these in `male.chr` and strip their tracks from the clips they ship.
  - Game clips carry them (the run and sprint clips key the back slots), so drop them from a copy.
  - Your clips then stop overriding those mods. Without such a mod, the slot holds still in your clip instead of swaying.
- **Keep** the game clip's `BackSlot_quiver` track (both slot mods keep it), its other weapon and sheath bones (`RightWeaponSlot`, `LeftWeaponSheath`), and every IK target and IK blend weight (`*_IKTarget`, `*_IKBlend`). The engine reads them from the pose.

## Blend spaces

Two mods' copies of one blend space (`.bspace`) or combined blend space (`.comb`) do not merge. The last one loaded wins, and the log lists it (`AnimCompat: ... not merged`). Ship your own instead:

1. Copy the game's blend space under a new name, in a folder of your own, such as `Animations/humans/male/blendspace/my_mod/2d_sprint_turn_player_my_mod.bspace`. Point its examples at your clips.
2. Point your option at it in a database patch: `<Animation name="2d_sprint_turn_player_my_mod" flags="Loop"/>`.
3. Give it the game blend space's sounds in an event list patch (see [Sounds come from the name an option plays](#sounds-come-from-the-name-an-option-plays)). Without them, the option plays no footsteps.

Also:

- A blend space's `<JointList>` limits it to the joints it names (`<Joint Name="Spine3"/>`). Use the names in `male.chr`, case included, at most 255. The other joints are left to the layers below.
- A blend space that wraps a full-body clip twice (two examples at 0 and 1 of one parameter), with an upper-body joint list, plays that clip on the upper body only.
- The game reads a blend space once per session and keeps it by path, so a broken one stays broken until a restart. While you iterate, give each version a new name.
- An option that names a blend space the game can't find leaves Henry unable to move until the option changes.

## Skeletons

Mods that ship a whole `male.chr` to move a few bones are merged bone by bone:

- Each bone a mod moved is kept.
- A bone two mods move differently goes to the later mod, and the log says so (`SkeletonMerge: ... overrides ...`).
- Differences of float noise count as the same.
- Change only the bones you mean to move, and keep the game's bone list and chunk layout. A copy whose bones or chunks differ is not merged, and the newest copy loads as it is.

A moved bone shows only in clips that don't key it. Strip the bones you move from every clip you ship (see [What a clip should key](#what-a-clip-should-key)). A game clip that keys them still overrides your skeleton while it plays.

## Third-person animations and custom tags

- **Third person:** give an option the game's tags plus `thirdPerson` (`Tags="walk+player+thirdPerson"` next to the game's `walk+player`). It plays for Henry while a camera mod shows him from outside. In first person, the game's option plays.
- **Custom tags:** declare a plain tag with an `animfw_cvar` in `kcd_male_tags__my_mod.xml`, then tag options with it. Any script turns it on through that console variable.

Either kind of option only replaces the option the game would pick: `walk+player+thirdPerson` replaces `walk+player`, while a backward walk still plays the game's `walk+backward+player`. Patch only the options you change. The [README](../README.md#third-person-animations) has the full rules, the ranking of several active tags, and `mod_camera_view`.

## Playing an animation from a script

The game's Lua has it built in, and the framework opens it to NPCs:

```lua
player.human:PlayAnim("MyMod_Pray", "")   -- plays the fragment's best option for Henry's current tags
player.human:StopAnim()                    -- ends it at once, a looping or a one-shot option

local npc = System.GetEntityByName("kcer_suchyCert")
npc.human:PlayAnim("MyMod_Pray", "")      -- the same on an NPC (framework)
npc.human:StopAnim()
```

`PlayAnim` queues the fragment on the character's own Mannequin controller, over every body layer. It interrupts what they are doing and blends like the game's own actions.

- **A one-shot option** ends by itself, and the character goes back to their idle and movement.
- **A looping option** plays until `StopAnim`.
- **`StopAnim`** ends either one at once. The game alone lets a one-shot finish.
- **A second `PlayAnim`** replaces the first at once, even a one-shot that still plays. The game alone lets the first one finish and queues the second.
- Any fragment works, the game's or your own, with clips of your own too.
- The fragment's `thirdPerson` and custom-tag options take part as in the game's own fragments. Give `MyMod_Pray` a `thirdPerson` option and it plays while a camera mod shows Henry from outside. Both are Henry's tags, so an NPC always gets the base option.

### Tags for PlayAnim

The second argument takes the fragment's own tags (its `FragTags`), joined with `+`. The game treats any other tag name as a global tag: it switches the tag on for that character and leaves it on. The framework adds these tags:

- **`animfw_own_scopes`** plays a fragment made for part of the body, such as the `ADLG_FA_*` faces, over what the character is doing, on the scopes its definition names: `player.human:PlayAnim("ADLG_FA_HahaEvil", "animfw_own_scopes")`.
  - Without the tag, the fragment lands on the base layer and leaves the character in a T-pose.
  - Use it on partial fragments only. A fragment with no option for the character's current tags flashes a T-pose by default, and with the tag it stays in one.
- **`animfw_option_<n>`** picks one option. When several options share the tags the query matches, the game picks one at random. This tag asks for option n of them (0 is the first, in database order, and a number past the last wraps around): `player.human:PlayAnim("ADLG_Wave", "animfw_option_1")`.
- **`animfw_hold`** keeps a one-shot on its last pose when it ends, until `StopAnim` or another `PlayAnim` releases it. It holds the option's last clip. An option whose clip list ends on an empty clip (the game's way to blend out to nothing) therefore holds the bind pose: copy such a fragment without its last empty clip for holding.
- **`animfw_queue`** leaves the current fragment playing. The new one waits behind it and starts where it ends, with the database's blend, as the game's own play does without the framework.
  - For a sequence, queue each part once the one before has started: `PlayAnim("PrayKneelingIn", "")`, then `PlayAnim("PrayKneeling", "animfw_queue+animfw_hold")`.
  - Cut to the last part with a plain `PlayAnim("PrayKneelingOut", "")`.
- **`animfw_temporary_tags`** gives the character an option's tags for this play only (next section).

### Playing a fragment on tags the character lacks

A fragment with no option for the character's tags still plays if you name an option's tags, whether or not the character is in that state: a chair's fragment sits them on the air.

- Add `animfw_temporary_tags` so the character doesn't keep the tags.
- Play `animfw_restore_tags` once the fragment shows. If you don't, the next `PlayAnim` on that character does it.

```lua
player.human:PlayAnim("ADLG_AngryLeaning_In", "sitting+angryStanding+animfw_temporary_tags")
-- when GetCurrentAnimationState() shows the fragment:
player.human:PlayAnim("animfw_restore_tags", "")
```

- Only the bits that the play changed go back, so a tag the character already had stays: a seated Henry keeps `sitting`.
- A fragment's tags are read only when it starts, so taking them off doesn't touch it.
- On Henry, the tags never enter his tag state. The framework hands them only to that fragment's option choice, so even tags the game sets itself on every update work, such as `walk` and `run` (`Juke180Left` plays on a standing Henry).
- On an NPC, they go into its tag state until restored. The game may switch such a tag off before the fragment starts, which shows the bind pose.
- The option's tags are in the fragment's `Tags` and `FragTags` in the database. Leave out the `Scope_` ones.

### NPCs

On its own, the game plays it on Henry only: its `PlayAnim` checks that the entity is the player and does nothing for an NPC's `human` table. The framework runs the same play for NPCs (INI `NpcActions`), without their AI.

- An NPC standing or on patrol stops for the action, and its schedule takes over again when the action ends.
- Male NPCs share Henry's database, so a fragment you add to `kcd_male_*` plays on them too.
- Women have their own database (`wh_female_fragmentids.xml`, `wh_female_controllerdefs.xml`, `wh_female_database.adb`). Patch it the same way, with a woman's clip.
- A fragment a character's database doesn't have does nothing.
- Only NPCs near the player animate. Call it on an NPC that is loaded.

### PlayAnim or a custom tag

- Use `PlayAnim` for actions a script triggers: an emote, a gesture, a custom interaction, an NPC reaction.
- Use a custom tag for states that last (tired, wet, drunk), so the game's own idle and movement switch to your options.

`PlayAnim` needs nothing else. The game's own action system does: quest animation nodes, `PlayerStateHandler.PlayAnimationAction` and the `AnimationAction` node of NPC behaviour trees play a fragment only when it has rows in the animation action tables (next section). Otherwise they refuse it.

### Rows for the game's action system

`Libs/Tables/animation/anim_fragment.xml` and `Libs/Tables/action/actor_anim_action.xml` hold one row per actor class, fragment, option position and option GUID. The rows are the baked data the action system reads: the option's length, root motion, alignment and stance changes. A fragment of your own needs a row in each table, for every actor class that plays it, before the action system plays it.

The game cannot patch these two tables. It has no key to match rows by, and an `anim_fragment__<modid>.xml` in `Libs/Tables` stops it at startup ("Database system error - tables can't be loaded"). Ship the rows to the framework instead, in its own folder, which the game never reads:

```
Animations/Mannequin/Tables/anim_fragment__my_mod.xml
Animations/Mannequin/Tables/actor_anim_action__my_mod.xml
```

Each file is laid out like the game's table and holds only your rows:

```xml
<database name="barbora">
	<anim_fragments version="14">
		<anim_fragment actor_class_hash="..." fragment_guid="<your option's GUID>" mn_fragment_id="MyMod_Pray"
			mn_option_index="0" total_duration="6.7" ... />
	</anim_fragments>
</database>
```

- When the game loads the table, the framework adds your rows to it.
- A row with the same actor class, fragment, option position and GUID as one already there replaces it. Any other row is appended.
- The log lists each file (`AnimPatch: Libs\Tables\animation\anim_fragment.xml <- anim_fragment__my_mod.xml: 1 row(s) added, 0 replaced`).
- Tables load once, at startup, so row changes need a restart.

To write the rows, copy the rows of a game fragment that plays like yours: same body layers, similar length and movement. The game's file lists them by `mn_fragment_id`. Then change:

- `mn_fragment_id` to your fragment
- the GUID (`fragment_guid` in `anim_fragment`, `mn_fragment_guid` in `actor_anim_action`) to your option's GUID
- `mn_option_index` to your option's position
- the durations (`total_duration`, `animation_duration`) to your clip's length

Keep `actor_class_hash`. It says which kind of character the row is for, and a copied row is for the same characters as the fragment you copied.

## Mods that ship whole files

Older mods ship whole copies of game files. The framework handles them like this:

| Shipped whole by two or more mods | What happens |
|---|---|
| `.adb`, controller definitions, `.animevents` | Each copy becomes a patch of what it changed over the game's copy, merged in load order. |
| The animation action tables (`anim_fragment.xml`, `actor_anim_action.xml`) | Each copy's new and changed rows are added, in load order. Rows are never removed. |
| Tag and fragment ID lists (`kcd_male_tags.xml`, `kcd_male_fragmentids.xml`) | Each copy's new tags are added, in load order. Tags are never changed or removed, so every game fragment keeps its ID. |
| `Animations.img` | Merged. |
| `male.chr` (and other skeletons) | Merged bone by bone. |
| `.bspace`, `.comb`, a clip at the same path, anything else under `Animations/` | Not merged: the last one loaded wins, and the log lists it. |

A new mod should still ship patches. A whole copy turns into a patch of everything it differs in, including changes from the game version it was copied from.

## Testing

### The log

`KCD2_AnimationFramework.log`, beside `WHGame.dll`, shows what happened to your files:

| Line | Meaning |
|---|---|
| `AnimPatch: <database> <- <your patch>: options replaced 1, appended 0, ...` | Your patch merged. |
| `AnimPatch: ... did not load` | Your patch is not valid XML. |
| `AnimPatch: ... overrides ... on N key(s)` | Two patches set the same key. The later one wins. |
| `AnimClips: new clip <path> (...)` | Your clip registered. |
| `AnimClips: <path> replaces <path> (...)` | Your clip replaces a game clip. |
| `AnimClips: <path> (from <pak>) is not a compiled clip; skipped` | The file is not a compiled `.caf`. |
| `AnimCompat: <file> from <mods>: not merged; ...` | Two mods ship the same file whole, and it can't merge. |
| `SkeletonMerge: <skeleton>: merged N mods' copies bone by bone: ...` | Skeletons merged, with each mod's bones. |
| `AnimPatch: Libs\Tables\animation\anim_fragment.xml <- anim_fragment__my_mod.xml: N row(s) added, N replaced` | Your table rows merged. |
| `CustomTags: my_mod_tired on` | Your custom tag turned on. |
| `AnimClips: <path> (from <pak>) keys BackSlot_bow, ...` | Your clip keys a back slot that skeleton mods move (see [What a clip should key](#what-a-clip-should-key)). |
| `AnimClips: ... keys N bone(s) the skeleton lacks` | Your clip was exported for another skeleton, or with renamed bones. |

### The DEV folder

Files in `<game>\Bin\Win64MasterMasterSteamPGO\KCD2_AnimationFramework_Dev\` load without repacking:

- **Patches** placed there loose merge after every mod's. They are read from disk each time a database loads: edit one and run `mn_reload` in the console to see the change.
- **Paks** placed there mount at startup like a mod. The game keeps a mounted pak open until it exits, so give each test build a new pak name.

### Clips without a restart

1. Put compiled clips loose under `KCD2_AnimationFramework_Dev\Animations\`, at the path they will have in your mod (`KCD2_AnimationFramework_Dev\Animations\humans\male\my_mod\my_mod_wave.caf`).
2. Run `animfw_reload_clips` in the console.

- A new clip is added and plays by its name.
- A clip you exported again (or one that replaces a game clip) is reloaded in place, even while it plays.
- Loose blend spaces (`.bspace`, `.comb`) there are picked up too. A new one reloads the animation sets.
- The game reads clips from paks, so the framework packs the loose files into a pak of its own under `KCD2_AnimationFramework_Dev\loose_clips\`. Those paks are deleted at the next start.
- The same command mounts paks you drop into the DEV folder while the game runs. It also reloads the event lists when an `.animevents` patch in the DEV folder is new or changed.
- A loose clip you delete stays in the game until a restart.

### Checking a clip

`animfw_check_clip <clip> [reference] [loop]` plays a clip on Henry's skeleton and prints to the console what would go wrong in game. The clip is its file name without `.caf` (or its path): yours, or any of the game's, also those packed in `.dba` files.

```
animfw_check_clip my_mod_idle relaxed_idle_both_player loop
```

- Always: its length and keyed bones, bones Henry's skeleton lacks, and the back slots that skeleton mods move.
- With a reference (the game clip your option takes the place of): the bones it keys that yours leaves out, which fall to the bind pose on a full-body option.
- With `loop`: whether the last pose meets the first (twist bones and the engine's IK and pose helpers aside). Without it, a large difference is only noted.
- Its root motion against Henry's walk (1.40 m/s), run (3.57) and sprint (6.23), and how fast planted toes slide (the game's walks and runs: 1 to 13 cm/s).

The framework also checks every mod clip under `Animations/humans/male/` as it loads it, for the skeleton's bones and the back slots. It writes one log line per mod pak and finding, with the number of clips and the first of them (`LogLevel=DEBUG` lists every clip).

### What needs a restart

- `mn_reload` reloads the databases, tag and fragment ID lists and controller definitions, with every patch merged again. A new fragment can be added this way.
- `animfw_reload_clips` reloads the clips (above).
- Blend spaces, skeletons and the action table rows load once per session, so changes to them need a restart.

## Making a mod in the game

Three console commands turn a clip into a finished mod, with no file to write by hand:

1. Export your clip into `KCD2_AnimationFramework_Dev\Animations\humans\male\my_mod\` and run `animfw_reload_clips`.
2. Run `animfw_make_mod my_mod MyMod_Wave my_mod_wave ADLG_Wave`. It adds an action: a new fragment, `MyMod_Wave`, that plays the clip `my_mod_wave`, set up like the game's `ADLG_Wave` (the body layers it takes over, its blend, and procedural layers such as putting the weapons away).
   - It checks the clip against the clip the game fragment plays, writes the three patches to the DEV folder and reloads the databases. `player.human:PlayAnim("MyMod_Wave", "")` then plays it at once.
   - Add `loop` for a clip that loops until stopped.
   - Add `upper` for a clip that animates only the arms or head (a gesture). The action then plays on the upper body over what Henry is doing, through a blend space that keeps the chest up. Play it with `player.human:PlayAnim("MyMod_Wave", "animfw_own_scopes")`. If your clip has sounds, list them for that blend space too (`my_mod_u_my_mod_wave.bspace`, see [Sounds come from the name an option plays](#sounds-come-from-the-name-an-option-plays)).
   - Without `upper`, a full-body action puts every bone the clip leaves out in the bind pose. Exporters such as Bohemian Animator's (through `rc.exe`) drop every bone the animator never moved.
   - Run it again to change the action, or with another name to add one.
3. Run `animfw_pack_mod my_mod`. It writes the finished mod to `KCD2_AnimationFramework_Dev\packed\my_mod\`: its `mod.manifest`, and `Data\my_mod.pak` with the patches and every loose clip in a `my_mod` folder. Install it like any mod, then delete its patches and clips from the DEV folder so they do not load twice.

### Without the game running

`animfw.exe`, the framework's command line, does the same from a command prompt. It reads the game from its paks (the Steam install, or `--game <folder>`) and works on your mod's source folder in the usual layout, named after the mod id:

```
animfw make D:\Mods\my_mod MyMod_Wave D:\Export\my_mod_wave.caf ADLG_Wave
animfw pack D:\Mods\my_mod "D:\MO2\mods\My Mod\my_mod"
```

It sees the game as it ships, not other mods' changes. To try an action in game, run the in-game commands above, or install the packed mod. Run `animfw` without arguments for every command and option.

- `make <mod folder> <new fragment> <clip> <game fragment> [--loop] [--upper]` copies the clip into `Data\Animations\humans\male\my_mod\`, checks it, and writes the patches into `Data\Animations\Mannequin\ADB\` and a `mod.manifest`. A refused action leaves the folder as it was.
- `pack <mod folder> <install folder>` writes the mod as installed: `mod.manifest`, `Data\my_mod.pak` with everything under the source's `Data\` except `Levels\`, `Data\Levels\<level>\my_mod.pak` with each level folder (a cutscene's layer), and `Localization\<language>_xml.pak` with each `Localization\<language>_xml\` folder beside `Data\` (dialogue text). It deletes the mod's paks that an earlier pack left and the source no longer fills. Give it a mod manager's mod folder, or the game's `Mods\my_mod`.
- `actions <mod folder>` lists the mod's actions.
- `check <clip> [reference] [--loop]` checks a `.caf` file or a game clip. It exits with 2 when it warns, so a build script can stop on it.
- `fragments [text]` lists the game's fragments, to pick the one to copy.
- `clips <text>` lists the game's clips whose path holds the text.
- `compile <file.i_caf> <file.caf> [--loop|--no-loop] [--additive] [--overlay] [--compression <n>]` compiles an intermediate animation (what Blender exporters write for `rc.exe`) into a game clip without `rc.exe`, then checks it.
  - Unlike `rc.exe`, it keeps every bone, also the ones the animator never moved, so a full-body clip keeps its pose. Only the back slots and the right weapon root stay out unless the clip moves them, so skeleton mods keep their placement.
  - `--overlay` drops the unmoved bones as `rc.exe` does, for overlay clips.
  - It moves the root to start at the origin facing forward, and measures speed, distance and turn the way `rc.exe` does.
  - It flags the clip as looping when every bone's rotation ends where it starts. `--loop` or `--no-loop` decide instead.
  - `--additive` stores each frame's change from the first.
  - `--compression` is `rc.exe`'s value: 10 by default, which drops keys the others give within 0.32 mm and 0.036 degrees. 0 drops exact ones only.
- `retarget <file.bvh|file.fbx> <file.caf>` puts an animation made on another skeleton on Henry's and compiles it (next section).

A missing or bad option value stops every command with exit code 1 and names the option.

#### Animations from other skeletons

`retarget` takes animations made on other skeletons:

- BVH files: CMU, 100STYLE, Rokoko and other capture tools.
- FBX files: Mixamo, Meshy and SMPL exports, Unreal's mannequin, Blender exports.

It matches bones by name (`LeftArm`, `L_Shoulder`, `upperarm_l`, `mixamorig:LeftArm` and the like) and prints each match, so check the list. `--map LeftArm=<joint>` fixes a match, and `--map LeftShoulder=` leaves a bone out.

```
animfw retarget D:\Mocap\walk.bvh D:\Mods\my_mod\Data\Animations\humans\male\my_mod\my_mod_walk.caf --from 12 --to 15.5 --root in-place
```

- **Proportions:** Henry keeps his own. Each limb follows the source limb's turns, and the pelvis height scales with leg length.
- **Fingers:** they take a stock idle's pose when the source has none. `--hands <game clip>` picks another clip, and `--hands none` keeps the default pose.
- **Reference pose:** the source's rest pose (a T-pose or an A-pose). When the rest pose is not a plain stance (Meshy's SMPL rest tilts the head back), name a frame where it stands still: `--calibrate <frame>`.
- **Span and facing:** capture takes often start and end with a T-pose, and wander about the room. `--from` and `--to` keep seconds of it. The clip is turned to start facing forward, and `--keep-facing` keeps the source's facing.
- **Root:** `--root path` (default) moves the root along the pelvis's smoothed path. `line` moves it at one steady speed, for looping locomotion. `in-place` leaves no travel at all, for a cycle that walks on the spot. `--root-turns` turns the root with the body.
- **Feet:** it reports how far the planted feet slide and how low they get. A source whose feet slide or float does the same on Henry. `--feet lock` holds each planted heel and ball of the foot in place with leg IK, for sources whose feet skate. It costs the knees some smoothness, and retargeted feet usually slide no more than the game's own clips (2 to 7 cm/s), so it is off by default.
- **Wrist twist:** the hand's twist is spread over the forearm's roll bones (a quarter, a half and three quarters of the way down), so a turned wrist does not pinch the skin into a ring. The game's own clips leave them still, and so does `--no-twist`.
- **Palms and soles:** they line up with Henry's. A source whose rest pose holds the palms another way, or tilts the feet, still puts Henry's hands and feet the right way round.
- **Women:** `--skeleton female` makes the clip for the women's skeleton, for a female NPC or a double in a cutscene. Save it under `Animations\humans\female\`, where the women's clips load from.

### AnimFW Studio

`animfw-studio.exe` does all of it in a window:

- Pick the mod's folder, then drop a `.caf` on the window, or open one with the Studio.
- The clip plays on a mannequin of the game's own skeleton. The reference clip plays beside it in blue, and the bones the clip leaves to the bind pose show in red.
- Its tabs check the clip, make an action (with a search of the game's fragments), pack the mod, retarget a `.bvh` or `.fbx` and browse the game's clips. A retargeted clip plays on the mannequin.
- Drag the view to orbit it, and use the wheel to zoom.

### With an AI agent

`animfw mcp` runs a Model Context Protocol (MCP) server. An AI agent connected to it can find a game fragment to copy, check your clips, make the actions and pack the mod from what you ask.

This was added on request and has had little testing. Check what it makes before you release a mod.

Add it to your MCP client's server list. Most clients take an entry like this:

```json
{
  "mcpServers": {
    "kcd2-animation": {
      "command": "C:\\Tools\\animfw.exe",
      "args": ["mcp"]
    }
  }
}
```

- Add `--game <folder>` after `mcp` when the game is not the Steam install.
- Its tools: `find_game_fragments`, `find_game_clips`, `check_clip`, `retarget_clip`, `compile_clip`, `make_action`, `list_mod_actions`, `pack_mod`.

### From Blender

`KCD2_AnimationFramework_Blender.zip` is a Blender add-on (4.0 or later) for the armatures that [Bohemian Animator](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3639) (SirVanir's Blender add-on) builds from the game's skeletons. Animate with Bohemian Animator's tools, then compile, check and play from this add-on.

- Install it with **Edit > Preferences > Add-ons > Install from Disk**. It carries `animfw.exe`.
- It finds the game in Steam. For another install, set the game's folder in the add-on's preferences.
- Its panel is in the 3D view's sidebar, on the **AnimFW** tab.

The panel has four parts:

- **Mocap:** **Retarget mocap** picks a `.bvh` or `.fbx` and puts it on Henry's skeleton (as `retarget` above, with its root, span, reference frame and take settings). It keys the result onto the selected armature as a new action, through Bohemian Animator's import, ready to clean up, export or play.
- **Clip:** the mod id, the clip's name, the frames (the action's keys or the scene's range), the kind (full body, overlay or additive, as `compile` above), loop, compression, and where the clip goes. **Export clip** samples the armature at the game's 30 frames a second, whatever the scene's frame rate. It compiles the clip with `animfw.exe` (no `rc.exe`) and writes the check into the **AnimFW Report** text.
- **Action:** the new fragment's name and the game fragment it is set up like (the search button lists the game's fragments). **Play in game** exports into the DEV folder, reloads the game's clips, makes the action and plays it on Henry. Change a key, press it again, and Henry plays the new version in seconds. It needs the game in a world. With `BlenderBridge` off in the INI, or a framework older than 1.1.0, it goes through the [KCD2 Lua runner](https://github.com/yobson1/kcd2lua) (`vscodelua.asi`). **Stop** ends it.
- **Release:** **Make action** exports the clip into your mod's source folder and adds the action to it. **Pack mod** writes the installable mod, as `make` and `pack` above. Delete the action's patches and clip from the DEV folder once the packed mod is installed.

Notes:

- Play in game uses `animfw_make_mod`, so the action plays as soon as the databases reload.
- While Henry holds a torch, one-shot actions set up like `ADLG_Wave` end almost at once (the game's own `ADLG_Wave` does too). Test with empty hands, or as a loop.
- Pick the game fragment by what your action does: a gesture (`ADLG_Wave`), a full-body action that stops Henry (`Piss`), a looping state. The catalog lists them (`animfw_catalog_find`).
- The made option has no tags, so it plays for Henry and for male NPCs alike.
- Neither Blender nor `animfw_make_mod` writes rows for the game's action system (quest nodes, NPC behaviours). Copy them as [Rows for the game's action system](#rows-for-the-games-action-system) shows.

## Listing animations from a script

A picker needs no list of its own. The framework reads Henry's fragments and each of their options from the game's files and the mods' patches, the first time a script asks (about half a second, once per session). The list follows game updates and other mods.

Game Lua cannot call the framework, so a search goes through a console variable. The answer is there on the next line of the script:

```lua
System.SetCVar("animfw_catalog_text", "wave")                   -- what to find, in any field, any case; empty finds all
System.ExecuteCommand("animfw_catalog_find all 0 20")           -- list, first match (from 0), how many, up to 200
local answer = System.GetCVar("animfw_catalog_text")            -- how many match in all, then a line each

System.ExecuteCommand("animfw_catalog_line ADLG_Wave")          -- one fragment's line, empty when Henry has none
System.ExecuteCommand("animfw_catalog_hide MyMod_")             -- leaves your internal fragments out of searches, up to 64 prefixes
```

- The list is `fragments`, or `all` to add each option after the fragments.
- A fourth word, `seat` or `horse`, keeps what has a version there.
- Fields are separated by tabs:
  - **A fragment:** its name, the tags it plays on standing, `partial` when it leaves out the full body, `seat:<tags>` and `horse:<tags>` when it has an option there, `women` when the women's database has it.
  - **An option that plays a clip:** `@` and its fragment, its first clip, its tags, its number among the fragment's options with those tags (for `animfw_option_<n>`), its set (`stand`, `seat`, `horse` or empty), `partial`, the rest of its clips.
- When two mods each ship a whole copy of one database, the game merges them, but the list shows only the last copy.

## Keeping settings from a script

Game Lua cannot touch files, so the framework keeps a mod's settings for it: one text file in the mod's own folder, `Mods/<mod id>/<mod id>.settings`. The text goes through a console variable, and the commands run at once:

```lua
System.SetCVar("animfw_settings_text", "volume 3\nfights 1")
System.ExecuteCommand("animfw_settings_save my_mod")

System.ExecuteCommand("animfw_settings_load my_mod")
local text = System.GetCVar("animfw_settings_text")   -- empty when the mod has no file yet
```

- The text is yours to lay out, up to 1 MB. A larger file loads as empty text, and the log says why.
- Under Mod Organizer 2, a new file lands in the Overwrite folder, which the game still sees in the mod's folder.

## Cutscenes

Your own in-game cutscenes play through the game's cutscene player, as its quests start them: letterbox, fades, skipping, voices, subtitles and Henry's double included. Copy one of the game's (`animfw clone-cutscene`), edit one in the game's Sandbox editor and export it with one click, or write one by hand, then play it from a script or a quest. The [cutscene guide](CUTSCENES.md) has the setup, the Lua and quest side, and the rules a scene must follow.

## Checklist

- Patches named `<game file stem>__<modid>`, next to the file they patch.
- Only the options, blends and events you change, with no copies of the game's others.
- On-screen options are one looping clip.
- New clips and blend spaces under your own names and folders, never at the game's path unless you mean to replace that file.
- Clips key no `BackSlot_bow`, `BackSlot_crossbow`, `BackSlot_shield`, `BackSlot_cannon` or `RightWeaponRoot` tracks. Overlays key only what they animate.
- Every new clip, blend space and `.comb` an option plays has the sounds of the game animation it stands in for, in your `.animevents` patch (footsteps come from the name the option plays).
- The log lists each of your files as merged or registered, with no warnings about them.
