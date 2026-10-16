/**
 * @file core/collision/probe_record.hpp
 * @brief Recording and replay of the camera collision: every call of a frame's collision to the WorldProbe, with its
 *        answer, between the frame's input and state before and its output and state after.
 *
 * @details A DEV build records the camera frames' collision while [Debug] RecordFrames is on and writes them at the
 *          generation's shutdown (dev/frame_recorder.hpp). The replay test runs each recorded frame again through the
 *          core, answering the probe from the recording, and requires the same calls in the same order and the same
 *          output and state after, bit for bit. A recording is a local fixture for the build that wrote it: it holds
 *          raw structs, and its header carries their sizes, which the reader checks. Engine-free (no Windows or
 *          DetourModKit header), so the replay test compiles it as is.
 *
 *          Stream: Header, then frames until the end. A collision frame is FrameKind, its input (CollisionInput, or
 *          ShoulderInput), CollisionParams, CollisionState before, the probe calls (each a Call byte, its arguments
 *          and its answer), Call::End, the output (Vector3, or float), and CollisionState after. A lens frame is
 *          FrameKind::Lens, LensInput, LensState before, the probe calls, Call::End, the output (a has-value byte and
 *          a float) and LensState after. A rig frame is FrameKind::Rig and the rest core/rig/rig_record.hpp
 *          describes.
 */
#ifndef TPVCAMERA_CORE_COLLISION_PROBE_RECORD_HPP
#define TPVCAMERA_CORE_COLLISION_PROBE_RECORD_HPP

