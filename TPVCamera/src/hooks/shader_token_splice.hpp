/** @file hooks/shader_token_splice.hpp
 *  @brief Derives a hole twin's Z-pass pixel-shader token map from the stock one's, without parsing the twin (the DEV
 *         twin clone). Pure and bounded: no engine calls, no memory beyond the caller's arena, and the work is linear
 *         in a stock map that shader_token_compare.hpp's bounds already cap.
 */
#ifndef TPVCAMERA_HOOKS_SHADER_TOKEN_SPLICE_HPP
#define TPVCAMERA_HOOKS_SHADER_TOKEN_SPLICE_HPP

#include "shader_token_compare.hpp"
#include "generated/shader_twin_blobs.hpp"
#include "generated/shader_twin_splices.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <span>
#include <string_view>

namespace TPVCamera::ShaderSplice
{
    /** @brief The Z-pass pixel shader a token map belongs to. */
    enum class Entry : std::uint8_t
    {
        ZPrePass, // Common_ZPrePassPS
        ZPass,    // Common_ZPassPS
    };

    /** @brief Why a splice failed. Every anchor must occur once and the hole's dependencies be emitted. */
    enum class Status : std::uint8_t
    {
        Ok,
        BadTokenMap,
        Signature,
        EarlyDepth,
        DissolveCall,
        Dependency,
        HoleAnchor,
        WorldViewPosAnchor,
        AnchorOrder,
        TableConflict,
        ArenaFull,
    };

    /** @brief A status for logs. */
    [[nodiscard]] constexpr std::string_view describe(Status status) noexcept
    {
        switch (status)
        {
        case Status::Ok:
            return "spliced";
        case Status::BadTokenMap:
            return "the stock token map is malformed or too large";
        case Status::Signature:
            return "the pixel shader's signature is not there exactly once";
        case Status::EarlyDepth:
            return "the Z pre-pass has no early-depth keyword in front of its signature";
        case Status::DissolveCall:
            return "the ApplyDissolve call is not there exactly once";
        case Status::Dependency:
            return "CBPerViewGlobal or GetDissolveThreshold17 is not emitted";
        case Status::HoleAnchor:
            return "GetAlphaCoverageMask is not emitted exactly once";
        case Status::WorldViewPosAnchor:
            return "the fragment GetWorldViewPos follows is not emitted exactly once";
        case Status::AnchorOrder:
            return "the fragment GetWorldViewPos follows ends after GetAlphaCoverageMask";
        case Status::TableConflict:
            return "an added string's id names other text in the stock table";
        case Status::ArenaFull:
            return "the arena is too small";
        }
        return "unknown";
    }

    /** @brief The engine's TArray<uint32> SHData: the HW shader factory reads the pointer and a 32-bit count. */
    struct TokenArray
    {
        std::uint32_t *elements;
        std::uint32_t count;
        std::uint32_t capacity;
    };
    static_assert(sizeof(TokenArray) == 16);

    /** @brief The engine's STokenD: an id and its CryStringT text (mfStoreCacheTokenMap steps by 16 bytes). */
    struct TokenEntry
    {
        std::uint32_t id;
        std::uint32_t padding;
        const char *text;
    };
    static_assert(sizeof(TokenEntry) == 16 && offsetof(TokenEntry, text) == 8);

    /** @brief The engine's std::vector<STokenD>, the factory's string table. */
    struct TokenTable
    {
        TokenEntry *first;
        TokenEntry *last;
        TokenEntry *end;
    };

    /** @brief The CryStringT header right in front of each text; mfStoreCacheTokenMap reads the length at text - 8. */
    struct StringHeader
    {
        std::int32_t references;
        std::int32_t length;
        std::int32_t allocated;
    };
    static_assert(sizeof(StringHeader) == 12);

    // No engine copy can release this count to zero, so the engine never frees arena memory.
    inline constexpr std::int32_t k_pinned_references = 0x40000000;

    /** @brief A bump allocator over caller-owned bytes, which must come from an allocation or a std::byte array (they
     *         create the objects implicitly). Nothing is freed until the caller drops the whole arena.
     */
    class Arena
    {
    public:
        /// Hands out the bytes of memory from its start.
        explicit Arena(std::span<std::byte> memory) noexcept : m_memory(memory) {}

