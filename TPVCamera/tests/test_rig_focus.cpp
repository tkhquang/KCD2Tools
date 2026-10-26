/**
 * @file test_rig_focus.cpp
 * @brief core/rig/rig_stage.cpp under the game's focused look: a look the game turns faster than a jump is held off
 *        the camera, except while the focused look runs (RigInput::focused_look), where the camera takes the look as
 *        it is on every frame, with AimBasisSmoothing bypassed.
 */

#include "core/rig/rig_math.hpp"
#include "core/rig/rig_stage.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

namespace
{
    using TPVCamera::BodyReading;
    using TPVCamera::EyePose;
    using TPVCamera::level_rotation;
    using TPVCamera::LookReading;
    using TPVCamera::RigEvent;
    using TPVCamera::RigFrame;
    using TPVCamera::RigInput;
    using TPVCamera::RigParams;
    using TPVCamera::RigPort;
    using TPVCamera::RigState;
    using TPVCamera::Vector3;
    using TPVCamera::ZoomHeld;

    constexpr float k_deg = std::numbers::pi_v<float> / 180.0f;
    constexpr float k_frame = 1.0f / 60.0f;
    // The focused look turns the look about 11 degrees a frame at 60 fps (650 deg/s), past the look snap's jump speed.
    constexpr float k_turn_step = 11.0f * k_deg;
    constexpr int k_settle_frames = 30;
    constexpr int k_turn_frames = 20;
    constexpr std::uintptr_t k_player = 0x1000;

    /** @brief Which side turns the look in a FocusPort: the game (no look input) or the player (all look input). */
    enum class TurnSource
    {
        Game,
        Player,
    };

    /** @brief A player standing at the origin, with no movement input, whose look turns by @ref step each build. */
    class FocusPort final : public RigPort
    {
    public:
        explicit FocusPort(TurnSource source) noexcept : m_source(source) {}

        float look_yaw = 0.0f;
        float look_pitch = -0.1f;
        // This build's turn of the look, reported as the player's look input when the player turns it.
        float step = 0.0f;
        int snaps_held = 0;

        [[nodiscard]] ZoomHeld zoom_held() override { return {}; }
        void ease_out_zoom(float) override {}
        [[nodiscard]] std::optional<EyePose> eye_pose() override
        {
            return EyePose{
                .position = Vector3{0.0f, 0.0f, 1.7f},
                .rotation = level_rotation(look_yaw, look_pitch),
            };
        }
        [[nodiscard]] std::optional<LookReading> look(std::uintptr_t) override
        {
            return LookReading{
                .quat = level_rotation(look_yaw, look_pitch),
                .pitch = look_pitch,
                .input_yaw = m_source == TurnSource::Player ? step : 0.0f,
            };
        }
        [[nodiscard]] float game_fov() override { return 60.0f * k_deg; }
        void override_fov(float, float) override {}
        [[nodiscard]] BodyReading body(std::uintptr_t) override
        {
            return BodyReading{
                .entity = 0x2000,
                .origin = Vector3{},
                .yaw = look_yaw,
                .valid = true,
            };
        }
        [[nodiscard]] std::optional<Vector3> neck(std::uintptr_t) override { return std::nullopt; }
        [[nodiscard]] bool native_turn() override { return false; }
        [[nodiscard]] std::optional<float> move_input() override { return 0.0f; }
        [[nodiscard]] std::optional<float> game_move(std::uintptr_t) override { return 0.0f; }
        [[nodiscard]] bool cursor_shown() override { return false; }
        [[nodiscard]] std::uint32_t game_state() override { return 0u; }
        [[nodiscard]] bool aim_control(float, float, bool, float) override { return true; }
        void body_turn(float) override {}
        void publish_move_press(float, float) override {}
        void withdraw_move_press() override {}
        void event(RigEvent event, float, float, float, std::uint32_t) override
        {
            if (event == RigEvent::LookSnapHeld)
            {
                ++snaps_held;
            }
        }

    private:
        TurnSource m_source;
    };

    /// The rig settings of a plain follow camera on the stable aim basis, with @p aim_basis_smoothing.
    RigParams params_with_smoothing(float aim_basis_smoothing)
    {
        RigParams params{};
        params.follow_distance = 3.0f;
        params.follow_distance_min = 1.0f;
        params.follow_distance_max = 6.0f;
        params.eye_height = 1.7f;
        params.offset_right = 0.5f;
        params.aim_basis_smoothing = aim_basis_smoothing;
        params.stable_aim_basis = true;
        return params;
    }

    /// One engaged third-person build, with the focused look on or off.
    RigInput frame_input(bool focused_look)
    {
        return RigInput{
            .c_player = k_player,
            .view_blend = 1.0f,
            .seconds = k_frame,
            .focused_look = focused_look,
        };
    }

    /// An angle in radians, wrapped to [-pi, pi].
    float wrapped(float angle)
    {
        return std::remainder(angle, 2.0f * std::numbers::pi_v<float>);
    }

    /**
     * @brief Settles the rig on a still look, then turns the look by k_turn_step on each of k_turn_frames builds.
     * @return The largest gap between the rig basis heading and the look heading over the turn (radians).
     */
    float largest_gap_over_the_turn(bool focused_look, float aim_basis_smoothing, FocusPort &port)
    {
        RigState state{};
        const RigParams params = params_with_smoothing(aim_basis_smoothing);
        RigFrame out{};
        for (int i = 0; i < k_settle_frames; ++i)
        {
            EXPECT_TRUE(TPVCamera::run_rig(state, params, frame_input(focused_look), port, out));
        }
        port.step = k_turn_step;
        float largest = 0.0f;
        for (int i = 0; i < k_turn_frames; ++i)
        {
            port.look_yaw = wrapped(port.look_yaw + k_turn_step);
            EXPECT_TRUE(TPVCamera::run_rig(state, params, frame_input(focused_look), port, out));
            const float basis_yaw = std::atan2(-out.forward.x, out.forward.y);
            largest = std::max(largest, std::fabs(wrapped(basis_yaw - port.look_yaw)));
        }
        return largest;
    }
} // namespace

TEST(RigFocus, AGameJumpIsHeldOffTheCameraWithoutTheFocusedLook)
{
    FocusPort port{TurnSource::Game};
    const float gap = largest_gap_over_the_turn(false, 0.0f, port);
    // Every build's turn is a jump, held whole on its build: the camera stays put while the look runs off.
    EXPECT_GT(port.snaps_held, 0);
    EXPECT_GT(gap, 90.0f * k_deg);
}

TEST(RigFocus, TheFocusedLookCarriesTheCameraWithTheGameTurn)
{
    FocusPort port{TurnSource::Game};
    const float gap = largest_gap_over_the_turn(true, 0.0f, port);
    EXPECT_EQ(port.snaps_held, 0);
    EXPECT_LT(gap, 1e-3f);
}

TEST(RigFocus, TheFocusedLookBypassesAimBasisSmoothing)
{
    // The player turns the look, so the look snap holds nothing and only the smoothing can trail the look.
    FocusPort smoothed{TurnSource::Player};
    const float smoothed_gap = largest_gap_over_the_turn(false, 0.3f, smoothed);
    FocusPort focused{TurnSource::Player};
    const float focused_gap = largest_gap_over_the_turn(true, 0.3f, focused);
    EXPECT_EQ(smoothed.snaps_held, 0);
    EXPECT_GT(smoothed_gap, 1.0f * k_deg);
    EXPECT_LT(focused_gap, 1e-3f);
}
