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
- Automatic view switching by situation: switch to first or third person when you enter combat, aiming a bow, dialogue, minigames, riding, or menus (you can still toggle manually during it), with your previous view restored when the situation ends
- In-game preset manager: edit, add, and save camera presets from an overlay; built-in presets for normal play, combat, aiming, horseback, sneaking, and special poses (lying down, sitting, kneeling, riding a cart) apply automatically by situation, and you can bind your own presets to combinations of states (such as aiming while crouched), with the most specific match winning
- Free-look look sensitivity is set per axis for both mouse and gamepad, so horizontal and vertical can be tuned independently; set a negative value to invert that axis. Mark any preset setting as Shared to apply its value to every preset at once
- Camera collision with an optional see-through mode (off by default) that ignores thin posts, rails and fences you can see past and keeps out of corners, plus a cloth-roof clamp (on by default) so a look-down is not buried in tent or awning fabric; all tunable in the INI
- Crosshair convergence so the screen-center reticle lines up with what you point at
- Full keyboard and XInput controller support
- Almost every setting is editable live while the game runs

## Installation

This mod is an `.asi` plugin, so it needs an **ASI loader** to run. The loader is **not bundled** in the
download; you install it once yourself (Step 2). If you already have an ASI loader for KC:D 2 from another
mod, skip that step.

1. **Remove the old mod (if present).** If you previously installed the older **KCD2_TPVToggle** mod,
   delete `KCD2_TPVToggle.asi` and `KCD2_TPVToggle.ini` from the game's binary folder. TPVCamera replaces
   it, and running both third-person camera mods at once will conflict.
2. **Install an ASI loader.** Download [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases)
   by **ThirteenAG** and place one of these DLLs in your game's binary folder (the `Bin/Win64MasterMaster...`
   subfolder that contains `WHGame.dll`):
   - [`dinput8.dll`](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/dinput8-x64.zip) - recommended (tested with KC:D 2)
   - [`version.dll`](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/version-x64.zip) - alternative if `dinput8.dll` does not work
   - [`winmm.dll`](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/winmm-x64.zip) - another alternative

   Each link downloads a ZIP; extract just the DLL next to `WHGame.dll`.
3. **Install the mod.** Extract all files from this mod's archive into that **same** binary folder (next to
   `WHGame.dll` and the loader DLL). On Steam this is
   `<KC:D 2 installation folder>/Bin/Win64MasterMasterSteamPGO/`; the GOG version uses its own
   `Bin/Win64MasterMaster...` folder.
4. **Launch and play.** Third-person turns on automatically once you reach gameplay. Press `F3` (default),
   or hold `LB + RB` on a controller, to toggle back to first-person.

To check the loader is working, launch once and look for a `KCD2_TPVCamera.log` file in the binary folder.
If it is missing, the loader is not loading the mod; try the other loader DLL above (rename it if needed)
and relaunch.

### Linux / Steam Deck (Wine/Proton)

Your ASI loader DLL needs a Wine/Proton DLL override so the game loads it instead of the built-in
version. If you installed `dinput8.dll` (the recommended loader), set the following override:

- **Steam:** Go to the game's **Properties -> Launch Options** and add:
  `WINEDLLOVERRIDES="dinput8=n,b" %command%`
- **Command line:** Prepend your launch command with:
  `WINEDLLOVERRIDES="dinput8=n,b"`

If you chose a different loader DLL, use its name instead (for example `winmm=n,b` for `winmm.dll`).

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

Free-look orbit has two styles: `OrbitToggleKey` (default `F4`) latches it on and off, while
`OrbitHoldKey` (unbound by default) is a momentary "freelook" you hold to look around and release
to snap straight back to the aim camera. Bind `OrbitHoldKey` in the INI to use it.

On a controller the zoom shares the D-pad with the game's inventory/map shortcut, so the mod hides
that D-pad button from the game while you zoom (the `ZoomInKey.Consume` / `ZoomOutKey.Consume` keys,
on by default) so finishing a zoom does not open the inventory or map. Set them to `false` if you
would rather the D-pad keep reaching the game. The keyboard zoom keys are unaffected.