        /** @brief bytes at the given alignment, or nullptr when the rest of the arena is too small. */
        [[nodiscard]] void *allocate_bytes(std::size_t bytes, std::size_t alignment) noexcept
        {
            void *at = m_memory.data() + m_used;
            std::size_t space = m_memory.size() - m_used;
            if (std::align(alignment, bytes, at, space) == nullptr)
            {
                return nullptr;
            }
            m_used = m_memory.size() - space + bytes;
            return at;
        }

        /** @brief count objects of T, or nullptr when the rest of the arena is too small. */
        template <typename T> [[nodiscard]] T *allocate(std::size_t count) noexcept
        {
            if (count > (m_memory.size() - m_used) / sizeof(T))
            {
                return nullptr;
            }
            return static_cast<T *>(allocate_bytes(count * sizeof(T), alignof(T)));
        }

    private:
        std::span<std::byte> m_memory;
        std::size_t m_used = 0;
    };

    /** @brief A token map read in place (mfStoreCacheTokenMap's layout, see shader_token_compare.hpp). */
    struct TokenMap
    {
        std::span<const std::uint8_t> tokens; // count tokens of 4 bytes, not necessarily aligned
        std::span<const std::uint8_t> table;  // the (id, ASCIIZ) records, sorted by id
        std::uint32_t count = 0;
        std::uint32_t strings = 0;
        std::size_t string_bytes = 0; // the records' text bytes, without their ids and NULs
    };

    /** @brief Reads and checks a token map within shader_token_compare.hpp's bounds; false when it is malformed. */
    [[nodiscard]] inline bool read_token_map(std::span<const std::uint8_t> blob, TokenMap &map) noexcept
    {
        if (blob.size() < 4 || blob.size() > ShaderTokens::k_max_bytes)
        {
            return false;
        }
        const auto count = ShaderTokens::word(blob.data());
        if (count == 0 || count > ShaderTokens::k_max_tokens || blob.size() - 4 < std::size_t{count} * 4)
        {
            return false;
        }
        map.count = count;
        map.tokens = blob.subspan(4, std::size_t{count} * 4);
        map.table = blob.subspan(4 + std::size_t{count} * 4);
        map.strings = 0;
        map.string_bytes = 0;
        ShaderTokens::Table table{map.table};
        for (table.advance(); table.present; table.advance())
        {
            ++map.strings;
            map.string_bytes += table.text.size();
        }
        return table.valid;
    }

    /// Whether a token opens a '#define' line (either form).
    [[nodiscard]] constexpr bool is_define(std::uint32_t token) noexcept
    {
        return token == shader_twin_blobs::k_splice_define[0] || token == shader_twin_blobs::k_splice_define[1];
    }

    /** @brief The end of the top-level fragment that starts at begin, cut as SHData lays fragments out: a '#define'
     *         line through its 0 token, else through the '}' that returns the depth to 0 (and a ';' right after it),
     *         or through a ';' at depth 0. Past begin, and never past the stream's end.
     */
    [[nodiscard]] inline std::size_t fragment_end(std::span<const std::uint32_t> tokens, std::size_t begin) noexcept
    {
        const auto count = tokens.size();
        if (is_define(tokens[begin]))
        {
            auto end = begin + 1;
            while (end < count && tokens[end] != 0)
            {
                ++end;
            }
            return std::min(end + 1, count);
        }
        int depth = 0;
        for (auto end = begin; end < count; ++end)
        {
            const auto token = tokens[end];
            if (token == shader_twin_blobs::k_splice_open_brace)
            {
                ++depth;
            }
            else if (token == shader_twin_blobs::k_splice_close_brace)
            {
                if (--depth == 0)
                {
                    ++end;
                    return end < count && tokens[end] == shader_twin_blobs::k_splice_semicolon ? end + 1 : end;
                }
            }
            else if (token == shader_twin_blobs::k_splice_semicolon && depth == 0)
            {
                return end + 1;
            }
        }
        return count;
    }

