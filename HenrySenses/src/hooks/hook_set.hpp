/**
 * @file hooks/hook_set.hpp
 * @brief Owner of the mod's inline hooks, torn down in three steps: disable, drain, destroy.
 */
#ifndef HENRYSENSES_HOOK_SET_HPP
#define HENRYSENSES_HOOK_SET_HPP

#include <DetourModKit/hook.hpp>

#include <cstddef>
#include <memory>
#include <vector>

namespace HenrySenses
{
    /**
     * @class HookSet
     * @brief Immovable owner of the mod's inline hooks with newest-first disable and destruction.
     * @details Destroying an inline hook restores its prologue and frees its trampoline in one step, and DMK does not
     *          count threads inside an inline detour. A thread that took the patch jump just before the restore still
     *          calls the original through that trampoline, so teardown disables every hook first (prologue restored,
     *          trampoline kept), drains the detours, and only then destroys the handles. Each handle has its own
     *          allocation, so the reference @ref push returns stays valid until @ref clear.
     * @note Setup/control-plane only, like the hooks it holds: build and tear it down on the init and teardown
     *       threads, never from a detour.
     */
    class HookSet
    {
    public:
        HookSet() noexcept = default;

        /**
         * @brief Destroys every owned hook, newest first.
         * @note Runs at static destruction only when a teardown kept its hooks; each ~Hook fails closed there.
         */
        ~HookSet() noexcept;

        HookSet(const HookSet &) = delete;
        HookSet &operator=(const HookSet &) = delete;
        HookSet(HookSet &&) = delete;
        HookSet &operator=(HookSet &&) = delete;

        /**
         * @brief Stores @p hook on top of the set and returns the stored handle.
         * @param hook A freshly created hook. Store it before arming it, so a failed allocation never unwinds an
         *        armed hook.
         * @return The stored handle, valid until @ref clear.
         * @note May throw std::bad_alloc when the set cannot grow; @p hook is then destroyed unarmed by the unwinding.
         */
        [[nodiscard]] DMK::hook::Hook &push(DMK::hook::Hook hook);

        /**
         * @brief Disables every owned hook, newest first, keeping each trampoline allocated.
         * @return True when every hook reports disabled; false leaves at least one detour reachable.
         */
        [[nodiscard]] bool disable_all() noexcept;

        /**
         * @brief Destroys every owned hook, newest first, leaving the set empty.
         * @note Call only after the detours drained: destruction frees the trampolines a thread still inside a
         *       detour would return through.
         */
        void clear() noexcept;

        /// Reports whether the set owns no hooks.
        [[nodiscard]] bool empty() const noexcept { return m_hooks.empty(); }

        /// Returns the number of owned hooks.
        [[nodiscard]] std::size_t size() const noexcept { return m_hooks.size(); }

    private:
        std::vector<std::unique_ptr<DMK::hook::Hook>> m_hooks;
    };

} // namespace HenrySenses

#endif // HENRYSENSES_HOOK_SET_HPP