#include "core/collision/arm.hpp"
#include "core/collision/world_probe.hpp"
#include "core/lens/lens_plan.hpp"
#include "core/rig/rig_stage.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace TPVCamera::ProbeRecord
{
    inline constexpr std::uint32_t k_magic = 0x52565054u; // "TPVR"
    inline constexpr std::uint32_t k_version = 4;

    /** @brief The bytes of the rig's carry-over in a recording (core/rig/rig_record.cpp). */
    [[nodiscard]] std::uint32_t rig_carry_bytes() noexcept;

    /** @brief The file header: the format and the sizes of the raw structs the frames hold. */
    struct Header
    {
        std::uint32_t magic = k_magic;
        std::uint32_t version = k_version;
        std::uint32_t input_size = sizeof(CollisionInput);
        std::uint32_t shoulder_input_size = sizeof(ShoulderInput);
        std::uint32_t params_size = sizeof(CollisionParams);
        std::uint32_t state_size = sizeof(CollisionState);
        std::uint32_t hit_size = sizeof(RayHit);
        std::uint32_t lens_input_size = sizeof(LensInput);
        std::uint32_t lens_state_size = sizeof(LensState);
        std::uint32_t rig_params_size = sizeof(RigParams);
        std::uint32_t rig_input_size = sizeof(RigInput);
        std::uint32_t rig_frame_size = sizeof(RigFrame);
        std::uint32_t rig_carry_size = rig_carry_bytes();

        [[nodiscard]] bool matches_build() const noexcept
        {
            const Header here{};
            return std::memcmp(this, &here, sizeof(Header)) == 0;
        }
    };

    static_assert(std::is_trivially_copyable_v<CollisionInput> && std::is_trivially_copyable_v<ShoulderInput> &&
                      std::is_trivially_copyable_v<CollisionParams> && std::is_trivially_copyable_v<CollisionState> &&
                      std::is_trivially_copyable_v<RayHit> && std::is_trivially_copyable_v<LensInput> &&
                      std::is_trivially_copyable_v<LensState> && std::is_trivially_copyable_v<RigParams> &&
                      std::is_trivially_copyable_v<RigInput> && std::is_trivially_copyable_v<RigFrame> &&
                      std::is_trivially_copyable_v<ZoomHeld> && std::is_trivially_copyable_v<EyePose> &&
                      std::is_trivially_copyable_v<LookReading> && std::is_trivially_copyable_v<BodyReading>,
                  "A recording holds these structs as raw bytes.");

    /** @brief Which collision a frame record ran. */
    enum class FrameKind : std::uint8_t
    {
        Arm = 1,
        Shoulder = 2,
        Lens = 3,
        Rig = 4,
    };

    /** @brief One recorded probe call. */
    enum class Call : std::uint8_t
    {
        End = 0,
        Ray,
        RefreshScan,
        NodeForCollider,
        StructureDrawnOn,
        StructureOnArm,
        SpansTube,
        OccludersOnSightlines,
        PrefetchMeshes,
        StoppingNodeNear,
        NowMs,
    };

    /** @brief The name of @p call, for a divergence report. */
    [[nodiscard]] const char *call_name(Call call) noexcept;

    /** @brief Appends raw bytes to a recording. */
    class Writer
    {
    public:
        explicit Writer(std::vector<std::byte> &out) noexcept : m_out(out) {}

        void put_bytes(const void *data, std::size_t size)
        {
            const auto *bytes = static_cast<const std::byte *>(data);
            m_out.insert(m_out.end(), bytes, bytes + size);
        }

        template <typename T> void put(const T &value)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            put_bytes(&value, sizeof(T));
        }

    private:
        std::vector<std::byte> &m_out;
    };

    /** @brief Reads raw bytes back from a recording; a read past the end fails the reader for good. */
    class Reader
    {
    public:
        explicit Reader(std::span<const std::byte> in) noexcept : m_in(in) {}

        [[nodiscard]] bool get_bytes(void *data, std::size_t size) noexcept
        {
            if (!m_ok || m_in.size() - m_at < size)
            {
                m_ok = false;
                return false;
            }
            std::memcpy(data, m_in.data() + m_at, size);
            m_at += size;
            return true;
        }

        template <typename T> [[nodiscard]] bool get(T &value) noexcept
        {
            static_assert(std::is_trivially_copyable_v<T>);
            return get_bytes(&value, sizeof(T));
        }

        [[nodiscard]] bool ok() const noexcept { return m_ok; }
        [[nodiscard]] bool at_end() const noexcept { return m_at == m_in.size(); }
        [[nodiscard]] std::size_t offset() const noexcept { return m_at; }

    private:
        std::span<const std::byte> m_in;
        std::size_t m_at = 0;
        bool m_ok = true;
    };

    /** @brief A WorldProbe that forwards each call to @p inner and appends the call and its answer to a recording. */
    class RecordingProbe final : public WorldProbe
    {
    public:
        RecordingProbe(WorldProbe &inner, std::vector<std::byte> &out) noexcept : m_inner(inner), m_out(out) {}

        [[nodiscard]] std::optional<RayHit> ray(QueryLedger::Tag tag, const Vector3 &origin, const Vector3 &sweep,
                                                int objtypes, unsigned int flags, const std::uintptr_t *skip,
                                                int n_skip) override;
        void refresh_scan(const Vector3 *points, int n_points) override;
        [[nodiscard]] void *node_for_collider(std::uintptr_t collider, Rules::Behaviour *cls) override;
        [[nodiscard]] bool structure_drawn_on(QueryLedger::Tag tag, void *node, const Sightline *lines,
                                              int n_lines) override;
        [[nodiscard]] float structure_on_arm(QueryLedger::Tag tag, const Sightline *lines, int n_lines, int min_lines,
                                             float max_fraction, void **node_out, bool trace) override;
        [[nodiscard]] bool spans_tube(void *node, const Sightline *tube) override;
        [[nodiscard]] int occluders_on_sightlines(const Sightline *lines, int n_lines, std::span<void *const> skip,
                                                  SightlineOccluder *out, int cap) override;
        void prefetch_meshes(const Sightline *lines, int n_lines) override;
        [[nodiscard]] bool stopping_node_near(const float *box, float pad) override;
        [[nodiscard]] std::uint64_t now_ms() override;
        void scope_begin(const char *name) noexcept override { m_inner.scope_begin(name); }
        void scope_end() noexcept override { m_inner.scope_end(); }

    private:
        WorldProbe &m_inner;
        Writer m_out;
    };

    /**
     * @brief A WorldProbe that answers each call from a recording, after checking that it is the call recorded there
     *        with the same arguments, bit for bit.
     * @details The first call that differs, or one the recording does not have, marks the replay diverged; from then on
     *          every call answers as if nothing were there (no hit, no node), and divergence() says where it went
     *          wrong.
     */
    class ReplayProbe final : public WorldProbe
    {
    public:
        explicit ReplayProbe(Reader &in) noexcept : m_in(in) {}

        [[nodiscard]] std::optional<RayHit> ray(QueryLedger::Tag tag, const Vector3 &origin, const Vector3 &sweep,
                                                int objtypes, unsigned int flags, const std::uintptr_t *skip,
                                                int n_skip) override;
        void refresh_scan(const Vector3 *points, int n_points) override;
        [[nodiscard]] void *node_for_collider(std::uintptr_t collider, Rules::Behaviour *cls) override;
        [[nodiscard]] bool structure_drawn_on(QueryLedger::Tag tag, void *node, const Sightline *lines,
                                              int n_lines) override;
        [[nodiscard]] float structure_on_arm(QueryLedger::Tag tag, const Sightline *lines, int n_lines, int min_lines,
                                             float max_fraction, void **node_out, bool trace) override;
        [[nodiscard]] bool spans_tube(void *node, const Sightline *tube) override;
        [[nodiscard]] int occluders_on_sightlines(const Sightline *lines, int n_lines, std::span<void *const> skip,
                                                  SightlineOccluder *out, int cap) override;
        void prefetch_meshes(const Sightline *lines, int n_lines) override;
        [[nodiscard]] bool stopping_node_near(const float *box, float pad) override;
        [[nodiscard]] std::uint64_t now_ms() override;
        void scope_begin(const char *) noexcept override {}
        void scope_end() noexcept override {}

        /** @brief Reads the frame's Call::End: false (diverged) when the recording holds more calls. */
        [[nodiscard]] bool finish();

        [[nodiscard]] bool diverged() const noexcept { return !m_divergence.empty(); }
        [[nodiscard]] const std::string &divergence() const noexcept { return m_divergence; }
        [[nodiscard]] int calls() const noexcept { return m_calls; }

    private:
        /** @brief Reads the next call's type and checks it is @p expected. */
        [[nodiscard]] bool expect(Call expected);
        /** @brief Reads @p size bytes recorded as an argument and checks them against @p actual. */
        [[nodiscard]] bool expect_bytes(const void *actual, std::size_t size, const char *what);
        template <typename T> [[nodiscard]] bool expect_value(const T &actual, const char *what)
        {
            return expect_bytes(&actual, sizeof(T), what);
        }
        void diverge(const std::string &why);

        Reader &m_in;
        std::string m_divergence;
        int m_calls = 0;
    };

    /** @brief The trace sink that drops everything: a replay checks the decisions, not the log. */
    class QuietTrace final : public CollisionTrace
    {
    public:
        void motion(EaseMotion) noexcept override {}
        void walk(std::span<const WalkStep>, float) noexcept override {}
        void physics_hit(const RayHit &, float, float, int, const Vector3 &, const Vector3 &) noexcept override {}
        void arm_mesh(void *, float, float) noexcept override {}
        void pull_in(PullInTrace, float) noexcept override {}
        void shoulder(bool, float, void *, Rules::Behaviour, float) noexcept override {}
    };

    /**
     * @brief Runs resolve_camera_collision() on @p probe and appends the whole frame to @p out: the input, the params,
     *        the state before, every probe call with its answer, the output and the state after.
     */
    [[nodiscard]] Vector3 record_arm(std::vector<std::byte> &out, CollisionState &state, const CollisionInput &in,
                                     const CollisionParams &params, WorldProbe &probe, CollisionTrace &trace);

    /** @brief record_arm() for resolve_shoulder_collision(). */
    [[nodiscard]] float record_shoulder(std::vector<std::byte> &out, CollisionState &state, const ShoulderInput &in,
                                        const CollisionParams &params, WorldProbe &probe, CollisionTrace &trace);

    /** @brief record_arm() for plan_lens_near(). */
    [[nodiscard]] std::optional<float> record_lens(std::vector<std::byte> &out, LensState &state, const LensInput &in,
                                                   WorldProbe &probe);

    /** @brief The outcome of replaying a recording. */
    struct ReplayReport
    {
        bool ok = false;
        int frames = 0;
        int arm_frames = 0;
        int shoulder_frames = 0;
        int lens_frames = 0;
        int rig_frames = 0;
        long long calls = 0;
        std::string failure; // the first divergence: frame, call and what differed
    };

    /** @brief Replays every frame of the recording @p data through the core; stops at the first divergence. */
    [[nodiscard]] ReplayReport replay(std::span<const std::byte> data);
} // namespace TPVCamera::ProbeRecord

#endif // TPVCAMERA_CORE_COLLISION_PROBE_RECORD_HPP
