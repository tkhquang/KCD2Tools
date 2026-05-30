/**
 * @file hooks/event_hooks.hpp
 * @brief Header for event handling hooks functionality.
 *
 * Provides functions to initialize and manage the hooks that intercept
 * game events, particularly for filtering scroll wheel input during overlays.
 */
#ifndef EVENT_HOOKS_HPP
#define EVENT_HOOKS_HPP

#include <cstdint>
#include <cstddef>

namespace TPVToggle
{

/**
 * @brief Initialize event hooks for input filtering.
 * @param module_base Base address of the target game module.
 * @param module_size Size of the target game module in bytes.
 * @return true if initialization successful, false otherwise.
 */
[[nodiscard]] bool initializeEventHooks(uintptr_t module_base, size_t module_size);

/**
 * @brief Clean up event hook resources.
 */
void cleanupEventHooks() noexcept;

} // namespace TPVToggle

#endif // EVENT_HOOKS_HPP
