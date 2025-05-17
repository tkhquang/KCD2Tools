/**
 * @file constants.h
 * @brief Central definitions for constants used throughout the mod.
 *
 * Includes version info, filenames, default settings, AOB patterns, and memory offsets.
 * All hardcoded memory addresses have been replaced with AOB patterns for robustness.
 */
#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <string>
#include <math.h>

#include "version.h"

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

    // WHGame.DLL+6186A8 - 48 89 5C 24 10        - mov [rsp+10],rbx
    // WHGame.DLL+6186AD - 48 89 74 24 18        - mov [rsp+18],rsi
    // WHGame.DLL+6186B2 - 55                    - push rbp
    // WHGame.DLL+6186B3 - 57                    - push rdi
    // WHGame.DLL+6186B4 - 41 54                 - push r12
    // WHGame.DLL+6186B6 - 41 56                 - push r14
    // WHGame.DLL+6186B8 - 41 57                 - push r15
    // WHGame.DLL+6186BA - 48 8B EC              - mov rbp,rsp
    // WHGame.DLL+6186BD - 48 83 EC 60           - sub rsp,60 { 96 }
    // WHGame.DLL+6186C1 - 48 8D 99 80000000     - lea rbx,[rcx+00000080]
    // WHGame.DLL+6186C8 - 40 32 FF              - xor dil,dil
    /**
     * @brief AOB pattern for the event handler function that processes input events.
     *        Used to intercept and filter scroll wheel events during overlay display.
     */
    constexpr const char *EVENT_HANDLER_AOB_PATTERN =
        "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC ?? 48 8D 99 80 00 00 00";

    // WHGame.DLL+3924908 - 48 8B C4              - mov rax,rsp
    // WHGame.DLL+392490B - 48 89 58 08           - mov [rax+08],rbx
    // WHGame.DLL+392490F - 48 89 78 10           - mov [rax+10],rdi
    // WHGame.DLL+3924913 - 55                    - push rbp
    // WHGame.DLL+3924914 - 48 8B EC              - mov rbp,rsp
    // WHGame.DLL+3924917 - 48 83 EC 70           - sub rsp,70 { 112 }
    // WHGame.DLL+392491B - 80 3A 01              - cmp byte ptr [rdx],01 { 1 }
    // AOB for FUN_183924908 (TPV Camera Input Processing)
    constexpr const char *TPV_INPUT_PROCESS_AOB_PATTERN = "48 8B C4 48 89 58 08 48 89 78 10 55 48 8B EC 48 83 EC ?? 80 3A 01";

    // WHGame.DLL+36059C - 48 89 5C 24 08        - mov [rsp+08],rbx
    // WHGame.DLL+3605A1 - 48 89 74 24 10        - mov [rsp+10],rsi
    // WHGame.DLL+3605A6 - 48 89 7C 24 18        - mov [rsp+18],rdi
    // WHGame.DLL+3605AB - 41 56                 - push r14
    // WHGame.DLL+3605AD - 48 83 EC 20           - sub rsp,20 { 32 }
    // WHGame.DLL+3605B1 - 49 8B 01              - mov rax,[r9]
    // WHGame.DLL+3605B4 - 48 8B FA              - mov rdi,rdx
    // AOB for FUN_18036059c (Player State Copy Function)
    constexpr const char *PLAYER_STATE_COPY_AOB_PATTERN = "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC ?? 49 8B 01 48 8B FA";

    // AOB for TPV Camera Update Function
    // WHGame.DLL+392509C - 48 8B C4              - mov rax,rsp
    // WHGame.DLL+392509F - 48 89 58 08           - mov [rax+08],rbx
    // WHGame.DLL+39250A3 - 48 89 70 10           - mov [rax+10],rsi
    // WHGame.DLL+39250A7 - 48 89 78 18           - mov [rax+18],rdi
    // WHGame.DLL+39250AB - 55                    - push rbp
    // WHGame.DLL+39250AC - 41 56                 - push r14
    // WHGame.DLL+39250AE - 41 57                 - push r15
    // WHGame.DLL+39250B0 - 48 8D 68 A1           - lea rbp,[rax-5F]
    // WHGame.DLL+39250B4 - 48 81 EC D0000000     - sub rsp,000000D0 { 208 }
    // WHGame.DLL+39250BB - 0F29 70 D8            - movaps [rax-28],xmm6
    // WHGame.DLL+39250BF - 4C 8B F9              - mov r15,rcx
    // WHGame.DLL+39250C2 - 48 8B 0D E716AC01     - mov rcx,[WHGame.DLL+53E67B0] { (7FFE73DB90A0) }
    // WHGame.DLL+39250C9 - 48 8B F2              - mov rsi,rdx
    constexpr const char *TPV_CAMERA_UPDATE_AOB_PATTERN = "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 56 41 57 48 8D 68 ?? 48 81 EC ?? ?? ?? ?? 0F 29 70 ?? 4C 8B F9 48 8B 0D ?? ?? ?? ?? 48 8B F2";

    // AOB patterns for direct UI overlay hooks
    // WHGame.DLL+CCF1B4 (HideOverlays):
    // WHGame.DLL+CCF1B4 - 44 88 44 24 18        - mov [rsp+18],r8b
    // WHGame.DLL+CCF1B9 - 53                    - push rbx
    // WHGame.DLL+CCF1BA - 48 83 EC 20           - sub rsp,20 { 32 }
    // WHGame.DLL+CCF1BE - 0FB6 C2               - movzx eax,dl
    // WHGame.DLL+CCF1C1 - 48 8B D9              - mov rbx,rcx
    // WHGame.DLL+CCF1C4 - 48 8D 15 E50DDC02     - lea rdx,[WHGame.DLL+3A8FFB0] { ("HideOverlays") }
    // WHGame.DLL+CCF1CB - C6 84 08 80000000 01  - mov byte ptr [rax+rcx+00000080],01 { 1 }
    constexpr const char *UI_OVERLAY_HIDE_AOB_PATTERN =
        "44 88 44 24 18 53 48 83 EC 20 0F B6 C2 48 8B D9 48 8D 15 ?? ?? ?? ?? C6 84 08 80 00 00 00 01";

    // WHGame.DLL+CCF270 (ShowOverlays):
    // WHGame.DLL+CCF270 - 44 88 44 24 18        - mov [rsp+18],r8b
    // WHGame.DLL+CCF275 - 53                    - push rbx
    // WHGame.DLL+CCF276 - 48 83 EC 20           - sub rsp,20 { 32 }
    // WHGame.DLL+CCF27A - 0FB6 C2               - movzx eax,dl
    // WHGame.DLL+CCF27D - 48 8B D9              - mov rbx,rcx
    // WHGame.DLL+CCF280 - 80 BC 08 80000000 00  - cmp byte ptr [rax+rcx+00000080],00 { 0 }
    // WHGame.DLL+CCF288 - 74 48                 - je WHGame.DLL+CCF2D2
    constexpr const char *UI_OVERLAY_SHOW_AOB_PATTERN =
        "44 88 44 24 18 53 48 83 EC 20 0F B6 C2 48 8B D9 80 BC 08 80 00 00 00 00 74 ??";

    // --- UI Menu Hook Patterns ---
    // WHGame.DLL+5457B0 - 48 89 5C 24 10        - mov [rsp+10],rbx
    // WHGame.DLL+5457B5 - 48 89 74 24 18        - mov [rsp+18],rsi
    // WHGame.DLL+5457BA - 55                    - push rbp
    // WHGame.DLL+5457BB - 57                    - push rdi
    // WHGame.DLL+5457BC - 41 56                 - push r14
    // [snip]
    // WHGame.DLL+5457F7 - 48 8B 41 B0           - mov rax,[rcx-50]
    // WHGame.DLL+5457FB - 48 8B 48 30           - mov rcx,[rax+30]
    // WHGame.DLL+5457FF - 48 8B 01              - mov rax,[rcx]
    // WHGame.DLL+545802 - FF 10                 - call qword ptr [rax]
    // WHGame.DLL+545804 - 48 8D 15 7D035D03     - lea rdx,[WHGame.DLL+3B15B88] { ("SetInputId") }
    /**
     * @brief AOB pattern for UI menu open function (vftable[1]).
     *        Used to detect when the in-game menu is opened.
     */
    constexpr const char *UI_MENU_OPEN_AOB_PATTERN =
        "48 8B 41 B0 48 8B 48 30 48 8B 01 FF 10 48 8D 15 ?? ?? ?? ??";

    // WHGame.DLL+543E20 - 48 89 5C 24 18        - mov [rsp+18],rbx
    // [snip]
    // WHGame.DLL+544027 - 8A 57 48              - mov dl,[rdi+48]
    // WHGame.DLL+54402A - 48 8D 4F 28           - lea rcx,[rdi+28]
    // WHGame.DLL+54402E - C6 47 49 00           - mov byte ptr [rdi+49],00 { 0 }
    // WHGame.DLL+544032 - E8 BD140000           - call WHGame.DLL+5454F4
    // WHGame.DLL+544037 - C6 47 48 00           - mov byte ptr [rdi+48],00 { 0 }
    /**
     * @brief AOB pattern for UI menu close function (vftable[2]).
     *        Used to detect when the in-game menu is closed.
     */
    constexpr const char *UI_MENU_CLOSE_AOB_PATTERN =
        "8A 57 48 48 8D 4F 28 C6 47 49 00 E8 ?? ?? ?? ?? C6 47 48 00";

    // --- AOB Hook Offsets ---
    constexpr int EVENT_HANDLER_HOOK_OFFSET = 0;

    // --- Memory Offsets ---
    constexpr ptrdiff_t OFFSET_ManagerPtrStorage = 0x38;      // Global context to camera manager
    constexpr ptrdiff_t OFFSET_TpvObjPtrStorage = 0x28;       // Camera manager to TPV object
    constexpr ptrdiff_t OFFSET_TpvFlag = 0x38;                // TPV object to flag byte
    constexpr ptrdiff_t OVERLAY_FLAG_OFFSET = 0xD8;           // UI module to overlay flag
    constexpr ptrdiff_t OFFSET_ScrollAccumulatorFloat = 0x1C; // Scroll state to accumulator
    constexpr ptrdiff_t OFFSET_TpvFovWrite = 0x30;            // FOV calculation offset

    constexpr ptrdiff_t TPV_CAMERA_QUATERNION_OFFSET = 0x10; // Confirmed XYZW quaternion start in C_CameraThirdPerson object

    constexpr ptrdiff_t PLAYER_STATE_POSITION_OFFSET = 0x0;  // Verified (part of first MOVUPS)
    constexpr ptrdiff_t PLAYER_STATE_ROTATION_OFFSET = 0x10; // Verified
    constexpr size_t PLAYER_STATE_SIZE = 0xD4;               // Verified from assembly (212 bytes)
    // For CEntity World Transform Member (relative to CEntity* base)
    constexpr ptrdiff_t OFFSET_ENTITY_WORLD_MATRIX_MEMBER = 0x58;

    // Offsets relative to the outputPosePtr (RDX) in FUN_18392509c
    // Standard Pos(XYZ) followed by Quat(XYZW).
    constexpr ptrdiff_t TPV_OUTPUT_POSE_POSITION_OFFSET = 0x0;  // Base Offset (X, Y, Z = 12 bytes)
    constexpr ptrdiff_t TPV_OUTPUT_POSE_ROTATION_OFFSET = 0x0C; // Base Offset (Assuming starts right after Pos. Z = 0x8+0x4) (X, Y, Z, W = 16 bytes)
    constexpr size_t TPV_OUTPUT_POSE_REQUIRED_SIZE = 0x1C;      // Minimum size needed: Pos(12) + Quat(16) = 28 bytes (0x1C). Let's use 0x20 for alignment.

    // --- Input Event Offsets ---
    constexpr ptrdiff_t INPUT_EVENT_TYPE_OFFSET = 0x04;
    constexpr ptrdiff_t INPUT_EVENT_BYTE0_OFFSET = 0x00;
    constexpr ptrdiff_t INPUT_EVENT_ID_OFFSET = 0x10;
    constexpr ptrdiff_t INPUT_EVENT_VALUE_OFFSET = 0x18;
    constexpr int INPUT_EVENT_BYTE0_EXPECTED = 0x01;
    constexpr int MOUSE_INPUT_TYPE_ID = 8;
    constexpr int MOUSE_WHEEL_EVENT_ID = 0x10C;

    // --- Timing ---
    constexpr unsigned long OVERLAY_MONITOR_INTERVAL_MS = 66;
    constexpr unsigned long MAIN_MONITOR_SLEEP_MS = 33;

    /** @brief Name of the target game module. */
    constexpr const char *MODULE_NAME = "WHGame.dll";
} // namespace Constants
#endif // CONSTANTS_H
