/**
 * @file utils.cpp
 * @brief Implements utility functions including memory validation helpers.
 */

#include "utils.h"
#include <windows.h>

bool isMemoryReadable(const volatile void *address, size_t size)
{
    if (!address || size == 0)
        return false;

    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(const_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0)
        return false;

    if (mbi.State != MEM_COMMIT)
        return false;

    const DWORD READ_FLAGS = (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                              PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
    if (!(mbi.Protect & READ_FLAGS) || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD))
        return false;

    uintptr_t start = reinterpret_cast<uintptr_t>(address);
    uintptr_t end = start + size;
    if (end < start) // Overflow check
        return false;

    uintptr_t region_start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    uintptr_t region_end = region_start + mbi.RegionSize;

    return (start >= region_start && end <= region_end);
}

bool isMemoryWritable(volatile void *address, size_t size)
{
    if (!address || size == 0)
        return false;

    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(const_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0)
        return false;

    if (mbi.State != MEM_COMMIT)
        return false;

    const DWORD WRITE_FLAGS = (PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
    if (!(mbi.Protect & WRITE_FLAGS) || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD))
        return false;

    uintptr_t start = reinterpret_cast<uintptr_t>(address);
    uintptr_t end = start + size;
    if (end < start) // Overflow check
        return false;

    uintptr_t region_start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    uintptr_t region_end = region_start + mbi.RegionSize;

    return (start >= region_start && end <= region_end);
}
