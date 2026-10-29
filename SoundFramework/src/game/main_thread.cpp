/**
 * @file game/main_thread.cpp
 * @brief The CSystem::Update hook and the watchdog that hooks the function again after another mod overwrites it.
 */

#include "game/main_thread.hpp"
#include "constants.hpp"
#include "game/console_commands.hpp"
#include "game/cutscene_voices.hpp"
#include "game/entity_tracker.hpp"
#include "game/game_clock.hpp"
#include "game/lip_drive.hpp"
#include "game/speech.hpp"
#include "packs/sound_def.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>

namespace SoundFramework::game::main_thread
{
    namespace
    {
        using SystemUpdateFn = bool(__fastcall *)(void *system, int flags, int pause_mode);

        /// The first hook, then one slot for each hook the watchdog may add.
        constexpr std::size_t LAYER_COUNT = 1 + constants::SYSTEM_UPDATE_MAX_REHOOKS;
        /// The prologue window compared with the framework's own patch: any jump written over it changes it.
        constexpr std::size_t PROLOGUE_WITNESS_SIZE = 16;
        /// How many jumps the conflict report follows from the prologue to the code that owns it.
        constexpr int MAX_JUMP_HOPS = 4;
        /// `jmp rel32`: E9, then a disp32 from the end of the 5-byte instruction.
        constexpr std::size_t JMP_REL32_LENGTH = 5;
        /// `jmp qword ptr [rip+disp32]`: FF 25, then a disp32 that names the qword slot holding the destination.
        constexpr std::array<std::byte, 2> JMP_RIP_INDIRECT = {
            std::byte{0xFF},
            std::byte{0x25},
        };
        /// The length of `jmp qword ptr [rip+disp32]`.
        constexpr std::size_t JMP_RIP_INDIRECT_LENGTH = 6;
        /// `mov rax, imm64`: 48 B8, then the destination as 8 immediate bytes.
        constexpr std::array<std::byte, 2> MOV_RAX_IMM64 = {
            std::byte{0x48},
            std::byte{0xB8},
        };
        /// `jmp rax`: FF E0, right after `mov rax, imm64` in a far jump.
        constexpr std::array<std::byte, 2> JMP_RAX = {
            std::byte{0xFF},
            std::byte{0xE0},
        };
        /// The bytes read to decode one jump: the longest form, `mov rax, imm64` then `jmp rax`.
        constexpr std::size_t JUMP_WINDOW_SIZE = 12;

        using PrologueBytes = std::array<std::byte, PROLOGUE_WITNESS_SIZE>;

        /**
         * @brief What the watchdog knows about the hook chain.
         * @details start() fills it before the watchdog thread starts. From then on only that thread uses it, until
         *          stop() joins the thread.
         */
        struct WatchState
        {
            /// CSystem::Update.
            std::uintptr_t target = 0;
            /// The stack that owns every layer. The newest layer is pushed last, so it retires first and restores.
            DMK::hook::HookStack *hooks = nullptr;
            /// The layers installed so far, which is also the slot of the next one.
            std::size_t layers = 0;
            /// The prologue that the newest layer's arm wrote, or empty when that arm wrote nothing.
            std::optional<PrologueBytes> own_patch;
            /// True once the log says that no slot is left.
            bool exhausted_logged = false;
        };

        WatchState s_watch;
        std::optional<DMK::StoppableWorker> s_watchdog;
        std::array<std::atomic<SystemUpdateFn>, LAYER_COUNT> s_originals{};
        /// The slot whose detour runs the frame work, or LAYER_COUNT before the first arm.
        std::atomic<std::size_t> s_active_layer{LAYER_COUNT};
        std::atomic<DWORD> s_thread_id{0};
        /// GetTickCount64() at the last frame that reached the active layer, or at the last arm.
        std::atomic<std::uint64_t> s_last_frame_ms{0};

        void run_frame(void *system) noexcept
        {
            s_thread_id.store(GetCurrentThreadId(), std::memory_order_relaxed);
            s_last_frame_ms.store(GetTickCount64(), std::memory_order_release);
            game_clock::tick();
            commands::on_frame(system);
            entities::on_frame();
            cutscene_voices::on_frame();
            speech::on_frame();
            lip_drive::on_frame();
        }

        /**
         * @brief The detour of slot @p Layer.
         * @details A foreign hook placed over an older layer can still chain into it, so after a re-hook one call can
         *          pass through two framework layers. Only the newest armed layer runs the frame work and the others
         *          only forward, so the work runs once per call however the chain is built.
         */
        template <std::size_t Layer>
        bool __fastcall system_update_detour(void *system, int flags, int pause_mode) noexcept
        {
            if (s_active_layer.load(std::memory_order_acquire) == Layer)
            {
                run_frame(system);
            }
            return s_originals[Layer].load(std::memory_order_acquire)(system, flags, pause_mode);
        }

        using InstallFn = DMK::Result<DMK::hook::Hook> (*)(std::uintptr_t target, std::string name);

