/**
 * @file engine/seh.hpp
 * @brief The fault set a guarded engine call may claim.
 *
 * Reads and writes go through DetourModKit's guarded memory API. What remains under a local __try is a call into
 * engine code (or an interlocked update of an engine field), for which DetourModKit has no guarded equivalent. Such a
 * call claims the same faults DetourModKit's guarded reads claim, minus the guard page: an access violation and an
 * in-page error. A guard-page fault is left to its owner, because the OS clears PAGE_GUARD before dispatching it and
 * only the owner can re-arm the fence. Every other code (a C++ exception, a stack overflow, a breakpoint) continues
 * the handler search.
 */
#ifndef HENRYSENSES_SEH_HPP
#define HENRYSENSES_SEH_HPP

#include <windows.h>

namespace HenrySenses
{
    /**
     * @brief The __except filter of a guarded engine call.
     * @param code GetExceptionCode().
     * @return EXCEPTION_EXECUTE_HANDLER for an access violation or an in-page error, else EXCEPTION_CONTINUE_SEARCH.
     */
    [[nodiscard]] inline int engine_fault_filter(unsigned long code) noexcept
    {
        return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER
                                                                                     : EXCEPTION_CONTINUE_SEARCH;
    }

} // namespace HenrySenses

#endif // HENRYSENSES_SEH_HPP
