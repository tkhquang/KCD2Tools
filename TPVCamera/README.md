# Kingdom Come: Deliverance II - Third Person Camera

A third-person camera mod for Kingdom Come: Deliverance II.

## Overview

TPVCamera adds camera collision and frustum-culling correction, a partial raycast-based
crosshair fix, and an experimental free-look orbit mode.

Third-person turns on automatically once you reach gameplay (menus and loading stay first-person).
Press the toggle hotkey to switch back at any time, or set `AutoEnableTPV = false` in the INI to
start in first-person instead.

## Features

- Third-person view you can toggle on and off at any time
- Over-the-shoulder framing with adjustable distance, height, and side offset
- Zoom in and out on the fly
- Basic free-look orbit to look around your character (early; see Known Limitations)
- Henry turns on the spot with the game's own turn-in-place animations when you look around while standing still, and the camera stays put; switched off automatically in combat, aiming, riding, conversations and minigames
- The camera follows Henry's upper body through his own animations (sitting, praying, bending) and eases after the turns the game makes for him, so actions no longer jolt the view
- Works with the game's combat lock-on: Henry turns to the enemy it picks, the camera keeps that enemy in view beside Henry, and after a kill the lock moves to the next enemy your camera points at
- Automatic view switching by situation: switch to first or third person when you enter combat, aiming a bow, dialogue, minigames, riding, or menus (you can still toggle manually during it), with your previous view restored when the situation ends
- In-game preset manager: edit, add, and save camera presets from an overlay; built-in presets for normal play, combat, aiming, horseback, sneaking, and special poses (lying down, sitting, kneeling, riding a cart) apply automatically by situation, and you can bind your own presets to combinations of states (such as aiming while crouched), with the most specific match winning
- Several presets per situation, switched in game with a key; Close Shoulder (a tight over-the-shoulder camera) and Aiming Wide (the everyday framing while aiming) ship ready to use
- Free-look look sensitivity is set per axis for both mouse and gamepad, so horizontal and vertical can be tuned independently; set a negative value to invert that axis. Mark any preset setting as Shared to apply its value to every preset at once
- Camera collision that keeps its distance: posts, fences, open carts, lamps, furniture and hanging decorations between the camera and your character fade out instead of pulling the camera in
- The ground, rocks, buildings, plank decks and walls, fences too tall to see over, high-sided wagons, tall shelves and haystacks still stop the camera, so inside a house it stays in the room
- Trees, bushes and tall grass in front of your character open a dithered window around it, only where they hide it; the camera passes through bushes and trunks, and leaves right at the lens are cut away
- When there is no room behind your character (a low doorway, a wall right behind), the character fades out with everything it wears and holds instead of the view jumping to first person
- Camera rules decide, folder by folder or model by model, which objects stop the camera, fade, open in the window or are left alone, a mod's own models included
- The camera looks where you look, like first person carried back over your shoulder, so the crosshair stays on what you walk toward and zooming moves the camera straight in and out
- Full keyboard and XInput controller support
- Almost every setting is editable live while the game runs

## Installation

This is an `.asi` plugin and requires an **ASI loader**, which is **not bundled** in the download.

### Find the installation folder

Open your KC:D 2 installation folder, then open the `Bin/Win64MasterMaster...` subfolder that contains **`WHGame.dll`**. On Steam this is:

```text
<KC:D 2 installation folder>/Bin/Win64MasterMasterSteamPGO/
```

Other stores use their own `Bin/Win64MasterMaster...` folder; use `WHGame.dll` to identify the correct one. The loader and mod files go directly into this folder.

If you previously installed **KCD2_TPVToggle**, remove `KCD2_TPVToggle.asi` and `KCD2_TPVToggle.ini` first. TPVCamera replaces it, and running both camera mods at once will conflict. To keep a backup, move the old ASI outside the game folder or rename its extension to `.asi.bak`.

### Step 1: Install an ASI loader (once)

If you already have a working ASI loader for KC:D 2, skip to Step 2. The same loader can load TPVCamera, Henry's Senses and other ASI mods.

Download **one x64** variant of [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) by ThirteenAG:

| Download | When to use it |
| --- | --- |
| [dinput8-x64.zip](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/dinput8-x64.zip) | Usual choice if `dinput8.dll` is unused. |
| [version-x64.zip](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/version-x64.zip) | Alternative when `dinput8.dll` is already used, including by KCSE. |
| [winmm-x64.zip](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/winmm-x64.zip) | Another alternative if `version.dll` is also used or does not load. |

Extract the DLL from the chosen ZIP directly beside `WHGame.dll`. Use the **x64** build, not Win32, and install only one Ultimate ASI Loader variant. Do not overwrite a DLL belonging to another mod.

> **Using [Kingdom Come Script Extender (KCSE)](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3332)?** KCSE uses `dinput8.dll`. Keep KCSE's file and install Ultimate ASI Loader as **`version.dll`** or **`winmm.dll`** in the same folder. Choose an unused name from the downloads above; do not rename or replace KCSE's DLL.

### Step 2: Install the mod

