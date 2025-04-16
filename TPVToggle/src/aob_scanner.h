/**
 * @file aob_scanner.h
 * @brief Header for Array-of-Bytes (AOB) scanning functionality.
 *
 * Declares functions to parse AOB pattern strings and search memory
 * regions for those patterns, supporting wildcards.
 */
#ifndef AOB_SCANNER_H
#define AOB_SCANNER_H

#include <windows.h> // For BYTE definition
#include <vector>
#include <string>

/**
 * @brief Parses space-separated AOB string into a byte vector for scanning.
 * @details Converts hex strings (e.g., "4A") to bytes and wildcards ('??', '?')
 * to a placeholder byte (0xCC) expected by `FindPattern`. Logs parse errors.
 * @param aob_str The AOB pattern string (e.g., "48 8B ?? C1").
 * @return std::vector<BYTE> Vector of bytes (0xCC for wildcards), empty on
 * failure or empty input.
 */
std::vector<BYTE> parseAOB(const std::string &aob_str);

/**
 * @brief Scans memory for a byte pattern including wildcards (0xCC).
 * @details Compares pattern against memory from `start_address`. Treats 0xCC
 * in the pattern as matching any byte.
 * @param start_address Start address of the memory region. Must be valid.
 * @param region_size Size (bytes) of the memory region.
 * @param pattern_with_placeholders Pattern vector (0xCC = wildcard).
 * @return BYTE* Pointer to first match, or `nullptr` if not found/error.
 */
BYTE *FindPattern(BYTE *start_address, size_t region_size,
                  const std::vector<BYTE> &pattern_with_placeholders);

#endif // AOB_SCANNER_H
