/**
 * @file aob_scanner.cpp
 * @brief Implementation of Array-of-Bytes (AOB) parsing and scanning.
 */

#include "aob_scanner.h"
#include "logger.h"
#include "utils.h" // For trim() and format_address()

#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cctype> // For std::isxdigit

/**
 * @struct PatternByte
 * @brief Internal helper representing a parsed AOB element clearly.
 */
struct PatternByte
{
    BYTE value;       /**< Byte value (if not wildcard). */
    bool is_wildcard; /**< True if represents '??' or '?'. */
};

/**
 * @brief Internal parser: AOB string -> structured PatternByte vector.
 * @details Validates each token rigorously.
 * @param aob_str Raw AOB string (e.g., "48 ?? 8B").
 * @return std::vector<PatternByte> Parsed struct vector, empty on failure.
 */
std::vector<PatternByte> parseAOBInternal(const std::string &aob_str)
{
    std::vector<PatternByte> pattern_elements;
    std::string trimmed_aob = trim(aob_str);
    std::istringstream iss(trimmed_aob);
    std::string token;
    Logger &logger = Logger::getInstance();
    int token_idx = 0;

    if (trimmed_aob.empty())
    {
        logger.log(LOG_WARNING, "AOB Parser: Input string is empty.");
        return pattern_elements;
    }
    logger.log(LOG_DEBUG, "AOB Parser: Parsing string: '" + trimmed_aob + "'");

    while (iss >> token)
    {
        token_idx++;
        // Check for wildcard tokens
        if (token == "??" || token == "?")
        {
            pattern_elements.push_back({0x00, true});
        }
        // Check for valid 2-digit hex byte token
        else if (token.length() == 2 &&
                 std::isxdigit(static_cast<unsigned char>(token[0])) &&
                 std::isxdigit(static_cast<unsigned char>(token[1])))
        {
            try
            {
                BYTE byte_val = static_cast<BYTE>(
                    std::stoul(token, nullptr, 16));
                pattern_elements.push_back({byte_val, false});
            }
            catch (const std::exception &e)
            {
                logger.log(LOG_ERROR, "AOB Parser: Hex conversion error for '" +
                                          token + "' (Token #" + std::to_string(token_idx) +
                                          "): " + e.what());
                return {}; // Return empty on conversion error
            }
        }
        // Invalid token format
        else
        {
            std::ostringstream oss;
            // Break up the '??' sequence to avoid trigraph warning
            oss << "AOB Parser: Invalid token '" << token
                << "' at position " << token_idx
                << ". Expected '?"
                   "?"
                   "', '?', or 2 hex digits (e.g., FF).";
            logger.log(LOG_ERROR, oss.str());
            return {}; // Return empty on invalid token error
        }
    }

    if (pattern_elements.empty() && token_idx > 0)
    {
        logger.log(LOG_ERROR, "AOB Parser: Parsing yielded no valid elements.");
    }
    else if (!pattern_elements.empty())
    {
        logger.log(LOG_DEBUG, "AOB Parser: Successfully parsed " +
                                  std::to_string(pattern_elements.size()) + " elements.");
    }

    return pattern_elements;
}

/**
 * @brief Public AOB parser: string -> vector<BYTE> with 0xCC wildcards.
 * @param aob_str AOB pattern string (e.g., "48 8B ?? C1").
 * @return std::vector<BYTE> Byte vector (0xCC=wildcard), empty on error.
 */
std::vector<BYTE> parseAOB(const std::string &aob_str)
{
    std::vector<PatternByte> internal_pattern = parseAOBInternal(aob_str);
    std::vector<BYTE> byte_vector;

    if (internal_pattern.empty())
    {
        if (!trim(aob_str).empty())
        {
            Logger::getInstance().log(LOG_ERROR, "AOB: Final parsed pattern is "
                                                 "empty due to errors in input string.");
        }
        return byte_vector;
    }

    // Convert PatternByte vector to BYTE vector using 0xCC placeholder
    byte_vector.reserve(internal_pattern.size());
    for (const auto &element : internal_pattern)
    {
        byte_vector.push_back(element.is_wildcard ? 0xCC : element.value);
    }

    Logger::getInstance().log(LOG_DEBUG, "AOB: Converted pattern for scanning "
                                         "(0xCC=wildcard).");
    return byte_vector;
}

/**
 * @brief Scans memory region for a byte pattern including 0xCC wildcards.
 * @param start_address Start of the memory region.
 * @param region_size Size (bytes) of the region.
 * @param pattern_with_placeholders Byte vector pattern (0xCC = wildcard).
 * @return BYTE* Pointer to first match, or nullptr if not found/error.
 */
BYTE *FindPattern(BYTE *start_address, size_t region_size,
                  const std::vector<BYTE> &pattern_with_placeholders)
{
    Logger &logger = Logger::getInstance();
    const size_t pattern_size = pattern_with_placeholders.size();

    // --- Input Validation ---
    if (!pattern_size || !start_address)
    {
        logger.log(LOG_ERROR, "FindPattern: Invalid input (empty pattern or "
                              "null start address).");
        return nullptr;
    }
    if (region_size < pattern_size)
    {
        logger.log(LOG_WARNING, "FindPattern: Region size (" +
                                    std::to_string(region_size) + ") < pattern size (" +
                                    std::to_string(pattern_size) + "). No match possible.");
        return nullptr;
    }

    logger.log(LOG_DEBUG, "FindPattern: Scanning " +
                              std::to_string(region_size) + " bytes from " +
                              format_address(reinterpret_cast<uintptr_t>(start_address)) +
                              " for " + std::to_string(pattern_size) + " byte pattern.");

    // --- Wildcard Mask ---
    std::vector<bool> is_wildcard(pattern_size);
    int wildcard_count = 0;
    for (size_t i = 0; i < pattern_size; ++i)
    {
        is_wildcard[i] = (pattern_with_placeholders[i] == 0xCC);
        if (is_wildcard[i])
            wildcard_count++;
    }
    if (wildcard_count > 0)
    {
        logger.log(LOG_DEBUG, "FindPattern: Pattern has " +
                                  std::to_string(wildcard_count) + " wildcards.");
    }

    // --- Memory Scanning Loop ---
    BYTE *const scan_end_addr = start_address + region_size - pattern_size;

    for (BYTE *current_pos = start_address; current_pos <= scan_end_addr;
         ++current_pos)
    {
        bool match = true; // Assume match at current position
        for (size_t j = 0; j < pattern_size; ++j)
        {
            // Check for mismatch only if it's NOT a wildcard byte
            if (!is_wildcard[j] &&
                current_pos[j] != pattern_with_placeholders[j])
            {
                match = false; // Mismatch found
                break;         // Stop comparing at this position
            }
        }

        // If inner loop completed without mismatch, pattern found
        if (match)
        {
            logger.log(LOG_DEBUG, "FindPattern: Match found at address: " +
                                      format_address(reinterpret_cast<uintptr_t>(current_pos)));
            return current_pos;
        }
    }

    logger.log(LOG_WARNING, "FindPattern: Pattern not found in region.");
    return nullptr; // Pattern not found after scanning entire region
}
