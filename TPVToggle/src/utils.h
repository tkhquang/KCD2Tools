/**
 * @file utils.h
 * @brief Header for utility functions including memory validation and manipulation.
 *
 * Includes inline functions for formatting values (addresses, hex, keys),
 * string manipulation (trimming), thread-safe memory safety checks with caching,
 * and memory writing utilities.
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
#include <filesystem>
#include <mutex>
#include <atomic>
#include <iostream>

#include "constants.h"
#include "logger.h"
#include "math_utils.h"

// Forward declaration
class Logger;

// --- Memory Manipulation Functions ---

/**
 * @brief Safely writes bytes to a memory location with proper protection handling.
 * @details This function temporarily modifies memory protection to allow writing,
 *          performs the write operation, restores original protection, and flushes
 *          the instruction cache. It includes comprehensive error logging.
 *
 * @param targetAddress Pointer to the memory location to write to.
 * @param sourceBytes Pointer to the source data to be written.
 * @param numBytes Number of bytes to write.
 * @param logger Reference to the logger for error reporting.
 * @return true if the write operation was successful, false if any step failed
 *         (invalid parameters, VirtualProtect failure, or cache flush failure).
 *
 * @note This function is typically used for:
 *       - Patching game code at runtime
 *       - NOPing instructions (replacing with 0x90)
 *       - Modifying function prologues/epilogues
 *
 * @warning Modifying executable memory can cause crashes if done incorrectly.
 *          Always ensure the target address and size are valid.
 *
 * @example
 * @code
 * // NOP 5 bytes of code
 * BYTE nopBytes[5] = {0x90, 0x90, 0x90, 0x90, 0x90};
 * if (WriteBytes(targetAddr, nopBytes, 5, logger)) {
 *     logger.log(LOG_INFO, "Successfully NOPed instruction");
 * }
 * @endcode
 */
bool WriteBytes(BYTE *targetAddress, const BYTE *sourceBytes, size_t numBytes, Logger &logger);

// --- File System Utilities ---

/**
 * @brief Gets the directory containing the currently executing module (DLL/EXE).
 * @details Uses Windows API to determine the full path of the current module
 *          and extracts its parent directory. Falls back to current working
 *          directory if module path detection fails.
 *
 * @return std::string The directory path of the current module.
 *         Falls back to current working directory on error, or "." as last resort.
 *
 * @note This is useful for finding configuration files and resources that are
 *       stored alongside the mod DLL.
 *
 * @example
 * @code
 * std::string modDir = getRuntimeDirectory();
 * std::string configPath = modDir + "/config.ini";
 * @endcode
 */
inline std::string getRuntimeDirectory()
{
    HMODULE h_self = NULL;
    char module_path_buffer[MAX_PATH] = {0};
    std::string result_path = "";
    try
    {
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)&getRuntimeDirectory, &h_self) ||
            h_self == NULL)
        {
            throw std::runtime_error("GetModuleHandleExA failed: " + std::to_string(GetLastError()));
        }

        DWORD path_len = GetModuleFileNameA(h_self, module_path_buffer, MAX_PATH);
        if (path_len == 0)
            throw std::runtime_error("GetModuleFileNameA failed: " + std::to_string(GetLastError()));
        if (path_len == MAX_PATH && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
            throw std::runtime_error("GetModuleFileNameA failed: Buffer too small");

        std::filesystem::path module_full_path(module_path_buffer);
        result_path = module_full_path.parent_path().string();

        Logger::getInstance().log(LOG_DEBUG, "getRuntimeDirectory: Found module directory: " + result_path);
    }
    catch (const std::exception &e)
    {
        // Get current working directory as fallback
        char current_dir[MAX_PATH] = {0};
        if (GetCurrentDirectoryA(MAX_PATH, current_dir))
        {
            result_path = current_dir;
        }
        else
        {
            // Last resort fallback
            result_path = ".";
        }

        std::cerr << "[" << Constants::MOD_NAME << " WARNING] Failed to get module dir: "
                  << e.what() << ". Using fallback: " << result_path << std::endl;

        Logger::getInstance().log(LOG_WARNING, "getRuntimeDirectory: Failed to get module directory: " +
                                                   std::string(e.what()) + ". Using fallback: " + result_path);
    }
    catch (...)
    {
        // Get current working directory as fallback
        char current_dir[MAX_PATH] = {0};
        if (GetCurrentDirectoryA(MAX_PATH, current_dir))
        {
            result_path = current_dir;
        }
        else
        {
            // Last resort fallback
            result_path = ".";
        }

        std::cerr << "[" << Constants::MOD_NAME << " WARNING] Unknown exception getting module path. Using fallback: "
                  << result_path << std::endl;

        Logger::getInstance().log(LOG_WARNING, "getRuntimeDirectory: Unknown exception getting module directory. Using fallback: " + result_path);
    }

    return result_path;
}

