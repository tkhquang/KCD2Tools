[size=6]Third Person View Enabler for KCD2[/size]

[size=5]Description[/size]
This lightweight mod enables toggling between first-person and third-person camera views in Kingdom Come: Deliverance II using customizable hotkeys, giving you a new perspective on the medieval world of Bohemia.

[size=5]Features[/size]
[list]
[*] Toggle between first-person and third-person views with a keypress (default: F3)
[*] Customize toggle keys through INI configuration
[*] Minimal game modification for better stability
[*] Efficient memory scanning with minimal performance impact
[*] Open-source with full transparency
[/list]

[size=5]Requirements[/size]
[list]
[*] Kingdom Come: Deliverance II (Steam version)
[*] [url=https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases]Ultimate ASI Loader[/url]
[/list]

[size=5]Installation[/size]
1. Install [url=https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases]Ultimate ASI Loader[/url] by placing dinput8.dll in your game directory:
   [code]<KC:D 2 installation folder>/Bin/Win64MasterMasterSteamPGO/[/code]
2. Place KCD2_TPVToggle.asi and KCD2_TPVToggle.ini in the same directory
3. Launch the game and press F3 (default) to toggle the camera view

[size=5]Configuration[/size]
The mod can be configured by editing the KCD2_TPVToggle.ini file:
[code]
[Settings]
; Toggle keys (hex format, comma-separated)
ToggleKey = 0x72

; Logging level: DEBUG, INFO, WARNING, ERROR
LogLevel = INFO

; AOB Pattern (advanced users only)
AOBPattern = 48 8B 8F 58 0A 00 00
[/code]

[size=5]Known Issues[/size]
[list]
[*] Camera may clip through objects in third-person view
[*] Some game actions may temporarily be buggy in third-person view
[*] The third-person camera uses the game's experimental implementation and may not be perfect
[*] Currently only tested with the Steam version of the game
[/list]

[size=5]Troubleshooting[/size]
If you encounter issues:
[list]
[*] Set LogLevel = DEBUG in the INI file
[*] Check the log file in your game directory (KCD2_TPVToggle.log)
[*] After a game update, you may need to update the AOB pattern in the INI file
[*] Make sure the Ultimate ASI Loader is correctly installed
[/list]

[size=5]Credits[/size]
[list]
[*] Frans 'Otis_Inf' Bouma for his camera tools and inspiration
[*] [url=https://github.com/ThirteenAG]ThirteenAG[/url] for the Ultimate ASI Loader
[*] Warhorse Studios for creating this amazing game
[/list]

[size=5]Source Code[/size]
This mod is open source. The full source code is available on [url=https://github.com/tkhquang/KDC2Tools]GitHub[/url].
