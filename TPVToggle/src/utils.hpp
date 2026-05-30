/**
 * @file utils.hpp
 * @brief Header for utility functions that extend DetourModKit functionality.
 * @details This header provides utility functions not available in DetourModKit.
 *          For memory operations, use DMK::Memory:: directly from DetourModKit.hpp.
 *          For string operations, use DMK::String:: directly.
 *          For format operations, use DMK::Format:: directly.
 *          For AOB scanning, use DMK::Scanner:: directly.
 */

#ifndef UTILS_HPP
#define UTILS_HPP

#include <DetourModKit.hpp>
#include <string>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <stop_token>
#include <thread>
#include <windows.h>

#include "math_utils.hpp"

// Use DMK namespaces directly for memory/format/scanning operations:
// - DMK::Memory::is_readable(), DMK::Memory::is_writable(), DMK::Memory::write_bytes()
// - DMK::Format::format_address(), DMK::Format::format_vkcode(), etc.
// - DMK::String::trim()
// - DMK::Scanner::parse_aob(), DMK::Scanner::find_pattern()

namespace TPVToggle
{

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

/**
 * @brief Sleeps up to total_ms in short slices, returning early when stop is requested.
 * @details A StoppableWorker body uses this so it observes a shutdown within one
 *          slice instead of sleeping out a long interval.
 * @param st The worker's stop token.
 * @param total_ms Total time to sleep in milliseconds.
 * @return false if a stop was requested before or during the sleep, true otherwise.
 */
[[nodiscard]] inline bool sleep_until_stop(std::stop_token st, int total_ms) noexcept
{
    constexpr int slice_ms = 50;
    int remaining = total_ms;
    while (remaining > 0)
    {
        if (st.stop_requested())
            return false;
        const int slice = remaining < slice_ms ? remaining : slice_ms;
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        remaining -= slice;
    }
    return !st.stop_requested();
}

} // namespace TPVToggle

#endif // UTILS_HPP
