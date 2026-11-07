# TPV Animations

Third-person animations for Henry. In first person the game's own animations play.

- **Idle gestures:** standing still, Henry stretches, scratches, yawns, spits, looks around and shifts his weight.
  Moving, crouching or turning stops the gesture at once.
- **Pick your gestures:** each gesture is its own file. Watch them on the [preview page](https://tkhquang.github.io/KCD2Tools/tpv_animations/) and delete the ones
  you don't want.
- **Drunk idle** for a drunk Henry.
- **Torch sprint:** Henry holds the torch low in front of him instead of up.
- **Smooth turns:** turn the camera further while Henry turns on the spot, and he carries on turning instead of restarting his step or standing still.
- Open source, built on the Animation Framework (built with DetourModKit).

## Requirements

- [Animation Framework](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3981) 1.0.0 or newer.
- A third-person camera mod that sets `mod_camera_view`, such as
  [TPV Camera](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3263) 3.0.0 or newer.
- Smooth turns need TPV Camera 3.2.1 or newer. With an older version, delete `tpv_animations_smooth_turn.pak`, or Henry's big turns restart mid-step.

## Installation

Install with your mod manager, or extract into `<game>/Mods/`. To uninstall, delete `Mods/tpv_animations`.

## Choosing what plays

Each part is its own pak in `Mods/tpv_animations/Data/`. Delete one to remove that part. The
[preview page](https://tkhquang.github.io/KCD2Tools/tpv_animations/) plays every animation, with its pak, and shows the game's animation next to the ones
this mod replaces.

| Pak | Contains |
|---|---|
| `tpv_animations_idle.pak` | The calm idle, needed for any gesture |
| `tpv_animations_idle_<gesture>.pak` | One gesture: `neck_stretch`, `back_stretch`, `scratch_face`, `forehead_wipe`, `yawn`, `spit`, `look_around`, `weight_shift_left`, `weight_shift_right`, `fidget02`, `fidget03`, `fidget04`, `fidget05`, `fidget07` |
| `tpv_animations_drunk.pak` | Drunk idle |
| `tpv_animations_torch_sprint.pak` | Torch sprint |
| `tpv_animations_smooth_turn.pak` | Smooth turns |

## How it works

- **Idle:** every few seconds the calm idle changes the game's `Variations` tags (`var1`..`var6`). The game then picks
  a new idle at random from the installed gestures, while Henry stays in his normal movement, so the torch grip and
  turning in place keep working.
- **Drunk idle and torch sprint:** options tagged `thirdPerson` added next to the game's own.
- **Smooth turns:** transitions between the game's own turn fragments, tagged `thirdPerson`. A requested turn starts at once instead of waiting for the blend space's average length, and crossing 90 degrees keeps the current step. TPV Camera decides when the next turn is requested.
- Each pak gives its clips the sounds of the game animations they are made from.

All clips are made from the game's own animations. Each gesture is a game fidget or gesture followed by calm idle in
one looping clip, without face tracks. The torch sprint is the game's sprint with the left arm re-keyed from four
hand-set poses.

## Packaging

The release packs `pack/` with [kcd-pak-action](https://github.com/tkhquang/kcd-pak-action) 1.1.0 or newer:
each `Data/<name>.pak/` folder becomes its own pak.

## License

The scripts are MIT licensed. The animations are made from the game's own and are not covered by it: see
[LICENSE](LICENSE).
