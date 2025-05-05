/**
 * @file utils.h
 * @brief Header for utility functions including memory validation.
 *
 * Includes inline functions for formatting values (addresses, hex, keys),
 * string manipulation (trimming), and memory safety checks.
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

// --- Memory Validation Utilities ---

/**
 * @brief Checks if memory at the specified address is readable.
 * @param address Starting address to check.
 * @param size Number of bytes to check.
 * @return true if all bytes are readable, false otherwise.
 */
bool isMemoryReadable(const volatile void *address, size_t size);

/**
 * @brief Checks if memory at the specified address is writable.
 * @param address Starting address to check.
 * @param size Number of bytes to check.
 * @return true if all bytes are writable, false otherwise.
 */
bool isMemoryWritable(volatile void *address, size_t size);

#endif // UTILS_H
