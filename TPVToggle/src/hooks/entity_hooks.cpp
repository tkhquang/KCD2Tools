/**
 * @file entity_hooks.cpp
 * @brief Implementation of entity system hooks for player tracking using DetourModKit.
 *
 * Hooks into entity constructor to detect player entity creation and tracks
 * the player entity pointer for camera manipulation and position queries.
 */

#include "entity_hooks.h"
#include "constants.h"
#include "game_structures.h"
#include "utils.h"
#include "global_state.h"

#include <DetourModKit.hpp>

#include <string>
#include <mutex>

using DetourModKit::LogLevel;
using DMKFormat::format_address;

// AOB patterns for entity system functions
// WHGame.DLL+6783A1 - E8 068E0200           - call WHGame.DLL+6A11AC
// WHGame.DLL+6783A6 - 48 8B D8              - mov rbx,rax
// WHGame.DLL+6783A9 - EB 03                 - jmp WHGame.DLL+6783AE
// WHGame.DLL+6783AB - 48 8B DF              - mov rbx,rdi
// WHGame.DLL+6783AE - 41 8B C7              - mov eax,r15d
constexpr const char *CENTITY_CONSTRUCTOR_CALLER_AOB = "E8 ?? ?? ?? ?? 48 8B D8 EB ?? 48 8B DF 41 8B C7";

// WHGame.DLL+D15230 - E8 839664FF           - call WHGame.DLL+35E8B8
// WHGame.DLL+D15235 - EB 21                 - jmp WHGame.DLL+D15258
// WHGame.DLL+D15237 - 45 33 C0              - xor r8d,r8d
// WHGame.DLL+D1523A - F7 43 18 00400000     - test [rbx+18],00004000 { 16384 }
constexpr const char *CENTITY_SETWORLDTM_CALLER_AOB = "E8 ?? ?? ?? ?? EB ?? 45 33 C0 F7 43";

// Function typedefs
typedef void *(*CEntity_Constructor_t)(GameStructures::CEntity *this_ptr, uintptr_t unknown_param);
typedef void (*CEntity_SetWorldTM_t)(GameStructures::CEntity *this_ptr, float *tm_3x4, int flags);

// Static hook data
static CEntity_Constructor_t fpCEntityConstructorOriginal = nullptr;
static std::string g_CEntityConstructorHookId;
static std::mutex g_entityMutex; // Protect player entity pointer access

// Global entity pointer
extern GameStructures::CEntity *g_thePlayerEntity;
// Global function pointer (defined in global_state.cpp)
extern CEntity_SetWorldTM_Func_t g_funcCEntitySetWorldTM;

/**
 * @brief Detour function for CEntity constructor
 * @details Intercepts entity creation to detect and track the player entity.
 *          Identifies player by checking if entity name contains both "Dude" and "Player".
 * @param this_ptr Pointer to the entity being constructed
 * @param unknown_param Parameter passed to constructor (purpose unknown)
 * @return Result from original constructor
 */
static void *Detour_CEntity_Constructor(GameStructures::CEntity *this_ptr, uintptr_t unknown_param)
{
    DMKLogger &logger = DMKLogger::get_instance();
    void *result = nullptr;

    // Call original constructor
    if (fpCEntityConstructorOriginal)
    {
        result = fpCEntityConstructorOriginal(this_ptr, unknown_param);
    }
    else
    {
        logger.log(LogLevel::Error, "EntityHooks: fpCEntityConstructorOriginal is NULL");
        return nullptr;
    }

    // Get entity name safely
    std::string entityName = "Unknown";

    try
    {
        if (this_ptr && DMKMemory::is_readable(this_ptr, sizeof(void *)))
        {
            // Check if vtable is readable (need at least 19 entries for GetName)
            uintptr_t *vtable = *reinterpret_cast<uintptr_t **>(this_ptr);
            if (DMKMemory::is_readable(vtable, sizeof(void *) * 19))
            {
                const char *rawName = this_ptr->GetName();
                if (rawName && DMKMemory::is_readable(rawName, 1))
                {
                    // Copy the name string safely
                    entityName = std::string(rawName);
                }
            }
        }
    }
    catch (const std::exception &e)
    {
        logger.log(LogLevel::Warning, "EntityHooks: Exception getting entity name: " + std::string(e.what()));
    }
    catch (...)
    {
        logger.log(LogLevel::Warning, "EntityHooks: Unknown exception getting entity name");
    }

    // Check if this is the player entity
    if (entityName.find("Dude") != std::string::npos &&
        entityName.find("Player") != std::string::npos)
    {
        std::lock_guard<std::mutex> lock(g_entityMutex);

        // Check if we're updating the player entity
        if (g_thePlayerEntity != this_ptr)
        {
            if (g_thePlayerEntity == nullptr)
            {
                logger.log(LogLevel::Info, "EntityHooks: Player entity detected and assigned - Name: '" +
                                         entityName + "' Addr: " + format_address(reinterpret_cast<uintptr_t>(this_ptr)));
            }
            else
            {
                logger.log(LogLevel::Info, "EntityHooks: Player entity updated - Old: " +
                                         format_address(reinterpret_cast<uintptr_t>(g_thePlayerEntity)) +
                                         " New: " + format_address(reinterpret_cast<uintptr_t>(this_ptr)) +
                                         " Name: '" + entityName + "'");
            }

            g_thePlayerEntity = this_ptr;
        }
    }

    return result;
}

