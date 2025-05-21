#ifndef HOOK_MANAGER_H
#define HOOK_MANAGER_H

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <memory>         // For std::unique_ptr
#include <expected>       // For std::expected
#include "safetyhook.hpp" // Main SafetyHook header
#include "logger.h"
#include "aob_scanner.h" // For AOB scanning within HookManager
#include "utils.h"       // For format_address etc.

// Forward declaration
class Logger;

/**
 * @class Hook
 * @brief Represents a single managed hook.
 *
 * Encapsulates the SafetyHook object and metadata for a hook.
 * The safetyhook::InlineHook object itself manages the hook's lifetime (RAII).
 */
class Hook
{
public:
    // A unique_ptr to own the SafetyHook object.
    // When this Hook object is destroyed, the InlineHook unique_ptr is reset,
    // triggering the InlineHook's destructor, which unhooks.
    std::unique_ptr<safetyhook::InlineHook> m_safety_hook_inline;
    // Add std::unique_ptr<safetyhook::MidHook> m_safety_hook_mid; if you plan to use MidHooks

    std::string m_name;         // User-friendly name for logging
    uintptr_t m_target_address; // The address that was hooked

    Hook(std::string name, uintptr_t target_address, std::unique_ptr<safetyhook::InlineHook> hook_obj)
        : m_name(std::move(name)), m_target_address(target_address), m_safety_hook_inline(std::move(hook_obj)) {}

    // Prevent copying
    Hook(const Hook &) = delete;
    Hook &operator=(const Hook &) = delete;

    // Allow moving
    Hook(Hook &&other) noexcept = default;
    Hook &operator=(Hook &&other) noexcept = default;

    // Destructor: m_safety_hook_inline's destructor will be called, which
    // in turn calls the safetyhook::InlineHook destructor, performing cleanup.
    ~Hook() = default;
};

/**
 * @class HookManager
 * @brief Manages the lifecycle of all SafetyHook hooks.
 *
 * Provides a centralized way to create, enable, disable, and remove hooks.
 * This class is a singleton.
 */
class HookManager
{
public:
    // Singleton access
    static HookManager &getInstance();

    // --- Hook Creation ---
    /**
     * @brief Creates an inline hook.
     * @param name A descriptive name for the hook (for logging).
     * @param target_address The address to hook.
     * @param detour_function The function to call instead of the original.
     * @param original_trampoline Pointer to store the trampoline for calling the original function.
     *                            The trampoline is owned by the SafetyHook object.
     * @return A unique ID for the hook (currently the 'name') if successful, or an empty string on failure.
     */
    std::string create_inline_hook(
        const std::string &name,
        uintptr_t target_address,
        void *detour_function,
        void **original_trampoline);

    /**
     * @brief Creates an inline hook by first finding the target address using AOB scanning.
     * @param name A descriptive name for the hook.
     * @param module_base Base address of the module to scan.
     * @param module_size Size of the module to scan.
     * @param aob_pattern The AOB pattern string to find the target.
     * @param aob_offset An optional offset to add to the AOB match to get the final target_address.
     * @param detour_function The function to call instead of the original.
     * @param original_trampoline Pointer to store the trampoline for calling the original function.
     * @return A unique ID for the hook (currently the 'name') if successful, or an empty string on failure.
     */
    std::string create_inline_hook_aob(
        const std::string &name,
        uintptr_t module_base,
        size_t module_size,
        const std::string &aob_pattern_str,
        ptrdiff_t aob_offset,
        void *detour_function,
        void **original_trampoline);

    // --- Hook Management ---
    /**
     * @brief Removes a hook by its ID.
     * @param hook_id The ID (name) of the hook to remove.
     * @return true if the hook was found and removed, false otherwise.
     */
    bool remove_hook(const std::string &hook_id);

    /**
     * @brief Removes all managed hooks.
     *        This is automatically called by the HookManager's destructor.
     */
    void remove_all_hooks();

private:
    HookManager();  // Private constructor
    ~HookManager(); // Destructor will clean up all hooks

    // Delete copy/assignment
    HookManager(const HookManager &) = delete;
    HookManager &operator=(const HookManager &) = delete;

    // Member variables
    std::vector<Hook> m_hooks;                          // Stores active hooks. Order might matter for unhooking if dependent.
    Logger &m_logger;                                   // Reference to the logger
    std::shared_ptr<safetyhook::Allocator> m_allocator; // Allocator for SafetyHook

    // Helper to convert SafetyHook error to string
    std::string error_to_string(const safetyhook::InlineHook::Error &err);
};

#endif // HOOK_MANAGER_H
