KINGDOM COME: DELIVERANCE II - THIRD PERSON CAMERA
Version 3.2.1

This mod is an .asi plugin. Kingdom Come Script Extender (KCSE) loads .asi
files itself; without KCSE you need an ASI loader, which is NOT included in
this download (see Step 1).

INSTALLATION

Find the installation folder:
  The mod supports the Steam version of the game. Its binary folder is the one
  that contains WHGame.dll:
    <KC:D 2 installation folder>/Bin/Win64MasterMasterSteamPGO/
  The loader and mod files go directly into this folder.

  If you previously installed KCD2_TPVToggle, remove KCD2_TPVToggle.asi and
  KCD2_TPVToggle.ini first. Running both camera mods at once will conflict.
  To keep a backup, move the old ASI outside the game folder or rename its
  extension to .asi.bak.

Step 1 - Install an ASI loader (one time):
  KCSE loads .asi files itself, so KCSE users skip this step. If you already
  have a working ASI loader, skip it too.
  KCSE: https://www.nexusmods.com/kingdomcomedeliverance2/mods/3332
  Download ONE x64 variant of Ultimate ASI Loader by ThirteenAG:
    - dinput8.dll  (usual choice if this name is unused)
        https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/dinput8-x64.zip
    - version.dll  (alternative when dinput8.dll is already used)
        https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/version-x64.zip
    - winmm.dll    (alternative if version.dll is used or does not load)
        https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/winmm-x64.zip
  Extract the DLL from the chosen ZIP directly beside WHGame.dll. Use x64, not
  Win32, and install only one Ultimate ASI Loader variant. Do not overwrite a
  DLL belonging to another mod.
  All loader variants: https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases

Step 2 - Install the mod:
  Extract this archive directly into that SAME binary folder, next to WHGame.dll
  and the loader DLL, without an extra archive-named subfolder.
  KCD2_TPVCamera.asi is the mod; KCD2_TPVCamera.ini contains the settings.
  The camera rules, which set how the camera treats each object in the world
  (which ones stop it, fade or open in the see-through window), are built into
  the mod. Put your own changes in KCD2_TPVCamera_rules_User.txt, which you
  create. ExportRules = true in the INI writes the built-in rules, with notes
  that explain them, to KCD2_TPVCamera_rules.default.txt.
  KCD2_TPVCamera_presets.json is created on first run, not included in the archive.
  Nothing goes into the Mods folder.

Step 3 - Launch and play:
  Start the game; third-person turns on automatically once you reach gameplay.
  Press F3 (default), or hold LB + RB on a controller, to toggle first/third person.

VERIFY THE LOADER IS WORKING:
  After launching once, look for KCD2_TPVCamera.log beside the ASI.
  If it is missing, check that the ASI and the x64 loader DLL are directly
  beside WHGame.dll. With KCSE, update KCSE to its latest version. Without it,
  try another unused loader variant and relaunch. Never overwrite a DLL that
  belongs to another mod.
  On Wine/Proton, also check the override below.

FOLDER LAYOUT (Steam):
  Bin/Win64MasterMasterSteamPGO/
  |-- WHGame.dll                         (game file, already present)
  |-- dinput8.dll                        (KCSE or the ASI loader)
  |-- KCD2_TPVCamera.asi                  (this mod)
  |-- KCD2_TPVCamera.ini                  (settings)
  |-- KCD2_TPVCamera_rules_User.txt       (your own camera rules; optional)
  |-- KCD2_TPVCamera_presets.json         (created on first run)
  \-- KCD2_TPVCamera.log                  (created when the mod loads)

UPDATING:
  Back up your customized INI before extracting the new archive, then reapply
  your settings to the supplied INI. Keep KCD2_TPVCamera_presets.json to retain
  your saved camera presets. An update brings its camera rules built in and
  never touches KCD2_TPVCamera_rules_User.txt. If you replaced the built-in
  rules with a KCD2_TPVCamera_rules.txt of your own, that file stays in force,
  and the rules of an update reach you only once you delete it.

UNINSTALLING:
  Delete every file whose name starts with KCD2_TPVCamera beside WHGame.dll.
  Delete the ASI loader DLL only if no other mod uses it. Optionally, also
  delete the files starting with kcd2_tpvcamera_ in the game's shader cache,
  usually %USERPROFILE%\Saved Games\kingdomcome2\shaders\cache\d3d12\, and in
  its cg...shaders subfolders. Leave the game's own files there alone.

LINUX / STEAM DECK (WINE/PROTON):
  In Steam's Properties -> Launch Options, add an override for your loader:
    WINEDLLOVERRIDES="dinput8=n,b" %command%
  Use version=n,b or winmm=n,b instead when using that loader name.
  For a command-line launch, set the same WINEDLLOVERRIDES value before your
  launch command. Keep any overrides your other mods need.

Hotkeys and settings are in KCD2_TPVCamera.ini. Full details and support:
https://github.com/tkhquang/KCD2Tools