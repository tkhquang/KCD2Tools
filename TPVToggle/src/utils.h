/**
 * @file utils.h
 * @brief Header for utility functions that extend DetourModKit functionality.
 * @details This header provides utility functions not available in DetourModKit.
 *          For memory operations, use DMKMemory:: directly from DetourModKit.hpp.
 *          For string operations, use DMKString:: directly.
 *          For AOB scanning, use DMKScanner:: directly.
 */

#ifndef UTILS_H
#define UTILS_H

#include <DetourModKit.hpp>
#include <string>
#include <sstream>
#include <iomanip>
#include <windows.h>

#include "math_utils.h"

// Use DMK namespaces directly for memory/string/scanning operations:
// - DMKMemory::isMemoryReadable(), DMKMemory::isMemoryWritable(), DMKMemory::WriteBytes()
// - DMKString::format_address(), DMKString::trim(), DMKString::format_vkcode(), etc.
// - DMKScanner::parseAOB(), DMKScanner::FindPattern()

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

#endif // UTILS_H
