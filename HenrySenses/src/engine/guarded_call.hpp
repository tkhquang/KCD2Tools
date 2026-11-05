/**
 * @file engine/guarded_call.hpp
 * @brief Calls into engine code under SEH: a function address, or a virtual read from a live object's vtable.
 *
 * Each helper keeps its __try in a function without destructible locals, which MSVC requires (C2712).
 */
#ifndef HENRYSENSES_GUARDED_CALL_HPP
#define HENRYSENSES_GUARDED_CALL_HPP

#include "engine/engine_env.hpp"
#include "engine/seh.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace HenrySenses
{
    /**
     * @brief Calls @p fn(args...) under SEH.
     * @param out Receives the result.
     * @return True when the call returned.
     */
    template <typename R, typename... A> [[nodiscard]] bool guarded_call(std::uintptr_t fn, R *out, A... args) noexcept
    {
        __try
        {
            *out = reinterpret_cast<R(__fastcall *)(A...)>(fn)(args...);
            return true;
        }
        __except (engine_fault_filter(GetExceptionCode()))
        {
            return false;
        }
    }

    /**
     * @brief Calls the void function @p fn(args...) under SEH.
     * @return True when the call returned.
     */
    template <typename... A> [[nodiscard]] bool guarded_void_call(std::uintptr_t fn, A... args) noexcept
    {
        __try
        {
            reinterpret_cast<void(__fastcall *)(A...)>(fn)(args...);
            return true;
        }
        __except (engine_fault_filter(GetExceptionCode()))
        {
            return false;
        }
    }

    /**
     * @brief Calls the virtual at @p slot of @p object, whose vtable and target must lie in the game image.
     * @return The result, or std::nullopt when the slot is not plausible or the call faulted.
     */
    template <typename R, typename... A>
    [[nodiscard]] std::optional<R> vcall(std::uintptr_t object, std::ptrdiff_t slot, A... args) noexcept
    {
        const std::uintptr_t fn = read_vtable_slot(object, slot);
        if (fn == 0)
        {
            return std::nullopt;
        }
        R result{};
        if (!guarded_call<R>(fn, &result, object, args...))
        {
            return std::nullopt;
        }
        return result;
    }

    /**
     * @brief Calls the void virtual at @p slot of @p object, whose vtable and target must lie in the game image.
     * @return True when the call returned; false when the slot is not plausible or the call faulted.
     */
    template <typename... A>
    [[nodiscard]] bool vcall_void(std::uintptr_t object, std::ptrdiff_t slot, A... args) noexcept
    {
        const std::uintptr_t fn = read_vtable_slot(object, slot);
        return fn != 0 && guarded_void_call(fn, object, args...);
    }

} // namespace HenrySenses

#endif // HENRYSENSES_GUARDED_CALL_HPP
