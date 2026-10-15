## Third-Person Camera and HDR Fixes

- Fixed outlines turning into markers on items inside the radius while a third-person camera sits behind Henry.
- Fixed the game preparing the outline shader again at every start.
- Fixed outlines and fills not showing with HDR on in the game's options. With HDR, highlights keep their SDR contrast against the scene, and the game's UI brightness option sets their brightness as it does the HUD's. `[Render] HdrOutput` follows the game's HDR option by default.
