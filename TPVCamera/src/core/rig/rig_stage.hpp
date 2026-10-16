/**
 * @file core/rig/rig_stage.hpp
 * @brief The camera rig of one game-view build: the follow distance and zoom, the stable basis, the FOV, the body
 *        anchor, the free-look orbit, the follow angle, the aim hand-off, camera-relative movement and the up-only arm,
 *        up to where the camera sits before collision.
 *
 * @details The rig reaches the engine only through a RigPort (camera/rig_port.hpp is the live one): the CView pose,
 *          the look controller, the body, the FOV the builder sees, the input, the look and body writes and the trace
 *          lines. Its carry-over is the CameraState it is handed, whose input-thread fields (the orbit accumulators,
 *          the zoom and swap requests) it reads as they stand, and its settings are a RigParams read once per build.
 *          Engine-free, so a recorded frame replays exactly (core/rig/rig_record.hpp). Main thread only.
 */
#ifndef TPVCAMERA_CORE_RIG_RIG_STAGE_HPP
#define TPVCAMERA_CORE_RIG_RIG_STAGE_HPP

#include "core/math/vec.hpp"
#include "global_state.hpp"

#include <cstdint>
#include <optional>

namespace TPVCamera
{
    /** @brief The settings the rig reads, taken once per build (camera/rig_port.hpp, rig_params()). */
    struct RigParams
    {
        float follow_distance = 0.0f;
        float follow_distance_min = 0.0f;
        float follow_distance_max = 0.0f;
        float zoom_step = 0.0f;
        float offset_up = 0.0f;
        float eye_height = 0.0f;
        float offset_right = 0.0f;
        float follow_yaw = 0.0f;
        float follow_pitch = 0.0f;
        float arm_follow_up = 0.0f;
        float lens_drop_up = 0.0f;
        float fov = 0.0f;
        float preset_blend_in_seconds = 0.0f;
        float aim_basis_smoothing = 0.0f;
        float gamepad_orbit_speed_x = 0.0f;
        float gamepad_orbit_speed_y = 0.0f;
        float gamepad_orbit_curve = 0.0f;
        float gamepad_orbit_smoothing = 0.0f;
        float orbit_pitch_min = 0.0f;
        float orbit_pitch_max = 0.0f;
        float orbit_return_speed = 0.0f;
        float orbit_follow_speed = 0.0f;
        float orbit_smoothing = 0.0f;
        bool dynamic_eye_sync = false;
        bool stable_aim_basis = false;
        bool orbit_level_aim = false;
        bool orbit_body_turn = false;
        bool orbit_continuous_align = false;
    };

    /** @brief One build's input to the rig. */
    struct RigInput
    {
        std::uintptr_t c_player = 0; // the live C_Player, or 0: the rig then anchors to the eye
        float view_blend = 0.0f;     // smoothstepped, 0 first person .. 1 third person
        float seconds = 0.0f;        // the build's step (FrameStep::seconds)
        bool repeat_build = false;
        bool paused = false;
    };

    /** @brief The zoom keys held this build. */
    struct ZoomHeld
    {
        bool in = false;
        bool out = false;
    };

    /** @brief The untouched first-person eye pose of the game view. */
    struct EyePose
    {
        Vector3 position{};
        Quaternion rotation{};
    };

    /** @brief The player's look controller as read: its orientation and its scalar pitch, not yet checked. */
    struct LookReading
    {
        Quaternion quat{};
        float pitch = 0.0f;
    };

    /** @brief The player's body: its entity, and its world origin and heading when its matrix read. */
    struct BodyReading
    {
        std::uintptr_t entity = 0;
        Vector3 origin{};
        float yaw = 0.0f;
        bool valid = false;
    };

    /** @brief The rig's trace lines; RigPort::event() formats each (camera/rig_port.cpp). */
    enum class RigEvent : std::uint8_t
    {
        HandoffStart,        // a: turn, b: look pitch, c: target pitch (degrees)
        HandoffTurned,       // a: seconds
        HandoffLost,         //
        HandoffMoved,        //
        HandoffTimeout,      // a: heading gap (degrees), b: seconds
        HandoffWriteFailed,  //
        MoveArmedByGame,     // a: magnitude
        MoveStart,           // a: magnitude, b: continuous align (0 or 1), mask: game state
        MoveStaleSuppressed, // a: magnitude
        MoveStop,            // a: magnitude
        MoveStopGameIdle,    // a: magnitude
        BodyTurnEngaged,     //
        BodyTurnReleased,    //
    };

