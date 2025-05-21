#include "hook_manager.hpp"
#include <sstream> // For error_to_string
#include <expected>

// --- HookManager Class Implementation ---

HookManager &HookManager::getInstance()
{
    static HookManager instance;
    return instance;
}

HookManager::HookManager()
    : m_logger(Logger::getInstance())
{
    // Initialize the SafetyHook allocator.
    // Using the global allocator provided by SafetyHook.
    m_allocator = safetyhook::Allocator::global();
    if (!m_allocator)
    {
        // This is a critical failure.
        m_logger.log(LOG_ERROR, "HookManager: Failed to get SafetyHook global allocator!");
        // Consider throwing an exception or setting an error state to halt initialization.
        // For now, we'll log and subsequent hook creations will fail.
    }
    m_logger.log(LOG_INFO, "HookManager: Initialized");
}

HookManager::~HookManager()
{
    remove_all_hooks();
    m_logger.log(LOG_INFO, "HookManager: Shutdown complete, all hooks removed.");
}

std::string HookManager::error_to_string(const safetyhook::InlineHook::Error &err)
{
    std::ostringstream oss;
    // SafetyHook doesn't provide a direct string conversion for its errors,
    // so we map them manually.
    oss << "SafetyHook Error (type " << static_cast<int>(err.type) << "): ";
    switch (err.type)
    {
    case safetyhook::InlineHook::Error::BAD_ALLOCATION:
        oss << "Bad allocation (allocator error: " << static_cast<int>(err.allocator_error) << ")";
        break;
    case safetyhook::InlineHook::Error::FAILED_TO_DECODE_INSTRUCTION:
        oss << "Failed to decode instruction at " << format_address(reinterpret_cast<uintptr_t>(err.ip));
        break;
    case safetyhook::InlineHook::Error::SHORT_JUMP_IN_TRAMPOLINE:
        oss << "Short jump in trampoline at " << format_address(reinterpret_cast<uintptr_t>(err.ip));
        break;
    case safetyhook::InlineHook::Error::IP_RELATIVE_INSTRUCTION_OUT_OF_RANGE:
        oss << "IP-relative instruction out of range at " << format_address(reinterpret_cast<uintptr_t>(err.ip));
        break;
    case safetyhook::InlineHook::Error::UNSUPPORTED_INSTRUCTION_IN_TRAMPOLINE:
        oss << "Unsupported instruction in trampoline at " << format_address(reinterpret_cast<uintptr_t>(err.ip));
        break;
    case safetyhook::InlineHook::Error::FAILED_TO_UNPROTECT:
        oss << "Failed to unprotect memory at " << format_address(reinterpret_cast<uintptr_t>(err.ip));
        break;
    case safetyhook::InlineHook::Error::NOT_ENOUGH_SPACE:
        oss << "Not enough space for hook at " << format_address(reinterpret_cast<uintptr_t>(err.ip));
        break;
    default:
        oss << "Unknown SafetyHook::InlineHook::Error type.";
        break;
    }
    // For allocator errors, provide more details if possible (SafetyHook's Allocator::Error doesn't have public sub-types to switch on)
    if (err.type == safetyhook::InlineHook::Error::BAD_ALLOCATION)
    {
        oss << " Allocator specific error code: " << static_cast<int>(err.allocator_error);
    }
    return oss.str();
}

