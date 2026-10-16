/**
 * @file core/rig/rig_record.hpp
 * @brief Recording and replay of the camera rig: every call of a build's rig to the RigPort, with its answer, between
 *        the rig's carry-over before and its frame and carry-over after.
 *
 * @details Part of the frame recording (core/collision/probe_record.hpp), which holds the stream format and the replay
 *          loop. A rig frame is FrameKind::Rig, RigParams, RigInput, the carry-over before, the port calls (each a
 *          RigCall byte, its arguments and its answer), RigCall::End, a built byte, RigFrame and the carry-over after.
 *          The carry-over is every CameraState field the rig reads or writes, written one by one in a fixed order
 *          (rig_record.cpp, for_each_carry_field()), the input thread's included, as they stand right before the rig
 *          runs. Engine-free, so the replay test compiles it as is.
 */
#ifndef TPVCAMERA_CORE_RIG_RIG_RECORD_HPP
#define TPVCAMERA_CORE_RIG_RIG_RECORD_HPP

#include "core/collision/probe_record.hpp"
#include "core/rig/rig_stage.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace TPVCamera::ProbeRecord
{
    /** @brief One recorded rig port call. */
    enum class RigCall : std::uint8_t
    {
        End = 0,
        ZoomHeld,
        EaseOutZoom,
        EyePose,
        Look,
        GameFov,
        OverrideFov,
        Body,
        NativeTurn,
        MoveInput,
        GameMove,
        CursorShown,
        GameState,
        AimControl,
        BodyTurn,
        PublishMovePress,
        WithdrawMovePress,
        Event,
    };

    /** @brief The name of @p call, for a divergence report. */
    [[nodiscard]] const char *rig_call_name(RigCall call) noexcept;

    /** @brief A RigPort that forwards each call to @p inner and appends the call and its answer to a recording. */
    class RecordingRigPort final : public RigPort
    {
    public:
        RecordingRigPort(RigPort &inner, std::vector<std::byte> &out) noexcept : inner_(inner), out_(out) {}

        [[nodiscard]] ZoomHeld zoom_held() override;
        void ease_out_zoom(float offset) override;
        [[nodiscard]] std::optional<EyePose> eye_pose() override;
        [[nodiscard]] std::optional<LookReading> look(std::uintptr_t c_player) override;
        [[nodiscard]] float game_fov() override;
        void override_fov(float fov, float ratio) override;
        [[nodiscard]] BodyReading body(std::uintptr_t c_player) override;
        [[nodiscard]] bool native_turn() override;
        [[nodiscard]] std::optional<float> move_input() override;
        [[nodiscard]] std::optional<float> game_move(std::uintptr_t c_player) override;
        [[nodiscard]] bool cursor_shown() override;
        [[nodiscard]] std::uint32_t game_state() override;
        [[nodiscard]] bool aim_control(float pitch_ease, float pitch_target, bool set_yaw, float yaw) override;
        void body_turn(float yaw) override;
        void publish_move_press(float heading, float body_yaw) override;
        void withdraw_move_press() override;
        void event(RigEvent event, float a, float b, float c, std::uint32_t mask) override;

    private:
        RigPort &inner_;
        Writer out_;
    };

    /**
     * @brief A RigPort that answers each call from a recording, after checking that it is the call recorded there
     *        with the same arguments, bit for bit.
     * @details The first call that differs, or one the recording does not have, marks the replay diverged; from then on
     *          every call answers as if nothing read (no pose, no body, no input), and divergence() says where it went
     *          wrong.
     */
    class ReplayRigPort final : public RigPort
    {
    public:
        explicit ReplayRigPort(Reader &in) noexcept : in_(in) {}

        [[nodiscard]] ZoomHeld zoom_held() override;
        void ease_out_zoom(float offset) override;
        [[nodiscard]] std::optional<EyePose> eye_pose() override;
        [[nodiscard]] std::optional<LookReading> look(std::uintptr_t c_player) override;
        [[nodiscard]] float game_fov() override;
        void override_fov(float fov, float ratio) override;
        [[nodiscard]] BodyReading body(std::uintptr_t c_player) override;
        [[nodiscard]] bool native_turn() override;
        [[nodiscard]] std::optional<float> move_input() override;
        [[nodiscard]] std::optional<float> game_move(std::uintptr_t c_player) override;
        [[nodiscard]] bool cursor_shown() override;
        [[nodiscard]] std::uint32_t game_state() override;
        [[nodiscard]] bool aim_control(float pitch_ease, float pitch_target, bool set_yaw, float yaw) override;
        void body_turn(float yaw) override;
        void publish_move_press(float heading, float body_yaw) override;
        void withdraw_move_press() override;
        void event(RigEvent event, float a, float b, float c, std::uint32_t mask) override;

        /** @brief Reads the frame's RigCall::End: false (diverged) when the recording holds more calls. */
        [[nodiscard]] bool finish();

        [[nodiscard]] bool diverged() const noexcept { return !divergence_.empty(); }
        [[nodiscard]] const std::string &divergence() const noexcept { return divergence_; }
        [[nodiscard]] int calls() const noexcept { return calls_; }

    private:
        /** @brief Reads the next call's type and checks it is @p expected. */
        [[nodiscard]] bool expect(RigCall expected);
        /** @brief Reads a value recorded as an argument and checks it against @p actual, bit for bit. */
        template <typename T> [[nodiscard]] bool expect_value(const T &actual, const char *what);
        /** @brief Reads a recorded answer into @p value. */
        template <typename T> [[nodiscard]] bool answer(T &value);
        void diverge(const std::string &why);

        Reader &in_;
        std::string divergence_;
        int calls_ = 0;
    };

    /**
     * @brief Runs run_rig() on @p port and appends the whole build to @p out: the params, the input, the carry-over
     *        before, every port call with its answer, the frame and the carry-over after.
     */
    [[nodiscard]] bool record_rig(std::vector<std::byte> &out, CameraState &cam, const RigParams &params,
                                  const RigInput &in, RigPort &port, RigFrame &frame);

    /**
     * @brief Replays one rig frame whose FrameKind @p reader has just read.
     * @param calls Incremented by the frame's port calls.
     * @return The divergence, or an empty string when the frame replayed bit for bit.
     */
    [[nodiscard]] std::string replay_rig_frame(Reader &reader, long long &calls);
} // namespace TPVCamera::ProbeRecord

#endif // TPVCAMERA_CORE_RIG_RIG_RECORD_HPP
