/**
 * @file utils.h
 * @brief Header for utility functions including memory validation.
 *
 * Includes inline functions for formatting values (addresses, hex, keys),
 * string manipulation (trimming), and thread-safe memory safety checks with caching.
 */
#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cstdint>
#include <algorithm>
#include <cstddef>
#include <chrono>
#include <windows.h>
#include <mutex>
#include <atomic>

// --- String Formatting Utilities ---

/**
 * @brief Formats a memory address into a standard hex string.
 */
inline std::string format_address(uintptr_t address)
{
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase
        << std::setw(sizeof(uintptr_t) * 2) << std::setfill('0') << address;
    return oss.str();
}

/**
 * @brief Formats an integer as a 2-digit uppercase hex string.
 */
inline std::string format_hex(int value)
{
    std::ostringstream oss;
    oss << "0x" << std::uppercase << std::hex
        << std::setw(2) << std::setfill('0') << (value & 0xFF);
    return oss.str();
}

/**
 * @brief Formats a Virtual Key (VK) code as a standard 2-digit hex string.
 */
inline std::string format_vkcode(int vk_code)
{
    return format_hex(vk_code);
}

/**
 * @brief Formats a vector of VK codes into a human-readable hex list string.
 */
inline std::string format_vkcode_list(const std::vector<int> &keys)
{
    if (keys.empty())
        return "(None)";

    std::ostringstream oss;
    for (size_t i = 0; i < keys.size(); ++i)
    {
        oss << format_vkcode(keys[i]);
        if (i < keys.size() - 1)
        {
            oss << ", ";
        }
    }
    return oss.str();
}

// --- String Manipulation Utilities ---

/**
 * @brief Trims leading and trailing whitespace characters from a string.
 */
inline std::string trim(const std::string &s)
{
    const char *WHITESPACE = " \t\n\r\f\v";
    size_t first = s.find_first_not_of(WHITESPACE);
    if (first == std::string::npos)
        return "";
    size_t last = s.find_last_not_of(WHITESPACE);
    return s.substr(first, (last - first + 1));
}

// --- Memory Region Cache System ---

/**
 * @brief Structure to hold cached memory region information.
 */
struct MemoryRegionInfo
{
    uintptr_t baseAddress;                           // Region base address
    size_t regionSize;                               // Size of the region in bytes
    DWORD protection;                                // Memory protection flags
    std::chrono::steady_clock::time_point timestamp; // When this entry was created/updated
    bool valid;                                      // Whether this entry is valid

    MemoryRegionInfo()
        : baseAddress(0), regionSize(0), protection(0), valid(false) {}
};

// Thread-safe, one-time initialization flag for memory cache
extern std::once_flag g_memoryCacheInitFlag;

/**
 * @brief Initializes the memory region cache system.
 * @details Thread-safe initialization using std::call_once.
 *          Should be called once during DLL initialization.
 */
void initMemoryCache();

/**
 * @brief Clears all entries from the memory region cache.
 * @details Thread-safe operation. Called during cleanup.
 */
void clearMemoryCache();

/**
 * @brief Get current cache statistics (for tuning and debugging)
 * @return String containing hit/miss count and hit rate percentage.
 */
std::string getMemoryCacheStats();

// --- Memory Validation Utilities ---

/**
 * @brief Checks if memory at the specified address is readable.
 * @details Thread-safe implementation with memory region caching to minimize
 *          VirtualQuery calls. Uses a hybrid locking approach to minimize
 *          contention while ensuring cache consistency.
 *
 * @param address Starting address to check. Marked const volatile to indicate
 *                that while this function won't modify the memory, the memory
 *                might change unexpectedly from other threads.
 * @param size Number of bytes to check.
 * @return true if all bytes are readable, false otherwise.
 */
bool isMemoryReadable(const volatile void *address, size_t size);

/**
 * @brief Checks if memory at the specified address is writable.
 * @details Thread-safe implementation with memory region caching to minimize
 *          VirtualQuery calls. Uses a hybrid locking approach to minimize
 *          contention while ensuring cache consistency.
 *
 * @param address Starting address to check. Marked volatile to indicate
 *                that the memory might change unexpectedly from other threads.
 * @param size Number of bytes to check.
 * @return true if all bytes are writable, false otherwise.
 */
bool isMemoryWritable(volatile void *address, size_t size);

#endif // UTILS_H
