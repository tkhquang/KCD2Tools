/**
 * @file core/clip_header.hpp
 * @brief Builds clip index headers from compiled .caf files, so a mod ships only its clips.
 *
 * The game's clip index (`animations/animations.img`) holds one CHUNK_GAHCAF_INFO per clip, which CryEngine's
 * resource compiler writes only for a whole animation tree. Every field but the path comes from the compiled .caf: its
 * motion parameters chunk and its controller chunk count. An entry rebuilt from a stock loose clip matches the game's
 * image byte for byte.
 */
#ifndef ANIMFW_CORE_CLIP_HEADER_HPP
#define ANIMFW_CORE_CLIP_HEADER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationFramework
{
    /// CHUNK_GAHCAF_INFO, the clip index entry (chunk type 0x3007, version 0x971).
    inline constexpr std::uint32_t CLIP_HEADER_CHUNK_TYPE = 0x3007;
    inline constexpr std::uint32_t CLIP_HEADER_CHUNK_VERSION = 0x971;
    inline constexpr std::size_t CLIP_HEADER_SIZE = 404;
    /// CHUNK_GAHCAF_INFO fields: the path, its CRC, the DBA CRC (0 for a loose .caf), the duration, the controllers.
    inline constexpr std::size_t CLIP_HEADER_PATH_OFFSET = 4;
    inline constexpr std::size_t CLIP_HEADER_PATH_SIZE = 256;
    inline constexpr std::size_t CLIP_HEADER_PATH_CRC_OFFSET = 260;
    inline constexpr std::size_t CLIP_HEADER_DBA_CRC_OFFSET = 264;
    inline constexpr std::size_t CLIP_HEADER_FOOT_PLANTS_OFFSET = 268;
    inline constexpr std::size_t CLIP_HEADER_DURATION_OFFSET = 308;
    inline constexpr std::size_t CLIP_HEADER_CONTROLLERS_OFFSET = 312;
    /// A compiled .caf's motion parameters chunk (MotionParams905) and its per-bone controller chunks.
    inline constexpr std::uint32_t CAF_MOTION_CHUNK_TYPE = 0x3002;
    inline constexpr std::uint32_t CAF_MOTION_CHUNK_VERSION = 0x925;
    inline constexpr std::size_t CAF_MOTION_CHUNK_SIZE = 132;
    inline constexpr std::uint32_t CAF_CONTROLLER_CHUNK_TYPE = 0x100D;

    using ClipHeader = std::array<std::byte, CLIP_HEADER_SIZE>;

    /**
     * @brief The key the engine files a clip header under: CityHash64 (v1.1) of the lowercased path.
     * @details The image loader (Steam 0x1806A1F18) stores it at header +0x10 through Steam 0x1806A3014, which reads
     *          every byte through tolower. A path past 1,024 bytes hashes its first 1,024 only: no clip path is that
     *          long (the header's path field holds 255).
     */
    [[nodiscard]] std::uint64_t clip_path_hash(std::string_view path) noexcept;

    /**
     * @brief The index entry for the clip at @p path, from its motion parameters chunk and controller chunk count.
     * @details Matches the resource compiler (GlobalAnimationHeaderCAF::ReadMotionParameters and SaveToChunkFile):
     *          flags gain CA_ASSET_ONDEMAND, both path CRCs are 0 (the engine hashes the path itself), the end time is
     *          the end key minus the start key at 30 frames per second in float arithmetic, and the velocity is zero.
     * @return Nothing when the motion chunk is too short or the path does not fit the 256-byte field.
     */
    [[nodiscard]] std::optional<ClipHeader> build_clip_header(
        std::span<const std::byte> motion_chunk,
        std::uint32_t controller_count,
        std::string_view path
    ) noexcept;

    /**
     * @brief Every index entry of a clip index file (an Animations.img: CrCh 0x746 chunks of type 0x3007 v0x971, 404
     *        bytes each), in file order. Empty when @p data is not one.
     */
    [[nodiscard]] std::vector<ClipHeader> read_clip_image(std::span<const std::byte> data);

    /** @brief The path an index entry names (lowercased, forward slashes). */
    [[nodiscard]] std::string clip_header_path(const ClipHeader &header);

} // namespace AnimationFramework

#endif // ANIMFW_CORE_CLIP_HEADER_HPP
