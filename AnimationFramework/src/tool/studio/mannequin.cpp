/**
 * @file tool/studio/mannequin.cpp
 * @brief The mannequin figure (see mannequin.hpp).
 */

#include "tool/studio/mannequin.hpp"

#include <raymath.h>

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>

namespace AnimationFramework::studio
{
    namespace
    {
        // A plain Lambert light from above and in front, with ambient so the shadow side still reads.
        constexpr const char *VERTEX_SHADER = R"(#version 330
in vec3 vertexPosition;
in vec3 vertexNormal;
uniform mat4 mvp;
uniform mat4 matNormal;
out vec3 fragNormal;
void main()
{
    fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 0.0)));
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";
        constexpr const char *FRAGMENT_SHADER = R"(#version 330
in vec3 fragNormal;
uniform vec4 colDiffuse;
uniform vec3 lightDirection;
out vec4 finalColor;
void main()
{
    float lit = max(dot(normalize(fragNormal), -normalize(lightDirection)), 0.0);
    float rim = pow(1.0 - abs(normalize(fragNormal).z), 2.0) * 0.08;
    finalColor = vec4(colDiffuse.rgb * (0.38 + 0.62 * lit + rim), colDiffuse.a);
}
)";
        constexpr std::array<float, 3> LIGHT_DIRECTION{-0.45F, 0.6F, -0.65F};

        struct SegmentName
        {
            std::string_view from;
            std::string_view to;
            float radius;
        };

        // The body's bones as a mannequin draws them, by KCD2's male skeleton names, with each segment's thickness in
        // meters. A bone the skeleton lacks leaves its segment out.
        constexpr std::array<SegmentName, 12> TORSO{{
            {"Hips", "Spine", 0.115F},
            {"Spine", "Spine1", 0.12F},
            {"Spine1", "Spine2", 0.125F},
            {"Spine2", "Spine3", 0.13F},
            {"Spine3", "Spine4", 0.125F},
            {"Spine4", "Neck", 0.065F},
            {"Neck", "Neck1", 0.055F},
            {"Neck1", "Head", 0.055F},
            {"Hips", "LeftUpLeg", 0.1F},
            {"Hips", "RightUpLeg", 0.1F},
            {"Spine4", "LeftShoulder", 0.07F},
            {"Spine4", "RightShoulder", 0.07F},
        }};
        constexpr std::array<SegmentName, 12> SIDE{{
            {"UpLeg", "Leg", 0.075F},
            {"Leg", "Foot", 0.055F},
            {"Foot", "ToeBase", 0.045F},
            {"ToeBase", "ToeBaseEnd", 0.035F},
            {"Foot", "Heel", 0.045F},
            {"Shoulder", "Arm", 0.06F},
            {"Arm", "ForeArm", 0.052F},
            {"ForeArm", "Hand", 0.042F},
            {"Hand", "HandMiddle1", 0.036F},
            {"Hand", "HandIndex1", 0.03F},
            {"Hand", "InHandRing", 0.03F},
            {"Hand", "InHandPinky", 0.028F},
        }};
        constexpr std::array<std::string_view, 5> FINGERS{"Thumb", "Index", "Middle", "Ring", "Pinky"};
        constexpr float FINGER_RADIUS = 0.011F;
        constexpr float HEAD_RADIUS = 0.105F;

        [[nodiscard]] Vector3 to_vector(const Vec3 &v, Vector3 offset) noexcept
        {
            return Vector3{
                static_cast<float>(v.x) + offset.x,
                static_cast<float>(v.y) + offset.y,
                static_cast<float>(v.z) + offset.z
            };
        }
    } // namespace

    Mannequin::Mannequin(const std::vector<SkeletonBone> &bones)
    {
        std::unordered_map<std::string, std::size_t> index;
        for (std::size_t i = 0; i < bones.size(); ++i)
        {
            index.emplace(bones[i].name, i);
        }
        const auto add = [&](const std::string &from, const std::string &to, float radius) -> void
        {
            const auto a = index.find(from);
            const auto b = index.find(to);
            if (a != index.end() && b != index.end())
            {
                m_segments.push_back(Segment{.from = a->second, .to = b->second, .radius = radius});
            }
        };
        for (const SegmentName &segment : TORSO)
        {
            add(std::string{segment.from}, std::string{segment.to}, segment.radius);
        }
        for (const std::string side : {"Left", "Right"})
        {
            for (const SegmentName &segment : SIDE)
            {
                add(side + std::string{segment.from}, side + std::string{segment.to}, segment.radius);
            }
            add(side + "InHandRing", side + "HandRing1", 0.012F);
            add(side + "InHandPinky", side + "HandPinky1", 0.011F);
            add(side + "Hand", side + "HandThumb1", 0.014F);
            for (const std::string_view finger : FINGERS)
            {
                for (int joint = 1; joint < 4; ++joint)
                {
                    add(side + "Hand" + std::string{finger} + std::to_string(joint),
                        side + "Hand" + std::string{finger} + std::to_string(joint + 1),
                        FINGER_RADIUS);
                }
            }
        }
        if (const auto it = index.find("Head"); it != index.end())
        {
            m_head = it->second;
        }
        if (const auto it = index.find("HeadEnd"); it != index.end())
        {
            m_head_end = it->second;
        }
        if (const auto it = index.find("Hips"); it != index.end())
        {
            m_center = it->second;
        }

        m_cylinder = GenMeshCylinder(1.0F, 1.0F, 18);
        m_sphere = GenMeshSphere(1.0F, 12, 18);
        const Shader shader = LoadShaderFromMemory(VERTEX_SHADER, FRAGMENT_SHADER);
        SetShaderValue(
            shader,
            GetShaderLocation(shader, "lightDirection"),
            LIGHT_DIRECTION.data(),
            SHADER_UNIFORM_VEC3
        );
        m_material = LoadMaterialDefault();
        m_material.shader = shader;
    }

    Mannequin::~Mannequin()
    {
        UnloadMesh(m_cylinder);
        UnloadMesh(m_sphere);
        // Unloads the material's maps and its shader, the one loaded here.
        UnloadMaterial(m_material);
    }

    void Mannequin::draw_capsule(Vector3 a, Vector3 b, float radius, Color color)
    {
        const Vector3 span = Vector3Subtract(b, a);
        const float length = Vector3Length(span);
        if (length > 1.0e-4F)
        {
            const Quaternion turn =
                QuaternionFromVector3ToVector3(Vector3{0.0F, 1.0F, 0.0F}, Vector3Scale(span, 1.0F / length));
            const Matrix transform = MatrixMultiply(
                MatrixMultiply(MatrixScale(radius, length, radius), QuaternionToMatrix(turn)),
                MatrixTranslate(a.x, a.y, a.z)
            );
            m_material.maps[MATERIAL_MAP_DIFFUSE].color = color;
            DrawMesh(m_cylinder, m_material, transform);
        }
        draw_sphere(a, radius, color);
        draw_sphere(b, radius, color);
    }

    void Mannequin::draw_sphere(Vector3 center, float radius, Color color)
    {
        m_material.maps[MATERIAL_MAP_DIFFUSE].color = color;
        DrawMesh(
            m_sphere,
            m_material,
            MatrixMultiply(MatrixScale(radius, radius, radius), MatrixTranslate(center.x, center.y, center.z))
        );
    }

    void Mannequin::draw(
        std::span<const Vec3> positions,
        Vector3 offset,
        Color color,
        std::span<const std::uint8_t> flagged,
        Color flag_color
    )
    {
        const auto flag = [&](std::size_t bone) { return bone < flagged.size() && flagged[bone] != 0; };
        for (const Segment &segment : m_segments)
        {
            if (segment.from >= positions.size() || segment.to >= positions.size())
            {
                continue;
            }
            draw_capsule(
                to_vector(positions[segment.from], offset),
                to_vector(positions[segment.to], offset),
                segment.radius,
                flag(segment.to) ? flag_color : color
            );
        }
        if (m_head && *m_head < positions.size())
        {
            // The skull sits on the Head joint, along the head's own axis (Head to HeadEnd).
            const Vector3 head = to_vector(positions[*m_head], offset);
            Vector3 center = Vector3Add(head, Vector3{0.0F, 0.0F, HEAD_RADIUS * 0.8F});
            if (m_head_end && *m_head_end < positions.size())
            {
                const Vector3 axis = Vector3Subtract(to_vector(positions[*m_head_end], offset), head);
                if (Vector3Length(axis) > 1.0e-4F)
                {
                    center = Vector3Add(head, Vector3Scale(Vector3Normalize(axis), HEAD_RADIUS * 0.8F));
                }
            }
            draw_sphere(center, HEAD_RADIUS, flag(*m_head) ? flag_color : color);
        }
    }

    void Mannequin::draw_markers(
        std::span<const Vec3> positions,
        Vector3 offset,
        std::span<const std::size_t> bones,
        Color color
    )
    {
        for (const std::size_t bone : bones)
        {
            if (bone < positions.size())
            {
                draw_sphere(to_vector(positions[bone], offset), 0.035F, color);
            }
        }
    }
} // namespace AnimationFramework::studio
