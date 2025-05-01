/**
 * @file constants.h
 * @brief Central definitions for constants used throughout the mod.
 *
 * Includes version info, filenames, default settings, AOB patterns, and memory offsets.
 * All hardcoded memory addresses have been replaced with AOB patterns for robustness.
 */
#define _USE_MATH_DEFINES
#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <string>
#include <math.h>
#include "version.h" // Mod versioning definitions

/**
 * @namespace Constants
 * @brief Encapsulates global constants and config defaults.
 */
namespace Constants
{
    // Version information derived from version.h
    constexpr const char *MOD_VERSION = Version::VERSION_STRING;
    constexpr const char *MOD_NAME = Version::MOD_NAME;
    constexpr const char *MOD_WEBSITE = Version::REPOSITORY;

    // File extensions
    constexpr const char *INI_FILE_EXTENSION = ".ini";
    constexpr const char *LOG_FILE_EXTENSION = ".log";

    /** @brief Gets the INI config filename (e.g., "KCD2_TPVToggle.ini"). */
    inline std::string getConfigFilename()
    {
        return std::string(MOD_NAME) + INI_FILE_EXTENSION;
    }
    /** @brief Gets the base log filename (e.g., "KCD2_TPVToggle.log"). */
    inline std::string getLogFilename()
    {
        return std::string(MOD_NAME) + LOG_FILE_EXTENSION;
    }

    // --- Default Configuration Values ---
    /** @brief Default logging level ("INFO"). */
    constexpr const char *DEFAULT_LOG_LEVEL = "INFO";

    // --- AOB (Array-of-Bytes) Patterns ---

    // WHGame.DLL+A27E07 - 7F 0D                 - jg WHGame.DLL+A27E16
    // WHGame.DLL+A27E09 - 48 8B 05 18EA9B04     - mov rax,[WHGame.DLL+53E6828] { (1D22B1887A0) }
    // WHGame.DLL+A27E10 - 48 83 C4 20           - add rsp,20 { 32 }
    // WHGame.DLL+A27E14 - 5B                    - pop rbx
    // WHGame.DLL+A27E15 - C3                    - ret
    /**
     * @brief AOB pattern to find code near the return path of the function
     *        that loads the Global Context Pointer. This provides access to the
     *        TPV flag data structure.
     */
    constexpr const char *CONTEXT_PTR_LOAD_AOB_PATTERN =
        "7F ?? 48 8B 05 ?? ?? ?? ?? 48 83 C4 20 5B C3";

    // WHGame.DLL+A50976 - 48 83 BB D8000000 00  - cmp qword ptr [rbx+000000D8],00 { 0 }
    // WHGame.DLL+A5097E - 77 27                 - ja WHGame.DLL+A509A7
    // WHGame.DLL+A50980 - 48 8B CB              - mov rcx,rbx
    /**
     * @brief AOB pattern for the overlay check (`cmp qword ptr [rbx+D8h],0`).
     *        Used to find the code that checks the overlay status.
     */
    constexpr const char *OVERLAY_CHECK_AOB_PATTERN =
        "48 83 BB D8 00 00 00 00 77 ?? 48 8B CB";

    // WHGame.DLL+3602F0 - 48 8B C4              - mov rax,rsp
    // WHGame.DLL+3602F3 - 48 89 58 08           - mov [rax+08],rbx
    // WHGame.DLL+3602F7 - 48 89 70 10           - mov [rax+10],rsi
    // WHGame.DLL+3602FB - 48 89 78 18           - mov [rax+18],rdi
    // WHGame.DLL+3602FF - 55                    - push rbp
    // WHGame.DLL+360300 - 41 54                 - push r12
    // WHGame.DLL+360302 - 41 57                 - push r15
    // WHGame.DLL+360304 - 48 8B EC              - mov rbp,rsp
    // WHGame.DLL+360307 - 48 83 EC 70           - sub rsp,70 { 112 }
    // WHGame.DLL+36030B - 33 F6                 - xor esi,esi
    /**
     * @brief AOB pattern for TPV FOV calculation function entry.
     *        Targets the function that updates the view's field of view.
     */
    constexpr const char *TPV_FOV_CALCULATE_AOB_PATTERN =
        "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 ?? ?? ?? ?? ?? 48 8B EC 48 83 EC ?? 33 F6";