// --- Math Type String Conversion Utilities ---

/**
 * @brief Converts a Quaternion to a readable string format.
 * @param q The quaternion to convert.
 * @return std::string Formatted string: "Q(X=0.0000 Y=0.0000 Z=0.0000 W=1.0000)"
 */
inline std::string QuatToString(const ::Quaternion &q)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4)
        << "Q(X=" << q.x << " Y=" << q.y << " Z=" << q.z << " W=" << q.w << ")";
    return oss.str();
}

/**
 * @brief Converts a Vector3 to a readable string format.
 * @param v The vector to convert.
 * @return std::string Formatted string: "V(0.0000, 0.0000, 0.0000)"
 */
inline std::string Vector3ToString(const Vector3 &v)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4)
        << "V(" << v.x << ", " << v.y << ", " << v.z << ")";
    return oss.str();
}

// --- String Formatting Utilities ---

/**
 * @brief Formats a memory address into a standard hex string.
 * @param address The memory address to format.
 * @return std::string Formatted hex string with prefix (e.g., "0x7FFE12345678").
 * @note Always returns uppercase hex with zero-padding for consistent width.
 */
inline std::string format_address(uintptr_t address)
{
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase
        << std::setw(sizeof(uintptr_t) * 2) << std::setfill('0') << address;
    return oss.str();
}

/**
 * @brief Formats an integer as an uppercase hex string.
 * @param value The integer value to format.
 * @param width Optional width for zero-padding (0 = no padding).
 * @return std::string Formatted hex string with "0x" prefix.
 * @example format_hex(255) returns "0xFF", format_hex(10, 4) returns "0x000A"
 */
inline std::string format_hex(int value, int width = 0)
{
    std::ostringstream oss;
    oss << "0x" << std::uppercase << std::hex;
    if (width > 0)
    {
        oss << std::setw(width) << std::setfill('0');
    }
    oss << value;
    return oss.str();
}

/**
 * @brief Formats a Virtual Key (VK) code as a standard 2-digit hex string.
 * @param vk_code The virtual key code to format.
 * @return std::string Formatted VK code (e.g., "0x72" for F3 key).
 * @note Convenience wrapper around format_hex for key codes.
 */
inline std::string format_vkcode(int vk_code)
{
    return format_hex(vk_code);
}

/**
 * @brief Formats a vector of VK codes into a human-readable hex list string.
 * @param keys Vector of virtual key codes.
 * @return std::string Comma-separated list (e.g., "0x72, 0x73") or "(None)" if empty.
 * @example Used for logging configured hotkeys in a readable format.
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
 * @param s The string to trim.
 * @return std::string The trimmed string.
 * @note Removes spaces, tabs, newlines, carriage returns, form feeds, and vertical tabs.
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
 * @details Used by the memory validation system to reduce VirtualQuery calls.
 */
struct MemoryRegionInfo
{
    uintptr_t baseAddress;                           ///< Region base address
    size_t regionSize;                               ///< Size of the region in bytes
    DWORD protection;                                ///< Memory protection flags (PAGE_*)
    std::chrono::steady_clock::time_point timestamp; ///< When this entry was created/updated
    bool valid;                                      ///< Whether this entry is valid

    MemoryRegionInfo()
        : baseAddress(0), regionSize(0), protection(0), valid(false) {}
};

// Thread-safe, one-time initialization flag for memory cache
extern std::once_flag g_memoryCacheInitFlag;

/**
 * @brief Initializes the memory region cache system.
 * @details Thread-safe initialization using std::call_once.
 *          Should be called once during DLL initialization.
 * @note The cache improves performance by reducing system calls to VirtualQuery.
 */
void initMemoryCache();

/**
 * @brief Clears all entries from the memory region cache.
 * @details Thread-safe operation. Called during cleanup.
 * @warning After calling this, memory checks will be slower until cache rebuilds.
 */
void clearMemoryCache();

/**
 * @brief Get current cache statistics (for tuning and debugging).
 * @return String containing hit/miss count and hit rate percentage.
 * @note Only available in debug builds (#ifdef _DEBUG).
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
 *
 * @note This function is used throughout the mod to validate pointers before
 *       dereferencing them, preventing access violations.
 *
 * @example
 * @code
 * if (isMemoryReadable(gamePtr, sizeof(GameStruct))) {
 *     GameStruct* data = reinterpret_cast<GameStruct*>(gamePtr);
 *     // Safe to read from data
 * }
 * @endcode
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
 *
 * @note This function checks memory protection flags to determine writability.
 *       It does not attempt to write to the memory.
 *
 * @example
 * @code
 * if (isMemoryWritable(targetAddr, 4)) {
 *     *reinterpret_cast<int*>(targetAddr) = newValue;
 * } else {
 *     // Need to use WriteBytes() to change protection first
 * }
 * @endcode
 */
bool isMemoryWritable(volatile void *address, size_t size);

#endif // UTILS_H