## How It Works

The camera hooks the engine's frustum builder and offsets the game view camera's matrix there,
before the cull planes are computed, so the rendered view and its culling move together (this is
what keeps nearby geometry from being wrongly hidden). A companion hook keeps the player head
rendered, and an input hook powers the free-look orbit.

The offset is automatically suppressed while a menu or overlay (inventory, map, dialog, codex) is
open, and while the engine is already in its own built-in third-person view (such as horseback),
so those contexts render from the untouched engine view.

## Configuration

Edit `KCD2_TPVCamera.ini`. It is grouped into:

- `[Settings]` - log level, the view hotkeys (`ToggleViewKey`, `ForceFPVKey`, `ForceTPVKey`), the preset-overlay key (`ToggleOverlayKey`), and the start-of-session auto-enable flags
- `[Camera]` - zoom keys, the camera-space interaction toggle, the view-transition ease, and the camera-stability options (`StableAimBasis`, `AimBasisSmoothing`) that keep the view steady against head-bob, sway, and combat shake (the framing itself is per-preset; see `[Presets]`)
- `[Orbit]` - the free-look orbit keys (press-to-toggle and momentary hold) and the cursor freeze (the orbit feel is per-preset)
- `[Collision]` - the collision probe and radius, `UseCoverageCollision` (only pull in for things that hide your character) with its coverage / side-wall options, and the independent `UseRenderOcclusion` cloth-roof clamp (enable, skin, and return speed are per-preset)
- `[StateBehavior]` - switch first/third person on entering a situation (combat, aiming, dialogue, minigame, mount, menu, overlay), restore the prior view on exit (manual toggles during it stick), and suspend/restore free-look in chosen situations
- `[Presets]` - the preset blend speed; the camera framing lives in the in-game preset manager (open with `ToggleOverlayKey`) and is stored in `KCD2_TPVCamera_presets.json` next to the INI. That file is created automatically from the built-in defaults on first run; it is not shipped, so updating the mod never overwrites presets you have tuned

Most values apply the next frame after you save the file. Every option is documented in the INI.

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

- Set `LogLevel = DEBUG` in the INI file.
- Check `KCD2_TPVCamera.log` (next to the game executable) for details.
- If the camera looks wrong in a specific scene (cutscene, photo mode), toggle it off with
  `F3` and back on when normal play resumes.

## Known Limitations

- Free-look orbit is a raw proof-of-concept. It is fine for swinging the camera around to
  look at the front of Henry, but it is rough for normal gameplay (interaction while orbiting
  does not work yet).
- Free-look orbit conflicts with the game's own camera control on horseback and in combat, so
  it is auto-disabled in those situations.
- The first-person body rig can still look slightly off from behind in some animations.
- Camera collision keeps the view out of walls but has no soft-edge smoothing yet.
- The raycast-based crosshair fix is partial: it does not cover every interaction type, so some
  still show parallax or an offset, and in a dense area with several interactables the use-prompt
  can jump back and forth.
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
- Soft-edge camera collision so the view eases around obstacles instead of snapping.
- Handle thin occluders (fence rails, thin foliage) between the camera and the character more
  cleanly.
- Fix shadows being cut off behind the character (the shadow cascade still follows the
  first-person eye).
- Extend the raycast-based crosshair/interaction fix to cover more interaction types and stop
  the use-prompt jumping between nearby interactables in dense areas.

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

A two-DLL configuration builds a thin loader ASI plus a logic DLL that the loader reloads in
place (press Numpad 0 in game) for fast iteration.

```bash
# Configure and build the loader + logic DLL (point it at the game plugins dir)
cmake --preset msvc-dev -DTPVCAMERA_GAME_DIR="<game>/Bin/Win64MasterMasterSteamPGO"
cmake --build --preset msvc-dev
```

### Code style