/**
 * @brief Reset player entity pointer on destruction
 * @details Called to ensure we don't hold stale pointers when player entity is destroyed
 * @param entity Entity being checked for destruction
 */
void ResetPlayerEntityIfDestroyed(GameStructures::CEntity *entity)
{
    std::lock_guard<std::mutex> lock(g_entityMutex);

    if (g_thePlayerEntity == entity)
    {
        DMKLogger::get_instance().log(LogLevel::Info, "EntityHooks: Player entity being destroyed - Resetting pointer");
        g_thePlayerEntity = nullptr;
    }
}

bool initializeEntityHooks(uintptr_t moduleBase, size_t moduleSize)
{
    DMKLogger &logger = DMKLogger::get_instance();
    logger.log(LogLevel::Info, "EntityHooks: Initializing entity tracking hooks...");

    try
    {
        // Find CEntity constructor target
        auto ctorPattern = DMKScanner::parse_aob(CENTITY_CONSTRUCTOR_CALLER_AOB);
        if (!ctorPattern.has_value())
        {
            throw std::runtime_error("Failed to parse CEntity constructor caller AOB");
        }

        const std::byte *ctorMatch = DMKScanner::find_pattern(reinterpret_cast<const std::byte *>(moduleBase), moduleSize, *ctorPattern);
        if (!ctorMatch)
        {
            throw std::runtime_error("CEntity constructor caller pattern not found");
        }

        // Resolve call rel32 target: E8 xx xx xx xx (5 bytes, disp32 at offset 1)
        auto constructorAddress = DMKScanner::resolve_rip_relative(ctorMatch, 1, 5);
        if (!constructorAddress.has_value())
        {
            throw std::runtime_error("Failed to resolve constructor call target");
        }

        logger.log(LogLevel::Info, "EntityHooks: CEntity constructor found at " +
                                 format_address(constructorAddress.value()));

        // Create hook using DMKHookManager
        DMKHookManager &hook_manager = DMKHookManager::get_instance();

        auto ctorResult = hook_manager.create_inline_hook(
            "CEntity_Constructor",
            constructorAddress.value(),
            reinterpret_cast<void *>(&Detour_CEntity_Constructor),
            reinterpret_cast<void **>(&fpCEntityConstructorOriginal));

        if (!ctorResult.has_value())
        {
            throw std::runtime_error("Failed to create CEntity constructor hook: " + std::string(DMK::Hook::error_to_string(ctorResult.error())));
        }
        g_CEntityConstructorHookId = ctorResult.value();

        logger.log(LogLevel::Info, "EntityHooks: CEntity constructor hook successfully installed");

        // Find SetWorldTM function for future use
        auto setWorldPattern = DMKScanner::parse_aob(CENTITY_SETWORLDTM_CALLER_AOB);
        if (setWorldPattern.has_value())
        {
            const std::byte *setWorldMatch = DMKScanner::find_pattern(reinterpret_cast<const std::byte *>(moduleBase), moduleSize, *setWorldPattern);
            if (setWorldMatch)
            {
                // Resolve call rel32 target: E8 xx xx xx xx (5 bytes, disp32 at offset 1)
                auto setWorldAddress = DMKScanner::resolve_rip_relative(setWorldMatch, 1, 5);
                if (setWorldAddress.has_value())
                {
                    g_funcCEntitySetWorldTM = reinterpret_cast<CEntity_SetWorldTM_Func_t>(setWorldAddress.value());
                    logger.log(LogLevel::Info, "EntityHooks: SetWorldTM function found at " +
                                             format_address(setWorldAddress.value()));
                }
            }
            else
            {
                logger.log(LogLevel::Warning, "EntityHooks: SetWorldTM function not found - Feature limited");
            }
        }

        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LogLevel::Error, "EntityHooks: Initialization failed: " + std::string(e.what()));
        cleanupEntityHooks();
        return false;
    }
}

void cleanupEntityHooks()
{
    DMKLogger &logger = DMKLogger::get_instance();
    DMKHookManager &hook_manager = DMKHookManager::get_instance();

    // Remove constructor hook
    if (!g_CEntityConstructorHookId.empty())
    {
        (void)hook_manager.remove_hook(g_CEntityConstructorHookId);
        g_CEntityConstructorHookId.clear();
        fpCEntityConstructorOriginal = nullptr;
        logger.log(LogLevel::Info, "EntityHooks: Constructor hook removed");
    }

    // Clear function pointers and entity reference
    {
        std::lock_guard<std::mutex> lock(g_entityMutex);
        g_funcCEntitySetWorldTM = nullptr;
        g_thePlayerEntity = nullptr;
    }

    logger.log(LogLevel::Info, "EntityHooks: Cleanup complete");
}

// Safe accessor function for other modules
GameStructures::CEntity *GetPlayerEntity()
{
    std::lock_guard<std::mutex> lock(g_entityMutex);
    return g_thePlayerEntity;
}
