/**
 * @file menu_open_hook.S
 * @brief Assembly detour for menu open / show overlay event.
 * @details Replaces 'mov byte ptr [rax+rcx+80h], 1'.
 *          Preserves CPU flags and registers, validates pointers,
 *          executes original instruction, sets g_isOverlayActive=true,
 *          and ensures safe continuation.
 */
.section .text
.global MenuOpen_Detour
.extern g_isOverlayActive          // Direct reference to global C++ boolean flag
.extern fpMenuOpen_OriginalCode    // Continuation address

MenuOpen_Detour:
    /* --- Prologue: Save CPU flags and registers --- */
    pushfq                          /* Preserve CPU flags (RFLAGS) */
    push %rax                       /* Save registers that we'll use */
    push %rcx
    push %rdx
    push %r8                        /* Save volatile registers per x64 calling convention */
    push %r9
    push %r10
    push %r11

    /* --- Validate Pointers --- */
    test %rax, %rax                 /* Check if RAX is NULL */
    jz .LSkipExecution              /* Skip if RAX is NULL */
    test %rcx, %rcx                 /* Check if RCX is NULL */
    jz .LSkipExecution              /* Skip if RCX is NULL */

    /* --- Execute Original Instruction Bytes --- */
    /* mov byte ptr [rax+rcx+80h], 1 */
    .byte 0xc6, 0x84, 0x08, 0x80, 0x00, 0x00, 0x00, 0x01

    /* --- Update C++ Boolean Flag Directly --- */
    /* Set g_isOverlayActive to true (1) */
    movb $1, g_isOverlayActive(%rip)
    jmp .LRestoreAndContinue

.LSkipExecution:
    /* If we can't safely execute the original instruction, we still need
       to update our tracking flag, as we've detected a menu open attempt */
    movb $1, g_isOverlayActive(%rip)

.LRestoreAndContinue:
    /* --- Epilogue: Restore saved registers and flags --- */
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdx
    pop %rcx
    pop %rax
    popfq                           /* Restore CPU flags */

    /* --- Continue Execution --- */
    jmp *fpMenuOpen_OriginalCode(%rip)

/* End of file */