Download the TPVCamera release archive from [GitHub Releases](https://github.com/tkhquang/KCD2Tools/releases) or the mod's Nexus Files tab. Extract its contents directly into the **same folder** beside `WHGame.dll` and your loader DLL, without an extra archive-named subfolder.

- `KCD2_TPVCamera.asi` is the mod.
- `KCD2_TPVCamera.ini` contains hotkeys and global settings.
- The camera rules, which set how the camera treats each object in the world, are built into the ASI. Your own changes go in `KCD2_TPVCamera_rules_User.txt`, which you create (see [Camera rules](#camera-rules)).
- `KCD2_TPVCamera_presets.json` is created on first run; it is not included in the archive.

TPVCamera does not use the `Mods` folder.

```text
Bin/Win64MasterMasterSteamPGO/
|-- WHGame.dll                         (game file, already present)
|-- dinput8.dll                        (ASI loader; see KCSE note above)
|-- KCD2_TPVCamera.asi                  (this mod)
|-- KCD2_TPVCamera.ini                  (settings)
|-- KCD2_TPVCamera_rules_User.txt       (your own camera rules; optional, you create it)
|-- KCD2_TPVCamera_presets.json         (created on first run)
\-- KCD2_TPVCamera.log                  (created when the mod loads)
```

With KCSE, `dinput8.dll` in this example belongs to KCSE; your chosen ASI loader, `version.dll` or `winmm.dll`, sits alongside it.

### Step 3: Launch and verify

Launch the game; third-person turns on automatically once you reach gameplay. Press **F3** or hold **LB + RB** on a controller to toggle back to first-person.

Check for `KCD2_TPVCamera.log` beside the ASI. If it is missing, check that the ASI and x64 loader DLL are directly beside `WHGame.dll`, then try another unused loader variant and relaunch. Replace only the Ultimate ASI Loader DLL you installed; keep KCSE and other mods' DLLs. On Wine/Proton, also check the override below.

When updating, back up your customized INI before extracting the new archive, then reapply your settings to the supplied INI. Keep `KCD2_TPVCamera_presets.json` to retain your saved camera presets. An update brings its camera rules built into the ASI and never touches your own `KCD2_TPVCamera_rules_User.txt`. If you replaced the built-in rules with a `KCD2_TPVCamera_rules.txt` of your own, that file stays in force, and the rules of an update reach you only once you delete it (see [Camera rules](#camera-rules)).

### Linux / Steam Deck (Wine/Proton)

Add an override for your chosen loader DLL in the game's **Properties -> Launch Options** on Steam:

```text
WINEDLLOVERRIDES="dinput8=n,b" %command%
```

Use `version=n,b` or `winmm=n,b` instead when using that loader name. If you also use KCSE's `dinput8.dll` with the `version.dll` loader, retain both overrides:

```text
WINEDLLOVERRIDES="dinput8,version=n,b" %command%
```

For a command-line launch, set the same `WINEDLLOVERRIDES` value before your launch command. Keep any overrides your other mods need.

## Uninstall

1. In the folder beside `WHGame.dll`, delete every file whose name starts with `KCD2_TPVCamera`: the ASI, the INI, the rules files (your own, other mods' patches and an exported copy included), the presets, the logs and any signature files. Delete the ASI loader DLL only if no other mod uses it.
2. Optionally, delete the shaders TPVCamera wrote to the game's shader cache, usually `%USERPROFILE%\Saved Games\kingdomcome2\shaders\cache\d3d12\` (the log line `ShaderTwins: ... twin file(s) in <folder>` names it): the files whose names start with `kcd2_tpvcamera_`, in that folder and in its `cg...shaders` subfolders. Leave the rest of the folder alone: it is the game's own shader cache, and deleting it only makes the game compile its shaders again.

## Controls

All keys are rebindable in `KCD2_TPVCamera.ini`. Defaults:

| Action                     | Keyboard            | Controller             |
| -------------------------- | ------------------- | ---------------------- |
| Enter / exit third-person  | `F3`                | hold `LB` + `RB`       |
| Force first-person         | (unbound)           | (unbound)              |
| Force third-person         | (unbound)           | (unbound)              |
| Toggle free-look orbit     | `F4`                | hold `LB` + click `LS` |
| Hold for free-look orbit   | (unbound)           | (unbound)              |
| Open preset manager        | `Home`              | (unbound)              |
| Zoom in                    | `LShift + PageUp`   | hold `LB` + D-pad up   |
| Zoom out                   | `LShift + PageDown` | hold `LB` + D-pad down |
| Cycle to next preset       | (unbound)           | (unbound)              |
| Cycle to previous preset   | (unbound)           | (unbound)              |
| Swap shoulder              | (unbound)           | (unbound)              |

Free-look orbit has two styles: `OrbitToggleKey` (default `F4`) latches it on and off, while
`OrbitHoldKey` (unbound by default) is a momentary "freelook" you hold to look around and release
to snap straight back to the aim camera. Bind `OrbitHoldKey` in the INI to use it.

Presets bound to exactly the same situations form a cycle group, such as DEFAULT with the shipped
Close Shoulder, or AIMING with Aiming Wide. `NextPresetKey` / `PreviousPresetKey` switch between
them, for example `Numpad6,Gamepad_LB+Gamepad_DpadRight` / `Numpad4,Gamepad_LB+Gamepad_DpadLeft`.
The overlay shows each group and lets you pick one with a click. In an existing setup, press
Reset all in the overlay to get the shipped presets. Your own presets are kept. The preset you
switch to and the shoulder you swap to are saved right away; set `AutoSaveCyclePick` /
`AutoSaveShoulder` to `false` to keep them for the session only, unless you press Save.

On a controller the zoom shares the D-pad with the game's inventory/map shortcut, so the mod hides
that D-pad button from the game while you zoom (the `ZoomInKey.Consume` / `ZoomOutKey.Consume` keys,
on by default) so finishing a zoom does not open the inventory or map. Set them to `false` if you
would rather the D-pad keep reaching the game. The keyboard zoom keys are unaffected.

## How It Works

The camera hooks the engine's frustum builder and offsets the game view camera's matrix there,
before the cull planes are computed, so the rendered view and its culling move together (this is
what keeps nearby geometry from being wrongly hidden). A companion hook keeps the player head
rendered, and an input hook powers the free-look orbit. The game's AI picks which nearby characters
to keep updated from the view camera, so a hook keeps that check at your character's eyes; people
close by then do not vanish in third person. The native turn animation answers "third person" only
to the game's on-foot locomotion, so Henry plays his turn-in-place animations while everything else
stays in first person.

The offset is automatically suppressed while a menu or overlay (inventory, map, dialog, codex) is
open, and while the engine is already in its own built-in third-person view (such as horseback),
so those contexts render from the untouched engine view.

Other mods can follow the view through the `mod_camera_view` console variable: 1 while the camera shows your character from outside, 0 in first person. Read it every frame and never write it.

## Configuration

Edit `KCD2_TPVCamera.ini`. It is grouped into:

- `[Settings]` - log level, the view hotkeys (`ToggleViewKey`, `ForceFPVKey`, `ForceTPVKey`), the preset-overlay key (`ToggleOverlayKey`), and the start-of-session auto-enable flags
- `[Camera]` - zoom keys, the shoulder swap key (`SwapShoulderKey`) and whether the swap is saved (`AutoSaveShoulder`), the camera-space interaction toggle, the view-transition ease, the camera-stability options (`StableAimBasis`, `AimBasisSmoothing`) that keep the view steady against head-bob, sway, and combat shake, and the native turn animation (`NativeTurnAnimation`, with `NativeTurnAngle` and `NativeTurnSettleDelay` tuning when Henry turns on the spot to face where you look). The framing itself is per-preset; see `[Presets]`
- `[Orbit]` - the free-look orbit keys (press-to-toggle and momentary hold) and the cursor freeze (the orbit feel is per-preset)
- `[Collision]` - the collision probe and radius, `UseOccluderFade` (fade the props that hide your character instead of pulling in; buildings still stop the camera) with how much of them stays and which props count as in front of your character (enable, skin, return speed and shoulder collision are per-preset), `UseTreeSeeThrough` (open a dithered window around your character in the trees, bushes and tall grass in front of it) with how much of the plants stays, its size, where it starts up the body and its softness, `UseLensCut` (cut away what comes right up to the lens), `HeadClearance`, the room the camera needs behind your character, and `CloseUpFade` (fade the character and everything it wears and holds instead of switching to first person when there is less) with how much of the character always stays
- `[StateBehavior]` - switch first/third person on entering a situation (combat, aiming, dialogue, minigame, mount, menu, overlay), restore the prior view on exit (manual toggles during it stick), suspend/restore free-look in chosen situations, choose where the native turn animation is switched off (`NativeTurnExcludeState`), and whether loading a crossbow already counts as aiming (`ReloadCountsAsAiming`)
- `[Ranged]` - aim third-person shots (arrows, bolts, handgonne balls) and distraction stones at the crosshair (`AimAtCrosshair`), the largest correction allowed (`MaxAimCorrection`), the flight trail (`ShowProjectileTrail`, `ProjectileTrailSeconds`), a live preview of where a drawn shot or a held distraction stone will land (`ShowAimPreview`, `AimPreviewArc`, `AimPreviewOpacity`, `AimPreviewColor`, `AimPreviewThroughWalls`), and how much your shots drop per weapon (`BowGravity`, `CrossbowGravity`, `FirearmGravity`)
- `[Presets]` - the keys that cycle presets bound to the same situation (`NextPresetKey`, `PreviousPresetKey`) and whether the choice is saved (`AutoSaveCyclePick`); the camera framing lives in the in-game preset manager (open with `ToggleOverlayKey`) and is stored in `KCD2_TPVCamera_presets.json` next to the INI. That file is created automatically from the built-in defaults on first run; it is not shipped, so updating the mod never overwrites presets you have tuned
- `[Advanced]` - the self-heal window, `ExportSignatures` (see [Troubleshooting](#troubleshooting)) and `ExportRules` (see [Camera rules](#camera-rules))

Most values apply the next frame after you save the file. Every option is documented in the INI.

How the camera treats each object (which ones stop it, fade, let it through, open in the see-through window or thin away at the lens) is set by the camera rules, not in the INI; see [Camera rules](#camera-rules). `UseOccluderFade` and `UseTreeSeeThrough` still turn the fade and the window on or off for every object, whatever the rules say.

## Camera rules

The camera rules decide how the camera treats every object in the world: which ones stop it like a wall, which fade while they hide your character, which it passes through, which open in the see-through window and which thin away at the lens. The rules go by the model's path in the game files, for a whole folder or one model at a time, so you can change any of them and cover the models of other mods too. The rules built into the ASI give the camera behaviour described above. `ExportRules = true` writes them to a file whose header explains every word of the syntax (see [The rules files](#the-rules-files)); this section is a summary.

What the rules say is final: the mod adds nothing to them and overrides none of them. An object no rule matches gets the `@default` rule (`collide` in the built-in rules). Without any `@default` line (the main rules and your own file can hold one), such objects are left alone, as if their rule said `exclude`, and the log warns about it. The rules cannot change which collision the camera meets in the first place: objects the game moves by physics never stop it, a person or an animal has only a few small colliders the camera meets now and then (so even `@actor = collide` would stop it only there), and collision built into the level still stops it at a tree, bush, grass or character model placed as scenery (see [What the rules cannot change](#what-the-rules-cannot-change)).

### The rules files

The main rules are the ones built into the ASI or, when you put one there, your own `KCD2_TPVCamera_rules.txt`. Every rules file sits beside `KCD2_TPVCamera.asi`. TPVCamera reads the rules in this order, and at the same level a later file wins, whatever the length of its match (see [Which rule wins](#which-rule-wins)):

| Rules | What they are | On a TPVCamera update |
| --- | --- | --- |
| Built into the ASI | The main rules, read first, unless `KCD2_TPVCamera_rules.txt` is there. | Updated with the mod |
| `KCD2_TPVCamera_rules.txt` | Not shipped. Your own main rules, read first instead of the built-in ones (see below). | Untouched |
| `KCD2_TPVCamera_rules_<Name>.txt` | Patches, from another mod or from you, read next in name order (upper and lower case alike). | Untouched |
| `KCD2_TPVCamera_rules_User.txt` | Your own changes, read last: at the same level it wins over every other file, whatever the length of their match and whatever their `only=` says. | Untouched |

To read the built-in rules, set `ExportRules = true` in the INI's `[Advanced]` section. At startup, and whenever you switch it on while the game runs, TPVCamera writes them to `KCD2_TPVCamera_rules.default.txt` beside the ASI, replacing it each time. TPVCamera never reads that file. Its header explains every word of the syntax, and its line numbers are the ones the log names: `built-in KCD2_TPVCamera_rules.txt:342` is its line 342. While the built-in rules are in force, the log says so at startup (`Rules: built-in rules in force ...`).

For changes of your own, write lines in `KCD2_TPVCamera_rules_User.txt`: an update brings its new built-in rules and never touches that file. To replace the built-in rules entirely, rename `KCD2_TPVCamera_rules.default.txt` to `KCD2_TPVCamera_rules.txt` and edit it (Explorer hides the `.txt` of both names by default: rename `KCD2_TPVCamera_rules.default` to `KCD2_TPVCamera_rules`). That file is then final: TPVCamera never mixes it with the built-in rules, so the rules of later updates no longer reach you. Delete it to go back to them, also while the game runs. If it cannot be read, TPVCamera uses the built-in rules and says so in the log.

Only names that end in `.txt` are read, and never a name that ends in `.default.txt`. Renaming a patch to `KCD2_TPVCamera_rules_<Name>.txt.off` switches it off.

Save the files as UTF-8, ANSI or UTF-16 text (Notepad's "Unicode" works too). Windows hides known extensions, so a file you name in Explorer can end up as `KCD2_TPVCamera_rules_User.txt.txt`, which is read as a patch; the log warns about such a name.

Edits apply while the game runs, about a second after you save any of these files. An edit is held back while a line in a file you changed has an error: the log lists those lines, and once the edit applies, every rule that changed. A line with an error in a file you did not change (a mod's patch written for a later TPVCamera, say) is skipped, as at start. A model an edit turns into a tree, bush, grass or frame opens in the window or thins at the lens after the next pause (the Esc menu) or loading screen, where its shaders are prepared.

### Writing a rule

One rule per line. Text after `#` is a comment, upper and lower case are the same, and a match cannot hold a space:

```text
<match> = <behaviour> [options]
```

| Match | What it covers |
| --- | --- |
| `objects/natural/stones/` | A folder: every model under it |
| `objects/manmade/structures/logistical/fences/stone_fence` | The start of a file name: `stone_fence_a.cgf`, `stone_fence_b_lod1.cgf` ... |
| `objects/manmade/vehicles/wagons/wagon_c_hay.cgf` | One model (a match that ends in `.cgf`, `.cga`, `.chr`, `.cdf` or `.skin`) |
| `*hlod*` | Every model whose path contains this text |
| `@actor`, `@animated`, `@nodraw`, `@merged`, `@unnamed` | What the game says the object is: people, animals and what they carry; objects that play an animation (doors, mills); scenery drawn with one no-draw material; grass the engine merges into patches; vegetation whose path cannot be read |
| `@animated objects/mymod/windmill.cgf` | A state, for these models only (`@actor`, `@animated` and `@nodraw`) |
| `@default` | Everything no other rule matches (in the main rules or in your own file) |

| Behaviour | What the camera does |
| --- | --- |
| `collide` | Stops at its collision, as at a wall (see [What the rules cannot change](#what-the-rules-cannot-change)). It never fades. |
| `fade` | Passes through it, and it dithers out while it hides your character (when it is wider than a post). |
| `tree` | Passes through it, and the see-through window opens it in front of your character. It never fades whole. |
| `bush` | Like `tree`, and it also fades whole when the camera ends up inside it. |
| `grass` | Like `tree` (the log says grass). |
| `frame` | Passes through it, and only the part right at the lens thins away. |
| `pass` | Passes through it, and nothing else (invisible collision boxes). |
| `character` | Passes through it. It never fades (people, animals and what they carry). |
| `exclude` | TPVCamera leaves it out: it never fades, opens in the window or thins at the lens, and its collision stops the camera like any collision the mod knows nothing about. For a model that must always stop the camera, write `collide`. |

Options, after the behaviour:

- `mesh`: where the rule ends in `collide`, the camera also stops at the drawn model where it has no collision (cloth canopies, thatch under the roof collision), and passes through collision where the model draws nothing (the invisible canvas walls of a tent). For buildings, not for rocks or ground.
- `if-wall=<behaviour>`: measured in game as the camera comes near. A model taller than 1.8 m whose upper part no one sees through gets that behaviour, and so does one not measured yet or one that cannot be measured; a low or open one keeps the rule's.
- `if-bare=<behaviour>`: a model that draws no cloth (a bare tent frame) gets that behaviour; one that draws cloth keeps the rule's.
- `only=<kinds>`: the rule counts only for objects placed as these kinds, a comma list of `brush` (level scenery), `movable` (movable level scenery), `entity` (game objects: drivable carts, doors, chests ...) and `vegetation` (placed with the vegetation tool).
- `twins=character`: prepare the close-up fade's shaders for the materials of these models at the next pause or loading screen, whichever rule decides the camera for them.

### What the rules cannot change

The rules decide what TPVCamera does with the collision the camera meets. They cannot change which collision that is:

- The camera meets only collision fixed in the world, and the ground. Objects the game moves by physics (crates and barrels) never stop it, whatever their rule. `collide` on them only keeps them from fading, and `exclude` lets the camera through them. A person or an animal has no body the camera meets, only a few small static colliders (about 0.3 to 0.7 m across) that the camera's rays meet now and then: with `@actor = collide` the camera would stop at a person only where a ray happens to meet one of those. The built-in `@actor` line says `character`, which lets the camera pass them.
- Collision built into the level is matched to its model by its box, and `tree`, `bush`, `grass` and `character` models never take that match. A model placed as level scenery (`brush`) with one of those words still stops the camera at its built-in collision. Plants placed with the vegetation tool and game objects (`entity`) carry their own collision, so the camera passes them as their rule says.

### Which rule wins

The first of these that matches an object decides:

1. A state with a model path (`@animated objects/...`); in one file, the longest path first.
2. `@actor` or `@animated`.
3. A text match (`*...*`); in one file, the longest text first. A text match beats every model path, even the path of one whole model, so keep text matches rare.
4. Any other state (`@nodraw`, `@merged`, `@unnamed`).
5. A model path; in one file, the longest path first.
6. `@default`.

A level always comes first, whichever file it is in: a model path in your own file never beats the main rules' `@nodraw`, `@actor` or `@animated` line, their `@nodraw` tents line or their text matches (`*hlod*`, `*designer_*`). At the same level a later file wins over an earlier one, whatever the length of its match: a folder line in your own file changes every model under that folder, the models the main rules list by their whole path included. Within one file, the longest match wins, then a rule with `only=` for fewer kinds, then the later line.

The log names both lines whenever a line repeats a match or a later file overrides one, and names each model path a text match takes over. A line of a later file that overrides longer lines of earlier files gets one warning, with how many lines of each file:

```text
Rules: KCD2_TPVCamera_rules_User.txt:1: 'objects/manmade/structures/industrial/hay/' overrides 26 longer lines of built-in KCD2_TPVCamera_rules.txt (a later file wins over an earlier one at the same level, whatever the length of the match)
```

### Folders listed model by model

Most of the game's folders hold one kind of object, and one folder line decides them all. A few hold big structures and small props together: pillory platforms with pillory posts, broad haystacks with drying poles, covered wagons with loose wheels, stone walls with wooden fences, tents with their pennants and ropes. The built-in rules list every model of such a folder by its whole path, with a comment that says what the model is and gives its size in metres (x by y by height), so each one gets its own behaviour. The header of the exported rules (see [The rules files](#the-rules-files)) lists these folders.

A model such a folder holds that the main rules do not list (one a mod or a game update adds) falls back to the line of its nearest parent folder that has one: `objects/manmade/ = collide mesh` for the tents, fences, gates, hay and pillories, for example, and `objects/manmade/task_specific_props/ = fade` for the work props. To give a new model its own behaviour, write its whole path, as the log's TRACE line names it (see [Find a model's path](#find-a-models-path)), in `KCD2_TPVCamera_rules_User.txt` or in your mod's patch:

```text
objects/manmade/structures/industrial/hay/my_hay_pile.cgf = fade
```

A folder line there changes the whole folder instead, the models the main rules list included.

### Find a model's path

1. Set `LogLevel = TRACE` in `KCD2_TPVCamera.ini` and start the game.
2. Go up to the object with the camera. `KCD2_TPVCamera.log` then has one line for each model near the camera, once per model:

   ```text
   Rules: objects/manmade/vehicles/wagons/wagon_c_hay.cgf [brush] -> collide mesh (built-in KCD2_TPVCamera_rules.txt:342 'objects/manmade/vehicles/wagons/wagon_c_hay.cgf')
   ```

   It gives the full path, how the object is placed (`brush`, `movable`, `entity`, `vegetation` or `merged`, with the game's state after it, as in `[brush, nodraw]`) and the file, line and match of the rule that decided it. `built-in KCD2_TPVCamera_rules.txt:342` is line 342 of the built-in rules, the same line in the file `ExportRules = true` writes (see [The rules files](#the-rules-files)). Each model is listed once per set of rules. After a long session the log says the trace lists no more models; a restart or a changed rule starts it again.
3. Write your rule in `KCD2_TPVCamera_rules_User.txt`. What to write depends on the match the line names:

   | The line names | Write | Example |
   | --- | --- | --- |
   | A model path or `@default` | The model's path | `objects/x/y.cgf = fade` |
   | A state: `'@animated'`, `'@nodraw objects/...'` (the kind shows it too, as in `[entity, animated]`) | The same state, then the model's path. A model path alone never beats a state. | `@animated objects/x/y.cgf = fade` |
   | A text match, such as `'*hlod*'` | A text match, such as the model's whole path between stars. A model path never beats a text match. | `*objects/x/y.cgf* = collide` |

   `@merged` and `@unnamed` objects have no model path; only those two state lines change them.
4. Set `LogLevel` back to `INFO` afterwards; TRACE writes a lot.

### Examples

Put changes of your own in `KCD2_TPVCamera_rules_User.txt` (create it beside the ASI), so updates never undo them:

```text
objects/manmade/vehicles/wagons/wagon_c_hay.cgf = fade                         # the covered hay wagon fades
objects/manmade/structures/industrial/hay/ = collide mesh                      # all hay stops the camera
objects/manmade/structures/logistical/fences/fence_4rod_01.cgf = collide mesh  # one fence always stops the camera
objects/manmade/common_furniture/beds/bed_warmer_fancy.cgf = exclude           # one model left out of the mod
objects/mymod/trees/ = tree                                                    # a mod's trees open in the window
```

To let every covered wagon, the noble coach and the wagons loaded high with hay fade, one folder line does it, since at the same level your own file wins over the main rules' model lines:

```text
objects/manmade/vehicles/wagons/ = fade
```

`objects/manmade/structures/industrial/hay/ = fade` does the same for the haystacks, the hay piles, the hay barrack and its roofs. A folder line changes every model of its folder; to change one model only, write its path, as in the first example. The log then warns once for each such line that it overrides lines of the main rules; that is expected. To undo a change, delete its line.

### For mod authors

- Ship your rules as `KCD2_TPVCamera_rules_<YourMod>.txt`, in the same syntax, with the paths of your mod's models. Keep folder lines to your mod's own folders and name a game model by its whole path: a folder line on a game folder changes every model the main rules list under it, and a text match beats every model path, other mods' included.
- Players copy that file beside `KCD2_TPVCamera.asi` by hand, with or without a mod manager, and delete it when they remove your mod: TPVCamera reads no rules from the `Mods` folder, Mod Organizer 2 manages only that folder, and the third-party KCD2 Advanced Integration plugin for Mod Organizer 2 deletes a mod's `.txt` files when it installs the mod. Offer the file as a separate optional download, and say in its first lines where it goes and that it is deleted by hand with your mod.
- A match cannot hold a space, so keep spaces out of the paths of your models.
- At the same level, a patch wins over the main rules and over the patches whose names sort before it, whatever the length of its match, and the player's `KCD2_TPVCamera_rules_User.txt` always wins: a folder line in your patch also changes the models the main rules list under that folder, and the log says how many. It never beats a level that comes first, such as the main rules' `@nodraw`, `@actor` or `@animated` lines. A `@default` line in a patch is ignored, with a warning in the log.
- A plant you add as `tree`, `bush` or `grass` opens in the see-through window only if its material uses the game's Vegetation or Illum shader, and its shaders are prepared at the loading screen or the next pause. A plant the window cannot open still gets the camera behaviour of its rule.
- Check the result with the TRACE line above; it names your file and line.

```text
# OakMod 1.2 - camera rules for TPVCamera. Copy this file beside KCD2_TPVCamera.asi,
# and delete it when you remove OakMod.
objects/oakmod/trees/                         = tree
objects/oakmod/props/scarecrow.cgf            = exclude
@animated objects/oakmod/windmill_sails.cgf   = pass
```

## Using with Controllers

DetourModKit supports gamepad input natively via the **XInput** API. You can use gamepad button
names directly in the INI file.

### Hotkey Format

- Named keys: `V`, `F3`, `Numpad1`, `Mouse4`, `Gamepad_A`, etc.
- Modifiers: `Ctrl`, `Shift`, `Alt` (or `LCtrl`, `RCtrl`, etc.)
- Multiple combos: `F3,Gamepad_LB+Gamepad_RB` (`F3` alone OR hold `LB` + press `RB`)
- An empty value disables the binding

Examples:

```ini
; Single button
ToggleViewKey = Gamepad_Y

; Modifier combo (hold LB + press RB)
ToggleViewKey = Gamepad_LB+Gamepad_RB

; Multiple independent combos (comma = OR between combos)
; F3 alone OR (hold LB + press RB), so keyboard and gamepad work interchangeably
ToggleViewKey = F3,Gamepad_LB+Gamepad_RB
```

Supported gamepad inputs include `Gamepad_A`, `Gamepad_B`, `Gamepad_X`, `Gamepad_Y`,
`Gamepad_LB`, `Gamepad_RB`, `Gamepad_LT`, `Gamepad_RT`, `Gamepad_Start`, `Gamepad_Back`,
`Gamepad_LS`, `Gamepad_RS`, and the D-pad directions (`Gamepad_DpadUp`, `Gamepad_DpadDown`,
`Gamepad_DpadLeft`, `Gamepad_DpadRight`).

See the full list at the [Supported Input Names](https://github.com/tkhquang/DetourModKit?tab=readme-ov-file#supported-input-names) reference.

> **XInput only:** Xbox controllers work natively. For PS4/PS5/Switch controllers, use an XInput
> translation layer ([DS4Windows](https://github.com/Ryochan7/DS4Windows), DualSenseX, BetterJoy) or
> [Steam Input](https://store.steampowered.com/controller) to present your controller as XInput. See
> [Gamepad Compatibility](https://github.com/tkhquang/DetourModKit?tab=readme-ov-file#gamepad-compatibility) for details.

## Troubleshooting

- Set `LogLevel = DEBUG` in the INI file for shader readiness and projectile diagnostics.
- Use `LogLevel = TRACE` when diagnosing individual carried items or shader masks. Return to `INFO` for normal play;
  it keeps startup, settings, lifecycle summaries, warnings and errors.
- With `LogLevel = TRACE`, the log also names each model near the camera, once, with its full path and the camera rule that decided it (see [Find a model's path](#find-a-models-path)). Whenever the objects at the camera change, a `RenderOcclusion: around the camera:` line names each of them with its class and the file and line of its rule, which shows what stopped or hid the camera.
- Check `KCD2_TPVCamera.log` (next to the game executable) for details.
- If the camera looks wrong in a specific scene (cutscene, photo mode), toggle it off with
  `F3` and back on when normal play resumes.
- If a game update breaks a feature, the log names which signature stopped resolving
  (`Anchor <name> unresolved`) and which feature turned off (`Feature gate: <name> Fail`).
  The rest of the mod keeps working.
- A broken signature can be repaired without a new build of the mod:
  1. Set `ExportSignatures = true` in `[Advanced]` and start the game once. The mod writes every
     built-in signature to `KCD2_TPVCamera.signatures.captured.ini`.
  2. Copy the broken `[sig.<name>]` section and its `.rung.<N>` sections into a new
     `KCD2_TPVCamera.signatures.ini` beside the ASI, under the same `[manifest]` header.
  3. Change the `pattern` to match the new game build and delete the `fingerprint`,
     `image_identity` and `winning_bytes` lines of that section.
  4. A repair of a signature the mod only calls or reads takes effect at the next start. A
     repair of a hook target also needs its baselines: start the game once more with
     `ExportSignatures = true`, then copy the repaired section, with its new baseline lines,
     from the captured file into `KCD2_TPVCamera.signatures.ini`.

  The log reports each repair (`Signatures: <name> uses the repair from the signature file`) and
  refuses one that it cannot trust. A file written for another signature revision is ignored.

## Known Limitations

- Free-look orbit is a raw proof-of-concept. It is fine for swinging the camera around to
  look at the front of Henry, but it is rough for normal gameplay.
- Free-look orbit conflicts with the game's own camera control on horseback and in combat, so
  it is auto-disabled in those situations.
- The first-person body rig can still look slightly off from behind in some animations.
- Faded objects use the engine's dither, so a large faded cart or ladder wagon shows a fine screen-door pattern rather than smooth transparency.
- The plant window, the cut of tent frames at the lens and the fade of your character's eyes, hair and beard use copies of the game's shaders, prepared at the loading screen or the next pause (the Esc menu, or a menu screen while the game is paused). A plant first seen during play keeps its normal look until then, and a close-up before the character's copies are ready hides the head, as in first person. Loading may take longer.
- A held torch's flame and light do not fade with your character.
- In busy towns the fades, the see-through window and the lens cut take the camera about 0.3 ms per frame, against about 0.2 ms without them. For the lowest cost, set `UseOccluderFade`, `UseTreeSeeThrough`, `UseLensCut` and `CloseUpFade` to false and `CollisionPullInSeconds` to 0 in `[Collision]`: the camera then stops at what it hits, as in 2.0.0 (with this version's fixes), prepares no shaders at loading screens or pauses, and costs about what 2.0.0 did.
- The raycast-based crosshair fix is partial: it does not cover every interaction type, so some
  still show parallax or an offset.
- A third-person shot at a close target, such as the ground at Henry's feet, is turned up to about
  50 degrees from where the bow points, so the arrow visibly leaves the bow at an angle before it hits
  the crosshair. A target behind the bow cannot be reached that way and takes the game's own shot.
  Lower `MaxAimCorrection` in `[Ranged]` (90 degrees by default) to keep the game's shot for close
  targets instead.
- A large Offset Right (above about 1 m) can put the crosshair out of Henry's reach. The game only
  offers herbs within about 1.5 m of his eyes and doors, beds and other usables within about 1 m
  to his side, so the use prompt does not show for what the crosshair rests on. Lower the offset,
  or swing the crosshair back over something close to Henry.
- Certain world interactions in third person can still be janky or buggy; toggle the camera off
  if one looks wrong.
- The preset overlay does not hook the game's swap chain by design: it renders through a private
  WARP device and composites with a GDI blit, which keeps it compatible with overlays and
  injectors like ReShade and OptiScaler. The trade-off is a frame-rate hit while the overlay is
  open; since it is a tuning panel you open briefly rather than play with, that is acceptable.

## Roadmap

Planned improvements, roughly in priority order:

- Rework the free-look orbit so it is usable in normal gameplay (interaction during orbit,
  smoother engage/release, better mouse feel), including resolving the conflict with the game's
  own camera on horseback and in combat where orbit is currently auto-disabled.
- Soften the dither on large faded objects.
- Fix shadows being cut off behind the character (the shadow cascade still follows the
  first-person eye).
- Extend the raycast-based crosshair/interaction fix to cover more interaction types.
- Make the fades and the see-through window cheaper per frame, if a better way turns up (none is known yet).

## Recommended Mods

These pair well with a third-person camera:

- [No Helmet Vision](https://www.nexusmods.com/kingdomcomedeliverance2/mods/93) - removes the
  helmet vision letterbox/black bars that look out of place in third-person.
- [First Person Overhaul](https://www.nexusmods.com/kingdomcomedeliverance2/mods/269) - keeps
  certain scripted actions and interactions in third person instead of forcing the view back to
  first person.
- [Alternate Combat](https://www.nexusmods.com/kingdomcomedeliverance2/mods/2497) (optional) -
  modifies combat and camera lock-on behaviour to make melee combat more free and target
  switching easier.

## Changelog

See [CHANGELOG.md](CHANGELOG.md).

## Dependencies

This mod requires:

- An **ASI loader**, such as [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by [**ThirteenAG**](https://github.com/ThirteenAG). It is **not bundled** with the mod; download and install it yourself (see [Installation](#installation)).
- [DetourModKit](https://github.com/tkhquang/DetourModKit) - a lightweight C++ toolkit for game modding (provides SafetyHook, AOB scanning, logging, and configuration management)

> **Note:** the mod will not load without an ASI loader DLL present in the game's binary folder.

## Building from Source

### Prerequisites

- Visual Studio 2022 (MSVC toolchain) with the C++ workload
- CMake 3.28 or newer
- Git (for submodules)

### Building with CMake (MSVC)

```bash
# Fetch dependencies (DetourModKit and its submodules)
git submodule update --init --recursive

# Configure and build the production ASI (uses CMakePresets.json)
cmake --preset msvc-release
cmake --build --preset msvc-release
```

The built `KCD2_TPVCamera.asi` is placed at `build/release-msvc/KCD2_TPVCamera.asi`.

### Developer hot-reload build (optional)

A two-DLL configuration builds a resident loader ASI plus a logic DLL that the loader replaces in
the running game. It follows DetourModKit's staged-generation pattern
([hot-reload guide](https://github.com/tkhquang/DetourModKit/blob/main/docs/guides/hot-reload/README.md)).

```bash
# Configure and build the loader + logic DLL (point it at the game plugins dir)
cmake --preset msvc-dev -DTPVCAMERA_GAME_DIR="<game>/Bin/Win64MasterMasterSteamPGO"
cmake --build --preset msvc-dev
```

The build deploys the logic DLL and its PDB to `staging/` in the game folder. Release Numpad 0
while the game window has focus to reload:

1. The live generation's `Shutdown()` retires every hook once no game thread is inside a detour,
   joins the mod's threads, and drains DetourModKit's input and config callbacks. A game thread
   that stays inside a detour makes the loader retry for about a second; the old generation then
   keeps running until the next press. Any other failure keeps the old generation mapped.
2. The loader promotes the staged build and maps a copy under a unique name
   (`KCD2_TPVCamera.genNNNN.logic.dll`), so a rebuild never collides with a mapped image.
3. A generation that retires with a retained resource stays mapped, within a budget of 32
   images and 128 MiB. A full budget, an unproven retirement or a hook that stays reachable stops
   further reloads until the game restarts.

`KCD2_TPVCamera_Loader.log` beside the ASI records each generation, its build identity, and the
retirement verdict. A change to `src/dev/protocol.h` or `src/dev/mod_loader.cpp` needs a game
restart, because the loader itself is never reloaded.

A reloaded generation finds the shader copies an earlier one prepared through the engine's
resource cache. It creates none during play; missing ones wait for the next pause or loading
screen. Development builds also keep up to eight non-merged plants swapped by their native render
data beyond the camera's target list; release builds do not.

### Code style

The C++ sources follow a hard 120-column baseline and DetourModKit's coding conventions
([AGENTS.md](https://github.com/tkhquang/DetourModKit/blob/main/AGENTS.md)), codified in
`.clang-format` (clang-format 20), `.editorconfig`, and an
advisory `.clang-tidy`. Run the formatter over any changed sources before committing and keep it
idempotent (`clang-format --dry-run --Werror` must be silent). Editor IntelliSense and build tasks
are configured at the repository-root `.vscode/`, not under `TPVCamera/`.

## Credits

- [ThirteenAG](https://github.com/ThirteenAG) - for the Ultimate ASI Loader
- [cursey](https://github.com/cursey) - for SafetyHook
- [Brodie Thiesfield](https://github.com/brofield) - for SimpleIni
- [Frans 'Otis_Inf' Bouma](https://opm.fransbouma.com/intro.htm) - for his camera tools and inspiration
- Warhorse Studios - for Kingdom Come: Deliverance II

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