        /** @brief Installs the disabled hook of slot @p Layer and publishes its trampoline for the detour. */
        template <std::size_t Layer> DMK::Result<DMK::hook::Hook> install_layer(std::uintptr_t target, std::string name)
        {
            DMK_TRY(
                created,
                DMK::hook::inline_at<&system_update_detour<Layer>>(DMK::hook::InlineRequest{
                    .name = std::move(name),
                    .target = DMK::Address{target},
                })
            );
            s_originals[Layer].store(created.original<SystemUpdateFn>(), std::memory_order_release);
            return created;
        }

        template <std::size_t... Layers>
        constexpr std::array<InstallFn, LAYER_COUNT> make_installers(std::index_sequence<Layers...>) noexcept
        {
            return {&install_layer<Layers>...};
        }

        /// The installer of each slot: every slot has its own detour, so a counted drain waits only for its own calls.
        constexpr std::array<InstallFn, LAYER_COUNT> INSTALLERS =
            make_installers(std::make_index_sequence<LAYER_COUNT>{});

        [[nodiscard]] std::optional<PrologueBytes> read_prologue(std::uintptr_t address) noexcept
        {
            PrologueBytes bytes{};
            if (!DMK::memory::read_into(DMK::Address{address}, bytes))
            {
                return std::nullopt;
            }
            return bytes;
        }

        /**
         * @brief Installs the hook of slot @p layer on top of whatever opens the function, arms it and makes it the
         *        active layer.
         * @note Setup or watchdog thread only.
         */
        [[nodiscard]] DMK::Result<void> add_layer(std::size_t layer, std::string name)
        {
            DMK_TRY(created, INSTALLERS[layer](s_watch.target, std::move(name)));
            // Stored before it is armed: the stack owns it through teardown even when the arm fails half-way.
            DMK::hook::Hook &stored = s_watch.hooks->push(std::move(created));
            s_watch.layers = layer + 1;
            const std::optional<PrologueBytes> before = read_prologue(s_watch.target);
            DMK_TRY_VOID(stored.enable());
            const std::optional<PrologueBytes> after = read_prologue(s_watch.target);
            // An arm that claims a position an earlier generation vacated writes nothing. The prologue then proves
            // nothing about whether calls reach this layer, and only the frame stall decides.
            s_watch.own_patch = before != after ? after : std::nullopt;
            s_last_frame_ms.store(GetTickCount64(), std::memory_order_release);
            s_active_layer.store(layer, std::memory_order_release);
            return {};
        }

        /**
         * @brief Decodes the jump at @p address and returns its destination, or 0 when the bytes hold no jump.
         * @details A hook library opens a hooked function with one of three jumps:
         *          - `E9 rel32`, a near jump, usually to a relay within 2 GB of the target,
         *          - `FF 25 disp32`, an absolute jump through the qword slot that the disp32 names,
         *          - `48 B8 imm64` then `FF E0` (`mov rax, imm64; jmp rax`), a far jump with no slot.
         *          A relay is usually the second form, so a report follows the jumps one by one.
         */
        [[nodiscard]] std::uintptr_t jump_destination(std::uintptr_t address) noexcept
        {
            std::array<std::byte, JUMP_WINDOW_SIZE> code{};
            if (!DMK::memory::read_into(DMK::Address{address}, code))
            {
                return 0;
            }
            const std::span<const std::byte> bytes{code};
            if (std::ranges::equal(bytes.first(DMK::scan::PREFIX_JMP_REL32.size()), DMK::scan::PREFIX_JMP_REL32))
            {
                const DMK::Result<DMK::Address> destination = DMK::scan::resolve_rip_relative(
                    DMK::Address{address},
                    DMK::scan::PREFIX_JMP_REL32.size(),
                    JMP_REL32_LENGTH
                );
                return destination ? destination->raw() : 0;
            }
            if (std::ranges::equal(bytes.first(JMP_RIP_INDIRECT.size()), JMP_RIP_INDIRECT))
            {
                // For FF 25 the resolver returns the slot, not the destination stored in it.
                const DMK::Result<DMK::Address> slot = DMK::scan::resolve_rip_relative(
                    DMK::Address{address},
                    JMP_RIP_INDIRECT.size(),
                    JMP_RIP_INDIRECT_LENGTH
                );
                if (!slot)
                {
                    return 0;
                }
                const DMK::Result<std::uintptr_t> destination = DMK::memory::read<std::uintptr_t>(*slot);
                return destination ? *destination : 0;
            }
            if (std::ranges::equal(bytes.first(MOV_RAX_IMM64.size()), MOV_RAX_IMM64) &&
                std::ranges::equal(bytes.last(JMP_RAX.size()), JMP_RAX))
            {
                std::uintptr_t destination = 0;
                std::memcpy(&destination, code.data() + MOV_RAX_IMM64.size(), sizeof(destination));
                return destination;
            }
            return 0;
        }

