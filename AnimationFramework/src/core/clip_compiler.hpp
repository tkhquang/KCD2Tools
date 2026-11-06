/**
 * @file core/clip_compiler.hpp
 * @brief Compiles an animation into a game-ready `.caf` without the Resource Compiler (`rc.exe`).
 * @details Does what the game needs from the Resource Compiler's animation step (CryEngine
 *          Code/Tools/RC/ResourceCompilerPC/CGA/AnimationLoader.cpp and CompressionController.cpp), in the same way:
 *          - The root (bone 0, `Human_Male`) is rebased so its first key is the identity. Its first key becomes the
 *            clip's start location, and the other bones keep their keys.
 *          - The motion parameters: duration, cycle flag (every non-root bone ends within 0.1 per component of where it
 *            starts), distance and speed (the root's path in 100 steps), turn and slope (the root at 0, 0.5 and 1).
 *          - An additive clip stores each key's difference from the first frame and drops that frame.
 *          - Keys the interpolation already gives within the compression tolerance are dropped (compression 10:
 *            0.32 mm, 0.036 degrees), rotations are quantized to the 48 or 64-bit small-tree formats, positions stay
 *            float and key times are 16-bit frames.
 *          Unlike the Resource Compiler it keeps every bone the source keys (see TrackPolicy::KeepAll): its rule drops
 *          a bone that holds its default pose, which leaves that bone to the layers below on an overlay but puts it
 *          in the default pose on a full-body clip only by accident. Foot plants stay unset (-10000), as the Resource
 *          Compiler leaves them when the exporter writes none. Aim and look poses are not handled.
 */

#ifndef ANIMFW_CORE_CLIP_COMPILER_HPP
#define ANIMFW_CORE_CLIP_COMPILER_HPP

#include "core/caf_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace AnimationFramework
{
    /** @brief One bone's pose relative to its parent bone at every frame (CLIP_FRAME_RATE). */
    struct SampledTrack
    {
        std::uint32_t controller = 0;
        /// One per frame.
        std::vector<Quat> rotations;
        /// One per frame, or empty: the bone keeps its default offset from its parent.
        std::vector<Vec3> positions;
    };

    /** @brief An animation sampled at CLIP_FRAME_RATE, frames 0 to frames - 1. */
    struct SampledClip
    {
        std::size_t frames = 0;
        std::vector<SampledTrack> tracks;
        /// Source tracks no bone of the skeleton matches (left out).
        std::size_t unknown_tracks = 0;
    };

    /** @brief Which bones a compiled clip keys. */
    enum class TrackPolicy
    {
        /**
         * @brief Every bone the source keys (a full-body clip: a bone left out falls to its default pose), except
         *        a SKELETON_PLACED_BONES channel that holds the bone's default pose, which every policy drops.
         */
        KeepAll,
        /// As the Resource Compiler: a channel that never moves and holds the bone's default pose is dropped.
        DropUnchangedAtDefault,
    };

    /// How compile_clip compiles a clip.
    struct CompileOptions
    {
        bool additive = false;
        /// Whether the clip loops, or nothing to detect it as the Resource Compiler does.
        std::optional<bool> loop;
        /// The Resource Compiler's compression value: the key tolerance grows with its square root (10 is its default).
        int compression = 10;
        TrackPolicy tracks = TrackPolicy::KeepAll;
    };

    /// What compile_clip wrote: track and key counts, the detected loop and the root's motion.
    struct CompileReport
    {
        std::size_t frames = 0;
        std::size_t tracks = 0;
        std::size_t dropped_tracks = 0;
        std::size_t unknown_bones = 0;
        std::size_t keys_in = 0;
        std::size_t keys_out = 0;
        bool loop = false;
        bool additive = false;
        double distance = 0.0;
        double speed = 0.0;
        double turn = 0.0;
        std::size_t bytes = 0;
    };

    /// A compiled `.caf` file and its report.
    struct CompiledClip
    {
        std::vector<std::byte> file;
        CompileReport report;
    };

    /**
     * @brief Compiles @p clip for @p skeleton (its bones in file order, bone 0 the root).
     * @return The `.caf` bytes and what was written, or why the clip cannot be compiled (a track without one key per
     *         frame, or two tracks of one bone).
     */
    [[nodiscard]] std::expected<CompiledClip, std::string>
    compile_clip(const SampledClip &clip, std::span<const SkeletonBone> skeleton, const CompileOptions &options);

    /**
     * @brief The keys of an intermediate animation (`.i_caf`, as DCC exporters write it for the Resource Compiler:
     *        controller chunks 0x827 of {tick, position in cm, log of the conjugate rotation}, 4800 ticks a second),
     *        resampled at CLIP_FRAME_RATE from its first key to its last.
     * @details The ticks decide the timing, so an export at any frame rate plays at its own speed. Controller ids are
     *          matched to @p skeleton by its bones' controller ids, then by the CRC of their exact names. Keys that
     *          span more than MAX_CLIP_SECONDS fail as a damaged file, and so does a bone that two chunks key.
     */
    [[nodiscard]] std::expected<SampledClip, std::string>
    read_intermediate_clip(std::span<const std::byte> file, std::span<const SkeletonBone> skeleton);
} // namespace AnimationFramework

#endif // ANIMFW_CORE_CLIP_COMPILER_HPP
