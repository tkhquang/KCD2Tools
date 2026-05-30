/**
 * @file entity_hooks.cpp
 * @brief Implementation of entity system hooks for player tracking using DetourModKit.
 *
 * Hooks into entity constructor to detect player entity creation and tracks
 * the player entity pointer for camera manipulation and position queries.
 */

#include "entity_hooks.hpp"
#include "constants.hpp"
#include "game_structures.hpp"
#include "utils.hpp"
#include "global_state.hpp"

#include <DetourModKit.hpp>

#include <string>
#include <mutex>

using DMK::Format::format_address;

namespace TPVToggle
{

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

using CEntity_Constructor_t = void *(*)(GameStructures::CEntity *this_ptr, uintptr_t unknown_param);
using CEntity_SetWorldTM_t = void (*)(GameStructures::CEntity *this_ptr, float *tm_3x4, int flags);

static CEntity_Constructor_t fpCEntityConstructorOriginal = nullptr;
static std::string g_CEntityConstructorHookId;
static std::mutex g_entityMutex; // Protect player entity pointer access

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
    DMK::Logger &logger = DMK::Logger::get_instance();

    if (!fpCEntityConstructorOriginal)
    {
        logger.error("EntityHooks: fpCEntityConstructorOriginal is NULL");
        return nullptr;
    }

    void *result = fpCEntityConstructorOriginal(this_ptr, unknown_param);