std::string HookManager::create_inline_hook(
    const std::string &name,
    uintptr_t target_address,
    void *detour_function,
    void **original_trampoline)
{
    if (!m_allocator)
    {
        m_logger.log(LOG_ERROR, "HookManager: Allocator not initialized. Cannot create hook '" + name + "'.");
        return "";
    }
    if (target_address == 0)
    {
        m_logger.log(LOG_ERROR, "HookManager: Attempted to hook NULL address for '" + name + "'.");
        return "";
    }
    if (detour_function == nullptr)
    {
        m_logger.log(LOG_ERROR, "HookManager: Detour function is NULL for '" + name + "'.");
        return "";
    }
    if (original_trampoline == nullptr)
    {
        m_logger.log(LOG_ERROR, "HookManager: Original trampoline pointer is NULL for '" + name + "'.");
        return "";
    }

    // Check if a hook with the same name already exists to prevent duplicates if name is used as ID
    for (const auto &existing_hook : m_hooks)
    {
        if (existing_hook.m_name == name)
        {
            m_logger.log(LOG_ERROR, "HookManager: Hook with name '" + name + "' already exists.");
            // To allow re-hooking, one might remove the old one first or use a different strategy.
            // For now, consider it an error to create with a duplicate name.
            return "";
        }
    }

    try
    {
        // Create the InlineHook using the specific allocator.
        // safetyhook::InlineHook::create returns a std::expected<InlineHook, Error>.
        // We pass the m_allocator obtained in HookManager's constructor.
        std::expected<safetyhook::InlineHook, safetyhook::InlineHook::Error> hook_result =
            safetyhook::InlineHook::create(
                m_allocator,
                reinterpret_cast<void *>(target_address),
                detour_function
                // You can add flags here if needed, e.g., safetyhook::InlineHook::Default
            );

        if (!hook_result)
        {
            // Creation failed, log the error.
            m_logger.log(LOG_ERROR, "HookManager: Failed to create SafetyHook::InlineHook for '" + name + "' at " +
                                        format_address(target_address) + ". Error: " + error_to_string(hook_result.error()));
            return "";
        }

        // Creation succeeded. Move the InlineHook from std::expected into a unique_ptr.
        // This transfers ownership to the unique_ptr.
        std::unique_ptr<safetyhook::InlineHook> sh_hook =
            std::make_unique<safetyhook::InlineHook>(std::move(hook_result.value()));

        // Store the trampoline.
        // The trampoline is a member of the InlineHook object.
        // Call original() as a template function with the desired type.
        *original_trampoline = sh_hook->original<void *>();

        // Add the new Hook object (which owns the unique_ptr to safetyhook::InlineHook) to our list.
        m_hooks.emplace_back(name, target_address, std::move(sh_hook));
        m_logger.log(LOG_INFO, "HookManager: Successfully created and enabled inline hook for '" + name + "' at " +
                                   format_address(target_address));
        return name; // Use name as ID.
    }
    catch (const std::exception &e) // Catch standard exceptions that might occur during make_unique etc.
    {
        m_logger.log(LOG_ERROR, "HookManager: std::exception during SafetyHook creation for '" + name + "': " + e.what());
        return "";
    }
    catch (...) // Catch any other unknown exceptions
    {
        m_logger.log(LOG_ERROR, "HookManager: Unknown exception during SafetyHook creation for '" + name + "'.");
        return "";
    }
}

std::string HookManager::create_inline_hook_aob(
    const std::string &name,
    uintptr_t module_base,
    size_t module_size,
    const std::string &aob_pattern_str,
    ptrdiff_t aob_offset,
    void *detour_function,
    void **original_trampoline)
{
    if (!m_allocator)
    {
        m_logger.log(LOG_ERROR, "HookManager: Allocator not initialized. Cannot create AOB hook '" + name + "'.");
        return "";
    }

    m_logger.log(LOG_DEBUG, "HookManager: Attempting AOB scan for hook '" + name + "' with pattern '" + aob_pattern_str + "'.");

    std::vector<BYTE> pattern_bytes = parseAOB(aob_pattern_str);
    if (pattern_bytes.empty())
    {
        m_logger.log(LOG_ERROR, "HookManager: Failed to parse AOB pattern '" + aob_pattern_str + "' for '" + name + "'.");
        return "";
    }

    BYTE *found_address = FindPattern(reinterpret_cast<BYTE *>(module_base), module_size, pattern_bytes);
    if (!found_address)
    {
        m_logger.log(LOG_ERROR, "HookManager: AOB pattern '" + aob_pattern_str + "' not found for '" + name + "'.");
        return "";
    }

    uintptr_t target_address = reinterpret_cast<uintptr_t>(found_address) + aob_offset;
    m_logger.log(LOG_INFO, "HookManager: AOB pattern for '" + name + "' found at " + format_address(reinterpret_cast<uintptr_t>(found_address)) +
                               ". Target hook address: " + format_address(target_address));

    return create_inline_hook(name, target_address, detour_function, original_trampoline);
}

bool HookManager::remove_hook(const std::string &hook_id)
{
    auto it = std::find_if(m_hooks.begin(), m_hooks.end(),
                           [&](const Hook &h)
                           { return h.m_name == hook_id; });

    if (it != m_hooks.end())
    {
        // Erasing the Hook object from m_hooks will trigger its destructor.
        // The Hook destructor will trigger the unique_ptr<safetyhook::InlineHook>'s destructor.
        // The safetyhook::InlineHook destructor will unhook and free resources.
        std::string name = it->m_name;
        m_hooks.erase(it); // This calls ~Hook(), which calls ~unique_ptr(), which calls ~InlineHook()
        m_logger.log(LOG_INFO, "HookManager: Hook '" + name + "' removed and unhooked.");
        return true;
    }
    m_logger.log(LOG_WARNING, "HookManager: Hook ID '" + hook_id + "' not found for removal.");
    return false;
}

void HookManager::remove_all_hooks()
{
    // Clearing the vector triggers destructors for all contained Hook objects.
    // Each Hook object's unique_ptr<safetyhook::InlineHook> will then be destroyed,
    // leading to the automatic unhooking by safetyhook::InlineHook's destructor.
    if (!m_hooks.empty())
    {
        m_logger.log(LOG_INFO, "HookManager: Removing all " + std::to_string(m_hooks.size()) + " managed hooks...");
        m_hooks.clear(); // This will call destructors in reverse order of construction for elements
        m_logger.log(LOG_INFO, "HookManager: All managed hooks have been removed.");
    }
    else
    {
        m_logger.log(LOG_DEBUG, "HookManager: remove_all_hooks called, but no hooks were active.");
    }
}
