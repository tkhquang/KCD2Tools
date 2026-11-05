/**
 * @file fixes/inventory_refresh.cpp
 * @brief The C_Inventory::Clear and C_InventorySoul refresh detours (see fixes.hpp).
 *
 * One thread at a time owns the deferral: the first clear on a thread claims it, a nested clear on that thread joins
 * it, and a clear on another thread meanwhile runs unchanged. The owner flushes the recorded refreshes when its
 * outermost clear returns. The refreshed objects are inventory components of souls whose inventory the clear empties,
 * and they outlive the clear, because a soul clears its inventory before it releases the component.
 */

#include "config.hpp"
#include "fixes/diag.hpp"
#include "fixes/fixes.hpp"
#include "hooks/hook_install.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace EngineTweaks::fixes
{
    namespace
    {
        using ClearFunc = std::uintptr_t(__fastcall *)(void *inventory);
        using RefreshFunc = std::uintptr_t(__fastcall *)(void *inventory_soul);

        std::atomic<ClearFunc> s_clear_original{nullptr};
        std::atomic<RefreshFunc> s_refresh_original{nullptr};

        /** @brief The thread that owns the deferral, 0 when none. Only the owner touches the fields below. */
        std::atomic<DWORD> s_owner{0};
        int s_depth{0};
        /// True while the owner runs the recorded refreshes.
        bool s_flushing{false};
        /** @brief Distinct objects with a deferred refresh. A clear touches one soul's component, so a few suffice. */
        constexpr std::size_t MAX_PENDING = 8;
        std::array<void *, MAX_PENDING> s_pending{};
        std::size_t s_pending_count{0};
        std::size_t s_deferred_calls{0};

        /** @brief Records a refresh of @p inventory_soul. False when the slots are full, so it must run now. */
        [[nodiscard]] bool defer(void *inventory_soul) noexcept
        {
            ++s_deferred_calls;
            for (std::size_t i = 0; i < s_pending_count; ++i)
            {
                if (s_pending[i] == inventory_soul)
                {
                    return true;
                }
            }
            if (s_pending_count == s_pending.size())
            {
                --s_deferred_calls;
                diag::counters().clear_overflows.add();
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "InventoryRefresh: {} pending slots full, the refresh of {} runs now",
                    MAX_PENDING,
                    inventory_soul
                );
                return false;
            }
            s_pending[s_pending_count++] = inventory_soul;
            return true;
        }

        void flush(RefreshFunc refresh) noexcept
        {
            s_flushing = true;
            for (std::size_t i = 0; i < s_pending_count; ++i)
            {
                refresh(s_pending[i]);
                s_pending[i] = nullptr;
            }
            s_pending_count = 0;
            s_flushing = false;
        }

        /**
         * @brief The clear detour: the game's clear with the item-flag refreshes deferred, then one refresh each.
         * @note Any thread that clears an inventory. No-throw.
         */
        std::uintptr_t __fastcall detour_clear(void *inventory) noexcept
        {
            const ClearFunc original = s_clear_original.load(std::memory_order_acquire);
            if (!fix_enabled(Fix::InventoryRefresh))
            {
                (void)DMK::log()
                    .try_log(DMK::LogLevel::Trace, "InventoryRefresh: clear of {} passes through (fix off)", inventory);
                return original(inventory);
            }
            const DWORD self = GetCurrentThreadId();
            DWORD owner = s_owner.load(std::memory_order_acquire);
            if (owner != self)
            {
                owner = 0;
                if (!s_owner.compare_exchange_strong(owner, self, std::memory_order_acq_rel))
                {
                    diag::counters().clear_contended.add();
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Debug,
                        "InventoryRefresh: clear of {} on thread {} runs unchanged, thread {} owns the deferral",
                        inventory,
                        self,
                        owner
                    );
                    return original(inventory);
                }
            }
            else if (s_flushing)
            {
                // A clear that a flushed refresh starts runs as in the game, so the batch under flush stays intact.
                return original(inventory);
            }
            const diag::Stopwatch watch;
            ++s_depth;
            const std::uintptr_t result = original(inventory);
            if (--s_depth != 0)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Trace,
                    "InventoryRefresh: nested clear of {} returned, depth {}",
                    inventory,
                    s_depth
                );
                return result;
            }
            const std::size_t deferred = s_deferred_calls;
            const std::size_t pending = s_pending_count;
            s_deferred_calls = 0;
            flush(s_refresh_original.load(std::memory_order_acquire));
            s_owner.store(0, std::memory_order_release);
            diag::counters().clears.add();
            diag::counters().clear_refreshes_deferred.add(deferred);
            (void)DMK::log().try_log(
                deferred != 0 ? DMK::LogLevel::Debug : DMK::LogLevel::Trace,
                "InventoryRefresh: clear of {} on thread {}: {} refresh(es) deferred, {} run at the end, {:.2f} ms",
                inventory,
                self,
                deferred,
                pending,
                watch.elapsed_ms()
            );
            diag::maybe_log_summary();
            return result;
        }

        /**
         * @brief The refresh detour: recorded inside the owner's clear, run at once everywhere else.
         * @return 0 for a deferred call. The caller is the item-removed listener, which discards the value.
         * @note Any thread. No-throw.
         */
        std::uintptr_t __fastcall detour_refresh(void *inventory_soul) noexcept
        {
            const RefreshFunc original = s_refresh_original.load(std::memory_order_acquire);
            // The owner check comes first: only the owner thread reads the plain fields.
            if (s_owner.load(std::memory_order_acquire) == GetCurrentThreadId() && s_depth > 0 && defer(inventory_soul))
            {
                return 0;
            }
            return original(inventory_soul);
        }
    } // namespace

    DMK::Result<void> install_inventory_refresh(const Anchors &anchors, DMK::hook::HookStack &hooks)
    {
        if (!anchors.inventory_refresh)
        {
            return {};
        }
        // The refresh hook goes first, so the clear never defers into a refresh that has no trampoline yet.
        DMK_TRY_VOID(
            install_inline_hook<&detour_refresh>(
                hooks,
                "InventorySoulRefresh",
                anchors.inventory_soul_refresh,
                s_refresh_original
            )
        );
        return install_inline_hook<&detour_clear>(hooks, "InventoryClear", anchors.inventory_clear, s_clear_original);
    }
} // namespace EngineTweaks::fixes
