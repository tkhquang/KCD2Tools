#include "entity_hooks.h"
#include "logger.h"
#include "constants.h" // For CEntity and AOB patterns
#include "utils.h"
#include "aob_scanner.h"
#include "global_state.h" // For g_thePlayerEntity
#include "MinHook.h"
#include <string.h> // For strcmp

// AOB pattern strings from KCD2ModLoader scans
const char *CENTITY_CONSTRUCTOR_CALLER_AOB_STR = "E8 ?? ?? ?? ?? 48 8B D8 EB ?? 48 8B DF 41 8B C7";
const char *CENTITY_SETWORLDTM_CALLER_AOB_STR = "E8 ?? ?? ?? ?? EB ?? 45 33 C0 F7 43";

typedef void *(*CEntity_Constructor_t)(Constants::CEntity *this_ptr, uintptr_t unknown_param_a2);
static CEntity_Constructor_t fpCEntityConstructorOriginal = nullptr;
static BYTE *g_CEntityConstructorHookTargetAddress = nullptr; // Actual address of constructor

// Detour function as before
void *Detour_CEntity_Constructor(Constants::CEntity *this_ptr, uintptr_t unknown_param_a2)
{
    Logger &logger = Logger::getInstance();
    void *result = nullptr;

    if (fpCEntityConstructorOriginal)
    {
        result = fpCEntityConstructorOriginal(this_ptr, unknown_param_a2);
    }
    else
    {
        logger.log(LOG_ERROR, "EntityHooks: fpCEntityConstructorOriginal is NULL");
    }

    const char *entityName = nullptr;            // This will be the raw pointer from GetName()
    std::string nameStr = "GetNameFailedOrNull"; // This will be 'actualName' - a safe std::string version

    try
    {
        if (this_ptr && isMemoryReadable(this_ptr, sizeof(void *)))
        {
            uintptr_t *vtable = *reinterpret_cast<uintptr_t **>(this_ptr);
            if (isMemoryReadable(vtable, sizeof(void *) * 19))
            {                                     // VTABLE_INDEX_GETNAME = 18
                entityName = this_ptr->GetName(); // Call virtual GetName()
                if (entityName && isMemoryReadable(entityName, 1))
                {                                      // Check if char* is valid
                    nameStr = std::string(entityName); // CONVERT char* to std::string. THIS IS 'actualName'
                }
                else
                {
                    nameStr = "GetNameReturnedNullOrUnreadableString";
                }
            }
            else
            { /* ... */
            }
        }
        else
        { /* ... */
        }
    }
    catch (...)
    { /* ... */
    }
    // Using nameStr (the std::string version) for robust searching:
    if (nameStr.find("Dude") != std::string::npos && nameStr.find("Player") != std::string::npos)
    {
        if (g_thePlayerEntity == nullptr)
        {
            logger.log(LOG_INFO, "!!! Player Entity ASSIGNED (name contains 'Dude' and 'Player'): Addr=" +
                                     format_address(reinterpret_cast<uintptr_t>(this_ptr)) + " Full Name: " + nameStr);
        }
        g_thePlayerEntity = this_ptr;
    }
    // ...
    return result;
}

