## Updated to DetourModKit v3.4.0

- Updated the mod's underlying framework (DetourModKit) to v3.4.0 for better stability and future compatibility.
- Smoother performance: the third-person camera, FOV, and mouse-input features now read game memory more efficiently, reducing the chance of frame hitches.
- More crash-resistant: memory reads on those features are now fault-guarded, lowering the risk of rare crashes from stale game data.
- Smoother camera-offset adjustments: fixed a threading issue that could briefly glitch the camera while tweaking offsets.
- Safer scroll handling: fixed a memory-safety issue in the scroll-wheel reset.
- Leaner build: removed unused internal code and tidied the project, with no effect on features.
- Maintenance update only: no changes to settings, controls, or behavior. Existing configurations keep working as-is.
