## Initial Release

- Dev tool for mod authors, still in development: not for normal play, use at your own risk.
- Reloads a rebuilt mod pak while the game runs, with no restart and without touching the installed copy.
- Picks up a new build about a second after it is written, and skips a build that changes nothing.
- Textures, static meshes, character skins and materials update in place.
- Table changes reload live, and every table that refers to a changed one follows; changes to CharacterComponent wait for a restart.
- Changed mod scripts run again, after an optional OnHotReload cleanup call.
- Clothing that a broken build left missing is retried by the next build.
- The log names every change that still needs a restart.
- Recognizes the Steam, GOG and Game Pass versions of the game.
- Saving the settings INI applies it while playing.
