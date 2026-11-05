/**
 * @file anchors.cpp
 * @brief The anchor rungs, their resolution through the unwind table, and the fix gates.
 *
 * Rung kinds:
 * - Entry: the bytes at the function entry. The match must be the start of a primary unwind entry.
 * - Body: a sequence inside the function. The function is the primary unwind entry that covers the `|` mark (or the
 *   match), so a body rung needs no build-specific distance to the entry.
 * - Call: a call site in a caller. The `|` mark is the E8 rel32, and its target must be a function entry.
 * - Slot: a slot of an RTTI-named vtable, which must be a function entry.
 *
 * A byte rung counts only when it matches exactly once among the image's executable pages. Every rung that resolves
 * must name the same function, and a disagreement fails the anchor. Signature rules: every relative target, RIP
 * displacement and stack offset that a recompile can move is a wildcard, except the layouts a fix reads, which a
 * rung spells out on purpose so a build that moves them fails the fix.
 */

#include "anchors.hpp"
#include "constants.hpp"

#include <windows.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace EngineTweaks
{
    namespace
    {
        using DMK::scan::Pages;
        using DMK::scan::Pattern;

        /** @brief How a rung names its function (see the file comment). */
        enum class RungKind : std::uint8_t
        {
            Entry,
            Body,
            Call,
            Slot,
        };

        /** @brief One rung. A layout rung proves offsets its fix reads, so the fix needs it to resolve. */
        struct Rung
        {
            std::string_view name;
            RungKind kind{RungKind::Entry};
            const Pattern *pattern{nullptr};
            std::string_view rtti{};
            std::size_t slot{0};
            bool layout{false};
        };

        /**
         * @brief C_UIFullUIModeHelper's switch into and out of full-screen UI mode.
         * @details P2 is the e_ViewDistRatio save (helper +0x08, cvar +0x60, GetFVal +0x20, saved at +0x94) and its
         *          Set(int 100) (+0x38). P3 is the restore on exit, Set(float) (+0x40) of the saved value.
         */
        const Pattern FULL_UI_P1 = Pattern::literal(
            "40 56 48 83 EC 60 48 8B F1 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 ?? ?? ?? ?? 48 8B C8 48 8B 10 "
            "4C 8B 42 ?? 0F B6 16 41 FF D0 80 3E 00 0F 84"
        );
        const Pattern FULL_UI_P2 = Pattern::literal(
            "48 8B 46 08 48 8B 48 60 48 8B 01 FF 50 20 48 8B 46 08 BA 64 00 00 00 F3 0F 11 80 94 00 00 00 "
            "48 8B 46 08 48 8B 48 60 48 8B 01 FF 50 38"
        );
        const Pattern FULL_UI_P3 =
            Pattern::literal("48 8B 56 08 48 8B 4A 60 F3 0F 10 8A 94 00 00 00 48 8B 01 FF 50 40");
        const Rung FULL_UI_RUNGS[] = {
            {
                .name = "P1_Entry",
                .kind = RungKind::Entry,
                .pattern = &FULL_UI_P1,
            },
            {
                .name = "P2_ViewDistSave",
                .kind = RungKind::Body,
                .pattern = &FULL_UI_P2,
                .layout = true,
            },
            {
                .name = "P3_ViewDistRestore",
                .kind = RungKind::Body,
                .pattern = &FULL_UI_P3,
                .layout = true,
            },
        };

        /**
         * @brief C_LinkablesManager's unlink: an erase_if over the whole link map that tests key.from (+0x20) and
         *        key.to (+0x28) of every node.
         * @details P2's mark is the `cmp byte [rip+disp], 0` of the first-only flag.
         */
        const Pattern UNLINK_P1 = Pattern::literal(
            "48 89 5C 24 10 57 48 83 EC 20 48 8B 01 48 8B DA 48 8B F9 48 8B 00 48 89 44 24 30 [0-8] "
            "48 3B 07 74 ?? 48 39 58 20 74 ?? 48 39 58 28 74 ??"
        );
        const Pattern UNLINK_P2 = Pattern::literal(
            "48 39 58 20 74 ?? 48 39 58 28 74 ?? 48 8D 4C 24 30 E8 ?? ?? ?? ?? 48 8B 44 24 30 EB ?? "
            "48 8B D0 48 8B CF E8 ?? ?? ?? ?? | 80 3D ?? ?? ?? ?? 00"
        );
        const Pattern UNLINK_P3 = Pattern::literal("48 8D 8B 48 01 00 00 48 8B D0 | E8 ?? ?? ?? ?? 48 8B D7 48 8B CB");
        const Rung UNLINK_RUNGS[] = {
            {
                .name = "P1_Entry",
                .kind = RungKind::Entry,
                .pattern = &UNLINK_P1,
                .layout = true,
            },
            {
                .name = "P2_FirstOnlyFlag",
                .kind = RungKind::Body,
                .pattern = &UNLINK_P2,
                .layout = true,
            },
            {
                .name = "P3_CallFromUnregister",
                .kind = RungKind::Call,
                .pattern = &UNLINK_P3,
            },
        };

        /**
         * @brief The link map's erase(node).
         * @details Its only callers are the unlink above and the map's erase(range).
         */
        const Pattern ERASE_P1 = Pattern::literal(
            "40 53 48 83 EC 20 4C 8B D9 48 89 54 24 30 48 8D 4C 24 30 4C 8B D2 E8 ?? ?? ?? ?? 49 8B D2 49 8B CB "
            "E8 ?? ?? ?? ?? 48 8B D8 48 8D 48 50 E8 ?? ?? ?? ?? 48 8B 4B 38 48 85 C9"
        );
        const Pattern ERASE_P2 =
            Pattern::literal("48 8B 44 24 30 EB ?? 48 8B D0 48 8B CF | E8 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00");
        const Rung ERASE_RUNGS[] = {
            {
                .name = "P1_Entry",
                .kind = RungKind::Entry,
                .pattern = &ERASE_P1,
            },
            {
                .name = "P2_CallFromUnlink",
                .kind = RungKind::Call,
                .pattern = &ERASE_P2,
            },
        };

        /// The link map's try_emplace: 0x70-byte nodes, so max_size() is 0x0249249249249249.
        const Pattern INSERT_P1 = Pattern::literal(
            "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 60 48 8B FA 48 8B F1 48 8D 54 24 40 E8 ?? ?? ?? ?? "
            "48 8B 54 24 50 E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? 48 B8 49 92 24 49 92 24 49 02"
        );
        const Pattern INSERT_P2 =
            Pattern::literal("49 8D 4B 08 4C 8B C3 48 8D 54 24 20 | E8 ?? ?? ?? ?? 48 8B 08 48 83 C1 38");
        const Rung INSERT_RUNGS[] = {
            {
                .name = "P1_Entry",
                .kind = RungKind::Entry,
                .pattern = &INSERT_P1,
            },
            {
                .name = "P2_CallFromAddLink",
                .kind = RungKind::Call,
                .pattern = &INSERT_P2,
            },
        };

        /// C_Inventory::Clear: removes every item (+0x20 list), then every sub-object (+0x08 list).
        const Pattern CLEAR_P1 = Pattern::literal(
            "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B 57 20 48 3B 57 28 74 ?? 48 8B 12 45 33 C9 45 33 C0 "
            "48 8B CF E8 ?? ?? ?? ?? EB ?? 48 8B 5F 10 48 39 5F 08 74 ?? 48 8B 5B F8"
        );
        const Pattern CLEAR_P2 =
            Pattern::literal("48 8B F0 48 85 C9 74 ?? 48 85 C0 74 ?? | E8 ?? ?? ?? ?? 48 8B 93 ?? ?? ?? ?? 41");
        const Rung CLEAR_RUNGS[] = {
            {
                .name = "P1_Entry",
                .kind = RungKind::Entry,
                .pattern = &CLEAR_P1,
            },
            {
                .name = "P2_CallFromInventoryCopy",
                .kind = RungKind::Call,
                .pattern = &CLEAR_P2,
            },
        };

        /// C_InventorySoul's refresh of the soul's inventory-wide item flags.
        const Pattern REFRESH_P2 = Pattern::literal(
            "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 48 8D 68 A1 48 81 EC A0 00 00 00 48 8B 05 ?? ?? ?? ?? "
            "48 33 C4 48 89 45 47 48 8B F9 33 D2 48 8B 49 18 E8 ?? ?? ?? ?? 48 89 45 DF 48 85 C0"
        );
        const Rung REFRESH_RUNGS[] = {
            {
                .name = "P1_Slot",
                .kind = RungKind::Slot,
                .rtti = constants::RTTI_INVENTORY_SOUL,
                .slot = constants::INVENTORY_SOUL_REFRESH_SLOT,
            },
            {
                .name = "P2_Entry",
                .kind = RungKind::Entry,
                .pattern = &REFRESH_P2,
            },
        };

        /// The perk container copy: AddPerk for each perk of the source's +0x40 list, then the other two lists.
        const Pattern PERK_COPY_P1 = Pattern::literal(
            "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8B DA 48 8B F1 E8 ?? ?? ?? ?? "
            "48 8B 6B 48 48 8B 7B 40 EB ?? 4C 8B 07 48 8D 54 24 20 48 8B CE 41 0F 10 40 10"
        );
        const Pattern PERK_COPY_P2 = Pattern::literal(
            "48 8D 8B 88 00 00 00 E8 ?? ?? ?? ?? 48 8D 97 F8 01 00 00 48 8D 8B F8 01 00 00 | E8 ?? ?? ?? ?? "
            "0F 10 87 D8 01 00 00"
        );
        /**
         * @brief P3 is the copy's walk of the source: owned perks (+0x40/+0x48), unlocked perks (+0x28/+0x30), extra
         *        definitions (+0x10/+0x18), then the progression block test (+0x58).
         */
        const Pattern PERK_COPY_P3 = Pattern::literal(
            "48 8B 6B 48 48 8B 7B 40 EB ?? 4C 8B 07 48 8D 54 24 20 48 8B CE 41 0F 10 40 10 F3 0F 7F 44 24 20 "
            "E8 ?? ?? ?? ?? 48 83 C7 08 48 3B FD 75 ?? 48 8B 6B 30 48 8B 7B 28 48 3B FD 75 ?? 48 8B 6B 18 "
            "48 8B 7B 10 48 3B FD 0F 85 ?? ?? ?? ?? 48 83 7B 58 00"
        );
        const Rung PERK_COPY_RUNGS[] = {
            {
                .name = "P1_Entry",
                .kind = RungKind::Entry,
                .pattern = &PERK_COPY_P1,
            },
            {
                .name = "P2_CallFromStatCopy",
                .kind = RungKind::Call,
                .pattern = &PERK_COPY_P2,
            },
            {
                .name = "P3_ListLayout",
                .kind = RungKind::Body,
                .pattern = &PERK_COPY_P3,
                .layout = true,
            },
        };

        /**
         * @brief The perk auto-unlock pass.
         * @details P1 tests the progression block (+0x58), P2 the suspend bit (+0x60 & 1).
         */
        const Pattern AUTO_UNLOCK_P1 = Pattern::literal(
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 20 48 83 79 58 00 "
            "48 8B F1 75 ?? 32 C0"
        );
        const Pattern AUTO_UNLOCK_P2 = Pattern::literal("F6 41 60 01 75 ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01");
        const Pattern AUTO_UNLOCK_P3 =
            Pattern::literal("48 8B 48 F8 48 8B 01 FF 50 ?? 48 8B CE | E8 ?? ?? ?? ?? 40 B7 01");
        const Rung AUTO_UNLOCK_RUNGS[] = {
            {
                .name = "P1_Entry",
                .kind = RungKind::Entry,
                .pattern = &AUTO_UNLOCK_P1,
                .layout = true,
            },
            {
                .name = "P2_SuspendBit",
                .kind = RungKind::Body,
                .pattern = &AUTO_UNLOCK_P2,
                .layout = true,
            },
            {
                .name = "P3_CallFromAddPerk",
                .kind = RungKind::Call,
                .pattern = &AUTO_UNLOCK_P3,
            },
        };

        /**
         * @brief C_RPGUtils's cached skip-time prediction.
         * @details P2 is its hour count: getter call, `mulss [rax+3A8h]` (time ratio), `divss` by the seconds of an
         *          hour, `cvttss2si`, the cap `cmp r13d, [rax+59Ch]`, the cache at this+0x10, and its table at
         *          this+0x18.
         */
        const Pattern PREDICTION_P2 = Pattern::literal(
            "E8 ?? ?? ?? ?? 0F 28 C6 F3 0F 59 80 A8 03 00 00 F3 0F 5E 05 ?? ?? ?? ?? F3 44 0F 2C E8 "
            "E8 ?? ?? ?? ?? 44 3B A8 9C 05 00 00 0F 8F ?? ?? ?? ?? 44 8A 45 48 48 8D 4E 10 "
            "41 8A D6 E8 ?? ?? ?? ?? 33 DB 84 C0 74 ?? 41 8D 45 FF 85 C0 0F 49 D8 48 8B 46 18"
        );
        const Pattern PREDICTION_P3 =
            Pattern::literal("88 4C 24 28 48 8B C8 F3 0F 11 7C 24 20 | E8 ?? ?? ?? ?? 48 8B 0F 48 8D 77 08");
        const Rung PREDICTION_RUNGS[] = {
            {
                .name = "P1_Slot",
                .kind = RungKind::Slot,
                .rtti = constants::RTTI_RPG_UTILS,
                .slot = constants::RPG_UTILS_SLEEP_PREDICTION_SLOT,
            },
            {
                .name = "P2_HourCount",
                .kind = RungKind::Body,
                .pattern = &PREDICTION_P2,
                .layout = true,
            },
            {
                .name = "P3_CallFromScriptNode",
                .kind = RungKind::Call,
                .pattern = &PREDICTION_P3,
            },
        };

        /**
         * @brief The prediction cache's validity check.
         * @details P2 compares the stamp at cache+0 (a stamp of 0 is never valid) and the key at cache+4. P3 is the
         *          prediction's call, which passes the cache at C_RPGUtils+0x10.
         */
        const Pattern CACHE_CHECK_P1 =
            Pattern::literal("48 89 5C 24 08 57 48 83 EC 30 8A C2 0F 29 74 24 20 48 8B D9 41 8A D0 8A C8 E8");
        const Pattern CACHE_CHECK_P2 = Pattern::literal(
            "F3 0F 5C 33 E8 ?? ?? ?? ?? F3 0F 10 0B F3 0F 10 80 98 05 00 00 0F 2F C6 0F 97 C1 32 C0 "
            "0F 2F 0D ?? ?? ?? ?? 76 ?? 39 7B 04 75 ?? 84 C9"
        );
        const Pattern CACHE_CHECK_P3 =
            Pattern::literal("44 8A 45 48 48 8D 4E 10 41 8A D6 | E8 ?? ?? ?? ?? 33 DB 84 C0");
        const Rung CACHE_CHECK_RUNGS[] = {
            {
                .name = "P1_Entry",
                .kind = RungKind::Entry,
                .pattern = &CACHE_CHECK_P1,
            },
            {
                .name = "P2_StampLayout",
                .kind = RungKind::Body,
                .pattern = &CACHE_CHECK_P2,
                .layout = true,
            },
            {
                .name = "P3_CallFromPrediction",
                .kind = RungKind::Call,
                .pattern = &CACHE_CHECK_P3,
                .layout = true,
            },
        };

        /**
         * @brief C_SoulSimulator's hour step.
         * @details P2 is the copy soul (+0x10) the step returns.
         */
        const Pattern SIM_STEP_P1 = Pattern::literal(
            "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 70 20 55 48 8B EC 48 81 EC 80 00 00 00 "
            "80 7D 38 00 41 8A D8 0F 29 70 E8 48 8B F1 0F 29 78 D8 0F 28 F1 0F 28 FB"
        );
        const Pattern SIM_STEP_P2 = Pattern::literal("48 8D 7E 10 48 8B 17 4C 8D 4D ?? 48 89 45 ?? 4C 8D 45 ??");
        const Pattern SIM_STEP_P3 =
            Pattern::literal("0F 95 C0 88 44 24 28 40 88 74 24 20 | E8 ?? ?? ?? ?? F3 0F 11 7C 24 28");
        const Rung SIM_STEP_RUNGS[] = {
            {
                .name = "P1_Entry",
                .kind = RungKind::Entry,
                .pattern = &SIM_STEP_P1,
            },
            {
                .name = "P2_CopySoul",
                .kind = RungKind::Body,
                .pattern = &SIM_STEP_P2,
                .layout = true,
            },
            {
                .name = "P3_CallFromTable",
                .kind = RungKind::Call,
                .pattern = &SIM_STEP_P3,
            },
        };

        /**
         * @brief Clones a source inventory's items into a destination inventory.
         * @details P2 is the soul copy's call: the destination inventory (+0x118 of its holder), the mapping flag (1)
         *          and the item type filter (21, the base Item).
         */
        const Pattern CLONE_ITEMS_P1 = Pattern::literal(
            "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 20 45 8B E1 41 8A F0 4C 8B FA "
            "4C 8B F1 E8 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 75 ?? E8 ?? ?? ?? ?? 45 33 C0 48 8D 54 24 50 "
            "41 8D 48 10 FF 15"
        );
        const Pattern CLONE_ITEMS_P2 =
            Pattern::literal("48 8B 93 18 01 00 00 41 B9 15 00 00 00 41 B0 01 48 8B CE | E8 ?? ?? ?? ?? 0F 10 47 50");
        const Rung CLONE_ITEMS_RUNGS[] = {
            {
                .name = "P1_Entry",
                .kind = RungKind::Entry,
                .pattern = &CLONE_ITEMS_P1,
            },
            {
                .name = "P2_CallFromInventoryCopy",
                .kind = RungKind::Call,
                .pattern = &CLONE_ITEMS_P2,
            },
        };

        /// The first-only flag: `cmp byte [rip+disp32], 0`, 7 bytes with the displacement at +2.
        constexpr std::size_t CMP_BYTE_RIP_DISP_OFFSET = 2;
        constexpr std::size_t CMP_BYTE_RIP_LENGTH = 7;
        /// A rel32 call: E8 and its displacement.
        constexpr std::size_t CALL_REL32_DISP_OFFSET = 1;
        constexpr std::size_t CALL_REL32_LENGTH = 5;
        /// In the hour-count rung: the `divss xmm0, [rip+disp32]` follows the call (5), `movaps` (3) and `mulss` (8).
        constexpr std::ptrdiff_t HOUR_COUNT_DIVSS_AT = 16;
        constexpr std::size_t DIVSS_RIP_DISP_OFFSET = 4;
        constexpr std::size_t DIVSS_RIP_LENGTH = 8;
        /// Bound on chained unwind entries, far above what MSVC emits.
        constexpr int MAX_UNWIND_CHAIN = 16;

        DMK::Region s_image{};

        /**
         * @brief The entry of the function that contains @p address, or 0.
         * @details MSVC splits a cold part of a function into its own unwind entry whose unwind info carries
         *          UNW_FLAG_CHAININFO and, after its codes, the RUNTIME_FUNCTION of the part it continues. The walk
         *          follows that chain to the primary entry.
         */
        [[nodiscard]] std::uintptr_t function_entry_of(std::uintptr_t address) noexcept
        {
            if (!s_image.contains(DMK::Address{address}))
            {
                return 0;
            }
            DWORD64 image_base = 0;
            const RUNTIME_FUNCTION *entry = RtlLookupFunctionEntry(static_cast<DWORD64>(address), &image_base, nullptr);
            for (int hop = 0; entry != nullptr && hop < MAX_UNWIND_CHAIN; ++hop)
            {
                const auto *unwind = reinterpret_cast<const std::uint8_t *>(image_base + entry->UnwindData);
                const std::uint8_t flags = static_cast<std::uint8_t>(unwind[0] >> 3);
                if ((flags & UNW_FLAG_CHAININFO) == 0)
                {
                    return static_cast<std::uintptr_t>(image_base + entry->BeginAddress);
                }
                // UNWIND_INFO: 4 header bytes, then the 2-byte codes padded to an even count, then the chained entry.
                const std::size_t code_count = unwind[2];
                const std::size_t padded = (code_count + 1) & ~static_cast<std::size_t>(1);
                entry = reinterpret_cast<const RUNTIME_FUNCTION *>(unwind + 4 + padded * 2);
            }
            return 0;
        }

        /** @brief True when @p address starts a primary unwind entry, which only code in the image's .text has. */
        [[nodiscard]] bool is_function_entry(std::uintptr_t address) noexcept
        {
            return address != 0 && function_entry_of(address) == address;
        }

        /**
         * @brief The address a rung's `|` mark points at (the match when it has none), when the rung matches once.
         * @details require_unique fails closed on a second match and on a sweep that skipped an unreadable region.
         * @throws std::bad_alloc If the one-candidate ladder cannot allocate.
         */
        [[nodiscard]] std::optional<std::uintptr_t> scan_unique(std::string_view name, const Pattern &pattern)
        {
            const std::array<DMK::scan::Candidate, 1> ladder = {
                DMK::scan::Candidate::direct(std::string{name}, pattern),
            };
            const DMK::Result<DMK::scan::Hit> hit = DMK::scan::resolve(
                DMK::scan::ScanRequest{
                    .ladder = ladder,
                    .label = name,
                    .scope = s_image,
                    .require_unique = true,
                    .pages = Pages::Executable,
                }
            );
            if (!hit.has_value())
            {
                return std::nullopt;
            }
            return hit->address.raw();
        }

        /** @brief The target of the rel32 call at @p site, or 0. */
        [[nodiscard]] std::uintptr_t call_target(std::uintptr_t site) noexcept
        {
            const DMK::Result<DMK::Address> target =
                DMK::scan::resolve_rip_relative(DMK::Address{site}, CALL_REL32_DISP_OFFSET, CALL_REL32_LENGTH);
            return target.has_value() ? target->raw() : 0;
        }

        /** @brief One rung's function and, for a byte rung, its mark. Both 0 when the rung did not resolve. */
        struct RungHit
        {
            std::uintptr_t function{0};
            std::uintptr_t mark{0};
        };

        /**
         * @brief Resolves one rung to the function it names, or to nothing.
         * @throws std::bad_alloc If a byte rung's ladder cannot allocate.
         */
        [[nodiscard]] RungHit resolve_rung(const Rung &rung)
        {
            if (rung.kind == RungKind::Slot)
            {
                const std::optional<DMK::Address> vtable = DMK::rtti::vtable_for_type(rung.rtti, s_image);
                if (!vtable.has_value())
                {
                    return {};
                }
                const DMK::Result<std::uintptr_t> target =
                    DMK::memory::read<std::uintptr_t>(DMK::Address{vtable->raw() + rung.slot * sizeof(std::uintptr_t)});
                if (!target.has_value() || !is_function_entry(*target))
                {
                    return {};
                }
                return RungHit{
                    .function = *target,
                };
            }
            const std::optional<std::uintptr_t> mark = scan_unique(rung.name, *rung.pattern);
            if (!mark.has_value())
            {
                return {};
            }
            std::uintptr_t function = 0;
            switch (rung.kind)
            {
            case RungKind::Entry:
                function = is_function_entry(*mark) ? *mark : 0;
                break;
            case RungKind::Body:
                function = function_entry_of(*mark);
                break;
            case RungKind::Call:
            {
                const std::uintptr_t target = call_target(*mark);
                function = is_function_entry(target) ? target : 0;
                break;
            }
            case RungKind::Slot:
                break;
            }
            if (function == 0)
            {
                return {};
            }
            return RungHit{
                .function = function,
                .mark = *mark,
            };
        }

        /** @brief A resolved function and the marks of its rungs, in rung order. */
        struct Resolved
        {
            std::uintptr_t function{0};
            std::array<std::uintptr_t, 4> marks{};
        };

        /**
         * @brief Resolves one anchor from its rungs and logs the outcome.
         * @return The function, or 0 when no rung resolved, two rungs disagree, or a layout rung did not resolve.
         */
        [[nodiscard]] Resolved resolve_function(std::string_view label, std::span<const Rung> rungs)
        {
            Resolved resolved{};
            std::size_t agreeing = 0;
            bool layout_missing = false;
            for (std::size_t i = 0; i < rungs.size() && i < resolved.marks.size(); ++i)
            {
                const RungHit hit = resolve_rung(rungs[i]);
                if (hit.function == 0)
                {
                    layout_missing |= rungs[i].layout;
                    DMK::log().warning("Anchor {}: rung {} did not resolve", label, rungs[i].name);
                    continue;
                }
                if (resolved.function != 0 && hit.function != resolved.function)
                {
                    DMK::log().error(
                        "Anchor {}: rung {} names RVA {:#x}, another rung RVA {:#x}, so the anchor fails",
                        label,
                        rungs[i].name,
                        hit.function - s_image.base.raw(),
                        resolved.function - s_image.base.raw()
                    );
                    return {};
                }
                resolved.function = hit.function;
                resolved.marks[i] = hit.mark;
                ++agreeing;
            }
            if (resolved.function == 0 || layout_missing)
            {
                DMK::log().warning("Anchor {} unresolved", label);
                return {};
            }
            DMK::log().info(
                "Anchor {} at {} (RVA {:#x}), {} of {} rungs agree",
                label,
                DMK::format::format_address(resolved.function),
                resolved.function - s_image.base.raw(),
                agreeing,
                rungs.size()
            );
            return resolved;
        }

        /** @brief The global a `cmp byte [rip+disp32], imm8` at @p site reads, or 0. */
        [[nodiscard]] std::uintptr_t cmp_byte_target(std::uintptr_t site) noexcept
        {
            const DMK::Result<DMK::Address> target =
                DMK::scan::resolve_rip_relative(DMK::Address{site}, CMP_BYTE_RIP_DISP_OFFSET, CMP_BYTE_RIP_LENGTH);
            return target.has_value() && s_image.contains(*target) ? target->raw() : 0;
        }

        /** @brief The seconds-per-hour divisor the hour count divides by, or 0. */
        [[nodiscard]] float seconds_per_hour_at(std::uintptr_t hour_count_mark) noexcept
        {
            const DMK::Result<DMK::Address> constant = DMK::scan::resolve_rip_relative(
                DMK::Address{hour_count_mark + HOUR_COUNT_DIVSS_AT},
                DIVSS_RIP_DISP_OFFSET,
                DIVSS_RIP_LENGTH
            );
            if (!constant.has_value() || !s_image.contains(*constant))
            {
                return 0.0f;
            }
            const DMK::Result<float> value = DMK::memory::read<float>(*constant);
            return value.has_value() && std::isfinite(*value) && *value > 0.0f ? *value : 0.0f;
        }

        void log_gate(std::string_view fix, bool on)
        {
            if (on)
            {
                DMK::log().info("Fix {}: available", fix);
            }
            else
            {
                DMK::log().warning("Fix {}: an anchor did not resolve, so the fix stays off", fix);
            }
        }
    } // namespace

    Anchors resolve_anchors(DMK::Region game)
    {
        s_image = game;
        Anchors anchors{};

        const Resolved full_ui = resolve_function("FullUiModeSwitch", FULL_UI_RUNGS);
        anchors.full_ui_mode_switch = full_ui.function;

        const Resolved unlink = resolve_function("LinkUnlinkScan", UNLINK_RUNGS);
        anchors.link_unlink_scan = unlink.function;
        if (unlink.function != 0)
        {
            anchors.link_unlink_first_only_flag = cmp_byte_target(unlink.marks[1]);
        }
        anchors.link_map_erase_node = resolve_function("LinkMapEraseNode", ERASE_RUNGS).function;
        anchors.link_map_insert = resolve_function("LinkMapInsert", INSERT_RUNGS).function;

        anchors.inventory_clear = resolve_function("InventoryClear", CLEAR_RUNGS).function;
        anchors.inventory_soul_refresh = resolve_function("InventorySoulRefresh", REFRESH_RUNGS).function;

        anchors.perk_copy = resolve_function("PerkCopy", PERK_COPY_RUNGS).function;
        anchors.perk_auto_unlock = resolve_function("PerkAutoUnlock", AUTO_UNLOCK_RUNGS).function;

        const Resolved prediction = resolve_function("SleepPrediction", PREDICTION_RUNGS);
        anchors.sleep_prediction = prediction.function;
        if (prediction.function != 0)
        {
            const std::uintptr_t getter = call_target(prediction.marks[1]);
            anchors.rpg_params_getter = is_function_entry(getter) ? getter : 0;
            anchors.seconds_per_hour = seconds_per_hour_at(prediction.marks[1]);
        }
        anchors.sleep_prediction_cache_check = resolve_function("SleepPredictionCache", CACHE_CHECK_RUNGS).function;
        anchors.soul_sim_step = resolve_function("SoulSimStep", SIM_STEP_RUNGS).function;
        anchors.inventory_clone_items = resolve_function("InventoryCloneItems", CLONE_ITEMS_RUNGS).function;

        anchors.menu_view_distance = anchors.full_ui_mode_switch != 0;
        anchors.link_index = anchors.link_unlink_scan != 0 && anchors.link_unlink_first_only_flag != 0 &&
                             anchors.link_map_erase_node != 0 && anchors.link_map_insert != 0;
        anchors.inventory_refresh = anchors.inventory_clear != 0 && anchors.inventory_soul_refresh != 0;
        anchors.perk_copy_fix = anchors.perk_copy != 0 && anchors.perk_auto_unlock != 0;
        anchors.sleep_prediction_fix = anchors.sleep_prediction != 0 && anchors.rpg_params_getter != 0 &&
                                       anchors.seconds_per_hour > 0.0f && anchors.sleep_prediction_cache_check != 0 &&
                                       anchors.soul_sim_step != 0;
        anchors.sleep_prediction_items_fix = anchors.sleep_prediction_fix && anchors.inventory_clone_items != 0;

        log_gate("MenuViewDistance", anchors.menu_view_distance);
        log_gate("LinkIndex", anchors.link_index);
        log_gate("InventoryRefresh", anchors.inventory_refresh);
        log_gate("PerkCopy", anchors.perk_copy_fix);
        log_gate("SleepPrediction", anchors.sleep_prediction_fix);
        log_gate("SleepPredictionWithoutItems", anchors.sleep_prediction_items_fix);
        return anchors;
    }
} // namespace EngineTweaks
