# Kingdom Come: Deliverance II - Third Person Camera

A third-person camera for Kingdom Come: Deliverance II. The game stays in its normal first-person mode, and TPVCamera carries the view back over Henry's shoulder, with camera collision, see-through foliage and a crosshair that lines up with what you look at.

Third person turns on once you reach gameplay. Press `F3` (or hold `LB` + `RB`) to toggle it, or set `AutoEnableTPV = false` in the INI to start in first person.

## Features

- Customizable over-the-shoulder third person, toggled with `F3` (or hold `LB` + `RB`)
- Camera presets per situation (combat, aiming, horseback, sneaking...), edited in game
- Switches to first or third person by situation, if you want
- Props fade instead of pulling the camera in; walls still stop it
- See-through trees, bushes and tall grass
- Shots and thrown stones fly to the crosshair
- Hides first-person screen effects (helmet visor, eyelids, water drops) in third person
- [Open source](https://github.com/tkhquang/KCD2Tools), built with [DetourModKit](https://github.com/tkhquang/DetourModKit)

## Installation

Requires Kingdom Come: Deliverance II on Steam, and [Kingdom Come Script Extender (KCSE)](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3332) **or** [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases). Neither is bundled.

Everything goes in the game's binary folder, the one with `WHGame.dll`. On Steam: `<KC:D 2 folder>/Bin/Win64MasterMasterSteamPGO/`.

1. **ASI loader**: KCSE loads `.asi` files itself, so **KCSE users skip this step**. Everyone else: extract one x64 Ultimate ASI Loader DLL beside `WHGame.dll`, [dinput8.dll](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/dinput8-x64.zip) (usual), or [version.dll](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/version-x64.zip) / [winmm.dll](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/winmm-x64.zip) if that name is taken.
2. **The mod**: extract the archive from [GitHub Releases](https://github.com/tkhquang/KCD2Tools/releases) or Nexus into the same folder, with no extra subfolder. It does not use the `Mods` folder.
3. **Check**: launch the game. `KCD2_TPVCamera.log` appears beside the ASI once the mod loads.

Coming from **KCD2_TPVToggle**? Delete `KCD2_TPVToggle.asi` and `KCD2_TPVToggle.ini` first; the two mods conflict.

<details>
<summary>Folder layout, no log file, Linux, updating, uninstalling</summary>

```text
Bin/Win64MasterMasterSteamPGO/
|-- WHGame.dll                         (game file)
|-- dinput8.dll                        (KCSE or the ASI loader)
|-- KCD2_TPVCamera.asi                 (this mod)
|-- KCD2_TPVCamera.ini                 (settings)
|-- KCD2_TPVCamera_rules_User.txt      (your camera rules; optional, you create it)
|-- KCD2_TPVCamera_presets.json        (created on first run)
\-- KCD2_TPVCamera.log                 (created when the mod loads)
```

**No log file?** Check that the ASI and the loader DLL sit directly beside `WHGame.dll`, and that the loader is the x64 build. With KCSE, update KCSE to its latest version. Without it, try another loader name. Never overwrite a DLL that belongs to another mod.

**Linux / Steam Deck (Wine/Proton):** add this to the game's Steam launch options, using your loader's name (`version` or `winmm`) if it is not `dinput8`:

```text
WINEDLLOVERRIDES="dinput8=n,b" %command%
```

**Updating:** back up your INI, extract the new archive, then copy your settings back in. Keep `KCD2_TPVCamera_presets.json` and `KCD2_TPVCamera_rules_User.txt`; updates never touch them. An update brings new built-in camera rules, unless you replaced them with your own `KCD2_TPVCamera_rules.txt` (see [Camera rules](#camera-rules)).

**Uninstalling:** delete every file starting with `KCD2_TPVCamera` beside `WHGame.dll`, and the loader DLL if no other mod needs it. Optionally, delete the files starting with `kcd2_tpvcamera_` in the game's shader cache, usually `%USERPROFILE%\Saved Games\kingdomcome2\shaders\cache\d3d12\` and its `cg...shaders` subfolders (the log line `ShaderTwins: ... twin file(s) in <folder>` names it). Leave the game's own files there.

</details>

## Controls

All keys are rebindable in `KCD2_TPVCamera.ini`.

| Action                 | Keyboard                         | Controller                  |
| ---------------------- | -------------------------------- | --------------------------- |
| Toggle third person    | `F3`                             | hold `LB` + `RB`            |
| Toggle free-look orbit | `F4`                             | hold `LB` + click `LS`      |
| Preset manager         | `Home`                           | (unbound)                   |
| Zoom in / out          | `LShift` + `PageUp` / `PageDown` | hold `LB` + D-pad up / down |

Unbound by default: `OrbitHoldKey` (hold to look around, release to snap back), `ForceFPVKey`, `ForceTPVKey`, `NextPresetKey` / `PreviousPresetKey` and `SwapShoulderKey`.

<details>
<summary>Hotkey format, controllers, preset cycling, D-pad zoom</summary>

**Hotkey format**

- Named keys: `V`, `F3`, `Numpad1`, `Mouse4`, `Gamepad_A`, etc. ([full list](https://github.com/tkhquang/DetourModKit?tab=readme-ov-file#supported-input-names))
- Modifiers: `Ctrl`, `Shift`, `Alt` (or `LCtrl`, `RCtrl`, etc.)
- Comma means OR: `F3,Gamepad_LB+Gamepad_RB` is `F3` alone, or hold `LB` and press `RB`
- An empty value disables the binding

Gamepad inputs: `Gamepad_A`, `Gamepad_B`, `Gamepad_X`, `Gamepad_Y`, `Gamepad_LB`, `Gamepad_RB`, `Gamepad_LT`, `Gamepad_RT`, `Gamepad_Start`, `Gamepad_Back`, `Gamepad_LS`, `Gamepad_RS`, `Gamepad_DpadUp`, `Gamepad_DpadDown`, `Gamepad_DpadLeft`, `Gamepad_DpadRight`.

**Controllers:** XInput only. Xbox pads work as they are; for PlayStation or Switch pads, use [Steam Input](https://store.steampowered.com/controller), [DS4Windows](https://github.com/Ryochan7/DS4Windows), DualSenseX or BetterJoy (see [Gamepad Compatibility](https://github.com/tkhquang/DetourModKit?tab=readme-ov-file#gamepad-compatibility)).

**Preset cycling:** presets bound to exactly the same situations form a cycle group, such as DEFAULT with the shipped Close Shoulder, or AIMING with Aiming Wide. `NextPresetKey` / `PreviousPresetKey` switch between them (for example `Numpad6,Gamepad_LB+Gamepad_DpadRight` / `Numpad4,Gamepad_LB+Gamepad_DpadLeft`), or pick one in the overlay. Reset all in the overlay restores the shipped presets and keeps your own. The picked preset and shoulder are saved right away; set `AutoSaveCyclePick` / `AutoSaveShoulder` to `false` to keep them for the session only.

**D-pad zoom:** while you zoom, the mod hides that D-pad press from the game so the inventory or map does not open. Set `ZoomInKey.Consume` / `ZoomOutKey.Consume` to `false` to turn that off.

</details>

## Configuration

Camera framing lives in the in-game preset manager (`Home`) and is saved to `KCD2_TPVCamera_presets.json`. That file is created from the built-in defaults on first run and is not shipped, so updates never overwrite it. Everything else is in `KCD2_TPVCamera.ini`, where every option is documented. Most changes apply the next frame after you save.

<details>
<summary>INI sections</summary>

- `[Settings]` - log level, view hotkeys, the overlay key, auto-enable at start
- `[Camera]` - zoom and shoulder keys, screen effects hidden per view (`HideScreenEffectsTPV`, `HideScreenEffectsFPV`), view transition, camera stability against head-bob and shake (`StableAimBasis`, `AimBasisSmoothing`), native turn animation (`NativeTurnAnimation`, `NativeTurnAngle`, `NativeTurnSettleDelay`)
- `[Orbit]` - free-look keys and cursor freeze
- `[Collision]` - probe and radius, prop fading (`UseOccluderFade`), foliage window (`UseTreeSeeThrough`), lens cut (`UseLensCut`), `HeadClearance`, character fade (`CloseUpFade`)
- `[StateBehavior]` - which situations switch the view, restore it or pause free-look; where the turn animation is off (`NativeTurnExcludeState`); `ReloadCountsAsAiming`
- `[Ranged]` - aim at the crosshair (`AimAtCrosshair`, `MaxAimCorrection`), trail (`ShowProjectileTrail`), landing preview (`ShowAimPreview` and its look), drop per weapon (`BowGravity`, `CrossbowGravity`, `FirearmGravity`)
- `[Presets]` - cycle keys and `AutoSaveCyclePick`
- `[Advanced]` - self-heal window, `ExportSignatures` (see [Troubleshooting](#troubleshooting)), `ExportRules` (see [Camera rules](#camera-rules))

`UseOccluderFade` and `UseTreeSeeThrough` switch the fade and the window off for every object, whatever the camera rules say.

</details>

## Camera rules

Camera rules decide what the camera does with each object: stop at it like a wall, fade it while it hides Henry, pass through it, open it in the see-through window, or thin it at the lens. They match model paths in the game files, for a whole folder or one model, so they cover other mods' models too.

The defaults are built into the mod. Put your own changes in `KCD2_TPVCamera_rules_User.txt` beside the ASI. Edits apply while the game runs, about a second after you save, and updates never touch that file.

```text
objects/manmade/vehicles/wagons/wagon_c_hay.cgf = fade                         # the covered hay wagon fades
objects/manmade/vehicles/wagons/ = fade                                        # every covered wagon fades
objects/manmade/structures/industrial/hay/ = collide mesh                      # all hay stops the camera
objects/manmade/structures/logistical/fences/fence_4rod_01.cgf = collide mesh  # one fence always stops the camera
objects/manmade/common_furniture/beds/bed_warmer_fancy.cgf = exclude           # one model left out of the mod
objects/mymod/trees/ = tree                                                    # a mod's trees open in the window
```

A folder line changes every model in it, including the ones the built-in rules list one by one; the log warns once per such line, which is expected. To undo a change, delete its line.

### Find a model's path

1. Set `LogLevel = TRACE` in the INI and walk up to the object. The log names each model near the camera once, with the rule that decided it:

   ```text
   Rules: objects/manmade/vehicles/wagons/wagon_c_hay.cgf [brush] -> collide mesh (built-in KCD2_TPVCamera_rules.txt:342 'objects/manmade/vehicles/wagons/wagon_c_hay.cgf')
   ```

   The brackets show how the object is placed (`brush`, `movable`, `entity`, `vegetation` or `merged`) and its state, as in `[brush, nodraw]`. `built-in KCD2_TPVCamera_rules.txt:342` is line 342 of the exported rules. After a long session the trace stops listing new models; a restart or a rules change starts it again.
2. Write your rule in `KCD2_TPVCamera_rules_User.txt`, using the kind of match the line names:

   | The line names | Write | Example |
   | --- | --- | --- |
   | A model path or `@default` | The model's path | `objects/x/y.cgf = fade` |
   | A state (`'@animated'`, `'@nodraw objects/...'`) | The same state, then the path; a path alone never beats a state | `@animated objects/x/y.cgf = fade` |
   | A text match (`'*hlod*'`) | A text match, such as the whole path between stars | `*objects/x/y.cgf* = collide` |

   `@merged` and `@unnamed` objects have no path; only those two state lines change them.
3. Set `LogLevel` back to `INFO`; TRACE writes a lot.

<details>
<summary>Rules reference: files, syntax, which rule wins, limits, mod authors</summary>

### The rules files

Every rules file sits beside `KCD2_TPVCamera.asi`. They are read in this order; at the same level, a later file wins whatever the length of its match (see [Which rule wins](#which-rule-wins)).

| Rules | What they are | On an update |
| --- | --- | --- |
| Built into the ASI | The main rules, unless `KCD2_TPVCamera_rules.txt` exists. | Updated |
| `KCD2_TPVCamera_rules.txt` | Not shipped. Your own main rules, used instead of the built-in ones. | Untouched |
| `KCD2_TPVCamera_rules_<Name>.txt` | Patches from other mods or from you, in name order (case-insensitive). | Untouched |
| `KCD2_TPVCamera_rules_User.txt` | Your changes, read last, so they win, whatever the other files' `only=` says. | Untouched |

- **Read the built-in rules:** set `ExportRules = true` in `[Advanced]`. At startup, or when you switch it on in game, the mod writes them to `KCD2_TPVCamera_rules.default.txt`, replacing it each time, and never reads that file. Its header explains the syntax, and its line numbers match the log's.
- **Replace the built-in rules:** rename that file to `KCD2_TPVCamera_rules.txt` and edit it (Explorer hides `.txt`, so rename `KCD2_TPVCamera_rules.default` to `KCD2_TPVCamera_rules`). It is never mixed with the built-in rules, so later updates' rules no longer reach you. Delete it to go back, also in game. If it cannot be read, the built-in rules apply and the log says so.
- Only names ending in `.txt` are read, never `.default.txt`. Rename a patch to `.txt.off` to switch it off. Explorer can create `KCD2_TPVCamera_rules_User.txt.txt`, which is read as a patch; the log warns.
- Save as UTF-8, ANSI or UTF-16 (Notepad's "Unicode").
- An edit with an error is held back until you fix it; the log lists the bad lines, then every rule that changed. An error in a file you did not change (say a patch written for a later version) skips that line.
- A model an edit turns into `tree`, `bush`, `grass` or `frame` changes look after the next pause (Esc) or loading screen, where its shaders are prepared.

### Writing a rule

One rule per line: `<match> = <behaviour> [options]`. Text after `#` is a comment, case does not matter, and a match cannot contain a space.

| Match | What it covers |
| --- | --- |
| `objects/natural/stones/` | A folder: every model under it |
| `objects/manmade/structures/logistical/fences/stone_fence` | Names starting with this: `stone_fence_a.cgf`, `stone_fence_b_lod1.cgf` ... |
| `objects/manmade/vehicles/wagons/wagon_c_hay.cgf` | One model (ends in `.cgf`, `.cga`, `.chr`, `.cdf` or `.skin`) |
| `*hlod*` | Every model whose path contains this text |
| `@actor` | People, animals and what they carry |
| `@animated` | Objects that play an animation (doors, mills) |
| `@nodraw` | Scenery drawn with one no-draw material |
| `@merged` | Grass the engine merges into patches |
| `@unnamed` | Vegetation whose path cannot be read |
| `@animated objects/mymod/windmill.cgf` | A state for these models only (`@actor`, `@animated`, `@nodraw`) |
| `@default` | Everything else (main rules or your own file only; ignored with a warning in a patch) |

| Behaviour | What the camera does |
| --- | --- |
| `collide` | Stops at its collision, like a wall. Never fades. |
| `fade` | Passes through; it dithers out while it hides Henry (if wider than a post). |
| `tree` | Passes through; the see-through window opens it. Never fades whole. |
| `bush` | Like `tree`, and fades whole when the camera is inside it. |
| `grass` | Like `tree` (the log says grass). |
| `frame` | Passes through; only the part at the lens thins away. |
| `pass` | Passes through, nothing else (invisible collision boxes). |
| `character` | Passes through. Never fades (people, animals, what they carry). |
| `exclude` | Left out of the mod: no fade, window or lens cut, and its collision stops the camera. Use `collide` for a model that must always stop it. |

Options, after the behaviour:

- `mesh`: with `collide`, the camera also stops at the drawn model where it has no collision (cloth canopies, thatch), and passes collision that draws nothing (invisible tent walls). For buildings, not rocks or ground.
- `if-wall=<behaviour>`: measured in game as the camera nears. A model over 1.8 m whose upper part cannot be seen through, or that is not or cannot be measured, gets that behaviour; a low or open one keeps the rule's.
- `if-bare=<behaviour>`: a model that draws no cloth (a bare tent frame) gets that behaviour.
- `only=<kinds>`: the rule counts only for these placements, comma-separated: `brush` (level scenery), `movable`, `entity` (carts, doors, chests ...) and `vegetation`.
- `twins=character`: prepare the close-up fade's shaders for these models' materials at the next pause or loading screen.

Unmatched objects get `@default` (`collide` in the built-in rules). With no `@default` line at all they are left alone, as with `exclude`, and the log warns. The mod adds nothing to the rules and overrides none of them.

### Which rule wins

The first level that matches decides:

1. A state with a path (`@animated objects/...`); in one file, the longest path first.
2. `@actor` or `@animated`.
3. A text match (`*...*`); in one file, the longest first. It beats every model path, so keep text matches rare.
4. Any other state (`@nodraw`, `@merged`, `@unnamed`).
5. A model path; in one file, the longest first.
6. `@default`.

A higher level wins whichever file it is in: a path in your file never beats the main rules' `@nodraw`, `@actor`, `@animated` or text lines (`*hlod*`, `*designer_*`). At the same level a later file wins whatever its match length, so a folder line in your file changes every model under it. Within one file: longest match, then the `only=` with fewer kinds, then the later line.

The log names both lines when a match repeats or a later file overrides one, and sums up overrides of longer lines:

```text
Rules: KCD2_TPVCamera_rules_User.txt:1: 'objects/manmade/structures/industrial/hay/' overrides 26 longer lines of built-in KCD2_TPVCamera_rules.txt (a later file wins over an earlier one at the same level, whatever the length of the match)
```

### What the rules cannot change

The rules decide what the mod does with the collision the camera meets, not which collision that is:

- The camera meets only collision fixed in the world, and the ground. Physics objects (crates, barrels) never stop it: `collide` only keeps them from fading, `exclude` lets the camera through. People and animals have only a few small colliders (0.3 to 0.7 m), so even `@actor = collide` would stop the camera only now and then; the built-in rules use `character`.
- Collision built into the level is matched to its model by its box, and never to `tree`, `bush`, `grass` or `character` models. Such a model placed as `brush` still stops the camera at that collision. Vegetation-tool plants and `entity` objects carry their own collision and follow their rule.

### Folders listed model by model

A few folders mix big structures with small props (pillory platforms and posts, haystacks and drying poles, covered wagons and loose wheels, tents and their ropes). The built-in rules list each of their models by full path, with its size in metres; the exported header lists these folders. A model added later (by a mod or an update) falls back to the nearest parent folder's line, such as `objects/manmade/ = collide mesh`. Give it its own behaviour with its full path:

```text
objects/manmade/structures/industrial/hay/my_hay_pile.cgf = fade
```

### For mod authors

- Ship `KCD2_TPVCamera_rules_<YourMod>.txt` with your models' paths. Keep folder lines to your own folders and name game models by full path: a folder line on a game folder changes every model under it, and a text match beats every path, other mods' included.
- Players copy the file beside `KCD2_TPVCamera.asi` by hand and delete it with your mod. TPVCamera reads no rules from `Mods`, Mod Organizer 2 manages only that folder, and the KCD2 Advanced Integration plugin for MO2 deletes a mod's `.txt` files on install. Offer it as a separate optional download, and say in its first lines where it goes.
- Keep spaces out of your model paths.
- At the same level your patch wins over the main rules and earlier-sorting patches, and the player's `_User.txt` wins over yours. It never beats a higher level, such as the main rules' `@nodraw`, `@actor` or `@animated` lines.
- A `tree`, `bush` or `grass` plant opens in the window only if its material uses the game's Vegetation or Illum shader; otherwise it still gets its rule's camera behaviour.
- Check the result with the TRACE line; it names your file and line.

```text
# OakMod 1.2 - camera rules for TPVCamera. Copy this file beside KCD2_TPVCamera.asi,
# and delete it when you remove OakMod.
objects/oakmod/trees/                         = tree
objects/oakmod/props/scarecrow.cgf            = exclude
@animated objects/oakmod/windmill_sails.cgf   = pass
```

</details>

## Troubleshooting

- Check `KCD2_TPVCamera.log` beside the ASI. `LogLevel = DEBUG` adds shader and projectile detail; `TRACE` adds carried items, shader masks and each model's camera rule, plus a `RenderOcclusion: around the camera:` line naming what stopped or hid the camera. Go back to `INFO` for normal play.
- If the camera looks wrong in a scene (cutscene, photo mode), toggle it off and back on later.
- After a game update, the log names any signature that stopped resolving (`Anchor <name> unresolved`) and the feature that turned off (`Feature gate: <name> Fail`). The rest keeps working.

<details>
<summary>Repair a broken signature without a new build</summary>

1. Set `ExportSignatures = true` in `[Advanced]` and start the game once. The mod writes every built-in signature to `KCD2_TPVCamera.signatures.captured.ini`.
2. Copy the broken `[sig.<name>]` section and its `.rung.<N>` sections into a new `KCD2_TPVCamera.signatures.ini` beside the ASI, under the same `[manifest]` header.
3. Change the `pattern` to match the new game build and delete that section's `fingerprint`, `image_identity` and `winning_bytes` lines.
4. A repaired signature the mod only calls or reads works at the next start. A repaired hook target also needs baselines: start once more with `ExportSignatures = true`, then copy the section, with its new baseline lines, from the captured file into `KCD2_TPVCamera.signatures.ini`.

The log reports each repair (`Signatures: <name> uses the repair from the signature file`) and refuses one it cannot trust. A file written for another signature revision is ignored.

</details>

## Known Limitations

- Free-look orbit is a rough proof of concept. It turns off on horseback and in combat, and pauses during the focused look (Left Alt, or `LB`).
- Some world interactions can still be janky in third person; toggle the camera off if one looks wrong.

<details>
<summary>All limitations</summary>

- The first-person body can look slightly off from behind in some animations.
- Faded objects use the engine's dither, so a large faded cart or ladder wagon shows a fine screen-door pattern.
- The foliage window, the lens cut of tent frames and the fade of Henry's eyes, hair and beard use shader copies prepared at loading screens and pauses (Esc, or a menu while paused). A plant first seen during play keeps its normal look until then, a close-up before Henry's copies are ready hides his head as in first person, and loading can take longer.
- A held torch's flame and light do not fade with Henry.
- In busy towns the fades, window and lens cut cost about 0.3 ms per frame, against 0.2 ms without them. For the lowest cost, set `UseOccluderFade`, `UseTreeSeeThrough`, `UseLensCut` and `CloseUpFade` to `false` and `CollisionPullInSeconds` to `0` in `[Collision]`: the camera then stops at what it hits, as in 2.0.0, and prepares no shaders.
- The crosshair fix does not cover every interaction type; some still show parallax or an offset.
- A shot at a close target, such as the ground at Henry's feet, is turned up to about 50 degrees, so the arrow visibly leaves the bow at an angle. A target behind the bow takes the game's own shot. Lower `MaxAimCorrection` in `[Ranged]` (default 90) to keep the game's shot for close targets.
- An Offset Right above about 1 m can put the crosshair out of Henry's reach (herbs within about 1.5 m of his eyes, usables within about 1 m to his side), so the use prompt does not show.
- The preset overlay renders through a private WARP device and a GDI blit instead of hooking the swap chain, to stay compatible with ReShade and OptiScaler. It costs frame rate while open.

</details>

## Roadmap

<details>
<summary>Planned, roughly in priority order</summary>

- Make free-look orbit usable in normal play (interaction, smoother engage/release, mouse feel), including on horseback and in combat.
- Soften the dither on large faded objects.
- Fix shadows cut off behind Henry (the shadow cascade still follows the first-person eye).
- Extend the crosshair fix to more interaction types.
- Make the fades and the see-through window cheaper per frame.

</details>

## Recommended Mods

- [True Light - Real Torch Shadows](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3921) - Henry's torch lights the world from its flame and casts real shadows, his own included, instead of the game's first-person cone.
- [First Person Overhaul](https://www.nexusmods.com/kingdomcomedeliverance2/mods/269) - keeps more scripted actions and interactions in third person.
- [Alternate Combat](https://www.nexusmods.com/kingdomcomedeliverance2/mods/2497) (optional) - freer melee and easier target switching.

## How It Works

Other mods can read the `mod_camera_view` console variable every frame: `1` while the camera shows Henry from outside, `0` in first person. Never write it.

<details>
<summary>Implementation</summary>

The camera hooks the engine's frustum builder and offsets the view camera's matrix before the cull planes are computed, so the rendered view and its culling move together and nearby geometry is not wrongly hidden. A companion hook keeps the player's head rendered, and an input hook drives the free-look orbit. The game's AI picks which nearby characters to keep updated from the view camera, so a hook keeps that check at Henry's eyes and nearby people do not vanish. The native turn animation answers "third person" only to the game's on-foot locomotion, so Henry plays his turn-in-place animations while everything else stays first person.

The offset is suppressed while a menu or overlay (inventory, map, dialog, codex) is open, and while the engine is in its own third-person view (such as horseback).

</details>

## Building from Source

Requires Visual Studio 2022 (C++ workload), CMake 3.28+ and Git.

```bash
git submodule update --init --recursive
cmake --preset msvc-release
cmake --build --preset msvc-release
```

The ASI is written to `build/release-msvc/KCD2_TPVCamera.asi`.

<details>
<summary>Developer hot-reload build</summary>

A resident loader ASI plus a logic DLL that the loader swaps in the running game, following DetourModKit's [staged-generation pattern](https://github.com/tkhquang/DetourModKit/blob/main/docs/guides/hot-reload/README.md).

```bash
cmake --preset msvc-dev -DTPVCAMERA_GAME_DIR="<game>/Bin/Win64MasterMasterSteamPGO"
cmake --build --preset msvc-dev
```

The build deploys the logic DLL and PDB to `staging/` in the game folder. Release Numpad 0 with the game focused to reload:

1. The live generation's `Shutdown()` retires every hook once no game thread is inside a detour, joins the mod's threads and drains DetourModKit's input and config callbacks. A thread that stays in a detour makes the loader retry for about a second, after which the old generation keeps running until the next press. Any other failure keeps the old generation mapped.
2. The loader promotes the staged build and maps a copy under a unique name (`KCD2_TPVCamera.genNNNN.logic.dll`).
3. A generation that retires with a retained resource stays mapped, within a budget of 32 images and 128 MiB. A full budget, an unproven retirement or a still-reachable hook stops further reloads until the game restarts.

`KCD2_TPVCamera_Loader.log` records each generation, its build identity and the retirement verdict. Changes to `src/dev/protocol.h` or `src/dev/mod_loader.cpp` need a game restart.

A reloaded generation finds earlier shader copies through the engine's resource cache and creates none during play; missing ones wait for the next pause or loading screen. Development builds also keep up to eight non-merged plants swapped by their native render data beyond the camera's target list.

</details>

**Code style:** a 120-column baseline and DetourModKit's conventions ([AGENTS.md](https://github.com/tkhquang/DetourModKit/blob/main/AGENTS.md)), codified in `.clang-format` (clang-format 20), `.editorconfig` and an advisory `.clang-tidy`. `clang-format --dry-run --Werror` must be silent on changed sources. Editor and build tasks live in the repository-root `.vscode/`.

## Credits

- [ThirteenAG](https://github.com/ThirteenAG) - Ultimate ASI Loader
- [cursey](https://github.com/cursey) - SafetyHook
- [Brodie Thiesfield](https://github.com/brofield) - SimpleIni
- [Frans 'Otis_Inf' Bouma](https://opm.fransbouma.com/intro.htm) - camera tools and inspiration
- Warhorse Studios - Kingdom Come: Deliverance II

Built with [DetourModKit](https://github.com/tkhquang/DetourModKit). See [CHANGELOG.md](CHANGELOG.md) for changes. Licensed under the [MIT License](LICENSE).
