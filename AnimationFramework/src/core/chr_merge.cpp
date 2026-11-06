/**
 * @file core/chr_merge.cpp
 * @brief Bone-by-bone merge of several mods' whole skeleton copies (see chr_merge.hpp).
 */

#include "core/chr_merge.hpp"
#include "core/caf_reader.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>

namespace AnimationFramework
{
    namespace
    {
        constexpr std::uint32_t COMPILED_BONES_TYPE = 0x2000;
        constexpr std::uint32_t COMPILED_BONES_VERSION = 0x800;
        constexpr std::size_t BONES_HEADER = 32;
        constexpr std::size_t BONE_RECORD = 584;
        constexpr std::size_t BONE_NAME_OFFSET = 312;
        constexpr std::size_t BONE_PARENT_OFFSET = 572;
        /// Name and parent offset: what makes two records the same bone in the same place.
        constexpr std::size_t BONE_IDENTITY_END = BONE_PARENT_OFFSET + 4;
        /// The default transforms, two row-major 3x4 float matrices (world to bone, bone to world).
        constexpr std::size_t BONE_TRANSFORMS_BEGIN = 216;
        constexpr std::size_t BONE_TRANSFORMS_END = 312;
        /**
         * @brief A transform value closer than this to another counts as the same. Re-exported copies carry float noise
         *        of about 5e-7, and 1e-5 is a hundredth of a millimetre.
         */
        constexpr float TRANSFORM_TOLERANCE = 1e-5F;

        /// The value at @p at of @p bytes. The caller checks the bounds.
        template <typename T> [[nodiscard]] T read_le(std::string_view bytes, std::size_t at) noexcept
        {
            T value{};
            std::memcpy(&value, bytes.data() + at, sizeof(T));
            return value;
        }

        /** @brief One chunk table entry, its chunk's place given as an offset into the file. */
        struct ChunkEntry
        {
            std::uint32_t type = 0;
            std::uint32_t version = 0;
            std::uint32_t id = 0;
            std::size_t size = 0;
            std::size_t offset = 0;
        };

        /** @brief The chunk table of @p file, every chunk inside the file (read_chunk_views), or nothing. */
        [[nodiscard]] std::optional<std::vector<ChunkEntry>> read_chunk_table(std::string_view file)
        {
            const std::span<const std::byte> bytes = std::as_bytes(std::span{file.data(), file.size()});
            const auto views = read_chunk_views(bytes);
            if (!views)
            {
                return std::nullopt;
            }
            std::vector<ChunkEntry> entries;
            entries.reserve(views->size());
            for (const ChunkView &view : *views)
            {
                entries.push_back(
                    ChunkEntry{
                        .type = view.type,
                        .version = view.version,
                        .id = view.id,
                        .size = view.data.size(),
                        .offset = static_cast<std::size_t>(view.data.data() - bytes.data()),
                    }
                );
            }
            return entries;
        }

        /** @brief The index of the one CompiledBones chunk whose size fits whole records, or nothing. */
        [[nodiscard]] std::optional<std::size_t> bones_chunk(const std::vector<ChunkEntry> &entries) noexcept
        {
            std::optional<std::size_t> found;
            for (std::size_t i = 0; i < entries.size(); ++i)
            {
                if (entries[i].type == COMPILED_BONES_TYPE && entries[i].version == COMPILED_BONES_VERSION)
                {
                    if (found.has_value())
                    {
                        return std::nullopt;
                    }
                    found = i;
                }
            }
            if (found &&
                (entries[*found].size < BONES_HEADER || (entries[*found].size - BONES_HEADER) % BONE_RECORD != 0))
            {
                return std::nullopt;
            }
            return found;
        }

