/**
 * @file overlay_hook.S
 * @brief Assembly detour for capturing RBX during overlay check.
 * @details Replaces 'cmp qword ptr [rbx+D8h], 0'. Captures RBX before
 *          the cmp instruction, stores it, executes original cmp bytes,
 *          restores state, and jumps to continuation.
 */

.section .text
.global Overlay_CaptureRBX_Detour     /* Expose to C++ */
.extern g_rbx_for_overlay_flag      /* Import C++ pointer variable */
.extern fpOverlay_OriginalCode        /* Import C++ continuation function ptr */

Overlay_CaptureRBX_Detour:
    /* --- Prologue: Save state potentially clobbered by capture or original code --- */
    pushfq        /* cmp modifies RFLAGS, preserve them */
    push %rax     /* Used for temporary storage */
    push %rbx     /* Save RBX before using it to load global pointer address */

    /* --- Capture original RBX value --- */
    movq %rbx, %rax /* Copy current RBX into RAX */

    /* --- Store captured value --- */
    /* Load the address of the global pointer variable into RBX */
    movq g_rbx_for_overlay_flag(%rip), %rbx
    /* Store the captured RBX (now in RAX) into the location pointed to by RBX */
    movq %rax, (%rbx)            /* *g_rbx_for_overlay_flag = captured_RBX */

    /* --- Restore original RBX before executing original instruction --- */
    pop %rbx                     /* Original RBX value is back in RBX */

    /* --- Execute Original Instruction Bytes: cmp qword ptr [rbx+D8h], 0 --- */
    .byte 0x48, 0x83, 0xbb, 0xd8, 0x00, 0x00, 0x00, 0x00

    /* --- Epilogue: Restore remaining saved state --- */
    /* RBX was restored earlier. Pop others in reverse order. */
    pop %rax
    popfq

    /* --- Continue Execution --- */
    /* Jump to the original code following the cmp instruction. */
    jmp *fpOverlay_OriginalCode(%rip)

/* End of file */
