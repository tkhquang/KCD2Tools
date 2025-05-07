# -----------------------------------------------------------------------------
# overlay_hook.S
# Assembly detour for capturing RBX during overlay check
#
# This function replaces the game's overlay check instruction:
# 'cmp qword ptr [rbx+D8h], 0'
#
# Purpose: Capture the RBX register which points to the UI module during
# overlay state checks, store it in a global variable, then execute the
# original instruction and continue normal execution.
# ----------------------------------------------------------------------------

.section .text
.global Overlay_CaptureRBX_Detour     # Exported for C++ linkage
.extern g_rbx_for_overlay_flag        # Import pointer to storage for captured RBX
.extern fpOverlay_OriginalCode        # Import function pointer to continue execution

Overlay_CaptureRBX_Detour:
    # ---- Prologue: Preserve CPU state -----
    pushfq                    # Save RFLAGS since the CMP will modify them
    push %rax                 # Save RAX (used as temporary register)
    push %rbx                 # Save original RBX value for restoration

    # ---- Capture and store the UI module pointer (RBX) -----
    movq %rbx, %rax           # Copy UI module pointer (RBX) to RAX

    movq g_rbx_for_overlay_flag(%rip), %rbx  # Load pointer to global storage variable
    movq %rax, (%rbx)         # Store the UI module pointer: *g_rbx_for_overlay_flag = original_RBX

    # ---- Restore RBX before executing original code -----
    pop %rbx                  # Restore original RBX value (UI module pointer)

    # ---- Execute original instruction: "cmp qword ptr [rbx+D8h], 0" -----
    # Check if UI overlay is active by comparing qword at RBX+0xD8 with 0
    .byte 0x48, 0x83, 0xbb, 0xd8, 0x00, 0x00, 0x00, 0x00

    # ---- Epilogue: Restore remaining CPU state -----
    pop %rax                  # Restore RAX
    popfq                     # Restore RFLAGS

    # ---- Continue execution at original code -----
    jmp *fpOverlay_OriginalCode(%rip)  # Jump to address immediately after the hooked instruction
