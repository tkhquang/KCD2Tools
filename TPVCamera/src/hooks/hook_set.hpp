/**
 * @file hooks/hook_set.hpp
 * @brief Owner of the mod's hooks, retired in three steps: disable, drain, destroy.
 */
#ifndef TPVCAMERA_HOOK_SET_HPP
#define TPVCAMERA_HOOK_SET_HPP

#include <DetourModKit/hook.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace TPVCamera
{

    /**
     * @class DetourScope
     * @brief Counts one thread inside an inline detour body for HookSet::retire().
     * @details DMK does not count threads inside an inline detour (the inline_at quiescence contract), and a game
     *          thread cannot be joined. Each inline detour declares one scope as its first statement, so the retire
     *          drain can prove that no game thread still executes detour code or returns through a trampoline.
     *          Mid hooks need no scope: DMK owns the rundown of mid callbacks. The count is striped by thread id,
     *          because the IsThirdPerson and movement detours run for every actor on every job worker, and one
     *          shared counter moves one cache line between those cores many times a frame.
     * @note Callback-safe: one atomic increment and one atomic decrement on the calling thread's stripe.
     */
    class DetourScope
    {
    public:
        DetourScope() noexcept;
        ~DetourScope() noexcept;

        DetourScope(const DetourScope &) = delete;
        DetourScope &operator=(const DetourScope &) = delete;
        DetourScope(DetourScope &&) = delete;
        DetourScope &operator=(DetourScope &&) = delete;

    private:
        std::atomic<std::uint32_t> &m_count;
    };

    /** @brief The outcome of HookSet::retire(). */
    enum class RetireStatus : std::uint8_t
    {
        /// Every hook restored its target and was destroyed.
        Retired,
        /// A detour stayed busy past the drain deadline. The hooks stay disabled with their trampolines allocated, and
        /// a later retire() can finish the work.
        Busy,
        /// A hook failed to disable or restore its target. Its detour can stay reachable, so the image must stay mapped
        /// for the process lifetime.
        Failed,
    };

    /**
     * @class HookSet
     * @brief Immovable owner of the mod's hooks with newest-first disable and destruction.
     * @details Destruction of an inline hook restores its prologue and frees its trampoline in one step. A thread that
     *          took the patch jump just before the restore still calls the original through that trampoline, so
     *          retire() disables every hook first (prologue restored, trampoline kept), drains the DetourScope count,
     *          and only then destroys the handles. Each handle has its own allocation, so the reference that push()
     *          returns stays valid until the handle is destroyed.
     * @note Setup/control-plane only: build and retire it on the init and teardown threads, never from a detour.
     */
    class HookSet
    {
    public:
        HookSet() noexcept = default;

        /**
         * @brief Destroys every owned hook, newest first.
         * @note Runs only at static destruction after a retire() that kept its hooks. Each ~Hook fails closed there.
         */
        ~HookSet() noexcept;

        HookSet(const HookSet &) = delete;
        HookSet &operator=(const HookSet &) = delete;
        HookSet(HookSet &&) = delete;
        HookSet &operator=(HookSet &&) = delete;

        /**
         * @brief Stores @p hook on top of the set and returns the stored handle.
         * @param hook A freshly created, disabled hook. Store it before enable(), so the set owns it through teardown
         *        even when the arm fails with the patch live.
         * @return The stored handle, valid until retire() destroys it.
         * @throws std::bad_alloc If the set cannot grow. The unwind then destroys @p hook unarmed.
         */
        [[nodiscard]] DMK::hook::Hook &push(DMK::hook::Hook hook);

        /**
         * @brief Disables every hook, drains the detours, and destroys the hooks.
         * @return Retired when the set is empty with every target restored. Busy and Failed keep the remaining hooks.
         * @details A HookManager leak delta across the destruction reports a hook that pinned its backend over a target
         *          that it failed to restore. Failed is permanent: a later call returns Failed without a change.
         */
        [[nodiscard]] RetireStatus retire() noexcept;

        /// Reports whether the set owns no hooks.
        [[nodiscard]] bool empty() const noexcept { return m_hooks.empty(); }

    private:
        /// Destroys every owned hook, newest first, so a layered target restores its newest patch first.
        void destroy_all() noexcept;

        /// Disables every owned hook, newest first. False leaves at least one detour reachable.
        [[nodiscard]] bool disable_all() noexcept;

        std::vector<std::unique_ptr<DMK::hook::Hook>> m_hooks;
        bool m_failed = false;
    };

} // namespace TPVCamera

#endif // TPVCAMERA_HOOK_SET_HPP
