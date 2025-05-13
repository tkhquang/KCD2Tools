/**
 * @file entity_hooks.h
 * @brief Header for entity system hooks functionality
 *
 * Provides functions to initialize and manage hooks for entity tracking,
 * primarily focused on identifying and tracking the player entity.
 */
#pragma once

#include <cstdint>

// Forward declarations
namespace GameStructures
{
    class CEntity;
}

// Function pointer type
typedef void (*CEntity_SetWorldTM_Func_t)(GameStructures::CEntity *this_ptr, float *tm_3x4, int flags);

/**
 * @brief Initialize entity system hooks
 * @param moduleBase Base address of the target game module
 * @param moduleSize Size of the target game module in bytes
 * @return true if initialization successful, false otherwise
 */
bool initializeEntityHooks(uintptr_t moduleBase, size_t moduleSize);

/**
 * @brief Clean up entity hook resources
 */
void cleanupEntityHooks();

/**
 * @brief Reset player entity pointer if the given entity is the player
 * @param entity Entity being destroyed
 */
void ResetPlayerEntityIfDestroyed(GameStructures::CEntity *entity);

/**
 * @brief Get the current player entity pointer safely
 * @return Pointer to player entity or nullptr if not found
 */
GameStructures::CEntity *GetPlayerEntity();
