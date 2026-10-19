/**
 * @file proxy_reload.cpp
 * @brief The manager-pass detour that resets proxies whose files changed or whose last load failed.
 *
 * Locks: s_targets_mutex (shared in a pass, exclusive while the main thread publishes) is always taken before
 * s_window_mutex. A pass holds neither while it calls the game's Reset.
 */

#include "proxy_reload.hpp"
#include "constants.hpp"
#include "game_memory.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace HotPak
{
    namespace
    {
        using ManagerPassFn = void(__fastcall *)(void *manager);
        using ResetFn = void(__fastcall *)(void *proxy);

        ManagerPassFn s_original = nullptr;
        ResetFn s_reset = nullptr;
        std::uintptr_t s_proxy_vtable = 0;
        std::uintptr_t s_element_vtable = 0;
        std::uintptr_t s_attachment_vtable = 0;
        std::uintptr_t s_skin_vtable = 0;
        std::uintptr_t s_material_info_vtable = 0;
        std::uintptr_t s_material_instance_vtable = 0;

        constexpr std::size_t NAME_CAPACITY = 96;
        using Name = std::array<char, NAME_CAPACITY>;

        /** @brief A fixed set of lowercase file names. Readers and the writer hold s_targets_mutex. */
        struct NameSet
        {
            static constexpr std::size_t CAPACITY = 64;
            std::array<Name, CAPACITY> names{};
            std::size_t count = 0;

            void publish(std::span<const std::string> values) noexcept
            {
                count = 0;
                for (const std::string &value : values)
                {
                    if (count == CAPACITY || value.empty() || value.size() >= NAME_CAPACITY)
                    {
                        continue;
                    }
                    value.copy(names[count].data(), value.size());
                    names[count][value.size()] = '\0';
                    ++count;
                }
            }

            [[nodiscard]] bool contains(std::string_view name) const noexcept
            {
                if (name.empty())
                {
                    return false;
                }
                for (std::size_t i = 0; i < count; ++i)
                {
                    if (std::string_view(names[i].data()) == name)
                    {
                        return true;
                    }
                }
                return false;
            }
        };

        /** @brief A fixed set of object addresses. Readers and the writer hold s_targets_mutex. */
        struct PointerSet
        {
            static constexpr std::size_t CAPACITY = 64;
            std::array<std::uintptr_t, CAPACITY> values{};
            std::size_t count = 0;

            void publish(std::span<const std::uintptr_t> source) noexcept
            {
                count = 0;
                for (const std::uintptr_t value : source)
                {
                    if (count < CAPACITY && value != 0)
                    {
                        values[count++] = value;
                    }
                }
            }

            [[nodiscard]] bool contains(std::uintptr_t value) const noexcept
            {
                for (std::size_t i = 0; i < count; ++i)
                {
                    if (values[i] == value)
                    {
                        return true;
                    }
                }
                return false;
            }
        };

        std::shared_mutex s_targets_mutex;
        NameSet s_shipped_files;
        NameSet s_restream_files;
        PointerSet s_replaced_skins;

        /** @brief One reset, recorded inside the pass for the main thread's follow-up. */
        struct PendingRecord
        {
            std::uintptr_t proxy = 0;
            Name model{};
            Name material{};
            std::uint8_t state_before = 0;
            ResetReason reason = ResetReason::Error;
        };

        /** @brief A character's manager as the window saw it. */
        struct ManagerSighting
        {
            std::uintptr_t manager = 0;
            /// GetTickCount64() of the first pass of the window that examined it.
            std::uint64_t first_seen = 0;
            /// GetTickCount64() of HotPak's latest reset of one of its proxies, or 0 when there was none.
            std::uint64_t last_reset = 0;
        };

        /** @brief What a pass knows about the character it examines. */
        struct ManagerFacts
        {
            /// The manager ran a pass early enough in the window to predate it.
            bool existing = false;
            /// Milliseconds since HotPak last reset one of its proxies in this window, if it did.
            std::optional<std::uint64_t> since_reset_ms;
        };

        // Proxies reset in the current window, the managers it examined, and the records the main thread has not taken
        // yet. Managers can run their passes on several threads at once, so all three share one lock, taken only for a
        // manager that holds a proxy of the pak's files.
        std::mutex s_window_mutex;
        constexpr std::size_t MAX_DONE = 1024;
        std::array<std::uintptr_t, MAX_DONE> s_done{};
        std::size_t s_done_count = 0;
        constexpr std::size_t MAX_MANAGERS = 256;
        std::array<ManagerSighting, MAX_MANAGERS> s_managers{};
        std::size_t s_manager_count = 0;
        constexpr std::size_t MAX_RECORDS = 128;
        std::array<PendingRecord, MAX_RECORDS> s_records{};
        std::size_t s_record_count = 0;

        /// GetTickCount64() deadline of the reset window; 0 while closed.
        std::atomic<std::uint64_t> s_window_end{0};
        /// GetTickCount64() when the window opened.
        std::atomic<std::uint64_t> s_window_start{0};

        // Every manager of a live character runs its pass many times a second, paused included. One that first runs
        // later than this into the window belongs to a character created after the window opened (a save load, for
        // example), which already reads every file and table row fresh.
        constexpr std::uint64_t EXISTING_MANAGER_MS = 1000;

        std::atomic<std::uint64_t> s_passes{0};
        std::atomic<std::uint64_t> s_resets{0};

        constexpr std::size_t MAX_RESETS_PER_PASS = 32;
        constexpr std::size_t MAX_INSTANCE_ATTACHMENTS = 256;

        [[nodiscard]] int read_state(std::uintptr_t proxy) noexcept
        {
            const auto state = read_value<std::uint8_t>(proxy + constants::PROXY_STREAMING_STATE_OFFSET);
            return state.has_value() ? static_cast<int>(*state) : -1;
        }

        /**
         * @brief Reads the C string at the pointer stored in @p field into @p out, lowercased and with backslashes
         *        turned into slashes; guarded and allocation free.
         * @return False on a fault, an empty string, or a name that does not fit.
         */
        [[nodiscard]] bool read_name(std::uintptr_t field, Name &out) noexcept
        {
            const std::size_t length = read_c_string(read_ptr(field), out);
            if (length == 0)
            {
                out[0] = '\0';
                return false;
            }
            for (std::size_t i = 0; i < length; ++i)
            {
                char &c = out[i];
                if (c == '\\')
                {
                    c = '/';
                }
                else if (c >= 'A' && c <= 'Z')
                {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            return true;
        }

        /** @brief The file-name part of a lowercase path (rows usually hold a bare name already). */
        [[nodiscard]] std::string_view base_name(const Name &path) noexcept
        {
            const std::string_view text(path.data());
            const std::size_t slash = text.rfind('/');
            return slash == std::string_view::npos ? text : text.substr(slash + 1);
        }

        /** @brief Reads the model and material names of a proxy's row; false when the row is not a C_SkinElement. */
        [[nodiscard]] bool read_row(std::uintptr_t proxy, Name &model, Name &material) noexcept
        {
            const std::uintptr_t element = read_ptr(proxy + constants::PROXY_ELEMENT_OFFSET);
            if (element == 0 || s_element_vtable == 0 || read_ptr(element) != s_element_vtable)
            {
                return false;
            }
            (void)read_name(element + constants::SKIN_ELEMENT_MODEL_OFFSET, model);
            (void)read_name(element + constants::SKIN_ELEMENT_MATERIAL_OFFSET, material);
            return model[0] != '\0' || material[0] != '\0';
        }

        /**
         * @brief Whether the attachment @p proxy shows carries a CSkin that HotPak marked unshared.
         * @details The flag is read again because a freed skin's memory can hold a new CSkin at the same address.
         */
        [[nodiscard]] bool shows_replaced_skin(std::uintptr_t proxy) noexcept
        {
            const std::uintptr_t attachment = read_ptr(proxy + constants::PROXY_ATTACHMENT_OFFSET);
            if (attachment == 0 || read_ptr(attachment) != s_attachment_vtable)
            {
                return false;
            }
            const std::uintptr_t skin = read_ptr(attachment + constants::ATTACHMENT_SKIN_OFFSET);
            if (skin == 0 || !s_replaced_skins.contains(skin) || s_skin_vtable == 0 || read_ptr(skin) != s_skin_vtable)
            {
                return false;
            }
            const auto flags = read_value<std::uint32_t>(skin + constants::SKIN_FLAGS_OFFSET);
            return flags.has_value() && (*flags & constants::SKIN_FLAG_UNSHARED) != 0;
        }

        /** @brief Whether @p proxy renders with a material instance built on a material HotPak renamed. */
        [[nodiscard]] bool shows_retired_material(std::uintptr_t proxy) noexcept
        {
            const std::uintptr_t instance = read_ptr(proxy + constants::PROXY_MATERIAL_INSTANCE_OFFSET);
            if (instance == 0 || read_ptr(instance) != s_material_instance_vtable)
            {
                return false;
            }
            const std::uintptr_t material = read_ptr(instance + constants::MATERIAL_INSTANCE_BASE_OFFSET);
            if (material == 0 || s_material_info_vtable == 0 || read_ptr(material) != s_material_info_vtable)
            {
                return false;
            }
            std::array<char, 256> name{};
            const std::size_t length = read_c_string(read_ptr(material + constants::MATERIAL_NAME_OFFSET), name);
            return std::string_view(name.data(), length).find(constants::RETIRED_MATERIAL_MARKER) !=
                   std::string_view::npos;
        }

        /**
         * @brief Records @p manager on its first examination in this window and reports what the window knows of it.
         * @details A full table answers as a new, untouched manager, so no time-based rule resets its proxies.
         */
        [[nodiscard]] ManagerFacts sight_manager(std::uintptr_t manager, std::uint64_t now) noexcept
        {
            const std::uint64_t window_start = s_window_start.load(std::memory_order_acquire);
            const std::lock_guard lock(s_window_mutex);
            for (std::size_t i = 0; i < s_manager_count; ++i)
            {
                const ManagerSighting &sighting = s_managers[i];
                if (sighting.manager == manager)
                {
                    ManagerFacts facts{
                        .existing = sighting.first_seen - window_start <= EXISTING_MANAGER_MS,
                    };
                    if (sighting.last_reset != 0)
                    {
                        facts.since_reset_ms = now - sighting.last_reset;
                    }
                    return facts;
                }
            }
            if (s_manager_count == MAX_MANAGERS)
            {
                return ManagerFacts{};
            }
            s_managers[s_manager_count++] = ManagerSighting{
                .manager = manager,
                .first_seen = now,
            };
            return ManagerFacts{
                .existing = now - window_start <= EXISTING_MANAGER_MS,
            };
        }

        /** @brief A proxy a pass decided to reset, with what the record needs. */
        struct ResetClaim
        {
            std::uintptr_t proxy = 0;
            std::uintptr_t manager = 0;
            std::uint8_t state = 0;
            ResetReason reason = ResetReason::Error;
            std::uint64_t now = 0;
        };

        /** @brief Records a reset in this window; false when the proxy already was reset or the set is full. */
        [[nodiscard]] bool claim(const ResetClaim &reset, const Name &model, const Name &material) noexcept
        {
            const std::lock_guard lock(s_window_mutex);
            if (s_done_count == MAX_DONE)
            {
                return false;
            }
            for (std::size_t i = 0; i < s_done_count; ++i)
            {
                if (s_done[i] == reset.proxy)
                {
                    return false;
                }
            }
            s_done[s_done_count++] = reset.proxy;
            for (std::size_t i = 0; i < s_manager_count; ++i)
            {
                if (s_managers[i].manager == reset.manager)
                {
                    s_managers[i].last_reset = reset.now;
                    break;
                }
            }
            if (s_record_count < MAX_RECORDS)
            {
                s_records[s_record_count++] = PendingRecord{
                    .proxy = reset.proxy,
                    .model = model,
                    .material = material,
                    .state_before = reset.state,
                    .reason = reset.reason,
                };
            }
            return true;
        }

        /**
         * @brief Collects every proxy of @p manager that reset_reason() picks and that was not reset in this window.
         * @details Every link is checked against the vtable its RTTI name resolved to, and every read is guarded, so
         *          a proxy array of an unexpected layout is skipped instead of followed.
         * @return The number of proxies written to @p hits.
         */
        [[nodiscard]] std::size_t
        find_target_proxies(std::uintptr_t manager, std::span<std::uintptr_t, MAX_RESETS_PER_PASS> hits) noexcept
        {
            const std::uintptr_t proxies = read_ptr(manager + constants::MANAGER_PROXIES_OFFSET);
            const std::uintptr_t count = read_ptr(manager + constants::MANAGER_PROXY_COUNT_OFFSET);
            if (proxies == 0 || count == 0 || count > constants::MANAGER_PROXY_LIMIT)
            {
                return 0;
            }

            const std::shared_lock lock(s_targets_mutex);
            const std::uint64_t now = GetTickCount64();
            std::optional<ManagerFacts> character;
            std::size_t hit_count = 0;
            for (std::uintptr_t i = 0; i < count && hit_count < hits.size(); ++i)
            {
                const std::uintptr_t proxy = read_ptr(proxies + i * sizeof(std::uintptr_t));
                if (proxy == 0 || read_ptr(proxy) != s_proxy_vtable)
                {
                    continue;
                }
                Name model{};
                Name material{};
                if (!read_row(proxy, model, material))
                {
                    continue;
                }
                const std::string_view model_name = base_name(model);
                const std::string_view material_name = base_name(material);
                const bool shipped = s_shipped_files.contains(model_name) || s_shipped_files.contains(material_name);
                const bool restream = s_restream_files.contains(model_name);
                if (!shipped && !restream)
                {
                    continue;
                }
                if (!character.has_value())
                {
                    character = sight_manager(manager, now);
                }
                const int state = read_state(proxy);
                const std::optional<ResetReason> reason = reset_reason(
                    ProxyFacts{
                        .shipped = shipped,
                        .restream = restream,
                        .replaced_skin = shipped && shows_replaced_skin(proxy),
                        .retired_material = shipped && shows_retired_material(proxy),
                        .state = state,
                        .manager_early = character->existing,
                        .since_manager_reset_ms = character->since_reset_ms,
                    }
                );
                if (!reason.has_value())
                {
                    continue;
                }
                const ResetClaim reset{
                    .proxy = proxy,
                    .manager = manager,
                    .state = static_cast<std::uint8_t>(state),
                    .reason = *reason,
                    .now = now,
                };
                if (claim(reset, model, material))
                {
                    hits[hit_count++] = proxy;
                }
            }
            return hit_count;
        }

        void __fastcall manager_pass_detour(void *manager) noexcept
        {
            const std::uint64_t window_end = s_window_end.load(std::memory_order_acquire);
            if (window_end != 0 && GetTickCount64() < window_end)
            {
                s_passes.fetch_add(1, std::memory_order_relaxed);
                std::array<std::uintptr_t, MAX_RESETS_PER_PASS> hits{};
                const std::size_t hit_count = find_target_proxies(reinterpret_cast<std::uintptr_t>(manager), hits);
                for (std::size_t i = 0; i < hit_count; ++i)
                {
                    s_reset(reinterpret_cast<void *>(hits[i]));
                    s_resets.fetch_add(1, std::memory_order_relaxed);
                }
            }
            s_original(manager);
        }
    } // namespace

    DMK::Result<void>
    initialize_proxy_reload(DMK::hook::HookStack &hooks, std::uintptr_t target, const EngineAnchors &engine)
    {
        const std::uintptr_t reset = vtable_slot(engine.clothing_proxy, constants::PROXY_RESET_SLOT);
        if (target == 0 || reset == 0 || engine.skin_element == 0 || engine.attachment_skin == 0 ||
            engine.material_instance == 0)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::InvalidArg, "proxy_reload/prerequisites"});
        }
        s_reset = reinterpret_cast<ResetFn>(reset);
        s_proxy_vtable = engine.clothing_proxy;
        s_element_vtable = engine.skin_element;
        s_attachment_vtable = engine.attachment_skin;
        s_skin_vtable = engine.skin;
        s_material_info_vtable = engine.material_info;
        s_material_instance_vtable = engine.material_instance;
        s_window_end.store(0, std::memory_order_release);

        DMK_TRY(
            created,
            DMK::hook::inline_at<&manager_pass_detour>(DMK::hook::InlineRequest{
                .name = "ProxyManagerUpdate",
                .target = DMK::Address{target},
            })
        );
        s_original = created.original<ManagerPassFn>();
        // Stored before it is armed: the stack owns it through teardown even when the arm fails half-way.
        DMK::hook::Hook &stored = hooks.push(std::move(created));
        DMK_TRY_VOID(stored.enable());
        DMK::log().info(
            "ProxyReload: manager pass hooked at {}; proxy Reset at {}",
            DMK::format::format_address(target),
            DMK::format::format_address(reset)
        );
        return {};
    }

    void begin_proxy_resets(const ResetTargets &targets, std::chrono::milliseconds window) noexcept
    {
        // Waits for every pass inside its scan, so none reads a set while it is rewritten, claims a proxy for the
        // window that is closing, or pairs the new sets with the old window start.
        const std::unique_lock published(s_targets_mutex);
        s_shipped_files.publish(targets.shipped_files);
        s_restream_files.publish(targets.restream_files);
        s_replaced_skins.publish(targets.replaced_skins);
        {
            const std::lock_guard window_state(s_window_mutex);
            s_done_count = 0;
            s_manager_count = 0;
        }
        if (targets.shipped_files.empty() && targets.restream_files.empty())
        {
            s_window_end.store(0, std::memory_order_release);
            return;
        }
        const std::uint64_t now = GetTickCount64();
        s_window_start.store(now, std::memory_order_release);
        s_window_end.store(now + static_cast<std::uint64_t>(window.count()), std::memory_order_release);
    }

    bool proxy_resets_active() noexcept
    {
        return s_window_end.load(std::memory_order_acquire) != 0;
    }

    bool expire_proxy_resets() noexcept
    {
        const std::uint64_t window_end = s_window_end.load(std::memory_order_acquire);
        if (window_end == 0 || GetTickCount64() < window_end)
        {
            return false;
        }
        std::uint64_t expected = window_end;
        return s_window_end.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    }

    ProxyReloadStats take_proxy_reload_stats() noexcept
    {
        return ProxyReloadStats{
            .manager_passes = s_passes.exchange(0, std::memory_order_relaxed),
            .proxies_reset = s_resets.exchange(0, std::memory_order_relaxed),
        };
    }

    std::vector<ResetRecord> take_reset_records()
    {
        // Allocated before the lock, so a pass never waits on an allocation.
        std::vector<PendingRecord> taken(MAX_RECORDS);
        {
            const std::lock_guard lock(s_window_mutex);
            std::copy_n(s_records.begin(), s_record_count, taken.begin());
            taken.resize(s_record_count);
            s_record_count = 0;
        }
        std::vector<ResetRecord> out;
        out.reserve(taken.size());
        for (const PendingRecord &record : taken)
        {
            out.push_back(
                ResetRecord{
                    .proxy = record.proxy,
                    .model = std::string(record.model.data()),
                    .material = std::string(record.material.data()),
                    .state_before = record.state_before,
                    .reason = record.reason,
                }
            );
        }
        return out;
    }

    std::vector<ProxyOutcome> read_outcomes(const std::vector<ResetRecord> &records)
    {
        std::vector<ProxyOutcome> out;
        out.reserve(records.size());
        for (const ResetRecord &record : records)
        {
            const bool alive = record.proxy != 0 && read_ptr(record.proxy) == s_proxy_vtable;
            out.push_back(
                ProxyOutcome{
                    .record = record,
                    .state = alive ? read_state(record.proxy) : -1,
                }
            );
        }
        return out;
    }

    std::vector<std::string> blocking_attachments(std::uintptr_t proxy)
    {
        std::vector<std::string> names;
        if (proxy == 0 || read_ptr(proxy) != s_proxy_vtable)
        {
            return names;
        }
        const std::uintptr_t request = read_ptr(proxy + constants::PROXY_REQUEST_OFFSET);
        const std::uintptr_t pending = request != 0 ? read_ptr(request + constants::REQUEST_ATTACHMENT_OFFSET) : 0;
        if (pending == 0 || read_ptr(pending) != s_attachment_vtable)
        {
            return names;
        }
        const std::uintptr_t manager = read_ptr(pending + constants::ATTACHMENT_MANAGER_OFFSET);
        const std::uintptr_t instance =
            manager != 0 ? read_ptr(manager + constants::ATTACHMENT_MANAGER_INSTANCE_OFFSET) : 0;
        const std::uintptr_t begin =
            instance != 0 ? read_ptr(instance + constants::INSTANCE_ATTACHMENTS_BEGIN_OFFSET) : 0;
        const std::uintptr_t end = instance != 0 ? read_ptr(instance + constants::INSTANCE_ATTACHMENTS_END_OFFSET) : 0;
        if (begin == 0 || end <= begin || (end - begin) / sizeof(std::uintptr_t) > MAX_INSTANCE_ATTACHMENTS)
        {
            return names;
        }
        for (std::uintptr_t slot = begin; slot < end; slot += sizeof(std::uintptr_t))
        {
            const std::uintptr_t attachment = read_ptr(slot);
            if (attachment == 0 || read_ptr(attachment) != s_attachment_vtable)
            {
                continue;
            }
            const auto state = read_value<std::uint8_t>(attachment + constants::ATTACHMENT_STATE_OFFSET);
            if (!state.has_value() || *state > 1)
            {
                continue;
            }
            Name name{};
            const bool named = read_name(attachment + constants::ATTACHMENT_NAME_OFFSET, name);
            names.push_back(named ? std::string(name.data()) : std::string("<unnamed>"));
        }
        return names;
    }

} // namespace HotPak
