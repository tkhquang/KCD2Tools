## Updated to DetourModKit v3.4.0 & FOV fix

- Fixed third-person custom FOV: works again, applies on the next frame after INI edits, survives FPV/TPV toggles, and installs even when no camera offset is set.
- Updated the mod's underlying framework (DetourModKit) to v3.4.0 for better stability and future compatibility.
- Smoother performance and fewer rare crashes: camera, FOV, and mouse-input features read game memory more efficiently and with fault guards.
- Smoother camera-offset adjustments and safer scroll-wheel handling.
- Leaner build with no effect on features; existing configurations keep working as-is.
