## Camera Sensitivity Control System

### Added
- **Camera Sensitivity Control System**
  - Independent pitch and yaw sensitivity multipliers
  - Configurable vertical pitch limits to restrict camera angles (WIP)
  - New `[CameraSensitivity]` section in configuration
  - Updated third-party licenses documentation

### Configuration
New INI settings under `[CameraSensitivity]`:
```ini
PitchSensitivity = 1.0      ; Vertical sensitivity (0.0-2.0)
YawSensitivity = 1.0        ; Horizontal sensitivity (0.0-2.0)
EnablePitchLimits = false   ; Enable pitch angle limits (WIP, has bugs)
PitchMin = -180.0           ; Minimum pitch angle in degrees
PitchMax = 180.0            ; Maximum pitch angle in degrees
```