    /** @brief A fragment's name: what a '#define' defines, else the identifier right before the first of ( = : ; { [
     *         that follows one; 0 when it has none (an identifier is never a keyword id).
     */
    [[nodiscard]] inline std::uint32_t fragment_name(std::span<const std::uint32_t> tokens, std::size_t begin,
                                                     std::size_t end) noexcept
    {
        if (is_define(tokens[begin]))
        {
            return begin + 1 < tokens.size() ? tokens[begin + 1] : 0;
        }
        for (auto i = begin + 1; i < end; ++i)
        {
            if (tokens[i - 1] >= shader_twin_blobs::k_splice_keyword_count &&
                std::ranges::find(shader_twin_blobs::k_splice_name_delimiters, tokens[i]) !=
                    std::end(shader_twin_blobs::k_splice_name_delimiters))
            {
                return tokens[i - 1];
            }
        }
        return 0;
    }

    /** @brief How often run occurs in tokens (2 for two or more), and where the first occurrence starts. */
    [[nodiscard]] inline int find_run(std::span<const std::uint32_t> tokens, std::span<const std::uint32_t> run,
                                      std::size_t &at) noexcept
    {
        int hits = 0;
        for (std::size_t i = 0; !run.empty() && i + run.size() <= tokens.size(); ++i)
        {
            if (tokens[i] == run[0] && std::equal(run.begin(), run.end(), tokens.begin() + i))
            {
                if (hits++ != 0)
                {
                    return 2;
                }
                at = i;
            }
        }
        return hits;
    }

    /** @brief A token stream edited in place, within its arena capacity. */
    struct Stream
    {
        std::uint32_t *tokens;
        std::size_t count;
        std::size_t capacity;

        /// The tokens in use.
        [[nodiscard]] std::span<const std::uint32_t> view() const noexcept { return {tokens, count}; }

        /// Removes the token at at, which must be below count.
        void erase(std::size_t at) noexcept
        {
            std::memmove(tokens + at, tokens + at + 1, (count - at - 1) * sizeof(std::uint32_t));
            --count;
        }

        /// Inserts block in front of the token at at (at most count); false, with nothing changed, past capacity.
        [[nodiscard]] bool insert(std::size_t at, std::span<const std::uint32_t> block) noexcept
        {
            if (block.size() > capacity - count)
            {
                return false;
            }
            std::memmove(tokens + at + block.size(), tokens + at, (count - at) * sizeof(std::uint32_t));
            std::memcpy(tokens + at, block.data(), block.size() * sizeof(std::uint32_t));
            count += block.size();
            return true;
        }
    };

    /** @brief A spliced token map in the arena, laid out for the HW shader factory. */
    struct Plan
    {
        Status status = Status::BadTokenMap;
        TokenArray *tokens = nullptr;
        TokenTable *table = nullptr;
    };

