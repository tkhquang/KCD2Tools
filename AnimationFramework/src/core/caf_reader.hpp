/**
 * @file core/caf_reader.hpp
 * @brief Reads compiled animation clips (`.caf`, and clips packed in `.dba` files) down to their keys, and a
 *        skeleton's bones (`.chr`), so the framework can check what a clip animates and how it moves.
 *
 * Formats, from the CryEngine 5 sources (CryHeaders.h, QuatQuantization.h, ControllerPQ.cpp) and KCD2's files:
 *   - A chunk file: "CrCh", version 0x746, a u32 chunk count and a u32 table offset. The table holds 16-byte entries
 *     {u16 type, u16 version, u32 id, u32 size, u32 offset}.
 *   - `.caf`: one MotionParams905 chunk (0x3002 v0x925, 132 bytes: start and end frame at +16 and +20) and one
 *     controller chunk (0x100D v0x829) per animated bone: a 16-byte descriptor {u32 controller id, u16 rotation keys,
 *     u16 position keys, u8 rotation format, rotation time format, position format, position keys info, position time
 *     format, tracks aligned} then rotation values, rotation key times, position values and position key times, each
 *     padded to 4 bytes when the tracks are aligned. Position key times are present only when the info byte is set,
 *     else the positions share the rotation key times.
 *   - `.dba`: one controller chunk 0x905 that holds many clips over shared track storage.
 *   - `.chr`: CompiledBones 0x2000 v0x800: a 32-byte header, then 584-byte records (controller id +0, default
 *     bone-to-world 3x4 row-major +264, name +312, parent offset +572).
 * Rotations: 0 and 1 are four floats (x, y, z, w). 5, 6 and 8 are "small tree" quaternions (15, 20 and 21/21/20-bit
 * components, the dropped largest one's index in the top 2 bits). Positions: 0 and 2 are three floats. Key times are
 * frames at 30 Hz: 0 f32, 1 u16, 2 u8 arrays, 3-5 start/stop pairs, 6 a bitset {start, end, count} and bit words.
 */

