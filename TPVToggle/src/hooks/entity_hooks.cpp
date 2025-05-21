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
// #include "aob_scanner.h" // No longer used here
#include "global_state.h"
#include "hook_manager.hpp" // Use HookManager

#include <string>
#include <mutex>
#include <stdexcept>

// AOB patterns for entity system functions
// WHGame.DLL+6783A1 - E8 068E0200           - call WHGame.DLL+6A11AC // Target for constructor
// WHGame.DLL+6783A6 - 48 8B D8              - mov rbx,rax
// WHGame.DLL+6783A9 - EB 03                 - jmp WHGame.DLL+6783AE
// WHGame.DLL+6783AB - 48 8B DF              - mov rbx,rdi
// WHGame.DLL+6783AE - 41 8B C7              - mov eax,r15d
constexpr const char *CENTITY_CONSTRUCTOR_CALL_AOB = "E8 ?? ?? ?? ?? 48 8B D8 EB ?? 48 8B DF 41 8B C7";

// WHGame.DLL+D15230 - E8 839664FF           - call WHGame.DLL+35E8B8 // Target for SetWorldTM
// WHGame.DLL+D15235 - EB 21                 - jmp WHGame.DLL+D15258
// WHGame.DLL+D15237 - 45 33 C0              - xor r8d,r8d
// WHGame.DLL+D1523A - F7 43 18 00400000     - test [rbx+18],00004000 { 16384 }
constexpr const char *CENTITY_SETWORLDTM_CALL_AOB = "E8 ?? ?? ?? ?? EB ?? 45 33 C0 F7 43";

// Function typedefs
// CEntity constructor is tricky. The AOB points to a CALL instruction.
// The actual constructor might be complex to define a compatible C++ signature for if it's non-trivial.
// For just detecting the call and getting 'this_ptr', a generic signature might work
// if the registers match for the first argument.
// Let's assume the call target takes CEntity* as the first argument (rcx on x64).
// The return is often the same CEntity* (this_ptr).
typedef void *(__fastcall *CEntity_Constructor_Called_t)(GameStructures::CEntity *this_ptr_in_rcx, uintptr_t unknown_param_rdx);

// Static hook data
static CEntity_Constructor_Called_t fpCEntityConstructorCalledOriginal = nullptr;
// static BYTE *g_CEntityConstructorHookAddress = nullptr; // Managed by HookManager
static std::string g_CEntityConstructorHookId = "";
static std::mutex g_entityMutex; // Protect player entity pointer access

// Global entity pointer (defined in global_state.h, definition in global_state.cpp)
// extern GameStructures::CEntity *g_thePlayerEntity;
// Global function pointer (defined in global_state.h, definition in global_state.cpp)
// extern CEntity_SetWorldTM_Func_t g_funcCEntitySetWorldTM; // Retained, as it's not a hook trampoline

/**
 * @brief Detour for the function *calling* the CEntity constructor or a related setup function.
 * @details Intercepts entity creation/setup to detect and track the player entity.
 *          Identifies player by checking if entity name contains "Dude" and "Player".
 * @param this_ptr_in_rcx Potentially the CEntity instance being constructed or set up.
 * @param unknown_param_rdx Another parameter passed to the called function.
 * @return Result from original called function.
 */