    /** @brief The engine as the rig sees it (see the file details). */
    class RigPort
    {
    public:
        RigPort() = default;
        RigPort(const RigPort &) = delete;
        RigPort &operator=(const RigPort &) = delete;
        RigPort(RigPort &&) = delete;
        RigPort &operator=(RigPort &&) = delete;
        virtual ~RigPort() = default;

        /** @brief Which zoom keys are held. */
        [[nodiscard]] virtual ZoomHeld zoom_held() = 0;
        /** @brief Hands a reset zoom offset to the preset ease, which eases it out (Presets::ease_out_zoom_offset). */
        virtual void ease_out_zoom(float offset) = 0;
        /** @brief The CView's eye pose, or std::nullopt when it does not read. */
        [[nodiscard]] virtual std::optional<EyePose> eye_pose() = 0;
        /** @brief The look controller of @p c_player, or std::nullopt when it does not read. */
        [[nodiscard]] virtual std::optional<LookReading> look(std::uintptr_t c_player) = 0;
        /** @brief The FOV SetFrustum wrote into the camera for this build (radians). */
        [[nodiscard]] virtual float game_fov() = 0;
        /**
         * @brief Writes @p fov (radians) into the camera and scales its cull-frustum terms by @p ratio, which is
         *        tan(fov / 2) / tan(game_fov / 2).
         */
        virtual void override_fov(float fov, float ratio) = 0;
        /** @brief The body of @p c_player. */
        [[nodiscard]] virtual BodyReading body(std::uintptr_t c_player) = 0;
        /** @brief Whether a native turn-in-place step is under way (native_turn_engaged()). */
        [[nodiscard]] virtual bool native_turn() = 0;
        /** @brief The action dispatcher's movement-input magnitude, or std::nullopt when its hook did not resolve. */
        [[nodiscard]] virtual std::optional<float> move_input() = 0;
        /** @brief The game's own movement input for @p c_player, or std::nullopt (player_move_input()). */
        [[nodiscard]] virtual std::optional<float> game_move(std::uintptr_t c_player) = 0;
        /** @brief Whether the game shows the OS cursor (a UI is up). */
        [[nodiscard]] virtual bool cursor_shown() = 0;
        /** @brief The debounced game state mask. */
        [[nodiscard]] virtual std::uint32_t game_state() = 0;
        /**
         * @brief Eases the look pitch toward @p pitch_target by @p pitch_ease and, with @p set_yaw, sets the look yaw
         *        (apply_orbit_aim_control()).
         * @return False when a requested write did not land.
         */
        [[nodiscard]] virtual bool aim_control(float pitch_ease, float pitch_target, bool set_yaw, float yaw) = 0;
        /** @brief Turns the body to @p yaw for this frame (apply_orbit_body_turn()). */
        virtual void body_turn(float yaw) = 0;
        /** @brief Publishes the heading a move press takes, with the body yaw now (orbit_move_pressed()). */
        virtual void publish_move_press(float heading, float body_yaw) = 0;
        /** @brief Withdraws the move-press heading. */
        virtual void withdraw_move_press() = 0;
        /** @brief A trace line (see RigEvent for the arguments). */
        virtual void event(RigEvent event, float a, float b, float c, std::uint32_t mask) = 0;
    };

    /** @brief What the rig hands the later stages: the camera before collision and what it was built from. */
    struct RigFrame
    {
        // The untouched first-person eye: the view-blend source.
        Vector3 eye_position{};
        Vector3 eye_forward{};
        Vector3 eye_up{};
        // The rig basis forward: the stable look, or the eye's.
        Vector3 forward{};
        // The rig's look and up, after the follow angle and the orbit turned them.
        Vector3 look_forward{};
        Vector3 look_up{};
        // Where the rig puts the camera, before collision.
        Vector3 camera_position{};
        Vector3 pivot{};
        // The head point (anchor_base) and the de-bobbed body origin less the turn hold (anchor_origin).
        Vector3 anchor_base{};
        Vector3 anchor_origin{};
        Vector3 turn_hold{};
        Vector3 body_origin{};
        std::uintptr_t entity = 0;
        // The shoulder offset after SwapShoulderKey's slide (meters, negative = left).
        float offset_right = 0.0f;
        bool body_valid = false;
        // The rendered orientation is rebuilt from look_forward / look_up: the basis was shaped, or the follow angle or
        // the orbit turned the look.
        bool rebuild_basis = false;
    };

    /**
     * @brief Builds the rig for this build.
     * @return False when the CView pose did not read: the caller leaves the view untouched this build. The zoom has
     *         advanced by then.
     */
    [[nodiscard]] bool run_rig(CameraState &cam, const RigParams &params, const RigInput &in, RigPort &port,
                               RigFrame &out);
} // namespace TPVCamera

#endif // TPVCAMERA_CORE_RIG_RIG_STAGE_HPP
