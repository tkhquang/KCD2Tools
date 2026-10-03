# Kingdom Come: Deliverance II - Henry's Senses

A loot and object highlighting mod for Kingdom Come: Deliverance II.

## Overview

Henry's Senses is the native ASI successor to [Loot Beacon](../LootBeacon/). Press a key to highlight nearby loot, herbs and other objects with outlines, fills or corner brackets, including through walls and grass.

I originally made Loot Beacon after spending a day searching for Canker's Mace in the tall grass. Henry's Senses builds on that idea with model highlights, broader detection and configurable groups for different objects and play styles.

> [!NOTE]
> This mod is in early development. If something doesn't work properly, please [report it](https://github.com/tkhquang/KCD2Tools/issues) with your game/mod versions, settings and log.

## Features

- Highlight lootable bodies, animal carcasses, dropped items, containers and herbs
- Optional highlighting for living NPCs, animals and interactive objects such as doors, beds and workstations
- Configurable colours, styles, range, duration and hotkeys per highlight group
- Pulse, Toggle, Hold and Always activation modes, with keyboard and controller bindings
- Filters for stolen, empty, locked and other tagged objects, using the game's own loot checks
- Live configuration reload when you save the INI

The default `Auto` renderer uses model highlights, with corner brackets as a fallback. Set `[Render] SeeThrough = false` to highlight only visible surfaces.

## Installation

Built around **KCD2 Steam 1.5.6**. An **ASI loader** is required and is not bundled.

1. **Install an ASI loader** if you don't already have one. Place the loader DLL (commonly `dinput8.dll`) beside `WHGame.dll`, in `<game>/Bin/Win64MasterMasterSteamPGO/` on Steam.
2. **Install the mod.** Copy all files from the release archive into the binary folder beside `WHGame.dll` and the loader: `<game>/Bin/Win64MasterMasterSteamPGO/` on Steam.
3. **Launch the game**, load a save and press **H** to highlight loot.

`KCD2_HenrySenses.log` appears beside the ASI when the mod loads. Custom `HenrySenses.*` particle effects require the optional `KCD2_HenrySenses.particles.xml` beside it and an `Effect` setting in a group; no group uses particles by default.

The Lua Loot Beacon can remain installed with its default **F4** binding. Henry's Senses does not use the `Mods` folder.

## Controls

All keys are rebindable in `KCD2_HenrySenses.ini`. Defaults:

| Action | Keyboard | Controller |
| --- | --- | --- |
| Highlight loot and butcherable carcasses | `H` | Hold `RB` + press `Start` |
| Highlight herbs | `Left Shift + H` | Hold `RB` + press `Back` |

Highlights pulse for **5 seconds**, then fade over **1 second**, within **20 metres**. Loot, Butcher and Herbs start enabled; the other groups start disabled. The Loot group excludes empty and stolen targets; Butcher separately highlights butcherable carcasses. Highlights hide during **dialogue** by default. Group hotkeys also work while menus are open.

## How It Works

The native ASI runs detection and highlight updates on the game's main thread after each game update. It scans nearby loot, creatures, herbs and interactive objects, uses the game's own loot checks to classify them, then applies your groups' targets, filters, range and colours. A registry tracks the highlighted objects and restores their original render state when highlights end.

For model highlights, the mod enables the game's otherwise unused silhouette rendering stage. Render hooks mark the selected models, draw their silhouettes into a mask and composite the outline or fill after tone mapping to keep colours consistent with changing light. Interactive objects without their own model can use a nearby static mesh; the Auto backend uses brackets when an outline is unavailable. SeeThrough controls whether highlights appear through walls or only on visible surfaces.

Engine addresses are located through AOB pattern scanning with fallback signatures. Features depend on their own resolved addresses, so a missing signature disables the affected feature. Major game updates may still need a mod update. Combat, dialogue and minigames are detected through game-state reads, and highlights are cleared on level loads.

### Shader source and runtime files

The custom composite shader draws even outlines and fills without the stock shader's scanlines or darkened screen edges. It also reads scene depth so the nearer highlighted object keeps its outline where objects overlap. The relevant files, relative to `HenrySenses/`, are:

| File | Purpose |
| --- | --- |
| [shaders/HenrySensesSilhouette.cfx](shaders/HenrySensesSilhouette.cfx) | Editable shader source for outlines, fills, visibility and focus effects. |
| [scripts/cfxb.py](scripts/cfxb.py) | Converts shader source into the game's tokenized `.cfxb` format and can inspect or verify shader binaries. |
| [src/render/silhouette_shader_blob.hpp](src/render/silhouette_shader_blob.hpp) | Generated binary and shader name embedded in the ASI. Regenerate this file after changing the shader source. |
| [src/render/silhouette_shader.cpp](src/render/silhouette_shader.cpp) | Writes the embedded binary into the shader cache and requests it through the game's shader manager. |
| [src/render/engine_silhouette.cpp](src/render/engine_silhouette.cpp) | Registers the silhouette stage, draws the masks and composites the result into the frame. |

