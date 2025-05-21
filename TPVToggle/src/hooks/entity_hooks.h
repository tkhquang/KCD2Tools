/**
 * @file entity_hooks.h
 * @brief Header for entity system hooks functionality
 *
 * Provides functions to initialize and manage hooks for entity tracking,
 * primarily focused on identifying and tracking the player entity.
 */
#pragma once

#include <cstdint>

// Forward declarations from game_structures.h to avoid circular dependencies if any
namespace GameStructures
{
    class CEntity;
}

// Typedef for the game's SetWorldTM function (if used directly, not just hooked)
// This type is used for g_funcCEntitySetWorldTM which is obtained by AOB scanning, not hooking.
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
 * @param entity Entity that might be the player and is being destroyed
 */
void ResetPlayerEntityIfDestroyed(GameStructures::CEntity *entity);

/**
 * @brief Get the current player entity pointer safely
 * @return Pointer to player CEntity object or nullptr if not found/set
 */
GameStructures::CEntity *GetPlayerEntity();

/**
 * @brief Check if the entity hooks (specifically constructor caller hook) are active.
 * @return true if the constructor caller hook is considered active.
 */
bool isEntityHooksActive();
