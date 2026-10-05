/**
 * @file hooks/hook_set.cpp
 * @brief HookSet: newest-first disable, detour drain, and destruction of the mod's hooks.
 */

#include "hook_set.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace TPVCamera
{
    namespace
    {
        constexpr std::size_t CACHE_LINE = 64;
        constexpr std::size_t IN_FLIGHT_STRIPES = 32;

        /// One DetourScope count, padded to its own cache line.
        struct InFlightStripe
        {
            std::atomic<std::uint32_t> count{0};
            char padding[CACHE_LINE - sizeof(std::atomic<std::uint32_t>)]{};
        };
        static_assert(sizeof(InFlightStripe) == CACHE_LINE, "One stripe per cache line.");

        std::array<InFlightStripe, IN_FLIGHT_STRIPES> s_in_flight{};

        // Bounds the wait for a detour still running. The hooked camera, input, and UI functions return within one
        // frame, so a longer stay means a stalled game thread, and the reload retries later.
        constexpr DWORD DRAIN_TIMEOUT_MS = 5000;
        constexpr DWORD DRAIN_POLL_MS = 5;
        // A thread that took the patch jump just before the prologue restore has not counted itself yet. The drain
        // reads the count again after this settle time, so such a thread is counted or gone by the final read.
        constexpr DWORD ENTRY_WINDOW_MS = 50;

        /// Reports whether no thread is inside an inline detour.
        [[nodiscard]] bool detours_idle() noexcept
        {
            for (const InFlightStripe &stripe : s_in_flight)
            {
                if (stripe.count.load(std::memory_order_seq_cst) != 0)
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief Waits until no thread is inside an inline detour.
         * @return True when every stripe stays zero across the entry window before the deadline.
         */
        [[nodiscard]] bool drain_detours() noexcept
        {
            const ULONGLONG deadline = GetTickCount64() + DRAIN_TIMEOUT_MS;
            for (;;)
            {
                while (!detours_idle() && GetTickCount64() < deadline)
                {
                    Sleep(DRAIN_POLL_MS);
                }
                Sleep(ENTRY_WINDOW_MS);
                if (detours_idle())
                {
                    return true;
                }
                if (GetTickCount64() >= deadline)
                {
                    return false;
                }
            }
        }
    } // namespace

    // Windows thread ids are multiples of four, so the shift spreads consecutive ids across the stripes.
    DetourScope::DetourScope() noexcept : m_count(s_in_flight[(GetCurrentThreadId() >> 2) % IN_FLIGHT_STRIPES].count)
    {
        m_count.fetch_add(1, std::memory_order_seq_cst);
    }

    DetourScope::~DetourScope() noexcept
    {
        m_count.fetch_sub(1, std::memory_order_seq_cst);
    }

    HookSet::~HookSet() noexcept
    {
        destroy_all();
    }

    DMK::hook::Hook &HookSet::push(DMK::hook::Hook hook)
    {
        m_hooks.reserve(m_hooks.size() + 1);
        m_hooks.push_back(std::make_unique<DMK::hook::Hook>(std::move(hook)));
        return *m_hooks.back();
    }

    void HookSet::destroy_all() noexcept
    {
        while (!m_hooks.empty())
        {
            m_hooks.pop_back();
        }
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
            // BackendFailed disarms the hook but can leave page protection unrestored. is_enabled() reports whether the
            // detour stays reachable, which is the only fact the drain depends on.
            if (auto disabled = hook.disable(); !disabled.has_value() && hook.is_enabled())
            {
                all_disabled = false;
                (void)DMK::log().try_log(DMK::LogLevel::Error, "Hooks: {} could not be disabled ({})", hook.name(),
                                         DMK::to_string(disabled.error().code));
            }
        }
        return all_disabled;
    }

    RetireStatus HookSet::retire() noexcept
    {
        if (m_failed)
        {
            return RetireStatus::Failed;
        }
        if (m_hooks.empty())
        {
            return RetireStatus::Retired;
        }
        if (!disable_all())
        {
            m_failed = true;
            return RetireStatus::Failed;
        }
        if (!drain_detours())
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "Hooks: a game thread stayed inside a detour; the hooks stay disabled until a later retry");
            return RetireStatus::Busy;
        }
        namespace diag = DMK::diagnostics;
        const std::size_t pins_before = diag::intentional_leak_count(diag::LeakSubsystem::HookManager);
        destroy_all();
        if (diag::intentional_leak_count(diag::LeakSubsystem::HookManager) != pins_before)
        {
            m_failed = true;
            (void)DMK::log().try_log(
                DMK::LogLevel::Error,
                "Hooks: a hook could not restore its prologue and pinned its backend; the module must stay mapped");
            return RetireStatus::Failed;
        }
        return RetireStatus::Retired;
    }

} // namespace TPVCamera
