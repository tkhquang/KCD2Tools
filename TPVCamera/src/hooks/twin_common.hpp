/**
 * @file hooks/twin_common.hpp
 * @brief Helpers the shader twins, the twin inventory and the twin clone share: ASCII case folding of engine names and
 *        the RTTI type check of a live engine object.
 */
#ifndef TPVCAMERA_HOOKS_TWIN_COMMON_HPP
#define TPVCAMERA_HOOKS_TWIN_COMMON_HPP

#include <DetourModKit/memory.hpp>
#include <DetourModKit/rtti.hpp>

#include <cstdint>
#include <string_view>

namespace TPVCamera
{
    /** @brief @p c in lower case when it is an ASCII capital letter, else @p c: engine names are ASCII. */
    [[nodiscard]] constexpr char ascii_lower(char c) noexcept
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
    }

    /**
     * @brief True when the NUL-terminated @p text equals @p want, ignoring ASCII case.
     * @details Stops at the first difference, so a mismatch costs no length scan of either string: the render-path
     *          shader-name checks compare against the few twin source names this way.
     */
    [[nodiscard]] constexpr bool equals_nocase(const char *text, const char *want) noexcept
    {
        for (;; ++text, ++want)
        {
            if (ascii_lower(*text) != ascii_lower(*want))
            {
                return false;
            }
            if (*want == '\0')
            {
                return true;
            }
        }
    }

    /** @brief True when the NUL-terminated @p text starts with @p prefix, ignoring ASCII case. */
    [[nodiscard]] constexpr bool starts_with_nocase(const char *text, const char *prefix) noexcept
    {
        for (; *prefix != '\0'; ++prefix, ++text)
        {
            if (*text == '\0' || ascii_lower(*text) != ascii_lower(*prefix))
            {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief True when @p object's vtable carries the RTTI name @p mangled. A null or unreadable object is not.
     * @note Setup/control-plane only: the RTTI walk takes the loader lock.
     */
    [[nodiscard]] inline bool object_has_type(std::uintptr_t object, std::string_view mangled) noexcept
    {
        const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{object});
        return vtable && *vtable != 0 && DMK::rtti::vtable_is_type(DMK::Address{*vtable}, mangled);
    }
} // namespace TPVCamera

#endif // TPVCAMERA_HOOKS_TWIN_COMMON_HPP