        /** @brief Two bone records are the same bone: exactly, except transform values that differ by float noise. */
        [[nodiscard]] bool records_match(std::string_view a, std::string_view b) noexcept
        {
            if (a.substr(0, BONE_TRANSFORMS_BEGIN) != b.substr(0, BONE_TRANSFORMS_BEGIN) ||
                a.substr(BONE_TRANSFORMS_END) != b.substr(BONE_TRANSFORMS_END))
            {
                return false;
            }
            for (std::size_t at = BONE_TRANSFORMS_BEGIN; at < BONE_TRANSFORMS_END; at += sizeof(float))
            {
                // Equal bytes match first, so a NaN the game's record also holds is no change.
                if (a.substr(at, sizeof(float)) == b.substr(at, sizeof(float)))
                {
                    continue;
                }
                const float x = read_le<float>(a, at);
                const float y = read_le<float>(b, at);
                if (!(x == y || std::fabs(x - y) <= TRANSFORM_TOLERANCE))
                {
                    return false;
                }
            }
            return true;
        }

        /// The name a bone record holds, up to its first NUL.
        [[nodiscard]] std::string bone_name(std::string_view record)
        {
            const std::string_view field = record.substr(BONE_NAME_OFFSET, BONE_PARENT_OFFSET - BONE_NAME_OFFSET);
            return std::string{field.substr(0, field.find('\0'))};
        }

        /// A chunk as the reports name it: "chunk 0x<type>".
        [[nodiscard]] std::string chunk_label(const ChunkEntry &entry)
        {
            return std::format("chunk {:#x}", entry.type);
        }

        /// The refusal that names @p source and why.
        [[nodiscard]] SkeletonMergeRefusal refuse(std::string_view source, std::string reason)
        {
            return SkeletonMergeRefusal{
                .source = std::string{source},
                .reason = std::move(reason),
            };
        }

        /** @brief The mod that last changed a bone record or chunk of the merged file. */
        struct Owner
        {
            std::string source;
        };
    } // namespace

