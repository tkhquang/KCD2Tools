#include "aob_scanner.h"
#include "logger.h"
#include <string>
#include <sstream>

std::vector<BYTE> parseAOB(const std::string &aob_str)
{
    std::vector<BYTE> bytes;
    std::istringstream iss(aob_str);
    std::string byte_str;
    Logger::getInstance().log(LOG_DEBUG, "AOB: Parsing '" + aob_str + "'");

    while (iss >> byte_str)
    {
        if (byte_str == "??" || byte_str == "?")
        {
            // Add a wildcard byte (we'll use 0xCC as a placeholder value)
            // and mark it for special handling during pattern matching
            bytes.push_back(0xCC);
        }
        else
        {
            try
            {
                bytes.push_back(static_cast<BYTE>(std::stoi(byte_str, nullptr, 16)));
            }
            catch (const std::exception &e)
            {
                Logger::getInstance().log(LOG_ERROR, "AOB: Failed to parse byte '" + byte_str + "': " + e.what());
                // Continue parsing other bytes instead of failing completely
            }
        }
    }
    return bytes;
}

BYTE *FindPattern(BYTE *start, size_t size, const std::vector<BYTE> &pattern)
{
    size_t pattern_size = pattern.size();
    if (pattern_size == 0)
        return nullptr;

    // Create a mask for wildcard bytes (where we don't care about the specific value)
    std::vector<bool> wildcard_mask(pattern_size, false);

    // Find all wildcards (marked with 0xCC in our parseAOB function)
    for (size_t i = 0; i < pattern_size; i++)
    {
        if (pattern[i] == 0xCC)
        {
            wildcard_mask[i] = true;
        }
    }

    // Scan memory
    for (size_t i = 0; i <= size - pattern_size; i++)
    {
        bool match = true;
        for (size_t j = 0; j < pattern_size; j++)
        {
            if (!wildcard_mask[j] && start[i + j] != pattern[j])
            {
                match = false;
                break;
            }
        }

        if (match)
        {
            return start + i;
        }
    }

    return nullptr;
}
