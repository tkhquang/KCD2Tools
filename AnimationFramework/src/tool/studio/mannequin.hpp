/**
 * @file tool/studio/mannequin.hpp
 * @brief An artist's mannequin on the game's own skeleton: lit capsules between the body's bones, a head and joints, so
 *        a clip's pose shows exactly (no retargeting), with bones a check flags drawn in another color.
 */

#ifndef ANIMFW_TOOL_STUDIO_MANNEQUIN_HPP
#define ANIMFW_TOOL_STUDIO_MANNEQUIN_HPP

#include "core/caf_reader.hpp"

#include <raylib.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace AnimationFramework::studio
{
    /** @brief The figure for one skeleton, drawn at any pose of it. */
    class Mannequin
    {
    public:
        /** @brief Builds the figure for @p bones. Needs a window (it loads meshes and a shader). */
        explicit Mannequin(const std::vector<SkeletonBone> &bones);
        ~Mannequin();
        Mannequin(const Mannequin &) = delete;
        Mannequin &operator=(const Mannequin &) = delete;
        Mannequin(Mannequin &&) = delete;
        Mannequin &operator=(Mannequin &&) = delete;

        /**
         * @brief Draws a pose: @p positions are the bones' model-space positions (meters, Z up), moved by @p offset.
         *        Bones set in @p flagged draw in @p flag_color. Call between BeginMode3D and EndMode3D.
         */
        void draw(
            std::span<const Vec3> positions,
            Vector3 offset,
            Color color,
            std::span<const std::uint8_t> flagged,
            Color flag_color
        );

        /** @brief Small markers at @p bones (helper bones the figure leaves out, such as back slots). */
        void
        draw_markers(std::span<const Vec3> positions, Vector3 offset, std::span<const std::size_t> bones, Color color);

        /** @brief The bone the camera follows (Hips), or nothing. */
        [[nodiscard]] std::optional<std::size_t> center() const noexcept { return m_center; }

    private:
        struct Segment
        {
            std::size_t from = 0;
            std::size_t to = 0;
            float radius = 0.0F;
        };

        void draw_capsule(Vector3 a, Vector3 b, float radius, Color color);
        void draw_sphere(Vector3 center, float radius, Color color);

        std::vector<Segment> m_segments;
        std::optional<std::size_t> m_head;
        std::optional<std::size_t> m_head_end;
        std::optional<std::size_t> m_center;
        Mesh m_cylinder{};
        Mesh m_sphere{};
        /// Owns the figure's shader: UnloadMaterial unloads it.
        Material m_material{};
    };
} // namespace AnimationFramework::studio

#endif // ANIMFW_TOOL_STUDIO_MANNEQUIN_HPP