    std::expected<SkeletonMerge, SkeletonMergeRefusal>
    merge_skeletons(std::string_view base, std::span<const SkeletonCopy> copies)
    {
        const std::optional<std::vector<ChunkEntry>> base_table = read_chunk_table(base);
        const std::optional<std::size_t> base_bones = base_table ? bones_chunk(*base_table) : std::nullopt;
        if (!base_table || !base_bones)
        {
            return std::unexpected(refuse("the game", "its skeleton is not a chunk file with one CompiledBones chunk"));
        }
        const ChunkEntry &bones_entry = (*base_table)[*base_bones];
        const std::size_t bone_count = (bones_entry.size - BONES_HEADER) / BONE_RECORD;
        const std::string_view base_bone_data = base.substr(bones_entry.offset, bones_entry.size);

        // Every copy must match the game's chunk layout and bone list before anything merges.
        std::vector<std::vector<ChunkEntry>> tables;
        tables.reserve(copies.size());
        for (const SkeletonCopy &copy : copies)
        {
            std::optional<std::vector<ChunkEntry>> table = read_chunk_table(copy.bytes);
            if (!table)
            {
                return std::unexpected(refuse(copy.source, "its skeleton is not a chunk file this reads"));
            }
            if (table->size() != base_table->size())
            {
                return std::unexpected(refuse(
                    copy.source,
                    std::format("it has {} chunks, the game's has {}", table->size(), base_table->size())
                ));
            }
            for (std::size_t i = 0; i < table->size(); ++i)
            {
                const ChunkEntry &a = (*base_table)[i];
                const ChunkEntry &b = (*table)[i];
                if (a.type != b.type || a.version != b.version || a.id != b.id || a.size != b.size)
                {
                    return std::unexpected(refuse(
                        copy.source,
                        std::format(
                            "its chunk {} is {:#x} v{:#x}, {} bytes; the game's is {:#x} v{:#x}, {} bytes",
                            i,
                            b.type,
                            b.version,
                            b.size,
                            a.type,
                            a.version,
                            a.size
                        )
                    ));
                }
            }
            const std::string_view bone_data = copy.bytes.substr((*table)[*base_bones].offset, bones_entry.size);
            for (std::size_t r = 0; r < bone_count; ++r)
            {
                const std::size_t at = BONES_HEADER + r * BONE_RECORD;
                if (bone_data.substr(at + BONE_NAME_OFFSET, BONE_IDENTITY_END - BONE_NAME_OFFSET) !=
                    base_bone_data.substr(at + BONE_NAME_OFFSET, BONE_IDENTITY_END - BONE_NAME_OFFSET))
                {
                    return std::unexpected(refuse(
                        copy.source,
                        std::format(
                            "its bone {} is {}, the game's is {}",
                            r,
                            bone_name(bone_data.substr(at, BONE_RECORD)),
                            bone_name(base_bone_data.substr(at, BONE_RECORD))
                        )
                    ));
                }
            }
            tables.push_back(std::move(*table));
        }

        SkeletonMerge merge{
            .bytes = std::string{base},
        };
        // Each bone record, then each chunk other than the bone records (the bones chunk's header included): the
        // source that last changed it.
        std::vector<std::optional<Owner>> bone_owner(bone_count);
        std::vector<std::optional<Owner>> chunk_owner(base_table->size());

        for (std::size_t c = 0; c < copies.size(); ++c)
        {
            const SkeletonCopy &copy = copies[c];
            const std::vector<ChunkEntry> &table = tables[c];
            SkeletonContribution contribution{
                .source = std::string{copy.source},
            };

            // Copies the copy's bytes [from, from + length) over the merged file's [at, at + length) when they differ
            // from the game's bytes there, and records the owner. Bone records compare with records_match, the rest
            // exactly. Returns whether the copy changed them.
            const auto take = [&](std::size_t from,
                                  std::size_t at,
                                  std::size_t length,
                                  std::optional<Owner> &owner,
                                  const std::string &what,
                                  bool bone) -> bool
            {
                const auto same = [bone](std::string_view x, std::string_view y) -> bool
                { return bone ? records_match(x, y) : x == y; };
                const std::string_view theirs = copy.bytes.substr(from, length);
                if (same(theirs, base.substr(at, length)))
                {
                    return false;
                }
                const std::string_view merged = std::string_view{merge.bytes}.substr(at, length);
                if (owner.has_value())
                {
                    if (same(merged, theirs))
                    {
                        if (bone)
                        {
                            ++contribution.same_as_earlier;
                        }
                        return true;
                    }
                    merge.conflicts.push_back(
                        SkeletonConflict{
                            .winner = contribution.source,
                            .loser = owner->source,
                            .what = what,
                        }
                    );
                }
                merge.bytes.replace(at, length, theirs);
                owner = Owner{
                    .source = contribution.source,
                };
                return true;
            };

            for (std::size_t i = 0; i < table.size(); ++i)
            {
                const ChunkEntry &mine = table[i];
                const ChunkEntry &game = (*base_table)[i];
                if (i == *base_bones)
                {
                    if (take(mine.offset, game.offset, BONES_HEADER, chunk_owner[i], "bones header", false))
                    {
                        contribution.chunks.push_back("bones header");
                    }
                    for (std::size_t r = 0; r < bone_count; ++r)
                    {
                        const std::size_t at = BONES_HEADER + r * BONE_RECORD;
                        const std::string name = bone_name(base_bone_data.substr(at, BONE_RECORD));
                        if (take(mine.offset + at, game.offset + at, BONE_RECORD, bone_owner[r], name, true))
                        {
                            contribution.bones.push_back(name);
                        }
                    }
                    continue;
                }
                if (take(mine.offset, game.offset, game.size, chunk_owner[i], chunk_label(game), false))
                {
                    contribution.chunks.push_back(chunk_label(game));
                }
            }
            merge.contributions.push_back(std::move(contribution));
        }
        return merge;
    }
} // namespace AnimationFramework
