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
        bytes.push_back(static_cast<BYTE>(std::stoi(byte_str, nullptr, 16)));
    }
    return bytes;
}
BYTE *FindPattern(BYTE *start, size_t size, const std::vector<BYTE> &pattern)
{
    size_t pattern_size = pattern.size();
    for (size_t i = 0; i <= size - pattern_size; i++)
    {
        if (memcmp(start + i, pattern.data(), pattern_size) == 0)
        {
            return start + i;
        }
    }
    return nullptr;
}