    // Memory faults below are handled by the seh_* primitives; this guard only
    // stops a C++ exception (e.g. std::string allocation) from unwinding into the
    // engine, which would terminate the process. It does not catch access
    // violations (catch(...) cannot, under MSVC /EHsc).
    try
    {
        std::string entityName = "Unknown";

        // A live CEntity's vtable points into the game module (WHGame.dll), whose
        // mapped range is captured in the resolved module info during startup.
        // Validate the vtable against that range with a branch-only test before the
        // virtual GetName() call, instead of taking a region-cache lock on every
        // construction. Note: host_module_range() is the bootstrap EXE, which holds
        // no game RTTI, so it must not be used here.
        const DMK::Memory::ModuleRange game_range{TPVToggle::module_info().base, TPVToggle::module_info().base + TPVToggle::module_info().size};
        const uintptr_t this_addr = reinterpret_cast<uintptr_t>(this_ptr);
        if (DMK::Memory::plausible_userspace_ptr(this_addr))
        {
            const auto vtable = DMK::Memory::seh_read<uintptr_t>(this_addr);
            if (vtable && DMK::Memory::contains(game_range, *vtable))
            {
                const char *rawName = this_ptr->GetName();
                if (rawName)
                {
                    // GetName() returns an engine-owned C string of unknown length.
                    // Copy it into a bounded buffer under one SEH frame so a name
                    // ending near an unmapped page cannot fault an std::string scan.
                    char name_buf[128] = {};
                    if (DMK::Memory::seh_read_bytes(reinterpret_cast<uintptr_t>(rawName), name_buf, sizeof(name_buf) - 1))
                    {
                        entityName = name_buf;
                    }
                }
            }
        }

        // Identify the player entity by name and latch its pointer.
        if (entityName.find("Dude") != std::string::npos &&
            entityName.find("Player") != std::string::npos)
        {
            std::lock_guard<std::mutex> lock(g_entityMutex);

            if (TPVToggle::player_transform().entity != this_ptr)
            {
                if (TPVToggle::player_transform().entity == nullptr)
                {
                    logger.info("EntityHooks: Player entity detected and assigned - Name: '{}' Addr: {}",
                                entityName, format_address(reinterpret_cast<uintptr_t>(this_ptr)));
                }
                else
                {
                    logger.info("EntityHooks: Player entity updated - Old: {} New: {} Name: '{}'",
                                format_address(reinterpret_cast<uintptr_t>(TPVToggle::player_transform().entity)),
                                format_address(reinterpret_cast<uintptr_t>(this_ptr)),
                                entityName);
                }

                TPVToggle::player_transform().entity = this_ptr;
            }
        }
    }
    catch (const std::exception &e)
    {
        logger.warning("EntityHooks: Exception in constructor detour: {}", e.what());
    }
    catch (...)
    {
        logger.warning("EntityHooks: Unknown exception in constructor detour");
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

    if (TPVToggle::player_transform().entity == entity)
    {
        DMK::Logger::get_instance().info("EntityHooks: Player entity being destroyed - Resetting pointer");
        TPVToggle::player_transform().entity = nullptr;
    }
}

bool initializeEntityHooks(uintptr_t moduleBase, size_t moduleSize)
{
    DMK::Logger &logger = DMK::Logger::get_instance();
    logger.info("EntityHooks: Initializing entity tracking hooks...");

    try
    {
        auto ctorPattern = DMK::Scanner::parse_aob(CENTITY_CONSTRUCTOR_CALLER_AOB);
        if (!ctorPattern.has_value())
        {
            throw std::runtime_error("Failed to parse CEntity constructor caller AOB");
        }

        const std::byte *ctorMatch = DMK::Scanner::find_pattern(reinterpret_cast<const std::byte *>(moduleBase), moduleSize, *ctorPattern);
        if (!ctorMatch)
        {
            throw std::runtime_error("CEntity constructor caller pattern not found");
        }

        // Resolve call rel32 target: E8 xx xx xx xx (5 bytes, disp32 at offset 1)
        auto constructorAddress = DMK::Scanner::resolve_rip_relative(ctorMatch, 1, 5);
        if (!constructorAddress.has_value())
        {
            throw std::runtime_error("Failed to resolve constructor call target");
        }

        logger.info("EntityHooks: CEntity constructor found at {}",
                    format_address(constructorAddress.value()));

        DMK::HookManager &hook_manager = DMK::HookManager::get_instance();

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

        logger.info("EntityHooks: CEntity constructor hook successfully installed");

        // Find SetWorldTM function for future use
        auto setWorldPattern = DMK::Scanner::parse_aob(CENTITY_SETWORLDTM_CALLER_AOB);
        if (setWorldPattern.has_value())
        {
            const std::byte *setWorldMatch = DMK::Scanner::find_pattern(reinterpret_cast<const std::byte *>(moduleBase), moduleSize, *setWorldPattern);
            if (setWorldMatch)
            {
                // Resolve call rel32 target: E8 xx xx xx xx (5 bytes, disp32 at offset 1)
                auto setWorldAddress = DMK::Scanner::resolve_rip_relative(setWorldMatch, 1, 5);
                if (setWorldAddress.has_value())
                {
                    TPVToggle::player_transform().setWorldTM = reinterpret_cast<CEntity_SetWorldTM_Func_t>(setWorldAddress.value());
                    logger.info("EntityHooks: SetWorldTM function found at {}",
                                format_address(setWorldAddress.value()));
                }
            }
            else
            {
                logger.warning("EntityHooks: SetWorldTM function not found - Feature limited");
            }
        }

        return true;
    }
    catch (const std::exception &e)
    {
        logger.error("EntityHooks: Initialization failed: {}", e.what());
        cleanupEntityHooks();
        return false;
    }
}

void cleanupEntityHooks()
{
    DMK::Logger &logger = DMK::Logger::get_instance();
    DMK::HookManager &hook_manager = DMK::HookManager::get_instance();

    if (!g_CEntityConstructorHookId.empty())
    {
        (void)hook_manager.remove_hook(g_CEntityConstructorHookId);
        g_CEntityConstructorHookId.clear();
        fpCEntityConstructorOriginal = nullptr;
        logger.info("EntityHooks: Constructor hook removed");
    }

    {
        std::lock_guard<std::mutex> lock(g_entityMutex);
        TPVToggle::player_transform().setWorldTM = nullptr;
        TPVToggle::player_transform().entity = nullptr;
    }

    logger.info("EntityHooks: Cleanup complete");
}

// Safe accessor function for other modules
GameStructures::CEntity *GetPlayerEntity()
{
    std::lock_guard<std::mutex> lock(g_entityMutex);
    return TPVToggle::player_transform().entity;
}

} // namespace TPVToggle