#ifndef ANIMFW_CORE_CAF_READER_HPP
#define ANIMFW_CORE_CAF_READER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationFramework
{
    /// Clip key times count frames at this rate.
    inline constexpr double CLIP_FRAME_RATE = 30.0;
    /**
     * @brief The longest animation the framework reads, compiles, retargets or measures: an hour, 108,000 frames.
     * @details No game or mod clip comes near it, so a longer span marks a damaged file. The bound keeps the frame
     *          count of such a file from sizing allocations and loops.
     */
    inline constexpr double MAX_CLIP_SECONDS = 3600.0;

    /// A rotation quaternion, the identity by default.
    struct Quat
    {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        double w = 1.0;
    };

    /// A position or offset in metres.
    struct Vec3
    {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    /// The rotation @p b followed by @p a (the Hamilton product a * b).
    [[nodiscard]] Quat multiply(const Quat &a, const Quat &b) noexcept;
    /// The inverse of the unit quaternion @p q.
    [[nodiscard]] Quat conjugate(const Quat &q) noexcept;
    /// @p v rotated by the unit quaternion @p q.
    [[nodiscard]] Vec3 rotate(const Quat &q, const Vec3 &v) noexcept;
    /** @brief The angle between two rotations, in degrees (0 to 180). */
    [[nodiscard]] double angle_between(const Quat &a, const Quat &b) noexcept;

    /** @brief One bone's keys: rotations and positions relative to its parent bone, at frames (CLIP_FRAME_RATE). */
    struct ClipTrack
    {
        std::uint32_t controller = 0;
        std::vector<double> rotation_frames;
        std::vector<Quat> rotations;
        std::vector<double> position_frames;
        std::vector<Vec3> positions;
    };

    /** @brief A clip's keys. */
    struct CafClip
    {
        std::string name;
        std::int32_t start_frame = 0;
        std::int32_t end_frame = 0;
        std::vector<ClipTrack> tracks;

        /** @brief The clip's length, as the engine reads it from the motion parameters (at least one frame). */
        [[nodiscard]] double seconds() const noexcept;
        /// The track of the bone with controller id @p controller, or null.
        [[nodiscard]] const ClipTrack *track(std::uint32_t controller) const noexcept;
    };

    /** @brief A chunk of a chunk file: its type, version, id and bytes. */
    struct ChunkView
    {
        std::uint32_t type = 0;
        std::uint32_t version = 0;
        std::uint32_t id = 0;
        std::span<const std::byte> data;
    };

    /** @brief A chunk file's chunks, or why it is not one. The views point into @p file. */
    [[nodiscard]] std::expected<std::vector<ChunkView>, std::string> read_chunk_views(std::span<const std::byte> file);

    /** @brief A compiled clip's keys from its chunks, or why it cannot be read. */
    [[nodiscard]] std::expected<CafClip, std::string> read_caf_clip(std::span<const ChunkView> chunks);
    /** @brief A compiled `.caf` file's keys, or why it cannot be read. */
    [[nodiscard]] std::expected<CafClip, std::string> read_caf_file(std::span<const std::byte> file);

    /**
     * @brief The clips of a `.dba` file whose path's file name (lowercase, without `.caf`) is in @p stems, or every
     *        clip when @p stems is empty. Each clip's name is its path as the pack records it.
     */
    [[nodiscard]] std::expected<std::vector<CafClip>, std::string>
    read_dba_clips(std::span<const std::byte> file, std::span<const std::string> stems = {});
    /** @brief The same from a `.dba` file's chunks. */
    [[nodiscard]] std::expected<std::vector<CafClip>, std::string>
    read_dba_chunks(std::span<const ChunkView> chunks, std::span<const std::string> stems = {});

    /** @brief A clip path's file name, lowercase, without its extension (`.caf`): the name the game plays it by. */
    [[nodiscard]] std::string clip_stem(std::string_view path);

    /** @brief A skeleton bone: its name, controller id, parent (-1 for the root) and its default model-space pose. */
    struct SkeletonBone
    {
        std::string name;
        std::uint32_t controller = 0;
        int parent = -1;
        Quat rotation;
        Vec3 position;
    };

    /** @brief The bones of a CompiledBones chunk (0x2000 v0x800), parents before children as the file keeps them. */
    [[nodiscard]] std::expected<std::vector<SkeletonBone>, std::string>
    read_skeleton_bones(std::span<const std::byte> bones_chunk);
    /** @brief The bones of a skeleton file (`.chr`). */
    [[nodiscard]] std::expected<std::vector<SkeletonBone>, std::string>
    read_skeleton_file(std::span<const std::byte> file);

    /** @brief A bone's controller id: the CRC-32 of its name, case included. */
    [[nodiscard]] std::uint32_t bone_controller_id(std::string_view name) noexcept;

    /** @brief The 48-bit small-tree encoding of @p q (rotation format 5), as the engine decodes it. */
    [[nodiscard]] std::array<std::uint16_t, 3> encode_small_tree_48(const Quat &q) noexcept;
    /** @brief The 64-bit extended small-tree encoding of @p q (rotation format 8), as the engine decodes it. */
    [[nodiscard]] std::array<std::uint32_t, 2> encode_small_tree_64(const Quat &q) noexcept;
    /// The rotation a format 5 key holds, as the engine decodes it.
    [[nodiscard]] Quat decode_small_tree_48(const std::array<std::uint16_t, 3> &words) noexcept;
    /// The rotation a format 8 key holds, as the engine decodes it.
    [[nodiscard]] Quat decode_small_tree_64(const std::array<std::uint32_t, 2> &words) noexcept;

    /** @brief A track's rotation at @p frame: shorter-arc normalized lerp between keys, held at the ends. */
    [[nodiscard]] std::optional<Quat> sample_rotation(const ClipTrack &track, double frame) noexcept;
    /** @brief A track's position at @p frame: linear between keys, held at the ends. */
    [[nodiscard]] std::optional<Vec3> sample_position(const ClipTrack &track, double frame) noexcept;

    /**
     * @brief Plays a clip on a skeleton: each bone's model-space pose at a frame. A bone the clip does not key keeps
     *        its default pose relative to its parent, as on the engine's base layer.
     * @warning The poser keeps references to the bones and the clip's tracks: both must outlive it.
     */
    class ClipPoser
    {
    public:
        /** @brief A poser for @p clip on @p bones (parents before children, as read_skeleton_bones returns them). */
        ClipPoser(const std::vector<SkeletonBone> &bones, const CafClip &clip);

        /** @brief The model-space rotation and position of every bone at @p frame. */
        void pose(double frame, std::vector<Quat> &rotations, std::vector<Vec3> &positions) const;
        /** @brief The index of the bone named @p name, or nothing. */
        [[nodiscard]] std::optional<std::size_t> bone(std::string_view name) const noexcept;

    private:
        const std::vector<SkeletonBone> &m_bones;
        std::vector<Quat> m_bind_rotations;
        std::vector<Vec3> m_bind_positions;
        std::vector<const ClipTrack *> m_tracks;
    };
} // namespace AnimationFramework

#endif // ANIMFW_CORE_CAF_READER_HPP