bool initializeEntityHooks(uintptr_t moduleBase, size_t moduleSize)
{
    Logger &logger = Logger::getInstance();
    logger.log(LOG_INFO, "EntityHooks: Initializing...");

    // 1. Find CEntity Constructor Target
    std::vector<BYTE> ctor_caller_pattern = parseAOB(CENTITY_CONSTRUCTOR_CALLER_AOB_STR);
    if (ctor_caller_pattern.empty())
    {
        logger.log(LOG_ERROR, "EntityHooks: Failed to parse CEntity constructor caller AOB.");
        return false;
    }
    BYTE *ctor_caller_match = FindPattern(reinterpret_cast<BYTE *>(moduleBase), moduleSize, ctor_caller_pattern);
    if (!ctor_caller_match)
    {
        logger.log(LOG_ERROR, "EntityHooks: CEntity constructor caller AOB not found.");
        return false;
    }
    // ctor_caller_match points to "E8". Relative offset is at ctor_caller_match + 1 (4 bytes)
    // Target = address of next instruction (ctor_caller_match + 5) + relative_offset
    if (!isMemoryReadable(ctor_caller_match + 1, sizeof(int32_t)))
    {
        logger.log(LOG_ERROR, "EntityHooks: Cannot read offset for CEntity constructor call.");
        return false;
    }
    int32_t ctor_relative_offset = *reinterpret_cast<int32_t *>(ctor_caller_match + 1);
    g_CEntityConstructorHookTargetAddress = ctor_caller_match + 5 + ctor_relative_offset;
    logger.log(LOG_INFO, "EntityHooks: CEntity constructor target resolved to " + format_address(reinterpret_cast<uintptr_t>(g_CEntityConstructorHookTargetAddress)));

    MH_STATUS status = MH_CreateHook(reinterpret_cast<LPVOID>(g_CEntityConstructorHookTargetAddress), reinterpret_cast<LPVOID>(&Detour_CEntity_Constructor), reinterpret_cast<LPVOID *>(&fpCEntityConstructorOriginal));
    if (status != MH_OK)
    { /* log, return false */
    }
    // ... (enable hook) ... (rest of init as above)
    if (!fpCEntityConstructorOriginal)
    {
        logger.log(LOG_ERROR, "EntityHooks: MH_CreateHook for CEntity constructor returned NULL trampoline.");
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_CEntityConstructorHookTargetAddress));
        return false;
    }
    status = MH_EnableHook(reinterpret_cast<LPVOID>(g_CEntityConstructorHookTargetAddress));
    if (status != MH_OK)
    {
        logger.log(LOG_ERROR, "EntityHooks: MH_EnableHook for CEntity constructor failed: " + std::string(MH_StatusToString(status)));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_CEntityConstructorHookTargetAddress));
        return false;
    }
    logger.log(LOG_INFO, "EntityHooks: CEntity constructor hook installed.");

    // 2. Find CEntity::SetWorldTM Target
    std::vector<BYTE> setworldtm_caller_pattern = parseAOB(CENTITY_SETWORLDTM_CALLER_AOB_STR);
    if (setworldtm_caller_pattern.empty())
    { /* log error */
        return false;
    }
    BYTE *setworldtm_caller_match = FindPattern(reinterpret_cast<BYTE *>(moduleBase), moduleSize, setworldtm_caller_pattern);
    if (!setworldtm_caller_match)
    { /* log error */
        return false;
    }

    if (!isMemoryReadable(setworldtm_caller_match + 1, sizeof(int32_t)))
    { /* log error */
        return false;
    }
    int32_t setworldtm_relative_offset = *reinterpret_cast<int32_t *>(setworldtm_caller_match + 1);
    BYTE *setworldtm_target_address = setworldtm_caller_match + 5 + setworldtm_relative_offset;
    g_funcCEntitySetWorldTM = reinterpret_cast<CEntity_SetWorldTM_Func_t>(setworldtm_target_address);
    logger.log(LOG_INFO, "EntityHooks: CEntity::SetWorldTM function resolved to " + format_address(reinterpret_cast<uintptr_t>(g_funcCEntitySetWorldTM)));

    if (!g_funcCEntitySetWorldTM)
    { // Redundant if above checks pass, but good final check
        logger.log(LOG_ERROR, "EntityHooks: Failed to resolve CEntity::SetWorldTM function pointer.");
        // cleanup CEntityConstructor hook if it was set
        if (g_CEntityConstructorHookTargetAddress && fpCEntityConstructorOriginal)
        {
            MH_DisableHook(reinterpret_cast<LPVOID>(g_CEntityConstructorHookTargetAddress));
            MH_RemoveHook(reinterpret_cast<LPVOID>(g_CEntityConstructorHookTargetAddress));
        }
        return false;
    }

    logger.log(LOG_INFO, "EntityHooks: Initialization complete.");
    return true;
}

void cleanupEntityHooks()
{
    Logger &logger = Logger::getInstance();
    if (g_CEntityConstructorHookTargetAddress && fpCEntityConstructorOriginal)
    { // Check if original hook was set up
        MH_DisableHook(reinterpret_cast<LPVOID>(g_CEntityConstructorHookTargetAddress));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_CEntityConstructorHookTargetAddress));
        g_CEntityConstructorHookTargetAddress = nullptr;
        fpCEntityConstructorOriginal = nullptr;
        logger.log(LOG_DEBUG, "EntityHooks: CEntity constructor hook cleaned up.");
    }
    g_funcCEntitySetWorldTM = nullptr;
    g_thePlayerEntity = nullptr;
}
