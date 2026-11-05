/**
 * @file fixes/perk_copy.cpp
 * @brief The perk container copy detour (see fixes.hpp).
 *
 * The game's copy runs an unlock pass after every owned perk and extra definition it adds, which does nothing while
 * the destination has no progression block, and one more pass when the source has a progression block (the copy then
 * gives the destination one). A pass unlocks the definitions whose skill condition holds and whose prerequisite is an
 * owned perk, and the copy changes neither the skills nor, after its first list, the owned perks. So the passes end on
 * the unlocked set that one pass after the copy reaches, and when none of them ran the copy unlocks nothing. Passes
 * repeat until one unlocks nothing, so the second one confirms the first.
 */

#include "config.hpp"
#include "constants.hpp"
#include "fixes/diag.hpp"
#include "fixes/fixes.hpp"
#include "hooks/hook_install.hpp"

#include <DetourModKit.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace EngineTweaks::fixes
{
    namespace
    {
        using PerkCopyFunc = std::uintptr_t(__fastcall *)(void *destination, const void *source);
        /// Returns the byte the pass ORs over its unlock results, nonzero when it unlocked a perk.
        using AutoUnlockFunc = std::uint8_t(__fastcall *)(void *perks);

        std::atomic<PerkCopyFunc> s_original{nullptr};
        std::atomic<AutoUnlockFunc> s_auto_unlock{nullptr};

        /** @brief Passes after a copy. One unlocks everything the copy allows, and the second finds nothing. */
        constexpr int MAX_UNLOCK_PASSES = 8;

        [[nodiscard]] std::uint8_t *flags_of(void *perks) noexcept
        {
            return static_cast<std::uint8_t *>(perks) + constants::PERKS_FLAGS_OFFSET;
        }

        [[nodiscard]] std::uintptr_t word_at(const void *perks, std::ptrdiff_t offset) noexcept
        {
            return *reinterpret_cast<const std::uintptr_t *>(static_cast<const std::uint8_t *>(perks) + offset);
        }

        [[nodiscard]] bool has_progression(const void *perks) noexcept
        {
            return word_at(perks, constants::PERKS_PROGRESSION_OFFSET) != 0;
        }

        [[nodiscard]] bool list_empty(const void *perks, std::ptrdiff_t begin, std::ptrdiff_t end) noexcept
        {
            return word_at(perks, begin) == word_at(perks, end);
        }

        /** @brief The byte length of a begin/end list, for the log. */
        [[nodiscard]] std::uintptr_t list_bytes(const void *perks, std::ptrdiff_t begin, std::ptrdiff_t end) noexcept
        {
            return word_at(perks, end) - word_at(perks, begin);
        }

        /**
         * @brief True when the game's copy runs an unlock pass that can unlock a perk.
         * @param destination_had_progression The destination's progression block before the copy, which its lists
         *        are copied under.
         */
        [[nodiscard]] bool copy_runs_a_pass(
            const void *destination,
            bool destination_had_progression,
            bool source_has_progression
        ) noexcept
        {
            if (source_has_progression)
            {
                return true;
            }
            // The copy empties both lists first, so a nonempty one means at least one add, and so one pass.
            return destination_had_progression &&
                   (!list_empty(destination, constants::PERKS_OWNED_BEGIN_OFFSET, constants::PERKS_OWNED_END_OFFSET) ||
                    !list_empty(
                        destination,
                        constants::PERKS_EXTRA_DEFS_BEGIN_OFFSET,
                        constants::PERKS_EXTRA_DEFS_END_OFFSET
                    ));
        }

        /**
         * @brief The copy detour: the game's copy with the auto-unlock pass suspended, then passes to a fixpoint.
         * @note The thread that copies a soul. No-throw.
         */
        std::uintptr_t __fastcall detour_perk_copy(void *destination, const void *source) noexcept
        {
            const PerkCopyFunc original = s_original.load(std::memory_order_acquire);
            // A copy into a container whose pass is already suspended belongs to code that runs the pass itself.
            const char *pass_through = nullptr;
            if (destination == nullptr || source == nullptr)
            {
                pass_through = "no container";
            }
            else if (!fix_enabled(Fix::PerkCopy))
            {
                pass_through = "fix off";
            }
            else if ((*flags_of(destination) & constants::PERKS_SUSPEND_AUTO_UNLOCK) != 0)
            {
                pass_through = "unlock pass already suspended";
            }
            if (pass_through != nullptr)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Trace,
                    "PerkCopy: copy {} -> {} passes through ({})",
                    source,
                    static_cast<const void *>(destination),
                    pass_through
                );
                return original(destination, source);
            }
            const diag::Stopwatch watch;
            const bool destination_had_progression = has_progression(destination);
            const bool source_has_progression = has_progression(source);
            *flags_of(destination) |= constants::PERKS_SUSPEND_AUTO_UNLOCK;
            const std::uintptr_t result = original(destination, source);
            // Clear only the suspend bit: the copy can change the others.
            *flags_of(destination) &= static_cast<std::uint8_t>(~constants::PERKS_SUSPEND_AUTO_UNLOCK);
            int passes = 0;
            const bool runs_a_pass = copy_runs_a_pass(destination, destination_had_progression, source_has_progression);
            if (runs_a_pass)
            {
                const AutoUnlockFunc auto_unlock = s_auto_unlock.load(std::memory_order_acquire);
                while (passes < MAX_UNLOCK_PASSES && auto_unlock(destination) != 0)
                {
                    ++passes;
                }
            }
            diag::Counters &counters = diag::counters();
            counters.perk_copies.add();
            counters.perk_passes.add(static_cast<std::uint64_t>(passes));
            if (passes != 0)
            {
                counters.perk_copies_unlocking.add();
            }
            if (passes == MAX_UNLOCK_PASSES)
            {
                counters.perk_pass_limits.add();
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "PerkCopy: copy {} -> {} still unlocked perks in pass {}, so the passes stopped there",
                    source,
                    static_cast<const void *>(destination),
                    MAX_UNLOCK_PASSES
                );
            }
            (void)DMK::log().try_log(
                passes != 0 ? DMK::LogLevel::Debug : DMK::LogLevel::Trace,
                "PerkCopy: copy {} -> {}: progression {}/{} (destination before/source), owned {} B, extra {} B, "
                "pass {}, {} unlocking pass(es), flags 0x{:02X}, {:.3f} ms",
                source,
                static_cast<const void *>(destination),
                destination_had_progression,
                source_has_progression,
                list_bytes(destination, constants::PERKS_OWNED_BEGIN_OFFSET, constants::PERKS_OWNED_END_OFFSET),
                list_bytes(
                    destination,
                    constants::PERKS_EXTRA_DEFS_BEGIN_OFFSET,
                    constants::PERKS_EXTRA_DEFS_END_OFFSET
                ),
                runs_a_pass ? "run" : "skipped as in the game's copy",
                passes,
                *flags_of(destination),
                watch.elapsed_ms()
            );
            diag::maybe_log_summary();
            return result;
        }
    } // namespace

    DMK::Result<void> install_perk_copy(const Anchors &anchors, DMK::hook::HookStack &hooks)
    {
        if (!anchors.perk_copy_fix)
        {
            return {};
        }
        s_auto_unlock.store(reinterpret_cast<AutoUnlockFunc>(anchors.perk_auto_unlock), std::memory_order_release);
        return install_inline_hook<&detour_perk_copy>(hooks, "PerkCopy", anchors.perk_copy, s_original);
    }
} // namespace EngineTweaks::fixes
