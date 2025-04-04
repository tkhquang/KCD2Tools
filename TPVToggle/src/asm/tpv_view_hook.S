/**
 * @file tpv_view_hook.S
 * @brief Assembly detour function for capturing the R9 register.
 * @details Replaces 'mov r9, [rax+38]'. Executes it, saves R9, resumes.
 * Steps:
 * 1. Save registers (RAX, flags).
 * 2. Manually execute 'mov r9, [rax+38]' bytes.
 * 3. Capture the resulting R9.
 * 4. Store R9 into the C++ global pointer 'g_r9_for_tpv_flag'.
 * 5. Restore registers.
 * 6. Jump to original code after the hook using 'fpTPV_OriginalCode'.
 * @note Ensures R9 is captured *after* being set by original logic.
 * @note Uses GNU AS (GAS) syntax for MinGW/GCC.
 */

.section .text

/* External symbols linked from C++ */
.global TPV_CaptureR9_Detour /* Our function visible to C++ */
.global fpTPV_OriginalCode   /* C++ var holding continuation addr */
.global g_r9_for_tpv_flag    /* C++ pointer to R9 storage */

/**
 * @brief Detour entry point. Replaces 'mov r9, [rax+38]'.
 */
TPV_CaptureR9_Detour:
    /* --- Prologue: Save State --- */
    pushfq        /* Save RFLAGS */
    push %rax     /* Save RAX (used for addressing global) */

    /* --- Execute Original Instruction Bytes --- */
    /* Manually execute: 4C 8B 48 38  (mov r9, [rax+38]) */
    .byte 0x4c, 0x8b, 0x48, 0x38

    /* --- Capture Resulting R9 --- */
    /* R9 now holds the pointer set by the instruction above. */

    /* Get the ADDRESS where g_r9_for_tpv_flag points to into RAX. */
    /* Use RIP-relative addressing to access the global pointer itself. */
    movq g_r9_for_tpv_flag(%rip), %rax

    /* Store the value IN R9 into the memory AT the address in RAX. */
    /* (Effectively *g_r9_for_tpv_flag = R9) */
    movq %r9, (%rax)

    /* --- Epilogue: Restore State --- */
    /* Restore in reverse order. */
    pop %rax
    popfq

    /* --- Continue Execution --- */
    /* Jump to the address in fpTPV_OriginalCode (the instruction */
    /* after the original 'mov r9, [rax+38]'). */
    jmp *fpTPV_OriginalCode(%rip)

/* End of file */
