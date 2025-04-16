/**
 * @file camera_distance_hook.S
 * @brief Assembly detour for capturing RBX during camera distance *read*.
 * @details Replaces 'movss xmm0,[rbx+20h]'. Captures RBX before the read,
 *          stores it, executes the original 'movss' bytes, restores state,
 *          and jumps to continuation. Hooking a read should be safer.
 */

.section .text
.global CameraDistance_CaptureRBX_Detour
.extern g_rbx_for_camera_distance
.extern fpCameraDistance_OriginalCode

CameraDistance_CaptureRBX_Detour:
    /* --- Prologue: Save registers used by capture logic --- */
    push %rax     /* Save RAX (used to hold captured RBX) */
    push %rcx     /* Save RCX (used to hold address of global) */
    /* Note: Original RBX is implicitly preserved */

    /* --- Capture Original RBX before the movss uses it for addressing --- */
    movq %rbx, %rax             /* RAX = Original RBX value */

    /* Load the ADDRESS of the C++ global pointer variable into RCX */
    movq g_rbx_for_camera_distance(%rip), %rcx /* RCX = &g_rbx_for_camera_distance */

    /* Store the captured RBX value (from RAX) into the memory pointed to by RCX */
    movq %rax, (%rcx)           /* *g_rbx_for_camera_distance = RAX */

    /* Original RBX is still intact. RAX/RCX are modified but will be restored. */

    /* --- Execute Original Instruction Bytes --- */
    /* Execute the original 5 bytes of the 'movss xmm0,[rbx+20h]' instruction */
    .byte 0xf3, 0x0f, 0x10, 0x43, 0x20 // *** CHANGED to MOVSS READ bytes ***

    /* --- Epilogue: Restore saved state --- */
    pop %rcx      /* Restore RCX */
    pop %rax      /* Restore RAX */
    /* movss doesn't modify arithmetic flags */

    /* --- Continue Execution --- */
    /* Jump to the original code that follows the hooked 'movss' instruction. */
    jmp *fpCameraDistance_OriginalCode(%rip)

/* End of file */