At runtime, the ASI writes `HenrySensesSilhouette_<CRC>.cfxb` into the shader cache inside KCD2's save-data folder. The default Windows location is `%USERPROFILE%\Saved Games\kingdomcome2\shaders\cache\d3d12\`, or `C:\Users\<your Windows username>\Saved Games\kingdomcome2\shaders\cache\d3d12\`. Paste the `%USERPROFILE%` path into File Explorer's address bar to open it. If you relocated Saved Games or use a custom save-data folder, the `SilhouetteShader:` lines in `KCD2_HenrySenses.log` show the resolved path. The CRC suffix changes with the shader's token stream so a new version gets a new name in the engine's cache. The loader replaces a differing binary and removes older versions of this mod's cached `.cfxb` file.

The retail game loads tokenized shaders, so the `.cfx` source is converted before embedding; the game compiles the GPU permutation at runtime. The source is self-contained, avoiding dependencies on game shader includes whose CRCs can change between builds. Normal installation only needs the ASI, which carries the shader binary.

The mask is drawn at a higher resolution with camera jitter compensated to keep edges stable. With `SeeThrough = false`, a separate depth-tested mask controls which parts are visible. Until the custom shader draws successfully, or if loading or compilation fails, the stock `DeferredSilhouettesOptimised` composite remains in use. This shader fallback is separate from Auto's bracket fallback when model highlighting is unavailable.

## Configuration

Open `KCD2_HenrySenses.ini` beside the ASI with a text editor such as Notepad. Save the file to apply changes while the game runs. Lines starting with `;` are comments explaining the options.

### Shared settings and highlight groups

A **highlight group** is a set of things you want to see together. Each `[Highlight.<Name>]` section can have its own colour, key, range and behaviour. The supplied groups are:

| Group | What it highlights | Enabled by default |
| --- | --- | --- |
| Loot | Corpses, carcasses, items and containers, excluding empty or stolen targets. | Yes |
| Butcher | Butcherable carcasses. | Yes |
| Locked | Locked, non-empty containers. | No |
| Stolen | Stolen items, containers, corpses and carcasses, excluding empty targets. | No |
| Herbs | Unpicked herbs and mushrooms, outlined. | Yes |
| Places | Doors, workstations, beds, use spots and other usable objects. Experimental. | No |
| Living | Living NPCs and animals, with separate colours for horses and dogs. | No |
| Hostiles | Bandits, Cumans and other public enemies. | No |

`[Settings]` provides the shared defaults. A value in a group's own section overrides that default **for that group only**. For example, changing `Key` under Settings changes the Loot and Butcher bindings, but Herbs keeps its own `LShift+H,Gamepad_RB+Gamepad_Back` binding. Filters belong to individual groups; Loot's `Except` list does not apply to Butcher.

Edit the existing sections for these common changes:

| What you want | Where to change it |
| --- | --- |
| See things farther away | Increase `Radius` under `[Settings]` (metres; default 20). A group's own Radius takes priority. |
| Keep a pulse visible longer | Increase `Duration` under `[Settings]` (seconds; default 5). |
| Highlight stolen loot in red | Set `Enabled = true` under `[Highlight.Stolen]`. The normal Loot group continues to skip stolen loot. |
| Show living people and animals | Set `Enabled = true` under `[Highlight.Living]`. |
| Highlight public enemies separately | Enable `[Highlight.Hostiles]`; it overrides Living for hostile NPCs. |
| Show doors, beds and workstations | Enable `[Highlight.Places]`. This detection is experimental. |
| Hide highlights behind walls and grass | Set `SeeThrough = false` under `[Render]`. |
| Hide highlights during minigames too | Set `HideIn = Dialogue, Minigame` under `[Settings]`. |

### What each group shows

`Enabled = true` turns a group on; `false` turns it off. `Targets` lists what it highlights, separated by commas:

- **Loot:** `Corpses, Carcasses, Items, Containers, Herbs`
- **Living creatures:** `NPCs, Animals, Horses, Dogs, Critters`
- **Interactive objects:** `Doors, Workstations, Beds, Seats, UseSpots, Objects`

`Only` restricts the group to matching objects; `Except` leaves matching objects out. Available tags are `Stolen, Locked, Empty, Butcherable, Hostile, Unconscious`. For example, `Targets = Containers` with `Only = Locked` shows locked containers, while `Except = Empty` skips empty ones. With multiple filters, every Only entry must match and no Except entry may match.