void *__fastcall Detour_CEntity_Constructor_Called(GameStructures::CEntity *this_ptr_in_rcx, uintptr_t unknown_param_rdx)
{
    Logger &logger = Logger::getInstance();
    void *result = nullptr;

    // Call the original function that was hooked (which in turn calls the actual constructor or setup)
    if (fpCEntityConstructorCalledOriginal)
    {
        result = fpCEntityConstructorCalledOriginal(this_ptr_in_rcx, unknown_param_rdx);
    }
    else
    {
        logger.log(LOG_ERROR, "EntityHooks: fpCEntityConstructorCalledOriginal (trampoline) is NULL");
        return nullptr; // Or some default if applicable to the original function's return type
    }

    // After the call, 'this_ptr_in_rcx' (if it was indeed the CEntity*) should be initialized,
    // or 'result' (if the called function returns the entity pointer, like some factories do).
    // We need to determine which one holds the CEntity instance.
    // Often, the first parameter (RCX) to a constructor-like call is the 'this' pointer.
    GameStructures::CEntity *entity_instance = this_ptr_in_rcx; // Assumption

    // Get entity name safely
    std::string entityName = "UnknownEntity";
    bool name_retrieved = false;

    try
    {
        // Validate the entity_instance pointer before using it
        if (entity_instance && isMemoryReadable(entity_instance, sizeof(void *))) // Check if this_ptr is readable for vtable
        {
            // Check if vtable is readable (need at least GetName's index + 1 entries)
            // GetName is vtable index 18 (0-indexed). So need 19 entries.
            uintptr_t *vtable_ptr_address = reinterpret_cast<uintptr_t *>(entity_instance);
            if (isMemoryReadable(vtable_ptr_address, sizeof(uintptr_t)))
            {
                uintptr_t *vtable = reinterpret_cast<uintptr_t *>(*vtable_ptr_address);
                if (isMemoryReadable(vtable, sizeof(void *) * 19))
                {
                    const char *rawName = entity_instance->GetName(); // Call through vtable
                    if (rawName && isMemoryReadable(rawName, 1))      // Check if name pointer and first char are readable
                    {
                        // Safely copy string up to a reasonable limit to prevent buffer overflows
                        // from malformed game strings
                        char name_buffer[256];
                        strncpy_s(name_buffer, sizeof(name_buffer), rawName, _TRUNCATE);
                        entityName = name_buffer;
                        name_retrieved = true;
                    }
                    else
                    {
                        // logger.log(LOG_TRACE, "EntityHooks: rawName pointer invalid or unreadable from GetName().");
                    }
                }
                else
                {
                    // logger.log(LOG_TRACE, "EntityHooks: VTable for GetName() not readable.");
                }
            }
            else
            {
                // logger.log(LOG_TRACE, "EntityHooks: VTable pointer not readable.");
            }
        }
        else
        {
            // logger.log(LOG_TRACE, "EntityHooks: entity_instance pointer invalid or unreadable.");
        }
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_WARNING, "EntityHooks: Exception getting entity name: " + std::string(e.what()));
    }
    catch (...)
    {
        logger.log(LOG_WARNING, "EntityHooks: Unknown exception getting entity name.");
    }

    // Check if this is the player entity
    // This check might need adjustment based on actual entity names in KCD2
    if (name_retrieved &&
        entityName.find("Dude") != std::string::npos &&
        entityName.find("Player") != std::string::npos)
    {
        std::lock_guard<std::mutex> lock(g_entityMutex);
        if (g_thePlayerEntity != entity_instance)
        {
            std::string status = g_thePlayerEntity == nullptr ? "detected" : "updated";
            logger.log(LOG_INFO, "EntityHooks: Player entity " +
                                     status +
                                     ". Name: '" + entityName +
                                     "' Addr: " + format_address(reinterpret_cast<uintptr_t>(entity_instance)) +
                                     (g_thePlayerEntity != nullptr ? " (Old: " + format_address(reinterpret_cast<uintptr_t>(g_thePlayerEntity)) + ")" : ""));
            g_thePlayerEntity = entity_instance;
        }
    }
    else if (name_retrieved && (entityName.find("Dude") != std::string::npos || entityName.find("Player") != std::string::npos))
    {
        // Log entities that partially match for debugging purposes
        // logger.log(LOG_TRACE, "EntityHooks: Potential (but not confirmed player) entity - Name: '" + entityName +
        //                         "' Addr: " + format_address(reinterpret_cast<uintptr_t>(entity_instance)));
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
        Logger::getInstance().log(LOG_INFO, "EntityHooks: Player entity pointer " + format_address(reinterpret_cast<uintptr_t>(entity)) + " is being reset (likely destroyed).");
        g_thePlayerEntity = nullptr;
    }
}

