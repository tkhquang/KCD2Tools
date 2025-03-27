// utils.h
#pragma once
#include <sstream>
#include <iomanip>

inline std::string format_address(uintptr_t address)
{
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << std::setw(sizeof(uintptr_t) * 2)
        << std::setfill('0') << address;
    return oss.str();
}

inline std::string format_vkcode(int vk)
{
    std::ostringstream oss;
    oss << "0x" << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << vk;
    return oss.str();
}
