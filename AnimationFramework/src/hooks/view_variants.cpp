/**
 * @file hooks/view_variants.cpp
 * @brief The option and transition query detours behind custom tags (see view_variants.hpp).
 */

#include "hooks/view_variants.hpp"
#include "constants.hpp"
#include "core/tag_rules.hpp"
#include "custom_tags.hpp"
#include "forced_options.hpp"
#include "hooks/hook_install.hpp"

#include <DetourModKit.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace AnimationFramework
{
    namespace
    {
        using TagState = forced_options::TagState;

        /// SFragTagState: the global tags (the controller context's) and the fragment's own tags.
        struct FragTagState
        {
            TagState global{};
            TagState fragment{};
        };

        /// SFragmentQuery: what a scope asks the database for.
        struct FragmentQuery
        {
            std::int32_t fragment_id = 0;
            FragTagState tags;
            TagState required{};
            std::uint32_t option_index = 0;
        };

        static_assert(sizeof(FragTagState) == 0x28);
        static_assert(
            offsetof(FragmentQuery, tags) == 0x04 && offsetof(FragmentQuery, required) == 0x2C &&
            offsetof(FragmentQuery, option_index) == 0x40 && sizeof(FragmentQuery) == 0x44
        );

        /**
         * @brief A query copy with room after it: the layout check proves the fields read, not that the game's struct
         *        ends where this one does, so a grown struct still reads into the slack.
         */
        struct PaddedQuery
        {
            FragmentQuery value{};
            std::array<std::uint8_t, 0x40> slack{};
        };

        /// A matched tag state with room after it, for the same reason (the game writes 0x28 bytes).
        struct PaddedTags
        {
            FragTagState value{};
            std::array<std::uint8_t, 0x20> slack{};
        };

        /**
         * @brief An SBlendQueryResult for a trial transition query, zeroed so the filler starts from "no blend". The
         *        filler writes out to +0x6C, and the rest is slack.
         */
        struct BlendResult
        {
            std::array<std::uint8_t, 0x100> bytes{};

            /// Whether the query found a transition (its SFragmentBlend is set).
            [[nodiscard]] bool found() const noexcept
            {
                std::uintptr_t blend = 0;
                std::memcpy(&blend, bytes.data() + constants::BLEND_RESULT_BLEND_OFFSET, sizeof(blend));
                return blend != 0;
            }

            /// The found variant's source SFragTagState, global tags first.
            [[nodiscard]] std::span<const std::uint8_t> from() const noexcept
            {
                return {bytes.data() + constants::BLEND_RESULT_TAG_STATE_FROM_OFFSET, sizeof(FragTagState)};
            }

            /// The found variant's target SFragTagState, global tags first.
            [[nodiscard]] std::span<const std::uint8_t> to() const noexcept
            {
                return {bytes.data() + constants::BLEND_RESULT_TAG_STATE_TO_OFFSET, sizeof(FragTagState)};
            }
        };

        using GetBestEntryFunc = const void *(__fastcall *)(const void *database,
                                                            const FragmentQuery *query,
                                                            void *selection);
        using FindBestMatchingTagFunc = std::uint32_t(__fastcall *)(
            const void *database,
            const FragmentQuery *query,
            void *matched,
            std::uint32_t *tag_set_index
        );

        using FindBestBlendsFunc =
            void(__fastcall *)(const void *database, std::uint8_t *query, void *result1, void *result2);

        std::atomic<GetBestEntryFunc> s_get_best_entry_original{nullptr};
        std::atomic<FindBestMatchingTagFunc> s_find_best_matching_tag_original{nullptr};
        std::atomic<FindBestBlendsFunc> s_find_best_blends_original{nullptr};
        // The database's CTagDefinition field, published once both option hooks are armed. While it is 0 every
        // detour passes its query through, so the two option queries never disagree.
        std::atomic<std::ptrdiff_t> s_tag_defs_offset{0};

        /// A tag's place in a tag state: the byte and the mask of its bit, mask 0 when the definition lacks the tag.
        struct TagBits
        {
            std::uint8_t byte = 0;
            std::uint8_t mask = 0;
        };

        /** @brief Where one tag definition keeps `player` and each custom tag of one generation and epoch. */
        struct DefinitionBits
        {
            std::uintptr_t definition = 0;
            /// cache_version() of the snapshot generation and tag definition epoch the bits were read for.
            std::uint64_t version = 0;
            TagBits player;
            std::array<TagBits, tag_rules::MAX_CUSTOM_TAGS> tags{};
            /// Bit i: the definition has custom tag i.
            std::uint32_t present = 0;
        };

        // Entries are filled once and then published into a cache slot. None is ever reused, so a query that read a
        // pointer keeps a valid entry. Few exist (one per character type, tag generation and tag definition epoch). The
        // cache has a slot for every pool entry: with fewer slots than character types, each miss past the last slot
        // walks the whole definition again on every query. Only an entry of an older version gives its slot up, so a
        // query that still holds the previous snapshot never evicts the current version's entries. Once the pool is
        // used up (many mn_reload runs in one session), every query reads its definition into a scratch entry: slower,
        // still correct.
        constexpr std::size_t BITS_POOL_SIZE = 64;
        std::array<DefinitionBits, BITS_POOL_SIZE> s_bits_pool{};
        std::atomic<std::size_t> s_bits_pool_used{0};
        std::array<std::atomic<const DefinitionBits *>, BITS_POOL_SIZE> s_bits_cache{};

        /**
         * @brief The cache key of @p snapshot's generation and the tag definition epoch (custom_tags.hpp). It grows
         *        with either, so an entry with a lower version is stale.
         */
        [[nodiscard]] std::uint64_t cache_version(const CustomTagSnapshot &snapshot, std::uint32_t epoch) noexcept
        {
            return (static_cast<std::uint64_t>(snapshot.generation) << 32) | epoch;
        }

        /** @brief The {byte, mask} of the plain tag at @p index, or none (grouped tags have no bit of their own). */
        [[nodiscard]] TagBits tag_bits(std::uintptr_t masks, std::uintptr_t record, std::int32_t index) noexcept
        {
            const auto group =
                DMK::memory::read<std::int32_t>(DMK::Address{record + constants::TAG_DEFINITION_TAG_GROUP_OFFSET});
            const auto pair = DMK::memory::read<std::array<std::uint8_t, 2>>(
                DMK::Address{masks + 2 * static_cast<std::uintptr_t>(index)}
            );
            if (!group || *group != -1 || !pair || (*pair)[0] >= constants::MANNEQUIN_TAG_STATE_BYTES ||
                (*pair)[1] == 0)
            {
                return {};
            }
            return TagBits{.byte = (*pair)[0], .mask = (*pair)[1]};
        }

        /**
         * @brief Reads `player` and @p snapshot's tags out of the CTagDefinition at @p definition into @p out, keyed by
         *        @p version.
         */
        void read_bits(
            std::uintptr_t definition,
            const CustomTagSnapshot &snapshot,
            std::uint64_t version,
            DefinitionBits &out
        ) noexcept
        {
            constexpr std::int32_t max_tags = 0x4000;
            static constexpr std::uint32_t player_crc = tag_rules::mannequin_crc(constants::PLAYER_TAG);
            out = DefinitionBits{
                .definition = definition,
                .version = version,
            };
            const auto tags =
                DMK::memory::read<std::uintptr_t>(DMK::Address{definition + constants::TAG_DEFINITION_TAGS_OFFSET});
            const auto masks = DMK::memory::read<std::uintptr_t>(
                DMK::Address{definition + constants::TAG_DEFINITION_TAG_MASKS_OFFSET}
            );
            if (!tags || !masks || *tags == 0 || *masks == 0)
            {
                return;
            }
            const auto count = DMK::memory::read<std::int32_t>(DMK::Address{*tags + constants::DYN_ARRAY_COUNT_OFFSET});
            if (!count || *count <= 0 || *count > max_tags)
            {
                return;
            }
            for (std::int32_t index = 0; index < *count; ++index)
            {
                const std::uintptr_t record =
                    *tags + static_cast<std::uintptr_t>(index) * constants::TAG_DEFINITION_TAG_STRIDE;
                const auto crc =
                    DMK::memory::read<std::uint32_t>(DMK::Address{record + constants::TAG_DEFINITION_TAG_CRC_OFFSET});
                if (!crc)
                {
                    out.player = {};
                    return;
                }
                if (*crc == player_crc)
                {
                    out.player = tag_bits(*masks, record, index);
                    continue;
                }
                for (std::uint32_t i = 0; i < snapshot.count; ++i)
                {
                    if (*crc == snapshot.crc[i])
                    {
                        out.tags[i] = tag_bits(*masks, record, index);
                        if (out.tags[i].mask != 0)
                        {
                            out.present |= 1u << i;
                        }
                    }
                }
            }
        }

        /// Logs which custom tags a definition with a `player` tag holds, once per cached definition and version.
        void log_bits(const DefinitionBits &bits, const CustomTagSnapshot &snapshot) noexcept
        {
            if (bits.player.mask == 0)
            {
                return;
            }
            try
            {
                std::string found;
                std::string missing;
                for (std::uint32_t i = snapshot.count; i-- > 0;)
                {
                    std::string &list = bits.tags[i].mask != 0 ? found : missing;
                    list += (list.empty() ? "" : ", ") + std::string{snapshot.name[i].data()};
                }
                (void)DMK::log().try_log(
                    DMK::LogLevel::Info,
                    "TagVariants: tag definition {:#x} (generation {}): custom tags {}{}",
                    bits.definition,
                    snapshot.generation,
                    found.empty() ? std::string{"none"} : found,
                    missing.empty() ? std::string{} : "; not in it: " + missing
                );
            }
            catch (...)
            {
            }
        }

        /**
         * @brief The bits of the CTagDefinition at @p definition for @p snapshot, read once per definition, generation
         *        and tag definition epoch, or nullptr when the definition has no `player` tag (or cannot be read).
         * @param scratch Holds the answer when the cache is full, for this query only.
         */
        [[nodiscard]] const DefinitionBits *definition_bits_of(
            std::uintptr_t definition,
            const CustomTagSnapshot &snapshot,
            DefinitionBits &scratch
        ) noexcept
        {
            if (definition == 0)
            {
                return nullptr;
            }
            const std::uint64_t version = cache_version(snapshot, tag_definition_epoch());
            std::atomic<const DefinitionBits *> *free_slot = nullptr;
            const DefinitionBits *stale = nullptr;
            for (std::atomic<const DefinitionBits *> &slot : s_bits_cache)
            {
                const DefinitionBits *entry = slot.load(std::memory_order_acquire);
                if (entry == nullptr)
                {
                    free_slot = free_slot != nullptr ? free_slot : &slot;
                    continue;
                }
                if (entry->definition == definition && entry->version == version)
                {
                    return entry->player.mask != 0 ? entry : nullptr;
                }
                if (free_slot == nullptr && entry->version < version)
                {
                    free_slot = &slot;
                    stale = entry;
                }
            }
            DefinitionBits *target = &scratch;
            if (free_slot != nullptr)
            {
                const std::size_t index = s_bits_pool_used.fetch_add(1, std::memory_order_relaxed);
                if (index < BITS_POOL_SIZE)
                {
                    target = &s_bits_pool[index];
                }
            }
            read_bits(definition, snapshot, version, *target);
            if (target != &scratch)
            {
                const DefinitionBits *expected = stale;
                if (free_slot->compare_exchange_strong(expected, target, std::memory_order_acq_rel))
                {
                    log_bits(*target, snapshot);
                }
            }
            return target->player.mask != 0 ? target : nullptr;
        }

        /** @brief definition_bits_of for @p database's tag definition (the field at @p tag_defs_offset). */
        [[nodiscard]] const DefinitionBits *definition_bits(
            const void *database,
            std::ptrdiff_t tag_defs_offset,
            const CustomTagSnapshot &snapshot,
            DefinitionBits &scratch
        ) noexcept
        {
            const auto definition = DMK::memory::read<std::uintptr_t>(
                DMK::Address{reinterpret_cast<std::uintptr_t>(database) + tag_defs_offset}
            );
            return definition ? definition_bits_of(*definition, snapshot, scratch) : nullptr;
        }

        /** @brief Henry's tag bits and the active custom tags his definition has (none: nothing to do). */
        struct Context
        {
            const DefinitionBits *bits = nullptr;
            std::uint32_t active = 0;
        };

        /// The Context of a query to @p database, empty while no custom tag of its definition is on.
        [[nodiscard]] Context context_for(const void *database, DefinitionBits &scratch) noexcept
        {
            const std::ptrdiff_t tag_defs_offset = s_tag_defs_offset.load(std::memory_order_acquire);
            const CustomTagSnapshot *snapshot = custom_tag_snapshot();
            if (tag_defs_offset == 0 || database == nullptr || snapshot == nullptr)
            {
                return {};
            }
            const std::uint32_t active = custom_tags_active(*snapshot);
            if (active == 0)
            {
                return {};
            }
            const DefinitionBits *bits = definition_bits(database, tag_defs_offset, *snapshot, scratch);
            if (bits == nullptr)
            {
                return {};
            }
            return Context{.bits = bits, .active = tag_rules::capped(active & bits->present)};
        }

        /// Whether the tag state @p tags sets @p bit (never for a tag the definition lacks).
        [[nodiscard]] bool has(const std::uint8_t *tags, TagBits bit) noexcept
        {
            return bit.mask != 0 && (tags[bit.byte] & bit.mask) != 0;
        }

        /** @brief Sets every custom tag of @p subset in the global tags @p tags. */
        void add_tags(std::uint8_t *tags, const DefinitionBits &bits, std::uint32_t subset) noexcept
        {
            for (std::uint32_t i = 0; subset != 0; ++i, subset >>= 1)
            {
                if ((subset & 1u) != 0)
                {
                    tags[bits.tags[i].byte] = static_cast<std::uint8_t>(tags[bits.tags[i].byte] | bits.tags[i].mask);
                }
            }
        }

        /** @brief Whether every custom tag of @p subset is set in the global tags @p a or @p b (b may be null). */
        [[nodiscard]] bool carries_all(
            const std::uint8_t *a,
            const std::uint8_t *b,
            const DefinitionBits &bits,
            std::uint32_t subset
        ) noexcept
        {
            for (std::uint32_t i = 0; subset != 0; ++i, subset >>= 1)
            {
                if ((subset & 1u) != 0 && !has(a, bits.tags[i]) && (b == nullptr || !has(b, bits.tags[i])))
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief The query Henry's option lookup runs: @p query with the custom tags of the first combination
         *        (tag_rules::next_subset order) whose best option carries all of them and covers the game's own best
         *        option (built in @p copy), else @p query untouched. A custom-tagged option thereby only ever replaces
         *        the option the game picks, never one it prefers.
         * @details CAnimationDatabase::FindBestMatchingTag answers the option count of the best tag set and writes
         *          that set's tags (each lookup takes the database's lock itself).
         */
        [[nodiscard]] const FragmentQuery *
        choose_query(const void *database, const FragmentQuery *query, PaddedQuery &copy) noexcept
        {
            if (query == nullptr)
            {
                return query;
            }
            DefinitionBits scratch;
            const Context context = context_for(database, scratch);
            if (context.active == 0 || !has(query->tags.global.data(), context.bits->player))
            {
                return query;
            }
            const FindBestMatchingTagFunc match = s_find_best_matching_tag_original.load(std::memory_order_acquire);
            if (match == nullptr)
            {
                return query;
            }
            PaddedTags plain;
            const std::uint32_t plain_count = match(database, query, &plain.value, nullptr);
            for (std::uint32_t subset = context.active; subset != 0;
                 subset = tag_rules::next_subset(context.active, subset))
            {
                copy.value = *query;
                add_tags(copy.value.tags.global.data(), *context.bits, subset);
                PaddedTags view;
                if (match(database, &copy.value, &view.value, nullptr) == 0 ||
                    !carries_all(view.value.global.data(), nullptr, *context.bits, subset))
                {
                    continue;
                }
                if (plain_count == 0 || (tag_rules::covers(view.value.global, plain.value.global) &&
                                         tag_rules::covers(view.value.fragment, plain.value.fragment)))
                {
                    return &copy.value;
                }
            }
            return query;
        }

        /**
         * @brief @p query with the tags a script forced for its fragment (forced_options.hpp), built in @p copy, else
         *        @p query.
         */
        [[nodiscard]] const FragmentQuery *forced_query(const FragmentQuery *query, PaddedQuery &copy) noexcept
        {
            if (query == nullptr || !forced_options::active())
            {
                return query;
            }
            copy.value = *query;
            return forced_options::apply(query->fragment_id, copy.value.tags.global.data()) ? &copy.value : query;
        }

        /**
         * @brief CAnimationDatabase::GetBestEntry detour: Henry's forced tags (forced_query), then his custom-tagged
         *        option where it applies (choose_query).
         * @note Game animation threads. No-throw.
         */
        const void *__fastcall
        detour_get_best_entry(const void *database, const FragmentQuery *query, void *selection) noexcept
        {
            const GetBestEntryFunc original = s_get_best_entry_original.load(std::memory_order_acquire);
            if (original == nullptr)
            {
                return nullptr;
            }
            PaddedQuery forced;
            PaddedQuery copy;
            return original(database, choose_query(database, forced_query(query, forced), copy), selection);
        }

        /**
         * @brief CAnimationDatabase::FindBestMatchingTag detour: the same decision as detour_get_best_entry, so
         *        IsDifferent agrees with the option queued (a disagreement re-queues the fragment every frame).
         * @note Game animation threads. No-throw.
         */
        std::uint32_t __fastcall detour_find_best_matching_tag(
            const void *database,
            const FragmentQuery *query,
            void *matched,
            std::uint32_t *tag_set_index
        ) noexcept
        {
            const FindBestMatchingTagFunc original = s_find_best_matching_tag_original.load(std::memory_order_acquire);
            if (original == nullptr)
            {
                return 0;
            }
            PaddedQuery forced;
            PaddedQuery copy;
            return original(
                database,
                choose_query(database, forced_query(query, forced), copy),
                matched,
                tag_set_index
            );
        }

        /**
         * @brief CAnimationDatabase::FindBestBlends detour: Henry's transitions follow the option rule. The game's own
         *        transition is looked up first. Then each combination of active custom tags (next_subset order) is
         *        tried in the source and target tag states that carry `player`, and the first variant that carries
         *        all of them and covers the game's variant (both tag states) is the one the caller gets.
         * @details The bits go into the caller's own query for each call and come out again after it. A copy needs
         *          the struct's full size, and the query is the caller's local, read only on this thread meanwhile.
         *          Trial calls fill zeroed BlendResults. The chosen query then runs once more into the caller's
         *          results, so they are filled exactly as the game fills them.
         * @note Game animation threads. No-throw.
         */
        void __fastcall
        detour_find_best_blends(const void *database, std::uint8_t *query, void *result1, void *result2) noexcept
        {
            const FindBestBlendsFunc original = s_find_best_blends_original.load(std::memory_order_acquire);
            if (original == nullptr)
            {
                return;
            }
            DefinitionBits scratch;
            const Context context = query != nullptr ? context_for(database, scratch) : Context{};
            if (context.active == 0)
            {
                original(database, query, result1, result2);
                return;
            }
            std::uint8_t *const from = query + constants::BLEND_QUERY_TAG_STATE_FROM_OFFSET;
            std::uint8_t *const to = query + constants::BLEND_QUERY_TAG_STATE_TO_OFFSET;
            const bool from_player = has(from, context.bits->player);
            const bool to_player = has(to, context.bits->player);
            if (!from_player && !to_player)
            {
                original(database, query, result1, result2);
                return;
            }
            TagState saved_from{};
            TagState saved_to{};
            std::memcpy(saved_from.data(), from, saved_from.size());
            std::memcpy(saved_to.data(), to, saved_to.size());
            const auto mark = [&](std::uint32_t subset)
            {
                if (from_player)
                {
                    add_tags(from, *context.bits, subset);
                }
                if (to_player)
                {
                    add_tags(to, *context.bits, subset);
                }
            };
            const auto restore = [&]()
            {
                std::memcpy(from, saved_from.data(), saved_from.size());
                std::memcpy(to, saved_to.data(), saved_to.size());
            };
            BlendResult plain1;
            BlendResult plain2;
            original(database, query, &plain1, &plain2);
            for (std::uint32_t subset = context.active; subset != 0;
                 subset = tag_rules::next_subset(context.active, subset))
            {
                mark(subset);
                BlendResult trial1;
                BlendResult trial2;
                original(database, query, &trial1, &trial2);
                restore();
                if (!trial1.found() || !carries_all(trial1.from().data(), trial1.to().data(), *context.bits, subset))
                {
                    continue;
                }
                if (plain1.found() &&
                    (!tag_rules::covers(trial1.from(), plain1.from()) || !tag_rules::covers(trial1.to(), plain1.to())))
                {
                    continue;
                }
                mark(subset);
                original(database, query, result1, result2);
                restore();
                return;
            }
            original(database, query, result1, result2);
        }

        using IsDifferentFunc = bool(__fastcall *)(
            const void *scope,
            std::uint32_t fragment,
            const void *fragment_tags,
            std::int32_t sub_context
        );
        std::atomic<IsDifferentFunc> s_is_different_original{nullptr};

        /** @brief Clears every custom tag of @p subset in the global tags @p tags. */
        void remove_tags(std::uint8_t *tags, const DefinitionBits &bits, std::uint32_t subset) noexcept
        {
            for (std::uint32_t i = 0; subset != 0; ++i, subset >>= 1)
            {
                if ((subset & 1u) != 0)
                {
                    tags[bits.tags[i].byte] = static_cast<std::uint8_t>(tags[bits.tags[i].byte] & ~bits.tags[i].mask);
                }
            }
        }

        /**
         * @brief CActionScope::IsDifferent detour: a custom tag change re-queries Henry's running fragments.
         * @details IsDifferent answers from a cache keyed by the fragment, its tags and the scope context's tag mask.
         *          The custom tags live only in the query copies the option detours build, never in that mask, so
         *          when one turns on or off (the view switching while Henry stands still) the key looks the same and
         *          the cached option keeps playing until something else changes (his next move). The detour keeps the
         *          active custom tags stamped into the cached mask's own bits for them, which the scope's real mask
         *          never sets. While they stay the same, it lifts the stamp for the call, so the cache works as in the
         *          game. Once they differ, it leaves every custom bit set, so the compare misses, the database is asked
         *          again through the option detours and an AutoReinstall fragment (idle, movement, held items)
         *          reinstalls the option for the new tags. The stamp names the tags of the last re-query, so a call
         *          that returns before the cache (another fragment on the scope) keeps it. Henry's scopes only (their
         *          mask carries `player`). A stamp left after the hooks retire costs each scope one extra query, as
         *          the game then rewrites the mask.
         * @note Game threads. No-throw.
         */
        bool __fastcall detour_is_different(
            const void *scope,
            std::uint32_t fragment,
            const void *fragment_tags,
            std::int32_t sub_context
        ) noexcept
        {
            const IsDifferentFunc original = s_is_different_original.load(std::memory_order_acquire);
            if (original == nullptr)
            {
                return false;
            }
            const CustomTagSnapshot *snapshot = custom_tag_snapshot();
            if (snapshot == nullptr || snapshot->count == 0 || scope == nullptr)
            {
                return original(scope, fragment, fragment_tags, sub_context);
            }
            const auto scope_address = reinterpret_cast<std::uintptr_t>(scope);
            const auto context = DMK::memory::read<std::uintptr_t>(
                DMK::Address{scope_address + static_cast<std::uintptr_t>(constants::ACTION_SCOPE_CONTEXT_OFFSET)}
            );
            if (!context || *context == 0)
            {
                return original(scope, fragment, fragment_tags, sub_context);
            }
            const auto definition = DMK::memory::read<std::uintptr_t>(
                DMK::Address{*context + static_cast<std::uintptr_t>(constants::ANIMATION_CONTEXT_TAG_DEFINITION_OFFSET)}
            );
            DefinitionBits scratch;
            const DefinitionBits *bits = definition ? definition_bits_of(*definition, *snapshot, scratch) : nullptr;
            const auto *live = reinterpret_cast<const std::uint8_t *>(
                *context + static_cast<std::uintptr_t>(constants::ANIMATION_CONTEXT_TAG_STATE_OFFSET)
            );
            if (bits == nullptr || bits->present == 0 || !has(live, bits->player))
            {
                return original(scope, fragment, fragment_tags, sub_context);
            }
            auto *cached = reinterpret_cast<std::uint8_t *>(
                scope_address + static_cast<std::uintptr_t>(constants::ACTION_SCOPE_CACHED_CONTEXT_MASK_OFFSET)
            );
            const std::uint32_t now = tag_rules::capped(custom_tags_active(*snapshot) & bits->present);
            std::uint32_t stamped = 0;
            for (std::uint32_t i = 0; i < tag_rules::MAX_CUSTOM_TAGS; ++i)
            {
                if (((bits->present >> i) & 1u) != 0 && has(cached, bits->tags[i]))
                {
                    stamped |= 1u << i;
                }
            }
            const bool force_miss = stamped != now;
            if (force_miss)
            {
                add_tags(cached, *bits, bits->present);
            }
            else
            {
                remove_tags(cached, *bits, bits->present);
            }
            const bool different = original(scope, fragment, fragment_tags, sub_context);
            // A re-query overwrites the cached mask with the scope's real mask, which never carries a custom bit. Bits
            // that are still set mean the original returned without a re-query (the scope plays another fragment), so
            // the cached option still belongs to the stamped tags.
            const std::uint32_t queried_under =
                force_miss && carries_all(cached, nullptr, *bits, bits->present) ? stamped : now;
            remove_tags(cached, *bits, bits->present);
            add_tags(cached, *bits, queried_under);
            return different;
        }
    } // namespace

    void install_view_variant_hooks(const Anchors &anchors, DMK::hook::HookStack &hooks)
    {
        DMK::Logger &logger = DMK::log();
        if (anchors.get_best_entry == 0 || anchors.find_best_matching_tag == 0 || anchors.database_tag_defs_offset == 0)
        {
            logger.warning("TagVariants: the option queries did not resolve; custom tags are off");
            return;
        }
        // Every detour passes its query through until the offset is published, which happens only once both option
        // hooks are armed. A failed second arm leaves the first one a pass, retired with the stack at shutdown.
        if (auto installed = install_inline_hook<&detour_find_best_matching_tag>(
                hooks,
                "AnimationDatabaseFindBestMatchingTag",
                anchors.find_best_matching_tag,
                s_find_best_matching_tag_original
            );
            !installed.has_value())
        {
            logger.warning(
                "TagVariants: FindBestMatchingTag hook failed ({}); custom tags are off",
                installed.error().message()
            );
            return;
        }
        if (auto installed = install_inline_hook<&detour_get_best_entry>(
                hooks,
                "AnimationDatabaseGetBestEntry",
                anchors.get_best_entry,
                s_get_best_entry_original
            );
            !installed.has_value())
        {
            logger.warning(
                "TagVariants: GetBestEntry hook failed ({}); custom tags are off",
                installed.error().message()
            );
            return;
        }
        // Transitions are optional: without them options still follow the tags and transitions keep the game's pick.
        bool blends = false;
        if (anchors.find_best_blends == 0)
        {
            logger.warning("TagVariants: FindBestBlends did not resolve; transitions ignore custom tags");
        }
        else if (
            auto installed = install_inline_hook<&detour_find_best_blends>(
                hooks,
                "AnimationDatabaseFindBestBlends",
                anchors.find_best_blends,
                s_find_best_blends_original
            );
            !installed.has_value()
        )
        {
            logger.warning(
                "TagVariants: FindBestBlends hook failed ({}); transitions ignore custom tags",
                installed.error().message()
            );
        }
        else
        {
            blends = true;
        }
        s_tag_defs_offset.store(anchors.database_tag_defs_offset, std::memory_order_release);
        forced_options::set_available(true);
        if (anchors.action_scope_is_different == 0)
        {
            logger.warning(
                "TagVariants: IsDifferent did not resolve; a tag change while Henry stands still waits for "
                "his next move"
            );
        }
        else if (
            auto installed = install_inline_hook<&detour_is_different>(
                hooks,
                "ActionScopeIsDifferent",
                anchors.action_scope_is_different,
                s_is_different_original
            );
            !installed.has_value()
        )
        {
            logger.warning(
                "TagVariants: IsDifferent hook failed ({}); a tag change while Henry stands still waits for his next "
                "move",
                installed.error().message()
            );
        }
        logger.info(
            "TagVariants: options{} tagged with custom tags play for Henry while the tags are on (GetBestEntry {}, "
            "FindBestMatchingTag {}, FindBestBlends {})",
            blends ? " and transitions" : "",
            DMK::format::format_address(anchors.get_best_entry),
            DMK::format::format_address(anchors.find_best_matching_tag),
            blends ? DMK::format::format_address(anchors.find_best_blends) : std::string{"off"}
        );
    }
} // namespace AnimationFramework
