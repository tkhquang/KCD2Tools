/**
 * @file hooks/hook_set.cpp
 * @brief HookSet: newest-first disable and destruction of the mod's inline hooks.
 */

#include "hooks/hook_set.hpp"

#include <DetourModKit.hpp>

#include <utility>

namespace HenrySenses
{
    HookSet::~HookSet() noexcept
    {
        clear();
    }

    DMK::hook::Hook &HookSet::push(DMK::hook::Hook hook)
    {
        m_hooks.reserve(m_hooks.size() + 1);
        m_hooks.push_back(std::make_unique<DMK::hook::Hook>(std::move(hook)));
        return *m_hooks.back();
    }

    bool HookSet::disable_all() noexcept
    {
        bool all_disabled = true;
        for (auto it = m_hooks.rbegin(); it != m_hooks.rend(); ++it)
        {
            DMK::hook::Hook &hook = **it;
            if (!hook)
            {
                continue;
            }
            if (auto disabled = hook.disable(); !disabled.has_value())
            {
                all_disabled = false;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Error,
                    "HookSet: hook {} could not be disabled ({})",
                    hook.name(),
                    DMK::to_string(disabled.error().code)
                );
            }
        }
        return all_disabled;
    }

    void HookSet::clear() noexcept
    {
        while (!m_hooks.empty())
        {
            m_hooks.pop_back();
        }
    }

} // namespace HenrySenses
