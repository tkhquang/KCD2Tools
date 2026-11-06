# KCD2 Animation Framework

Lets Kingdom Come: Deliverance II animation mods work together instead of overwriting each other, and lets them add or replace animation clips without shipping the game's whole clip index. Requires the Steam version of the game.

## What it does

- **Mannequin patches.** A mod ships `<database>__<modid>.adb` holding only the fragments, blends and sub-databases it adds, replaces or removes, and the framework merges it into the database the game loads. `clipswaps__<modid>.xml` renames clips in every fragment. When two patches set the same key, the log names the one that wins.
- **Controller definitions and animation events.** `<stem>_controllerdefs__<modid>.xml` sets a fragment's scopes and flags (which body layers an action takes over), replaces or removes its per-tag `Override`s and adds scopes; `<stem>__<modid>.animevents` beside an event list replaces, extends (`Mode="append"`) or removes one clip's sounds, footsteps and effects.
- **Whole-file mods together.** Mods that ship whole copies of the same database (First Person Overhaul, Greatswords, True Combat Overhaul), controller definition, tag or fragment ID list (Very Important Activities' `kcd_male_fragmentids.xml`) or animation event list (Swords on the Back's `male.animevents`) are turned into deltas against the game's copy and merged, so none of them is lost. Every other animation file two mods ship whole (a blend space, a clip) is listed in the log with the mod whose copy loads.
- **Skeletons together.** Mods that ship whole copies of the same skeleton (`.chr`) to move a few bones, such as the back slots Swords on the Back and Realistic Bow and Crossbow Position move, are merged bone by bone: each bone a mod moved is kept, a bone two mods move differently goes to the later one (logged), and transforms that differ only by float noise count as the same. A copy whose chunks or bone list differ from the game's is not merged, and the newest copy loads as it is.
- **Animation action tables.** The game plays a fragment through its own action system (quests, NPC behaviours, `PlayerStateHandler`) only when `anim_fragment.xml` and `actor_anim_action.xml` hold rows for it, and it cannot patch those two tables. A mod ships its rows as `Animations/Mannequin/Tables/<table>__<modid>.xml`, and the framework adds them when the game loads the table; whole copies several mods ship are merged row by row.
- **Animations on NPCs.** The game's `entity.human:PlayAnim(fragment, tags)` / `StopAnim()` work on NPCs too, not only on Henry: the fragment plays on the NPC's own Mannequin controller, and the NPC's schedule takes over again when it ends. The [modding guide](docs/MODDING.md#playing-an-animation-from-a-script) has the details.
- **Clips.** Every `.caf` a mod pak carries under `Animations/` registers in the game's clip index, built from the clip itself. A clip the game already has is replaced, including the ones the game keeps in `.dba` packs. Mods that ship whole `Animations.img` copies are merged. While you make a clip, the console command `animfw_reload_clips` picks up a new or re-exported `.caf` without a restart, and `animfw_check_clip` prints what would break it in game (see [Testing](docs/MODDING.md#testing)).
- **Mod maker.** `animfw_make_mod` turns a clip into a new action set up like a game fragment, and `animfw_pack_mod` writes the finished mod (see [Making a mod in the game](docs/MODDING.md#making-a-mod-in-the-game)). `animfw.exe`, the command line, does the same, and the clip check, without the game running. AnimFW Studio (`animfw-studio.exe`) does it in a window with a mannequin preview, and `animfw mcp` serves it to AI assistants (MCP).
- **Clip compiler, retargeting and Blender add-on.** `animfw compile` turns a Blender export (`.i_caf`) into a game clip without `rc.exe`, keeping the bones `rc.exe` drops, and `retarget` puts motion capture and other rigs' animations (BVH, FBX: Mixamo, SMPL, Unreal, CMU, 100STYLE) on Henry's skeleton. The Blender add-on (`KCD2_AnimationFramework_Blender.zip`, for [Bohemian Animator](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3639)'s armatures) compiles and checks the action, and **Play in game** shows it on Henry in seconds through the KCD2 Lua runner (see [From Blender](docs/MODDING.md#from-blender)).
- **Third-person animations.** An option tagged `thirdPerson` plays for Henry only while a camera mod shows him from outside (below); in first person the game's own option plays.
- **Custom tags.** A mod declares its own tag (a state the game does not tag, such as tired or wet) that turns on through a console variable any script can set; options tagged with it play for Henry while it is on. The INI ranks the tags and can switch any of them off.

[Sprint Start Fix](../SprintStartFix/) is a complete patch mod: one small `.adb` that makes Henry's sprint start in step with his run. The [modding guide](docs/MODDING.md) covers every file format, what a clip should key so it doesn't undo other mods, and how to test.

## Installing

Install one of the two builds, not both:

- **KCSE build** (the default): with [Kingdom Come Script Extender](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3332), install it with a mod manager or extract it into `<game>\Mods\`. The plugin and its INI land in `Mods\animation_framework\KCSE\Plugins\`.
- **ASI build**: put `KCD2_AnimationFramework.asi` and `KCD2_AnimationFramework.ini` in `<game>\Bin\Win64MasterMasterSteamPGO\`, beside an ASI loader.

The INI beside the mod switches each feature; changes apply on the next game start. Both builds write `KCD2_AnimationFramework.log` in `<game>\Bin\Win64MasterMasterSteamPGO\`, next to the DEV folder.

## Third-person animations

The supported camera mod is [Proper Third Person View (TPV Camera)](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3263), which publishes the view in `mod_camera_view`. Another third-person mod is supported once it publishes `mod_camera_view` itself, following the rules below.

The framework adds a `thirdPerson` tag to Henry's tag definition (`kcd_male_tags.xml`). In a `.adb` patch, give an option the game's tags plus `thirdPerson`, for example `Tags="walk+player+thirdPerson"` next to the game's `walk+player`, with its own GUID. While `mod_camera_view` is 1 or more, Henry's option queries carry the tag, so the more specific option wins; in first person the game's option plays. Switching the view while walking crossfades to the other option in step. The tag never enters the game's own tag state, and NPCs, which share Henry's database, are unaffected.

A third-person option only replaces the option the game would pick: Henry's query runs with and without the tag, and the third-person answer is taken only when its tags include every tag of the game's answer. So `walk+player+thirdPerson` replaces the game's `walk+player`, while walking backward still plays the game's `walk+backward+player` (or another mod's change to it). Patch only the options you change; never copy the game's other options to protect them.

An option meant to stay on screen (an idle, a locomotion cycle) must be one looping clip. A chain of one-shot clips holds the fragment until its last clip starts, so a change such as crouching waits for the whole chain; the log warns about such options.

## Custom tags

`thirdPerson` is the framework's built-in custom tag. A mod declares more in its patch to Henry's tag definition, `Animations/Mannequin/ADB/kcd_male_tags__<modid>.xml`, as a plain tag (outside any group) with an `animfw_cvar` attribute:

```xml
<TagDefinition version="2">
	<Tags>
		<Tag name="mymod_tired" animfw_cvar="mymod_tired" animfw_min="1" animfw_priority="0"/>
	</Tags>
</TagDefinition>
```

- `animfw_cvar`: the integer console variable that turns the tag on. The framework finds it or registers it (value 0) once the console is up, so any Lua script (`System.SetCVar("mymod_tired", 1)`), menu or the console can set it.
- `animfw_min` (default 1): the tag is on while the variable is at least this.
- `animfw_priority` (default 0): ranks the tag against the other tags; higher ranks first.

A name the game's own definition (or the framework) already has is refused, and the log says so. Use the game's tags where they exist (stealth, drunk, injured, carried items): custom tags are for states the game does not tag.

Options use the tag like `thirdPerson`: `Tags="walk+player+mymod_tired"` replaces the game's `walk+player` while the tag is on. The same rule holds: a tagged option only ever replaces the option the game picks. Several tags can be on at once. Henry's query tries combinations of the active tags, highest-ranked first: every combination holding the top tag before any without it, the most complete first, so `walk+player+mymod_tired+thirdPerson` beats either single-tag option, and of `walk+player+mymod_tired` and `walk+player+thirdPerson` the higher-ranked tag's option plays. Up to 4 active tags combine per query; transitions follow the same rule.

The INI has the final say over the ranking:

```ini
[CustomTags]
; Highest first; these rank above every other tag.
Priority=mymod_tired, thirdPerson
; Tags that never turn on.
Disabled=
```

The log lists the ranked tags (`CustomTags: generation N: ranked highest first: ...`), each tag's variable, and each change of a tag (`CustomTags: mymod_tired on`). Like `thirdPerson`, custom tags are Henry's only; NPCs never see them.

## `mod_camera_view`

A console variable any mod can read to know whether the player sees Henry from outside.

| Value | Meaning |
|---|---|
| 0 | First person (the game's default) |
| 1 | Third person |
| 2 and up | Reserved. Treat any value of 1 or more as "the body is seen from outside". |

Rules for every mod that uses it:

- Look it up first (`GetCVar`) and register it only when it is missing (`RegisterInt`, value 0, no flags). Whichever mod loads first creates it; nobody owns it.
- The console keeps a variable's name and help pointers as given, so pass copies that outlive your module, and set no change callback.
- Only the active camera mod writes it (Proper Third Person View does), and writes 0 when it turns off or unloads.
- Register, set and read it on the main thread. Read it once per frame; never cache the value.

From Lua: `System.GetCVar("mod_camera_view")`.

## Building

MSVC (Visual Studio 2022) and CMake 3.28. `external/DetourModKit` is DetourModKit v4.5.0.

```
cmake --preset msvc-release
cmake --build --preset msvc-release
ctest --test-dir build/release-msvc -C Release
```

The release build makes `KCD2_AnimationFramework.asi` and `KCSE\KCD2_AnimationFramework.dll`, the KCSE plugin, from the same code.

The unit tests cover the game-independent code (clip, skeleton and zip files, database merges, tag rules, the clip compiler, the mod maker and retargeting); `-DANIMFW_BUILD_TESTS=OFF` skips them. The `msvc-dev` preset builds a resident loader ASI and a logic DLL that hot reloads with Numpad 3. Set `-DANIMFW_GAME_DIR=<game>\Bin\Win64MasterMasterSteamPGO` to deploy into the game. Loose test patches and paks go in `<game>\Bin\Win64MasterMasterSteamPGO\KCD2_AnimationFramework_Dev\`.