bool initializeEntityHooks(uintptr_t moduleBase, size_t moduleSize)
{
    Logger &logger = Logger::getInstance();
    logger.log(LOG_INFO, "EntityHooks: Initializing entity tracking hooks...");

    // Hooking the CALLER of the constructor (or a key setup function)
    // The AOB CENTITY_CONSTRUCTOR_CALL_AOB points to an E8 (call) instruction.
    // We hook this E8 instruction itself.
    // The original parameters to this E8 instruction become the parameters to our detour.
    // The actual constructor is the target of this E8 call.

    try
    {
        HookManager &hookManager = HookManager::getInstance();
        // The target of our hook is the CALL instruction itself.
        // AOB_OFFSET is 0 because the pattern starts with E8.
        g_CEntityConstructorHookId = hookManager.create_inline_hook_aob(
            "CEntityConstructorCaller",
            moduleBase,
            moduleSize,
            CENTITY_CONSTRUCTOR_CALL_AOB,
            0, // AOB_OFFSET = 0, as pattern is the CALL E8 instruction
            reinterpret_cast<void *>(Detour_CEntity_Constructor_Called),
            reinterpret_cast<void **>(&fpCEntityConstructorCalledOriginal));

        if (g_CEntityConstructorHookId.empty() || fpCEntityConstructorCalledOriginal == nullptr)
        {
            throw std::runtime_error("Failed to create CEntity Constructor Caller hook via HookManager.");
        }
        logger.log(LOG_INFO, "EntityHooks: CEntity Constructor Caller hook successfully installed (ID: " + g_CEntityConstructorHookId + ").");

        // Find SetWorldTM function directly (not hooked, but address is obtained)
        // This part remains as before, as it's not creating a hook, just finding a function.
        std::vector<BYTE> setWorldPattern = parseAOB(CENTITY_SETWORLDTM_CALL_AOB);
        if (!setWorldPattern.empty())
        {
            BYTE *setWorldMatch = FindPattern(reinterpret_cast<BYTE *>(moduleBase), moduleSize, setWorldPattern);
            // The AOB points to an E8 (call) instruction. We want the target of this call.
            if (setWorldMatch && isMemoryReadable(setWorldMatch + 1, sizeof(int32_t)))
            {
                int32_t setWorldRelativeOffset = *reinterpret_cast<int32_t *>(setWorldMatch + 1);
                // The address of the instruction after CALL is setWorldMatch + 5 (size of E8 xx xx xx xx)
                BYTE *setWorldActualFuncAddress = setWorldMatch + 5 + setWorldRelativeOffset;
                g_funcCEntitySetWorldTM = reinterpret_cast<CEntity_SetWorldTM_Func_t>(setWorldActualFuncAddress);

                logger.log(LOG_INFO, "EntityHooks: CEntity::SetWorldTM function found at " +
                                         format_address(reinterpret_cast<uintptr_t>(g_funcCEntitySetWorldTM)));
            }
            else
            {
                logger.log(LOG_WARNING, "EntityHooks: CEntity::SetWorldTM function AOB match failed or offset unreadable. Feature might be limited.");
                g_funcCEntitySetWorldTM = nullptr;
            }
        }
        else
        {
            logger.log(LOG_WARNING, "EntityHooks: Failed to parse CEntity::SetWorldTM AOB pattern.");
            g_funcCEntitySetWorldTM = nullptr;
        }

        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "EntityHooks: Initialization failed: " + std::string(e.what()));
        cleanupEntityHooks(); // Attempt cleanup
        return false;
    }
}

void cleanupEntityHooks()
{
    Logger &logger = Logger::getInstance();
    HookManager &hookManager = HookManager::getInstance();

    if (!g_CEntityConstructorHookId.empty())
    {
        if (hookManager.remove_hook(g_CEntityConstructorHookId))
        {
            logger.log(LOG_INFO, "EntityHooks: Hook '" + g_CEntityConstructorHookId + "' removed.");
        }
        fpCEntityConstructorCalledOriginal = nullptr;
        g_CEntityConstructorHookId = "";
    }

    // Clear function pointers and entity reference
    {
        std::lock_guard<std::mutex> lock(g_entityMutex);
        g_funcCEntitySetWorldTM = nullptr; // This wasn't a hook, just a found address.
        g_thePlayerEntity = nullptr;
    }

    logger.log(LOG_INFO, "EntityHooks: Cleanup complete.");
}

// Safe accessor function for other modules
GameStructures::CEntity *GetPlayerEntity()
{
    std::lock_guard<std::mutex> lock(g_entityMutex);
    return g_thePlayerEntity;
}

bool isEntityHooksActive()
{
    return (fpCEntityConstructorCalledOriginal != nullptr && !g_CEntityConstructorHookId.empty());
}
