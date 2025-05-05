/**
 * @file hooks/event_hooks.h
 * @brief Header for event handling hooks functionality.
 *
 * Provides functions to initialize and manage the hooks that intercept
 * game events, particularly for filtering scroll wheel input during overlays.
 */
#ifndef EVENT_HOOKS_H
#define EVENT_HOOKS_H

#include <windows.h>
#include <cstdint>

/**
 * @brief Initialize event hooks for input filtering.
 * @param module_base Base address of the target game module.
 * @param module_size Size of the target game module in bytes.
 * @return true if initialization successful, false otherwise.
 */
bool initializeEventHooks(uintptr_t module_base, size_t module_size);

/**
 * @brief Clean up event hook resources.
 */
void cleanupEventHooks();

/**
 * @brief Check if event hooks are active and ready.
 * @return true if all required hooks are successfully installed and functional.
 */
bool areEventHooksActive();

#endif // EVENT_HOOKS_H
