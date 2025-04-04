/**
 * @file utils.h
 * @brief Header for general utility functions used across the mod.
 *
 * Includes inline functions for formatting values (addresses, hex, keys) and
 * string manipulation (trimming).
 */
#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cstdint> // For uintptr_t

// --- String Formatting Utilities ---

/**
 * @brief Formats a memory address into a standard hex string (e.g.,
 * "0x00007FFAC629C26E").
 * @details Ensures full width padding based on pointer size. Uses uppercase.
 * @param address Memory address value (cast pointer to uintptr_t).
 * @return std::string Formatted hexadecimal address string.
 */
inline std::string format_address(uintptr_t address)
{
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase
        << std::setw(sizeof(uintptr_t) * 2) // Full pointer width
        << std::setfill('0') << address;
    return oss.str();
}

/**
 * @brief Formats an integer as a 2-digit uppercase hex string ("0xHH").
 * @details Masks to lower 8 bits. Good for bytes, offsets, simple codes.
 * Example: 72 -> "0x48", 255 -> "0xFF"
 * @param value Integer value to format.
 * @return std::string Formatted 2-digit hex string.
 */
inline std::string format_hex(int value)
{
    std::ostringstream oss;
    oss << "0x" << std::uppercase << std::hex
        << std::setw(2) << std::setfill('0')
        << (value & 0xFF); // Mask to ensure single byte representation
    return oss.str();
}

/**
 * @brief Formats a Virtual Key (VK) code as a 2-digit uppercase hex string.
 * @details Example: VK_F3 (114) -> "0x72"
 * @param vk_code The integer Virtual Key code.
 * @return std::string Formatted hex representation of the VK code.
 */
inline std::string format_vkcode(int vk_code)
{
    // Alias for clarity, uses the same logic as format_hex
    return format_hex(vk_code);
}

/**
 * @brief Formats a vector of key codes into a human-readable hex list string.
 * @details Example: {114, 77} -> "0x72, 0x4D". Returns "(None)" for empty.
 * @param keys Const reference to vector containing integer key codes.
 * @return std::string Comma-separated string of formatted hex key codes.
 */
inline std::string format_vkcode_list(const std::vector<int> &keys)
{
    if (keys.empty())
    {
        return "(None)";
    }
    std::ostringstream oss;
    for (size_t i = 0; i < keys.size(); ++i)
    {
        oss << format_vkcode(keys[i]);
        if (i < keys.size() - 1)
        {
            oss << ", "; // Add separator unless it's the last one
        }
    }
    return oss.str();
}

// --- String Manipulation Utilities ---

/**
 * @brief Trims leading/trailing whitespace (space, tab, CR, LF) from a string.
 * @param s Input std::string.
 * @return std::string The trimmed string, or empty if input was all whitespace.
 */
inline std::string trim(const std::string &s)
{
    size_t first = s.find_first_not_of(" \t\r\n");
    if (std::string::npos == first)
    {
        return s; // Return original if empty or all whitespace
    }
    size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, (last - first + 1));
}

#endif // UTILS_H
