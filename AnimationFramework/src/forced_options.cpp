/**
 * @file forced_options.cpp
 * @brief The option a script asks for (see forced_options.hpp).
 */

#include "forced_options.hpp"
#include "core/tag_rules.hpp"
#include "game_memory.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace AnimationFramework::forced_options
{
    namespace
    {
        /**
         * @brief Long enough for the fragment to start (a frame or two), short enough that the game's own later query
         *        for the same fragment, such as Henry's real juke when he runs, keeps its own tags.
         */
        constexpr std::chrono::milliseconds FORCE_WINDOW{1000};
        /// A queued fragment picks its option only when the one before it ends.
        constexpr std::chrono::milliseconds QUEUED_FORCE_WINDOW{60000};
        /**
         * @brief SControllerDef::m_fragmentIDs, the CTagDefinition of the fragment names, which the play code reads as
         *        `mov rcx, [rcx+18h]` (npc_actions.cpp checks it). Its tag indices are the fragment IDs.
         */
        constexpr std::ptrdiff_t CONTROLLER_DEF_FRAGMENT_IDS_OFFSET = 0x18;
        constexpr std::int32_t MAX_TAGS = 0x4000;
        constexpr std::int32_t NO_GROUP = -1;
        /// Reads of a forcing that a concurrent force() may tear before a query gives up and keeps the game's tags.
        constexpr int LOAD_ATTEMPTS = 4;

        /** @brief One forcing, copied whole into and out of s_words. */
        struct Forced
        {
            /// The steady_clock tick count after which the forcing lapses.
            std::int64_t until = 0;
            std::int32_t fragment_id = 0;
            std::uint8_t player_byte = 0;
            std::uint8_t player_mask = 0;
            TagState before{};
            TagState after{};
        };
        static_assert(std::is_trivially_copyable_v<Forced>);
        constexpr std::size_t FORCED_WORDS = (sizeof(Forced) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
        using ForcedWords = std::array<std::uint64_t, FORCED_WORDS>;

        std::atomic<bool> s_available{false};
        // A sequence lock, so the option queries on the animation threads take no lock: force(), on the main thread
        // and the only writer, keeps s_sequence odd while it rewrites s_words, and a reader that sees the sequence
        // change under its copy discards the copy.
        std::atomic<std::uint64_t> s_sequence{0};
        std::array<std::atomic<std::uint64_t>, FORCED_WORDS> s_words{};
        // The sequence value of the live forcing, 0 for none.
        std::atomic<std::uint64_t> s_active{0};

        /** @brief The index of the tag whose name hashes to @p crc in the CTagDefinition at @p definition, or -1. */
        [[nodiscard]] std::int32_t tag_index(std::uintptr_t definition, std::uint32_t crc) noexcept
        {
            const std::uintptr_t tags = read_ptr(definition + constants::TAG_DEFINITION_TAGS_OFFSET);
            const std::int32_t count =
                tags != 0 ? read_value<std::int32_t>(tags + constants::DYN_ARRAY_COUNT_OFFSET).value_or(0) : 0;
            for (std::int32_t index = 0; index < count && index < MAX_TAGS; ++index)
            {
                const std::uintptr_t record =
                    tags + static_cast<std::uintptr_t>(index) * constants::TAG_DEFINITION_TAG_STRIDE;
                if (read_value<std::uint32_t>(record + constants::TAG_DEFINITION_TAG_CRC_OFFSET) == crc)
                {
                    return index;
                }
            }
            return -1;
        }

        /** @brief Makes @p forced the live forcing. Main thread only. */
        void publish(const Forced &forced) noexcept
        {
            ForcedWords words{};
            std::memcpy(words.data(), &forced, sizeof(forced));
            const std::uint64_t sequence = s_sequence.load(std::memory_order_relaxed);
            s_sequence.store(sequence + 1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            for (std::size_t i = 0; i < FORCED_WORDS; ++i)
            {
                s_words[i].store(words[i], std::memory_order_relaxed);
            }
            s_sequence.store(sequence + 2, std::memory_order_release);
            s_active.store(sequence + 2, std::memory_order_release);
        }

        /** @brief Copies the last published forcing into @p out, or returns false when every read raced a force(). */
        [[nodiscard]] bool load(Forced &out) noexcept
        {
            for (int attempt = 0; attempt < LOAD_ATTEMPTS; ++attempt)
            {
                const std::uint64_t sequence = s_sequence.load(std::memory_order_acquire);
                if ((sequence & 1u) != 0)
                {
                    continue;
                }
                ForcedWords words{};
                for (std::size_t i = 0; i < FORCED_WORDS; ++i)
                {
                    words[i] = s_words[i].load(std::memory_order_relaxed);
                }
                std::atomic_thread_fence(std::memory_order_acquire);
                if (s_sequence.load(std::memory_order_relaxed) == sequence)
                {
                    std::memcpy(&out, words.data(), sizeof(out));
                    return true;
                }
            }
            return false;
        }
    } // namespace

    void set_available(bool available) noexcept
    {
        s_available.store(available, std::memory_order_release);
    }

    bool available() noexcept
    {
        return s_available.load(std::memory_order_acquire);
    }

    std::int32_t resolve_fragment(std::uintptr_t context, const char *fragment) noexcept
    {
        const std::uintptr_t controller_def = context != 0 ? read_ptr(context) : 0;
        const std::uintptr_t fragment_ids =
            controller_def != 0 ? read_ptr(controller_def + CONTROLLER_DEF_FRAGMENT_IDS_OFFSET) : 0;
        return fragment != nullptr && fragment_ids != 0 ? tag_index(fragment_ids, tag_rules::mannequin_crc(fragment))
                                                        : -1;
    }

    bool force(
        std::uintptr_t context,
        std::int32_t fragment_id,
        const TagState &before,
        const TagState &after,
        bool queued
    ) noexcept
    {
        static constexpr std::uint32_t player_crc = tag_rules::mannequin_crc(constants::PLAYER_TAG);
        const std::uintptr_t definition = read_ptr(context + constants::ANIMATION_CONTEXT_TAG_DEFINITION_OFFSET);
        const std::int32_t player = definition != 0 ? tag_index(definition, player_crc) : -1;
        if (fragment_id < 0 || player < 0)
        {
            return false;
        }
        // `player` is a plain tag: no group, its own byte and bit.
        const std::uintptr_t record = read_ptr(definition + constants::TAG_DEFINITION_TAGS_OFFSET) +
                                      static_cast<std::uintptr_t>(player) * constants::TAG_DEFINITION_TAG_STRIDE;
        const auto bits = read_value<std::array<std::uint8_t, 2>>(
            read_ptr(definition + constants::TAG_DEFINITION_TAG_MASKS_OFFSET) + 2 * static_cast<std::uintptr_t>(player)
        );
        if (read_value<std::int32_t>(record + constants::TAG_DEFINITION_TAG_GROUP_OFFSET) != NO_GROUP || !bits ||
            (*bits)[0] >= constants::MANNEQUIN_TAG_STATE_BYTES || (*bits)[1] == 0)
        {
            return false;
        }
        const auto until = std::chrono::steady_clock::now() + (queued ? QUEUED_FORCE_WINDOW : FORCE_WINDOW);
        publish(
            Forced{
                .until = until.time_since_epoch().count(),
                .fragment_id = fragment_id,
                .player_byte = (*bits)[0],
                .player_mask = (*bits)[1],
                .before = before,
                .after = after,
            }
        );
        return true;
    }

    void clear() noexcept
    {
        s_active.store(0, std::memory_order_release);
    }

    bool active() noexcept
    {
        return s_active.load(std::memory_order_acquire) != 0;
    }

    bool apply(std::int32_t fragment_id, std::uint8_t *global) noexcept
    {
        std::uint64_t live = s_active.load(std::memory_order_acquire);
        Forced forced{};
        if (live == 0 || global == nullptr || !load(forced) || fragment_id != forced.fragment_id ||
            (global[forced.player_byte] & forced.player_mask) == 0)
        {
            return false;
        }
        if (std::chrono::steady_clock::now().time_since_epoch().count() > forced.until)
        {
            // Ends only the forcing that lapsed: a later force() has another sequence value.
            (void)s_active.compare_exchange_strong(live, 0, std::memory_order_acq_rel);
            return false;
        }
        tag_rules::take_changed_bits({global, constants::MANNEQUIN_TAG_STATE_BYTES}, forced.after, forced.before);
        return true;
    }
} // namespace AnimationFramework::forced_options