    /** @brief The token edits, in place: what the twin's pass copies and includes change in its Z-pass pixel shader.
     *  @details A1 deletes the early-depth keyword in front of the signature (the hole's clip() needs late depth); a
     *           Z pre-pass always has it, a ZPass lacks it on some masks. A2 inserts the hole call right after the
     *           dissolve call. A3 inserts the hole's fragments right after GetAlphaCoverageMask, ShadeLib's last
     *           function, since the twin includes the hole right after ShadeLib. A4, when the hole calls
     *           GetWorldViewPos and the stock stream does not emit it, inserts its fragment right after the fragment
     *           in front of it in Common.cfi. Both anchors are read after A2; A4's precedes A3's, so A3 goes first.
     */
    [[nodiscard]] inline Status splice_tokens(Stream &stream, Entry entry, std::span<const std::uint32_t> hole) noexcept
    {
        namespace blobs = shader_twin_blobs;
        const std::span<const std::uint32_t> signature = entry == Entry::ZPass
                                                             ? std::span{blobs::k_splice_zpass_signature}
                                                             : std::span{blobs::k_splice_zprepass_signature};
        std::size_t at = 0;
        if (find_run(stream.view(), signature, at) != 1)
        {
            return Status::Signature;
        }
        if (at > 0 && stream.tokens[at - 1] == blobs::k_splice_early_depth)
        {
            stream.erase(at - 1);
        }
        else if (entry == Entry::ZPrePass)
        {
            return Status::EarlyDepth;
        }

        if (find_run(stream.view(), blobs::k_splice_dissolve_call, at) != 1)
        {
            return Status::DissolveCall;
        }
        if (!stream.insert(at + std::size(blobs::k_splice_dissolve_call), blobs::k_splice_hole_call))
        {
            return Status::ArenaFull;
        }

        const auto anchor = entry == Entry::ZPass ? blobs::k_splice_zpass_world_view_pos_after
                                                  : blobs::k_splice_zprepass_world_view_pos_after;
        bool required_first = false;
        bool required_second = false;
        bool emits_world_view_pos = false;
        int hole_hits = 0;
        int anchor_hits = 0;
        std::size_t hole_end = 0;
        std::size_t anchor_end = 0;
        const auto tokens = stream.view();
        for (std::size_t begin = 0; begin < tokens.size();)
        {
            const auto end = fragment_end(tokens, begin);
            const auto name = fragment_name(tokens, begin, end);
            if (name == blobs::k_splice_required[0])
            {
                required_first = true;
            }
            if (name == blobs::k_splice_required[1])
            {
                required_second = true;
            }
            if (name == blobs::k_splice_world_view_pos)
            {
                emits_world_view_pos = true;
            }
            if (name == blobs::k_splice_hole_after)
            {
                ++hole_hits;
                hole_end = end;
            }
            if (name == anchor)
            {
                ++anchor_hits;
                anchor_end = end;
            }
            begin = end;
        }
        if (!required_first || !required_second)
        {
            return Status::Dependency;
        }
        if (hole_hits != 1)
        {
            return Status::HoleAnchor;
        }
        const bool add_world_view_pos =
            !emits_world_view_pos && std::ranges::find(hole, blobs::k_splice_world_view_pos) != hole.end();
        if (add_world_view_pos && anchor_hits != 1)
        {
            return Status::WorldViewPosAnchor;
        }
        if (add_world_view_pos && anchor_end > hole_end)
        {
            return Status::AnchorOrder;
        }
        if (!stream.insert(hole_end, hole) ||
            (add_world_view_pos && !stream.insert(anchor_end, blobs::k_splice_world_view_pos_block)))
        {
            return Status::ArenaFull;
        }
        return Status::Ok;
    }

    /** @brief The arena bytes splice() needs at most for a token map, alignment slack included. */
    [[nodiscard]] inline std::size_t arena_size(const TokenMap &map,
                                                const shader_twin_blobs::CloneSplice &kind) noexcept
    {
        std::size_t added_bytes = 0;
        for (const auto &string : kind.table_add)
        {
            added_bytes += sizeof(StringHeader) + alignof(StringHeader) + std::string_view{string.text}.size() + 1;
        }
        const std::size_t tokens = map.count + std::size(shader_twin_blobs::k_splice_hole_call) + kind.hole.size() +
                                   std::size(shader_twin_blobs::k_splice_world_view_pos_block);
        return sizeof(TokenArray) + sizeof(TokenTable) + 3 * alignof(std::max_align_t) +
               tokens * sizeof(std::uint32_t) + (map.strings + kind.table_add.size()) * sizeof(TokenEntry) +
               map.strings * (sizeof(StringHeader) + alignof(StringHeader) + 1) + map.string_bytes + added_bytes;
    }