For custom selections, `Class:`, `Name:` and `Model:` match an object's class, name or model filename. Matches are case-insensitive; `*` matches any text and `?` matches one character. `Targets = Model:*barrel*` selects models whose filenames contain "barrel". These selections do not apply lootability checks.

### Behaviour and appearance

Set a group's `Mode` to choose how its key works:

| Mode | Behaviour |
| --- | --- |
| `Pulse` | Press once to show highlights for Duration seconds. Press again to restart the timer. |
| `Toggle` | Press once to turn highlights on, then again to turn them off. |
| `Hold` | Highlights stay visible while you hold the key. |
| `Always` | Highlights stay on without a keypress. |

`FadeOut` controls how many seconds they take to disappear; the default is **1 second**. `HideIn` accepts `Dialogue, Combat, Minigame` and defaults to `Dialogue`. Add `Combat` or `Minigame` to hide highlights in those situations too. Group hotkeys also work while menus are open.

`Style` chooses `Outline` (edges), `Fill` (tint), `OutlineFill` (both) or `Box` (corner brackets). The shared `[Render] Style` is `OutlineFill`; Herbs overrides it with `Outline`.

The main appearance settings under `[Render]` are:

| Setting | Effect | Default |
| --- | --- | --- |
| `OutlineWidth` | Edge thickness in silhouette-mask pixels. | `2.0` |
| `Strength` | Edge brightness; higher is brighter. | `0.5` |
| `FillOpacity` | Fill opacity, from 0 (invisible) to 1 (opaque). | `0.35` |
| `Softness` | Blur in pixels; higher values soften and thin the outline. | `0.0` |
| `MinOpacity` | Minimum brightness during distance fading. Lower it to enable fading. | `1.0` (no distance fading) |

When enabled, distance fading starts at `FadeStart` (12 metres) and reaches `MinOpacity` at the group's `Radius`. `FadePower` (1.0) shapes the fade; higher values keep highlights brighter farther out.

### Colours

