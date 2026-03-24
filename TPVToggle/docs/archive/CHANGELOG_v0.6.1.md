## Migrated to DetourModKit v2

- **Breaking: Configuration key format updated** — Keys now use human-readable names (`F3`, `Numpad1`, `Ctrl+Shift+F3`) instead of hex VK codes (`0x72`). Hex codes are still supported for backwards compatibility.
- **Key combo support** — Key bindings now support modifier keys (e.g., `Ctrl+F3`, `Alt+Shift+T`). Commas separate independent combos with OR logic (e.g., `F3,F4` means F3 OR F4). Modifiers only apply to their own combo (e.g., `Ctrl+F3,F4` means Ctrl+F3 OR F4 alone; use `Ctrl+F3,Ctrl+F4` for Ctrl with both).
- **Removed legacy Logger adapter** — The custom `Logger` class and `LogLevel` enum have been removed. All logging now uses `DMKLogger` directly from DetourModKit.
- **Refactored configuration system** — Configuration loading now uses DMK v2's `register_key_combo`, `register_float`, `register_bool`, and `register_string` APIs.
- **Updated default INI template** — All key bindings use named keys, with documentation linking to the [supported input names](https://github.com/tkhquang/DetourModKit?tab=readme-ov-file#supported-input-names) reference.
- **Native gamepad support** — Controller buttons can now be used directly in key bindings (e.g., `Gamepad_Y`, `Gamepad_LB+Gamepad_A`).
- **New: Configurable overlay restore delay** — Added `OverlayRestoreDelayMs` setting to control the delay before restoring TPV after menus close (default: 200ms, set to 0 to disable).
- **Switched CI builds to MSVC** — Release binaries are now built with MSVC, reducing file size from ~2.4MB to ~1.1MB.
- **Simplified codebase** — Reduced wrapper layers and direct dependency on Windows virtual-key codes throughout the mod.