    /** @brief The twin's Z-pass pixel-shader token map from the stock one's, in the arena (see splice_tokens()).
     *  @details The string table is the stock one without the stock pass include names, plus the twin's strings,
     *           sorted by id. A string the twin adds under an id the stock table holds with other text fails closed.
     *           Each text gets a pinned CryStringT header, so the plan can be handed to the factory as it is.
     *  @param stock The stock pixel shader's token map (a copy of its HW +0xD0 bytes).
     *  @param entry The pixel shader's pass.
     *  @param kind The twin kind's hole and table additions (shader_twin_blobs::k_clone_splices).
     *  @param arena Takes everything the plan points to; arena_size() bytes always suffice.
     *  @return The plan; its pointers are set only when status is Ok.
     */
    [[nodiscard]] inline Plan splice(std::span<const std::uint8_t> stock, Entry entry,
                                     const shader_twin_blobs::CloneSplice &kind, Arena &arena) noexcept
    {
        Plan plan;
        TokenMap map;
        if (!read_token_map(stock, map))
        {
            return plan;
        }
        const auto failed = [&plan](Status status)
        {
            plan.status = status;
            return plan;
        };

        auto *array = arena.allocate<TokenArray>(1);
        const std::size_t capacity = map.count + std::size(shader_twin_blobs::k_splice_hole_call) + kind.hole.size() +
                                     std::size(shader_twin_blobs::k_splice_world_view_pos_block);
        auto *tokens = arena.allocate<std::uint32_t>(capacity);
        if (array == nullptr || tokens == nullptr)
        {
            return failed(Status::ArenaFull);
        }
        std::memcpy(tokens, map.tokens.data(), map.tokens.size());
        Stream stream{tokens, map.count, capacity};
        if (const auto status = splice_tokens(stream, entry, kind.hole); status != Status::Ok)
        {
            return failed(status);
        }
        *array = {tokens, static_cast<std::uint32_t>(stream.count), static_cast<std::uint32_t>(capacity)};

        auto *table = arena.allocate<TokenTable>(1);
        auto *entries = arena.allocate<TokenEntry>(map.strings + kind.table_add.size());
        if (table == nullptr || entries == nullptr)
        {
            return failed(Status::ArenaFull);
        }
        std::size_t used = 0;
        ShaderTokens::Table stock_strings{map.table};
        stock_strings.advance();
        auto added = kind.table_add.begin();
        while (stock_strings.present || added != kind.table_add.end())
        {
            if (stock_strings.present &&
                std::ranges::find(shader_twin_blobs::k_splice_table_removed, stock_strings.id) !=
                    std::end(shader_twin_blobs::k_splice_table_removed))
            {
                stock_strings.advance();
                continue;
            }
            std::uint32_t id = 0;
            std::string_view text;
            if (added != kind.table_add.end() && (!stock_strings.present || added->id <= stock_strings.id))
            {
                id = added->id;
                text = added->text;
                if (stock_strings.present && stock_strings.id == id)
                {
                    if (stock_strings.text != text)
                    {
                        return failed(Status::TableConflict);
                    }
                    stock_strings.advance();
                }
                ++added;
            }
            else
            {
                id = stock_strings.id;
                text = stock_strings.text;
                stock_strings.advance();
            }
            void *storage = arena.allocate_bytes(sizeof(StringHeader) + text.size() + 1, alignof(StringHeader));
            if (storage == nullptr)
            {
                return failed(Status::ArenaFull);
            }
            const auto length = static_cast<std::int32_t>(text.size());
            *static_cast<StringHeader *>(storage) = {k_pinned_references, length, length};
            auto *chars = static_cast<char *>(storage) + sizeof(StringHeader);
            std::memcpy(chars, text.data(), text.size());
            chars[text.size()] = '\0';
            entries[used++] = {id, 0, chars};
        }
        if (!stock_strings.valid)
        {
            return failed(Status::BadTokenMap);
        }
        *table = {entries, entries + used, entries + used};
        plan.status = Status::Ok;
        plan.tokens = array;
        plan.table = table;
        return plan;
    }

    /** @brief The clone splice of a hole twin kind by its label, or nullptr when the kind is only parsed. */
    [[nodiscard]] inline const shader_twin_blobs::CloneSplice *find_kind(std::string_view label) noexcept
    {
        for (const auto &kind : shader_twin_blobs::k_clone_splices)
        {
            if (label == kind.label)
            {
                return &kind;
            }
        }
        return nullptr;
    }

    /// True when every clone splice's table additions rise strictly by id, the order the table merge walks them in.
    [[nodiscard]] consteval bool additions_sorted() noexcept
    {
        for (const auto &kind : shader_twin_blobs::k_clone_splices)
        {
            for (std::size_t i = 1; i < kind.table_add.size(); ++i)
            {
                if (kind.table_add[i - 1].id >= kind.table_add[i].id)
                {
                    return false;
                }
            }
        }
        return true;
    }
    static_assert(additions_sorted(), "every clone splice's table additions must be sorted by id");
} // namespace TPVCamera::ShaderSplice

#endif // TPVCAMERA_HOOKS_SHADER_TOKEN_SPLICE_HPP
