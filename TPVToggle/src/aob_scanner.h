/**
 * @file aob_scanner.h
 * @brief Header for Array-of-Bytes (AOB) scanning functionality
 *
 * This file defines functions for scanning memory regions to find specific
 * byte patterns. It's used to locate code within the game's memory that
 * handles the third-person view flag.
 */

#ifndef AOB_SCANNER_H
#define AOB_SCANNER_H

#include <windows.h>
#include <vector>
#include <string>

/**
 * @brief Parses a space-separated hex string into a vector of bytes
 *
 * @param aob_str A string containing hex bytes separated by spaces (e.g., "48 8B 8F")
 * @return std::vector<BYTE> Vector of parsed bytes
 */
std::vector<BYTE> parseAOB(const std::string &aob_str);

/**
 * @brief Scans a memory region for a specific byte pattern
 *
 * @param start Pointer to the start of the memory region to scan
 * @param size Size of the memory region in bytes
 * @param pattern Vector of bytes to search for
 * @return BYTE* Pointer to the start of the pattern if found, nullptr otherwise
 */
BYTE *FindPattern(BYTE *start, size_t size, const std::vector<BYTE> &pattern);

#endif // AOB_SCANNER_H
