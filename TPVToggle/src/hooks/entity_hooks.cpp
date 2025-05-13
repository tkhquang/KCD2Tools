/**
 * @file entity_hooks.cpp
 * @brief Implementation of entity system hooks for player tracking
 *
 * Hooks into entity constructor to detect player entity creation and tracks
 * the player entity pointer for camera manipulation and position queries.
 */

#include "entity_hooks.h"
#include "logger.h"
#include "constants.h"
#include "game_structures.h"
#include "utils.h"
#include "aob_scanner.h"
#include "global_state.h"
#include "MinHook.h"

#include <string>
#include <mutex>

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
static BYTE *g_CEntityConstructorHookAddress = nullptr;
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
void *Detour_CEntity_Constructor(GameStructures::CEntity *this_ptr, uintptr_t unknown_param)
{
    Logger &logger = Logger::getInstance();
    void *result = nullptr;

    // Call original constructor
    if (fpCEntityConstructorOriginal)
    {
        result = fpCEntityConstructorOriginal(this_ptr, unknown_param);
    }
    else
    {
        logger.log(LOG_ERROR, "EntityHooks: fpCEntityConstructorOriginal is NULL");
        return nullptr;
    }

    // Get entity name safely
    std::string entityName = "Unknown";

    try
    {
        if (this_ptr && isMemoryReadable(this_ptr, sizeof(void *)))
        {
            // Check if vtable is readable (need at least 19 entries for GetName)
            uintptr_t *vtable = *reinterpret_cast<uintptr_t **>(this_ptr);
            if (isMemoryReadable(vtable, sizeof(void *) * 19))
            {
                const char *rawName = this_ptr->GetName();
                if (rawName && isMemoryReadable(rawName, 1))
                {
                    // Copy the name string safely
                    entityName = std::string(rawName);
                }
            }
        }
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_WARNING, "EntityHooks: Exception getting entity name: " + std::string(e.what()));
    }
    catch (...)
    {
        logger.log(LOG_WARNING, "EntityHooks: Unknown exception getting entity name");
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
                logger.log(LOG_INFO, "EntityHooks: Player entity detected and assigned - Name: '" +
                                         entityName + "' Addr: " + format_address(reinterpret_cast<uintptr_t>(this_ptr)));
            }
            else
            {
                logger.log(LOG_INFO, "EntityHooks: Player entity updated - Old: " +
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
        Logger::getInstance().log(LOG_INFO, "EntityHooks: Player entity being destroyed - Resetting pointer");
        g_thePlayerEntity = nullptr;
    }
}

bool initializeEntityHooks(uintptr_t moduleBase, size_t moduleSize)
{
    Logger &logger = Logger::getInstance();
    logger.log(LOG_INFO, "EntityHooks: Initializing entity tracking hooks...");

    try
    {
        // Find CEntity constructor target
        std::vector<BYTE> ctorPattern = parseAOB(CENTITY_CONSTRUCTOR_CALLER_AOB);
        if (ctorPattern.empty())
        {
            throw std::runtime_error("Failed to parse CEntity constructor caller AOB");
        }

        BYTE *ctorMatch = FindPattern(reinterpret_cast<BYTE *>(moduleBase), moduleSize, ctorPattern);
        if (!ctorMatch)
        {
            throw std::runtime_error("CEntity constructor caller pattern not found");
        }

        // Extract relative offset to get actual constructor address
        if (!isMemoryReadable(ctorMatch + 1, sizeof(int32_t)))
        {
            throw std::runtime_error("Cannot read constructor call offset");
        }

        int32_t relativeOffset = *reinterpret_cast<int32_t *>(ctorMatch + 1);
        g_CEntityConstructorHookAddress = ctorMatch + 5 + relativeOffset;

        logger.log(LOG_INFO, "EntityHooks: CEntity constructor found at " +
                                 format_address(reinterpret_cast<uintptr_t>(g_CEntityConstructorHookAddress)));

        // Create and enable constructor hook
        MH_STATUS status = MH_CreateHook(
            reinterpret_cast<LPVOID>(g_CEntityConstructorHookAddress),
            reinterpret_cast<LPVOID>(&Detour_CEntity_Constructor),
            reinterpret_cast<LPVOID *>(&fpCEntityConstructorOriginal));

        if (status != MH_OK)
        {
            throw std::runtime_error("MH_CreateHook failed: " + std::string(MH_StatusToString(status)));
        }

        if (!fpCEntityConstructorOriginal)
        {
            MH_RemoveHook(reinterpret_cast<LPVOID>(g_CEntityConstructorHookAddress));
            throw std::runtime_error("MH_CreateHook returned NULL trampoline");
        }

        status = MH_EnableHook(reinterpret_cast<LPVOID>(g_CEntityConstructorHookAddress));
        if (status != MH_OK)
        {
            MH_RemoveHook(reinterpret_cast<LPVOID>(g_CEntityConstructorHookAddress));
            throw std::runtime_error("MH_EnableHook failed: " + std::string(MH_StatusToString(status)));
        }

        logger.log(LOG_INFO, "EntityHooks: CEntity constructor hook successfully installed");

        // Find SetWorldTM function for future use
        std::vector<BYTE> setWorldPattern = parseAOB(CENTITY_SETWORLDTM_CALLER_AOB);
        if (!setWorldPattern.empty())
        {
            BYTE *setWorldMatch = FindPattern(reinterpret_cast<BYTE *>(moduleBase), moduleSize, setWorldPattern);
            if (setWorldMatch && isMemoryReadable(setWorldMatch + 1, sizeof(int32_t)))
            {
                int32_t setWorldOffset = *reinterpret_cast<int32_t *>(setWorldMatch + 1);
                BYTE *setWorldAddress = setWorldMatch + 5 + setWorldOffset;
                g_funcCEntitySetWorldTM = reinterpret_cast<CEntity_SetWorldTM_Func_t>(setWorldAddress);

                logger.log(LOG_INFO, "EntityHooks: SetWorldTM function found at " +
                                         format_address(reinterpret_cast<uintptr_t>(g_funcCEntitySetWorldTM)));
            }
            else
            {
                logger.log(LOG_WARNING, "EntityHooks: SetWorldTM function not found - Feature limited");
            }
        }

        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "EntityHooks: Initialization failed: " + std::string(e.what()));
        cleanupEntityHooks();
        return false;
    }
}

void cleanupEntityHooks()
{
    Logger &logger = Logger::getInstance();

    // Disable and remove constructor hook
    if (g_CEntityConstructorHookAddress && fpCEntityConstructorOriginal)
    {
        MH_DisableHook(reinterpret_cast<LPVOID>(g_CEntityConstructorHookAddress));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_CEntityConstructorHookAddress));
        g_CEntityConstructorHookAddress = nullptr;
        fpCEntityConstructorOriginal = nullptr;
        logger.log(LOG_INFO, "EntityHooks: Constructor hook removed");
    }

    // Clear function pointers and entity reference
    {
        std::lock_guard<std::mutex> lock(g_entityMutex);
        g_funcCEntitySetWorldTM = nullptr;
        g_thePlayerEntity = nullptr;
    }

    logger.log(LOG_INFO, "EntityHooks: Cleanup complete");
}

// Safe accessor function for other modules
GameStructures::CEntity *GetPlayerEntity()
{
    std::lock_guard<std::mutex> lock(g_entityMutex);
    return g_thePlayerEntity;
}
