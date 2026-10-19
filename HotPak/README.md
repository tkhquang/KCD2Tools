# KCD2_HotPak

> **Dev tool, still in development.** It is for mod authors testing their own builds. Do not use it in normal play. Use it at your own risk.

Reloads a rebuilt mod pak in the running Kingdom Come: Deliverance II, with no restart and without touching the installed copy. Works on the Steam, GOG and Game Pass builds.

## How it works

1. **Watch.** Each pak in `Paks=` is polled. A finished build is copied to `%LOCALAPPDATA%\KCD2_HotPak\<name>.gNNNN.pak`, so the build folder stays writable.
2. **Diff.** The copy is compared with what the game reads now. A build that changes nothing is not mounted.
3. **Mount.** The copy is mounted after every installed pak, so its files win.
4. **Reload what changed.**

| Changed file | What happens |
|---|---|
| `.dds` and split mips | The texture reloads. |
| `.cgf` | The static mesh refreshes. |
| `.skin`, `_lodN.skin` | The cached skin is marked old, and clothing that still shows it reloads. |
| `.mtl` | The loaded material is renamed out of the material manager, and clothing that uses it reloads. |
| `Libs/Tables/<folder>/<Name>[__<mod>].xml` | The table reloads, every table linked to it is linked again in the game's order, and characters wearing the pak's skins restream. `Character/CharacterComponent` needs a restart. |
| `.lua` | Each `Scripts/Mods/<modid>.lua` in the pak runs again, after `<modid>:OnHotReload()` when the mod defines it. |
| Storm XML, anything else | Logged as needing a restart. |

Overlays stay mounted until the game exits, and the next run deletes the copies. A part of the tool whose game code is not found on a game version stays off, and the log names it.

## Scripts that survive a rerun

A rerun repeats everything the script did, so a wrapper of `Player.OnAction` wraps the previous wrapper. Keep the mod table, install patches once, and undo the rest in `OnHotReload`:

```lua
mymod = mymod or {}                -- the same table across reruns

function mymod:OnAction(action, activation, value)
    -- replaced on every rerun
end

if not mymod.hooked then           -- installed once
    mymod.hooked = true
    local original = Player.OnAction
    function Player:OnAction(action, activation, value)
        if original then original(self, action, activation, value) end
        mymod:OnAction(action, activation, value)
    end
end

function mymod:OnHotReload()       -- called by HotPak before the rerun
    if self.timerId then Script.KillTimer(self.timerId); self.timerId = nil end
end
```

## Setup (dev build)

```
cmake --preset msvc-dev -DHOTPAK_GAME_DIR="<game>/Bin/Win64MasterMasterSteamPGO"
cmake --build --preset msvc-dev
ctest --test-dir build/dev-msvc -C RelWithDebInfo
```

Copy `build/template/KCD2_HotPak.ini` beside `KCD2_HotPak.asi` and list your built paks in `Paks=`. Numpad 7 loads a newly built logic DLL. Saving the INI applies it. The log is `KCD2_HotPak.log` beside the ASI.