        /**
         * @brief Names the code that the prologue at @p target leads to after up to MAX_JUMP_HOPS jumps, for the log.
         * @return The file name of the module that holds that code, or a description when no module holds it.
         * @note Never throws: the name only decorates a log line, so a failure here must not stop the re-hook.
         */
        [[nodiscard]] std::string prologue_owner(std::uintptr_t target) noexcept
        {
            try
            {
                std::uintptr_t code = target;
                for (int hop = 0; hop < MAX_JUMP_HOPS; ++hop)
                {
                    const std::uintptr_t next = jump_destination(code);
                    if (next == 0)
                    {
                        break;
                    }
                    code = next;
                }
                if (code == target)
                {
                    return "the game's own code";
                }
                const DMK::Region module = DMK::memory::module_of(DMK::Address{code});
                if (module.size == 0)
                {
                    return std::format("code at {:#x} outside any module", code);
                }
                // The handle of a loaded module is its base address.
                const auto handle = reinterpret_cast<HMODULE>(module.base.raw());
                std::wstring path(MAX_PATH, L'\0');
                for (;;)
                {
                    const DWORD length = GetModuleFileNameW(handle, path.data(), static_cast<DWORD>(path.size()));
                    if (length < path.size())
                    {
                        path.resize(length);
                        break;
                    }
                    path.resize(path.size() * 2);
                }
                return packs::path_text(std::filesystem::path(path).filename());
            }
            catch (...)
            {
                // Short enough for the small-string buffer, so this return allocates nothing and cannot throw.
                return "unknown code";
            }
        }

        /** @brief Hooks the function again when no frame reached the active layer while another patch opens it. */
        void check_hook()
        {
            // The frame thread reads the clock before it publishes, so this later read is never the smaller one.
            const std::uint64_t last_frame_ms = s_last_frame_ms.load(std::memory_order_acquire);
            const std::uint64_t now_ms = GetTickCount64();
            if (now_ms < last_frame_ms + static_cast<std::uint64_t>(constants::SYSTEM_UPDATE_STALL.count()))
            {
                return;
            }
            const std::optional<PrologueBytes> prologue = read_prologue(s_watch.target);
            // The framework's own patch still opens the function, so the main thread is only busy, as in a level load.
            if (!prologue.has_value() || (s_watch.own_patch.has_value() && *prologue == *s_watch.own_patch))
            {
                return;
            }
            if (s_watch.layers == LAYER_COUNT)
            {
                if (!s_watch.exhausted_logged)
                {
                    s_watch.exhausted_logged = true;
                    DMK::log().error(
                        "MainThread: CSystem::Update was taken over again after {} new hook(s); commands, voice lines "
                        "and faces stay off until the game restarts",
                        constants::SYSTEM_UPDATE_MAX_REHOOKS
                    );
                }
                return;
            }
            const std::size_t layer = s_watch.layers;
            DMK::log().warning(
                "MainThread: no frame reached the CSystem::Update hook for {} ms and the function now jumps into {}; "
                "another mod wrote over the hook, so it is hooked again on top ({} of {})",
                now_ms - last_frame_ms,
                prologue_owner(s_watch.target),
                layer,
                constants::SYSTEM_UPDATE_MAX_REHOOKS
            );
            if (const DMK::Result<void> added = add_layer(layer, std::format("CSystemUpdate.Rehook{}", layer)); !added)
            {
                DMK::log().error("MainThread: hooking CSystem::Update again failed ({})", added.error().message());
                return;
            }
            DMK::log().info("MainThread: CSystem::Update hooked again");
        }

        void watch(std::stop_token token)
        {
            std::mutex mutex;
            std::condition_variable_any wake;
            std::unique_lock lock(mutex);
            // Nothing notifies the variable: each wait is a sleep that a stop request ends early.
            while (!wake.wait_for(
                lock,
                token,
                constants::SYSTEM_UPDATE_CHECK_INTERVAL,
                [&token] { return token.stop_requested(); }
            ))
            {
                // A failed check skips this round only; the next one looks again.
                try
                {
                    check_hook();
                }
                catch (const std::exception &error)
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Error, "MainThread: hook check failed ({})", error.what());
                }
            }
        }
    } // namespace

    DMK::Result<void> start(const Anchors &anchors, DMK::hook::HookStack &hooks)
    {
        s_watch = WatchState{
            .target = anchors.system_update,
            .hooks = &hooks,
        };
        DMK_TRY_VOID(add_layer(0, "CSystemUpdate"));
        DMK::log().info("MainThread: CSystem::Update hooked");
        try
        {
            s_watchdog.emplace("SoundFramework.hookwatch", &watch);
        }
        catch (const std::exception &error)
        {
            // The hook works without the watchdog; only a later overwrite by another mod then stays unrepaired.
            DMK::log().warning("MainThread: hook watchdog not started ({})", error.what());
        }
        return {};
    }

    void stop() noexcept
    {
        s_watchdog.reset();
    }

    bool is_main_thread() noexcept
    {
        return s_thread_id.load(std::memory_order_relaxed) == GetCurrentThreadId();
    }
} // namespace SoundFramework::game::main_thread