`Color` sets the colour of targets without their own colour. Use `RRGGBB` or `RRGGBBAA` hex: `FF8000` and `FF8000FF` both give orange. Pick or convert a colour with the [colour picker](https://www.fleevio.com/en/tools/color-picker), copy **HEX (no #)** or **HEX + alpha (no #)**, then paste it into `Color` or after a target's colon:

```ini
Color = FF8000FF
Targets = Items:FF8000FF, Containers:FFD000FF
```

The final `AA` byte is ignored for highlights. Use `[Render] FillOpacity` to adjust fill transparency and `Style` to choose outline or fill. `FocusTint` uses **six digits only**, without `#`.

When active groups overlap, the **last group in the file wins** for that object. Put a general group first and a more specific one after it, for example, Loot followed by Stolen.

### Focus and particle effects

Set `Focus = true` in a group to darken and tint the background while it is active. Under `[Render]`, `FocusDarken` controls darkening: `0.0` leaves brightness unchanged and `1.0` is black; the supplied INI uses `0.5`. `FocusTint` defaults to `B8C4D8`; `FFFFFF` removes the tint. The effect eases in over `FocusFadeIn` (0.4 seconds) and out with the group's `FadeOut`. Outline and OutlineFill objects stay bright; Fill and Box objects darken with the background. No group enables Focus by default.

Add `Effect = HenrySenses.sparks` or `Effect = HenrySenses.glow` to a group for particles that show through walls; these require `KCD2_HenrySenses.particles.xml` beside the INI. Game effects such as `particles_smithery.particles_smithery.sparks` are hidden by walls. `EffectScale = 1.0` sets the size multiplier (0.1 to 10; default 1.0). An empty `Effect =` or `Effect = None` disables particles.

`Focus`, `Effect` and `EffectScale` are separate setting keys. To enable focus and sparks for loot, add these lines inside the existing `[Highlight.Loot]` section. Leave off the leading `;` used for comments in the template:

```ini
Focus = true
Effect = HenrySenses.sparks
EffectScale = 1.0
```

### Example: herbs on a separate toggle

Replace the existing `[Highlight.Herbs]` section with:

```ini
[Highlight.Herbs]
Enabled = true
Targets = Herbs
Color = 40FF40FF
Style = Outline
Key = F8,Gamepad_RB+Gamepad_Back
Key.Consume = true
Mode = Toggle
Radius = 40
```

After saving, **F8** or **RB + Back** switches green herb outlines on and off within **40 metres**. Loot keeps its usual key and range. This group still uses the shared FadeOut and HideIn settings because it does not specify its own.

### Keyboard and controller bindings

Set `Key` under Settings for shared controls, or inside a group for independent controls. Named keys such as `H`, `F7` and `Mouse4` work; `LShift+H` is a combination. Leave it empty or use `NONE` to remove a binding.

The shared binding defaults to **H** or **RB + Start**. To replace the controller combination with **RB + D-pad left**, use these entries under `[Settings]`:

```ini
Key = H,Gamepad_RB+Gamepad_DpadLeft
Key.Consume = true
```

The comma means "either binding"; the plus means "hold the first button and press the second". Herbs has its own Key, defaulting to **Left Shift + H** or **RB + Back**, so edit that section separately to change its controls.

`Key.Consume = true` hides the combination's last button from the game while held, so D-pad left does not also trigger its game action. It is **on by default** for the shared binding and Herbs. Only digital controller buttons and the mouse wheel can be consumed; modifiers, keyboard keys and analog triggers still reach the game.

Controller input uses **XInput**. For other controllers, use Steam Input or an XInput translation layer. The [supported input names](https://github.com/tkhquang/DetourModKit?tab=readme-ov-file#supported-input-names) include the available button names.

The [INI template](build/template/KCD2_HenrySenses.ini) documents all settings, defaults and further examples.

## Known Limitations

- Interactive-object detection is experimental; some objects may be missed or highlighted incorrectly.
- Some objects have no suitable model for an outline and use brackets with the `Auto` renderer.

## Troubleshooting

- Set `LogLevel = DEBUG` under `[Settings]`, save the INI and reproduce the problem.
- Check `KCD2_HenrySenses.log` beside the ASI. It records detection decisions and a state report when you save the INI.
- If outlines fail or the world disappears after loading, set `[Render] Backend = Markers` and restart the game.
- Include the log, INI, game/mod versions and reproduction steps in your report. A screenshot and location help with incorrect highlights.

## Building from Source

### Prerequisites

- Visual Studio 2022 with the C++ workload
- CMake 3.28 or newer
- Git (to fetch submodules)
- Python 3 (for version updates and rebuilding the embedded shader)

### Release Build

From the repository root in PowerShell:

```powershell
git submodule update --init --recursive
Set-Location HenrySenses
cmake --preset msvc-release
cmake --build --preset msvc-release
```

The single release ASI is placed at `build/release-msvc/KCD2_HenrySenses.asi`. Use the INI in `build/template/` for installation.

See [build notes](../BUILD_NOTES.md) for compiler memory limits and shell-specific commands.

### Dev Build (Hot-Reload)

The dev build produces a resident loader (`KCD2_HenrySenses.asi`) and a logic DLL (`KCD2_HenrySenses.logic.dll`), allowing you to rebuild and reload mod code without restarting the game.

From the `HenrySenses` directory, with the game closed for the initial build:

```powershell
cmake --preset msvc-dev -DHENRYSENSES_GAME_DIR="<game>/Bin/Win64MasterMasterSteamPGO"
cmake --build --preset msvc-dev
```

Replace the placeholder with your game's binary folder. The build deploys the loader there, replacing the release ASI, and copies the logic DLL and PDB into `staging/`. Keep the INI beside the loader. Without `HENRYSENSES_GAME_DIR`, the build stays local in `build/dev-msvc/`.

Start the game. After changing mod code, rebuild the logic DLL:

```powershell
cmake --build --preset msvc-dev --target KCD2_HenrySenses-logic
```

Press **Numpad 9** while the game window has focus to load the staged build. Loader changes require a game restart. A reload whose cleanup cannot finish retains the loaded generation and requires a restart before another reload. `KCD2_HenrySenses_Loader.log` records reloads; `KCD2_HenrySenses.log` contains the mod's diagnostics.

### Rebuilding the shader

After editing `shaders/HenrySensesSilhouette.cfx`, regenerate the embedded header from the `HenrySenses` directory:

```powershell
python scripts/cfxb.py header shaders/HenrySensesSilhouette.cfx src/render/silhouette_shader_blob.hpp --symbol SILHOUETTE_SHADER_CFXB
```

Then rebuild the release ASI or dev logic DLL using the commands above. For the dev build, press **Numpad 9** to load the new shader with the rebuilt logic. CMake uses the existing header and does not regenerate it automatically. Commit both the shader source and generated header; edit the source rather than the generated bytes.

### Code style

The C++23 sources follow a hard 120-column baseline and DetourModKit's coding conventions, with `.clang-format` (clang-format 20 or newer), `.editorconfig` and an advisory `.clang-tidy`. Long messages use adjacent string literals split at phrase boundaries.

Format changed source files before submitting changes and check them with `clang-format --dry-run --Werror <files>`. Editor IntelliSense, debugging and build tasks are configured in the repository-root `.vscode/` folder.