The C++ sources follow a hard 120-column baseline and DetourModKit's coding conventions
([AGENTS.md](https://github.com/tkhquang/DetourModKit/blob/main/AGENTS.md)), codified in
`.clang-format` (clang-format 20), `.editorconfig`, and an
advisory `.clang-tidy`. Run the formatter over any changed sources before committing and keep it
idempotent (`clang-format --dry-run --Werror` must be silent). Editor IntelliSense and build tasks
are configured at the repository-root `.vscode/`, not under `TPVCamera/`.

## Architecture

- `src/hooks/camera_hook.cpp` - the camera: frustum-builder matrix offset, head visibility,
  free-look orbit, collision, and aim convergence
- `src/hooks/ui_menu_hooks.cpp`, `src/hooks/ui_overlay_hooks.cpp` - menu/overlay detection used
  to suppress the offset under UI
- `src/game_interface.cpp` - resolves the engine's global-context pointer (the camera-manager
  root) that the game-state detection walks
- `src/game_state.cpp` - derives the game state (combat, dialogue, minigame, mount, menu, overlay)
  that drives the `[StateBehavior]` auto-switching, reading the active engine camera class by RTTI
- `src/physics_raycast.cpp` - the engine ray helper used by collision and aim convergence
- `src/presets/`, `src/overlay/` - the per-state camera preset model and JSON store, plus the
  self-hosted ImGui overlay (preset editor) that previews edits on the live camera
- `src/config.cpp`, `src/global_state.cpp`, `src/tpv_camera.cpp` - configuration, shared state,
  and the mod lifecycle

Game addresses are located patch-resiliently. Every hooked function and read global is found
through a multi-candidate AOB cascade (`src/aob_resolver.hpp`): each target carries several ordered
signatures, most-specific first, and the first that resolves wins, so a game patch only has to
leave one anchor intact for the feature to keep working. The cascades are declared as a single
DetourModKit anchor registry (`src/aob_resolver.cpp`) and resolved in one parallel pass at startup,
which logs a per-anchor status and an overall quality summary. The cascade is scanned only inside the
`WHGame.dll` image, so a signature that happens to also appear in another injected mod or graphics
overlay can never be mistaken for the game's. At least one candidate per cascade anchors past the
function prologue, so resolution still succeeds when another mod has inline-hooked the entry; the
hooks themselves install with a fail-closed prologue check so a mis-resolved entry is refused rather
than patched blindly. The `gEnv` global resolves through the same cascade (with a static RVA
fallback), and engine object types are matched by their MSVC RTTI names rather than hardcoded vtable
addresses. The cascade signatures follow DetourModKit's
[AOB signature guide](https://github.com/tkhquang/DetourModKit/blob/main/docs/misc/aob-signatures.md).

The AOB cascades resolve struct base addresses; a self-healing offset layer (`src/offset_heal.hpp`)
covers the field offsets walked inside those structs. A game patch that inserts or removes a member
shifts every field after it, so each in-scope offset is keyed to the MSVC RTTI name of the object its
slot points at, and DetourModKit's reverse-RTTI self-heal (`rtti_dissect.hpp`) scans a small window
around the nominal offset for the slot that still resolves to that type. The recovery runs once per
session the first time a live, RTTI-validated player and context resolve (never per frame); the
per-frame chains then read the cached offset. The look controller, whose pointee has no RTTI of its
own, is bracketed by its two RTTI-typed neighbours and only moves when both agree on one shift.
Everything is fail-closed: an offset the heal cannot recover stays at its built-in value, so the mod
degrades to its previous behaviour rather than reading the wrong memory.

## Credits

- [ThirteenAG](https://github.com/ThirteenAG) - for the Ultimate ASI Loader
- [cursey](https://github.com/cursey) - for SafetyHook
- [Brodie Thiesfield](https://github.com/brofield) - for SimpleIni
- [Frans 'Otis_Inf' Bouma](https://opm.fransbouma.com/intro.htm) - for his camera tools and inspiration
- Warhorse Studios - for Kingdom Come: Deliverance II

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