    // WHGame.DLL+6192FA - 48 8B 15 4F98DC04     - mov rdx,[WHGame.DLL+53E2B50] { (1D22DCA7C40) }
    // WHGame.DLL+619301 - 48 8B CB              - mov rcx,rbx
    // WHGame.DLL+619304 - C7 42 14 08000000     - mov [rdx+14],00000008 { 8 }
    // WHGame.DLL+61930B - 66 0F6E 83 D0000000   - movd xmm0,[rbx+000000D0]
    // WHGame.DLL+619313 - 0F5B C0               - cvtdq2ps xmm0,xmm0
    // WHGame.DLL+619316 - F3 0F11 42 1C         - movss [rdx+1C],xmm0
    /**
     * @brief AOB pattern for finding the scroll state base address.
     *        Locates the instruction that references the global scroll accumulator structure.
     */
    constexpr const char *SCROLL_STATE_BASE_AOB_PATTERN =
        "48 8B 15 ?? ?? ?? ?? 48 8B CB C7 42 14 ?? ?? ?? ?? 66 0F 6E 83 ?? ?? ?? ?? 0F 5B C0 F3 0F 11 42 1C";

    // WHGame.DLL+619316 - F3 0F11 42 1C         - movss [rdx+1C],xmm0
    // WHGame.DLL+61931B - E8 30F0FFFF           - call WHGame.DLL+618350
    /**
     * @brief AOB pattern for the accumulator write instruction to be NOPed.
     *        Used for scroll wheel input filtering when overlays are active.
     */
    constexpr const char *ACCUMULATOR_WRITE_AOB_PATTERN =
        "F3 0F 11 42 1C E8 ?? ?? ?? ??";
    constexpr int ACCUMULATOR_WRITE_HOOK_OFFSET = 0;     // Hook starts at movss
    constexpr size_t ACCUMULATOR_WRITE_INSTR_LENGTH = 5; // Size of 'movss [rdx+1c], xmm0'

    /**
     * @brief AOB pattern for the event handler function that processes input events.
     *        Used to intercept and filter scroll wheel events during overlay display.
     */
    constexpr const char *EVENT_HANDLER_AOB_PATTERN =
        "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 48 8D 99";

    // --- AOB Hook Offsets ---
    constexpr int OVERLAY_HOOK_OFFSET = 0;
    constexpr int EVENT_HANDLER_HOOK_OFFSET = 0;

    // --- Memory Offsets ---
    constexpr ptrdiff_t OFFSET_ManagerPtrStorage = 0x38;      // Global context to camera manager
    constexpr ptrdiff_t OFFSET_TpvObjPtrStorage = 0x28;       // Camera manager to TPV object
    constexpr ptrdiff_t OFFSET_TpvFlag = 0x38;                // TPV object to flag byte
    constexpr ptrdiff_t OVERLAY_FLAG_OFFSET = 0xD8;           // UI module to overlay flag
    constexpr ptrdiff_t OFFSET_ScrollAccumulatorFloat = 0x1C; // Scroll state to accumulator
    constexpr ptrdiff_t OFFSET_TpvFovWrite = 0x30;            // FOV calculation offset

    // --- Input Event Offsets ---
    constexpr ptrdiff_t INPUT_EVENT_TYPE_OFFSET = 0x04;
    constexpr ptrdiff_t INPUT_EVENT_BYTE0_OFFSET = 0x00;
    constexpr ptrdiff_t INPUT_EVENT_ID_OFFSET = 0x10;
    constexpr ptrdiff_t INPUT_EVENT_VALUE_OFFSET = 0x18;
    constexpr int INPUT_EVENT_BYTE0_EXPECTED = 0x01;
    constexpr int MOUSE_INPUT_TYPE_ID = 8;
    constexpr int MOUSE_WHEEL_EVENT_ID = 0x10C;

    // --- Timing ---
    constexpr unsigned long OVERLAY_MONITOR_INTERVAL_MS = 5; // Fast overlay thread sleep
    constexpr unsigned long MAIN_MONITOR_SLEEP_MS = 33;      // Main thread sleep

    /** @brief Name of the target game module. */
    constexpr const char *MODULE_NAME = "WHGame.dll";

} // namespace Constants
#endif // CONSTANTS_H
