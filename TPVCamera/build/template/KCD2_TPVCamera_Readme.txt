KINGDOM COME: DELIVERANCE II - THIRD PERSON CAMERA
Version 1.2.0

This mod is an .asi plugin, so it needs an ASI loader to run. The loader is NOT
included in this download - you install it once, yourself (see Step 1). If you
already have an ASI loader for KC:D 2 from another mod, skip Step 1.

INSTALLATION

Step 1 - Install an ASI loader (one time):
  Download Ultimate ASI Loader by ThirteenAG and place ONE of these DLLs in your
  game's binary folder (the Bin/Win64MasterMaster... folder that contains WHGame.dll):
    - dinput8.dll  (recommended)
        https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/dinput8-x64.zip
    - version.dll  (alternative, if dinput8.dll does not work)
        https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/version-x64.zip
    - winmm.dll    (another alternative)
        https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/winmm-x64.zip
  Each link downloads a ZIP; extract just the DLL and drop it next to WHGame.dll.
  All loader variants: https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases

Step 2 - Install the mod:
  Extract ALL files from this archive into that SAME binary folder, next to WHGame.dll
  and the loader DLL you placed. On Steam:
    <KC:D 2 installation folder>/Bin/Win64MasterMasterSteamPGO/
  The GOG version uses its own Bin/Win64MasterMaster... folder.

Step 3 - Launch and play:
  Start the game; third-person turns on automatically once you reach gameplay.
  Press F3 (default), or hold LB + RB on a controller, to toggle first/third person.

VERIFY THE LOADER IS WORKING:
  After launching once, look for KCD2_TPVCamera.log in the binary folder. If it is
  not there, the loader is not loading the mod - try one of the other loader DLLs
  above (rename it if needed) and relaunch.

Hotkeys and settings are in KCD2_TPVCamera.ini. Full details and support:
https://github.com/tkhquang/KCD2Tools
