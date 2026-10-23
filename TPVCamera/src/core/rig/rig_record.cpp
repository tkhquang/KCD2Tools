/**
 * @file core/rig/rig_record.cpp
 * @brief Recording and replay of the camera rig (see rig_record.hpp).
 */

#include "core/rig/rig_record.hpp"

#include <atomic>
#include <cstring>
#include <format>
#include <type_traits>

namespace TPVCamera::ProbeRecord
{
    namespace
    {
        /**
         * @brief Calls @p fn(name, field) for every RigState field, in the recording's order.
         * @details The one list of the rig's carry-over: the recording writes the fields in this order, the replay
         *          restores and compares them in it, and rig_carry_bytes() sizes it for the header. A field added to
         *          RigState must be added here, or its replay diverges. The fields go one by one rather than as the
         *          struct's raw bytes, whose padding is not part of the state.
         */
        template <typename State, typename Fn> void for_each_carry_field(State &state, Fn &&fn)
        {
            fn("zoom_offset", state.zoom_offset);
            fn("shoulder_swap", state.shoulder_swap);
            fn("shoulder_swap_valid", state.shoulder_swap_valid);
            fn("orbit_base_held", state.orbit_base_held);
            fn("orbit_base_pitch", state.orbit_base_pitch);
            fn("orbit_base_offset", state.orbit_base_offset);
            fn("orbit_moving", state.orbit_moving);
            fn("orbit_target_yaw", state.orbit_target_yaw);
            fn("orbit_target_valid", state.orbit_target_valid);
            fn("orbit_yaw_at_capture_deg", state.orbit_yaw_at_capture_deg);
            fn("orbit_yaw", state.orbit_yaw);
            fn("orbit_pitch", state.orbit_pitch);
            fn("orbit_pitch_lo", state.orbit_pitch_lo);
            fn("orbit_pitch_hi", state.orbit_pitch_hi);
            fn("orbit_pad_yaw", state.orbit_pad_yaw);
            fn("orbit_pad_pitch", state.orbit_pad_pitch);
            fn("orbit_pad_yaw_eased", state.orbit_pad_yaw_eased);
            fn("orbit_pad_pitch_eased", state.orbit_pad_pitch_eased);
            fn("orbit_yaw_render", state.orbit_yaw_render);
            fn("orbit_pitch_render", state.orbit_pitch_render);
            fn("orbit_render_valid", state.orbit_render_valid);
            fn("orbit_steer_smooth", state.orbit_steer_smooth);
            fn("orbit_steer_valid", state.orbit_steer_valid);
            fn("mount_follow_idle", state.mount_follow_idle);
            fn("mount_follow_seen_yaw", state.mount_follow_seen_yaw);
            fn("mount_follow_seen_valid", state.mount_follow_seen_valid);
            fn("orbit_move_armed", state.orbit_move_armed);
            fn("orbit_move_armed_by_game", state.orbit_move_armed_by_game);
            fn("orbit_move_game_idle", state.orbit_move_game_idle);
            // Member by member: the struct's padding bytes would make a bitwise comparison of the whole differ.
            fn("orbit_move_stand_in.active", state.orbit_move_stand_in.active);
            fn("orbit_move_stand_in.idle_seconds", state.orbit_move_stand_in.idle_seconds);
            fn("rig_stale_move_logged", state.rig_stale_move_logged);
            fn("rig_body_turn_logged", state.rig_body_turn_logged);
            fn("body_anchor_lean_x", state.body_anchor_lean_x);
            fn("body_anchor_lean_y", state.body_anchor_lean_y);
            fn("body_anchor_lean_target_x", state.body_anchor_lean_target_x);
            fn("body_anchor_lean_target_y", state.body_anchor_lean_target_y);
            fn("body_anchor_up", state.body_anchor_up);
            fn("body_anchor_up_target", state.body_anchor_up_target);
            fn("body_anchor_yaw", state.body_anchor_yaw);
            fn("body_anchor_rest", state.body_anchor_rest);
            fn("body_anchor_valid", state.body_anchor_valid);
            fn("real_eye_height", state.real_eye_height);
            fn("eye_sync_engaged", state.eye_sync_engaged);
            fn("eye_sync_effective", state.eye_sync_effective);
            fn("fov_ease", state.fov_ease);
            fn("fov_ease_valid", state.fov_ease_valid);
            fn("basis_quat_x", state.basis_quat_x);
            fn("basis_quat_y", state.basis_quat_y);
            fn("basis_quat_z", state.basis_quat_z);
            fn("basis_quat_w", state.basis_quat_w);
            fn("basis_quat_valid", state.basis_quat_valid);
            fn("look_snap_heading", state.look_snap_heading);
            fn("look_snap_pitch", state.look_snap_pitch);
            fn("look_snap_last_heading", state.look_snap_last_heading);
            fn("look_snap_last_pitch", state.look_snap_last_pitch);
            fn("look_snap_last_speed", state.look_snap_last_speed);
            fn("look_snap_last_valid", state.look_snap_last_valid);
            fn("look_input_idle", state.look_input_idle);
            fn("raw_look_seen", state.raw_look_seen);
            fn("raw_look_pad_heading", state.raw_look_pad_heading);
            fn("raw_look_pad_pitch", state.raw_look_pad_pitch);
            fn("raw_look_idle", state.raw_look_idle);
            fn("turn_hold_x", state.turn_hold_x);
            fn("turn_hold_y", state.turn_hold_y);
            fn("turn_hold_timer", state.turn_hold_timer);
            fn("turn_track_x", state.turn_track_x);
            fn("turn_track_y", state.turn_track_y);
            fn("turn_track_yaw", state.turn_track_yaw);
            fn("turn_track_step", state.turn_track_step);
            fn("turn_track_valid", state.turn_track_valid);
            fn("rig_last_built", state.rig_last_built);
            fn("rig_last_offset_x", state.rig_last_offset_x);
            fn("rig_last_offset_y", state.rig_last_offset_y);
            fn("rig_last_offset_z", state.rig_last_offset_z);
            fn("rig_last_orbited", state.rig_last_orbited);
            fn("rig_last_azimuth", state.rig_last_azimuth);
            fn("rig_last_elevation", state.rig_last_elevation);
            fn("rig_last_turn", state.rig_last_turn);
            fn("rig_last_rise", state.rig_last_rise);
            fn("rig_last_yaw", state.rig_last_yaw);
            fn("rig_last_pitch", state.rig_last_pitch);
            fn("aim_handoff_active", state.aim_handoff_active);
            fn("aim_handoff_written", state.aim_handoff_written);
            fn("aim_handoff_cancel", state.aim_handoff_cancel);
            fn("aim_handoff_opponent", state.aim_handoff_opponent);
            fn("facing_hold_valid", state.facing_hold_valid);
            fn("facing_hold_last_yaw", state.facing_hold_last_yaw);
            fn("lock_frame_weight", state.lock_frame_weight);
            fn("lock_frame_focus_valid", state.lock_frame_focus_valid);
            fn("lock_frame_focus_x", state.lock_frame_focus_x);
            fn("lock_frame_focus_y", state.lock_frame_focus_y);
            fn("lock_frame_focus_z", state.lock_frame_focus_z);
            fn("lock_frame_raw_x", state.lock_frame_raw_x);
            fn("lock_frame_raw_y", state.lock_frame_raw_y);
            fn("lock_frame_raw_z", state.lock_frame_raw_z);
            fn("lock_frame_from_x", state.lock_frame_from_x);
            fn("lock_frame_from_y", state.lock_frame_from_y);
            fn("lock_frame_from_z", state.lock_frame_from_z);
            fn("lock_frame_switch", state.lock_frame_switch);
            fn("lock_frame_entering", state.lock_frame_entering);
            fn("lock_frame_entry_x", state.lock_frame_entry_x);
            fn("lock_frame_entry_y", state.lock_frame_entry_y);
            fn("lock_frame_entry_z", state.lock_frame_entry_z);
            fn("lock_frame_entry_yaw", state.lock_frame_entry_yaw);
            fn("lock_frame_entry_pitch", state.lock_frame_entry_pitch);
            fn("aim_handoff_player", state.aim_handoff_player);
            fn("aim_handoff_azimuth", state.aim_handoff_azimuth);
            fn("aim_handoff_elevation", state.aim_handoff_elevation);
            fn("aim_handoff_yaw", state.aim_handoff_yaw);
            fn("aim_handoff_pitch", state.aim_handoff_pitch);
            fn("aim_handoff_seconds", state.aim_handoff_seconds);
        }

        /** @brief Appends the rig's carry-over in @p state to a recording. */
        void put_carry(Writer &out, const RigState &state)
        {
            for_each_carry_field(state, [&out](const char *, const auto &field) { out.put(field); });
        }

        /** @brief Sets the rig's carry-over in @p state from a recording. @return False when the recording ended. */
        [[nodiscard]] bool restore_carry(Reader &in, RigState &state)
        {
            bool ok = true;
            for_each_carry_field(state, [&in, &ok](const char *, auto &field) { ok = ok && in.get(field); });
            return ok;
        }

        /**
         * @brief Compares the rig's carry-over in @p state with the one recorded next, bit for bit.
         * @return The first field that differs, "the recording ended", or nullptr when all match.
         */
        [[nodiscard]] const char *carry_difference(Reader &in, const RigState &state)
        {
            const char *difference = nullptr;
            for_each_carry_field(state,
                                 [&in, &difference](const char *name, const auto &field)
                                 {
                                     std::remove_cvref_t<decltype(field)> recorded{};
                                     if (!in.get(recorded))
                                     {
                                         difference = difference != nullptr ? difference : "the recording ended";
                                         return;
                                     }
                                     if (difference == nullptr && std::memcmp(&field, &recorded, sizeof(field)) != 0)
                                     {
                                         difference = name;
                                     }
                                 });
            return difference;
        }

        [[nodiscard]] bool same_bits(const Vector3 &a, const Vector3 &b) noexcept
        {
            return std::memcmp(&a, &b, sizeof(Vector3)) == 0;
        }

        /** @brief The first field of two rig frames that differs, bit for bit, or nullptr. */
        [[nodiscard]] const char *frame_difference(const RigFrame &a, const RigFrame &b) noexcept
        {
            if (!same_bits(a.eye_position, b.eye_position) || !same_bits(a.eye_forward, b.eye_forward) ||
                !same_bits(a.eye_up, b.eye_up))
            {
                return "eye";
            }
            if (!same_bits(a.forward, b.forward) || !same_bits(a.look_forward, b.look_forward) ||
                !same_bits(a.look_up, b.look_up) || a.rebuild_basis != b.rebuild_basis)
            {
                return "orientation";
            }
            if (!same_bits(a.pick_forward, b.pick_forward) || a.lock_framed != b.lock_framed)
            {
                return "target pick forward";
            }
            if (!same_bits(a.camera_position, b.camera_position))
            {
                return "camera position";
            }
            if (!same_bits(a.pivot, b.pivot) || !same_bits(a.anchor_base, b.anchor_base) ||
                !same_bits(a.anchor_origin, b.anchor_origin) || !same_bits(a.turn_hold, b.turn_hold))
            {
                return "anchor";
            }
            if (!same_bits(a.body_origin, b.body_origin) || a.body_valid != b.body_valid || a.entity != b.entity)
            {
                return "body";
            }
            if (std::memcmp(&a.offset_right, &b.offset_right, sizeof(float)) != 0)
            {
                return "shoulder offset";
            }
            return nullptr;
        }

        template <typename T> void put_optional(Writer &out, const std::optional<T> &value)
        {
            out.put(static_cast<std::uint8_t>(value.has_value()));
            if (value.has_value())
            {
                out.put(*value);
            }
        }
    } // namespace

    std::uint32_t rig_carry_bytes() noexcept
    {
        static const std::uint32_t s_bytes = []
        {
            const RigState state{};
            std::uint32_t bytes = 0;
            for_each_carry_field(state, [&bytes](const char *, const auto &field)
                                 { bytes += static_cast<std::uint32_t>(sizeof(field)); });
            return bytes;
        }();
        return s_bytes;
    }

    const char *rig_call_name(RigCall call) noexcept
    {
        switch (call)
        {
        case RigCall::End:
            return "end";
        case RigCall::ZoomHeld:
            return "zoom_held";
        case RigCall::EaseOutZoom:
            return "ease_out_zoom";
        case RigCall::EyePose:
            return "eye_pose";
        case RigCall::Look:
            return "look";
        case RigCall::GameFov:
            return "game_fov";
        case RigCall::OverrideFov:
            return "override_fov";
        case RigCall::Body:
            return "body";
        case RigCall::Neck:
            return "neck";
        case RigCall::NativeTurn:
            return "native_turn";
        case RigCall::MoveInput:
            return "move_input";
        case RigCall::GameMove:
            return "game_move";
        case RigCall::CursorShown:
            return "cursor_shown";
        case RigCall::GameState:
            return "game_state";
        case RigCall::AimControl:
            return "aim_control";
        case RigCall::BodyTurn:
            return "body_turn";
        case RigCall::PublishMovePress:
            return "publish_move_press";
        case RigCall::WithdrawMovePress:
            return "withdraw_move_press";
        case RigCall::Event:
            return "event";
        }
        return "?";
    }

    ZoomHeld RecordingRigPort::zoom_held()
    {
        const ZoomHeld held = m_inner.zoom_held();
        m_out.put(RigCall::ZoomHeld);
        m_out.put(held);
        return held;
    }

    void RecordingRigPort::ease_out_zoom(float offset)
    {
        m_inner.ease_out_zoom(offset);
        m_out.put(RigCall::EaseOutZoom);
        m_out.put(offset);
    }

    std::optional<EyePose> RecordingRigPort::eye_pose()
    {
        const std::optional<EyePose> pose = m_inner.eye_pose();
        m_out.put(RigCall::EyePose);
        put_optional(m_out, pose);
        return pose;
    }

    std::optional<LookReading> RecordingRigPort::look(std::uintptr_t c_player)
    {
        const std::optional<LookReading> look = m_inner.look(c_player);
        m_out.put(RigCall::Look);
        m_out.put(c_player);
        put_optional(m_out, look);
        return look;
    }

    float RecordingRigPort::game_fov()
    {
        const float fov = m_inner.game_fov();
        m_out.put(RigCall::GameFov);
        m_out.put(fov);
        return fov;
    }

    void RecordingRigPort::override_fov(float fov, float ratio)
    {
        m_inner.override_fov(fov, ratio);
        m_out.put(RigCall::OverrideFov);
        m_out.put(fov);
        m_out.put(ratio);
    }

    BodyReading RecordingRigPort::body(std::uintptr_t c_player)
    {
        const BodyReading body = m_inner.body(c_player);
        m_out.put(RigCall::Body);
        m_out.put(c_player);
        m_out.put(body);
        return body;
    }

    std::optional<Vector3> RecordingRigPort::neck(std::uintptr_t entity)
    {
        const std::optional<Vector3> neck = m_inner.neck(entity);
        m_out.put(RigCall::Neck);
        m_out.put(entity);
        put_optional(m_out, neck);
        return neck;
    }

    bool RecordingRigPort::native_turn()
    {
        const bool turning = m_inner.native_turn();
        m_out.put(RigCall::NativeTurn);
        m_out.put(static_cast<std::uint8_t>(turning));
        return turning;
    }

    std::optional<float> RecordingRigPort::move_input()
    {
        const std::optional<float> magnitude = m_inner.move_input();
        m_out.put(RigCall::MoveInput);
        put_optional(m_out, magnitude);
        return magnitude;
    }

    std::optional<float> RecordingRigPort::game_move(std::uintptr_t c_player)
    {
        const std::optional<float> magnitude = m_inner.game_move(c_player);
        m_out.put(RigCall::GameMove);
        m_out.put(c_player);
        put_optional(m_out, magnitude);
        return magnitude;
    }

    bool RecordingRigPort::cursor_shown()
    {
        const bool shown = m_inner.cursor_shown();
        m_out.put(RigCall::CursorShown);
        m_out.put(static_cast<std::uint8_t>(shown));
        return shown;
    }

    std::uint32_t RecordingRigPort::game_state()
    {
        const std::uint32_t state = m_inner.game_state();
        m_out.put(RigCall::GameState);
        m_out.put(state);
        return state;
    }

    bool RecordingRigPort::aim_control(float pitch_ease, float pitch_target, bool set_yaw, float yaw)
    {
        const bool wrote = m_inner.aim_control(pitch_ease, pitch_target, set_yaw, yaw);
        m_out.put(RigCall::AimControl);
        m_out.put(pitch_ease);
        m_out.put(pitch_target);
        m_out.put(static_cast<std::uint8_t>(set_yaw));
        m_out.put(yaw);
        m_out.put(static_cast<std::uint8_t>(wrote));
        return wrote;
    }

    void RecordingRigPort::body_turn(float yaw)
    {
        m_inner.body_turn(yaw);
        m_out.put(RigCall::BodyTurn);
        m_out.put(yaw);
    }

    void RecordingRigPort::publish_move_press(float heading, float body_yaw)
    {
        m_inner.publish_move_press(heading, body_yaw);
        m_out.put(RigCall::PublishMovePress);
        m_out.put(heading);
        m_out.put(body_yaw);
    }

    void RecordingRigPort::withdraw_move_press()
    {
        m_inner.withdraw_move_press();
        m_out.put(RigCall::WithdrawMovePress);
    }

    void RecordingRigPort::event(RigEvent event, float a, float b, float c, std::uint32_t mask)
    {
        m_inner.event(event, a, b, c, mask);
        m_out.put(RigCall::Event);
        m_out.put(event);
        m_out.put(a);
        m_out.put(b);
        m_out.put(c);
        m_out.put(mask);
    }

    void ReplayRigPort::diverge(const std::string &why)
    {
        if (m_divergence.empty())
        {
            m_divergence = std::format("rig call {}: {}", m_calls, why);
        }
    }

    bool ReplayRigPort::expect(RigCall expected)
    {
        if (diverged())
        {
            return false;
        }
        ++m_calls;
        RigCall recorded = RigCall::End;
        if (!m_in.get(recorded))
        {
            diverge(std::format("{} asked, the recording ended", rig_call_name(expected)));
            return false;
        }
        if (recorded != expected)
        {
            diverge(std::format("{} asked, {} recorded", rig_call_name(expected), rig_call_name(recorded)));
            return false;
        }
        return true;
    }

    template <typename T> bool ReplayRigPort::expect_value(const T &actual, const char *what)
    {
        T recorded{};
        if (!m_in.get(recorded))
        {
            diverge(std::format("{}: the recording ended", what));
            return false;
        }
        if (std::memcmp(&recorded, &actual, sizeof(T)) != 0)
        {
            diverge(std::format("{} differs", what));
            return false;
        }
        return true;
    }

    template <typename T> bool ReplayRigPort::answer(T &value)
    {
        if (!m_in.get(value))
        {
            diverge("the answer: the recording ended");
            return false;
        }
        return true;
    }

    ZoomHeld ReplayRigPort::zoom_held()
    {
        ZoomHeld held{};
        if (!expect(RigCall::ZoomHeld) || !answer(held))
        {
            return ZoomHeld{};
        }
        return held;
    }

    void ReplayRigPort::ease_out_zoom(float offset)
    {
        (void)(expect(RigCall::EaseOutZoom) && expect_value(offset, "zoom offset"));
    }

    std::optional<EyePose> ReplayRigPort::eye_pose()
    {
        std::uint8_t has = 0;
        EyePose pose{};
        if (!expect(RigCall::EyePose) || !answer(has) || (has != 0 && !answer(pose)))
        {
            return std::nullopt;
        }
        return has != 0 ? std::optional<EyePose>{pose} : std::nullopt;
    }

    std::optional<LookReading> ReplayRigPort::look(std::uintptr_t c_player)
    {
        std::uint8_t has = 0;
        LookReading look{};
        if (!expect(RigCall::Look) || !expect_value(c_player, "look player") || !answer(has) ||
            (has != 0 && !answer(look)))
        {
            return std::nullopt;
        }
        return has != 0 ? std::optional<LookReading>{look} : std::nullopt;
    }

    float ReplayRigPort::game_fov()
    {
        float fov = 0.0f;
        if (!expect(RigCall::GameFov) || !answer(fov))
        {
            return 0.0f;
        }
        return fov;
    }

    void ReplayRigPort::override_fov(float fov, float ratio)
    {
        (void)(expect(RigCall::OverrideFov) && expect_value(fov, "fov") && expect_value(ratio, "fov ratio"));
    }

    BodyReading ReplayRigPort::body(std::uintptr_t c_player)
    {
        BodyReading body{};
        if (!expect(RigCall::Body) || !expect_value(c_player, "body player") || !answer(body))
        {
            return BodyReading{};
        }
        return body;
    }

    std::optional<Vector3> ReplayRigPort::neck(std::uintptr_t entity)
    {
        std::uint8_t has = 0;
        Vector3 neck{};
        if (!expect(RigCall::Neck) || !expect_value(entity, "neck entity") || !answer(has) ||
            (has != 0 && !answer(neck)))
        {
            return std::nullopt;
        }
        return has != 0 ? std::optional<Vector3>{neck} : std::nullopt;
    }

    bool ReplayRigPort::native_turn()
    {
        std::uint8_t turning = 0;
        return expect(RigCall::NativeTurn) && answer(turning) && turning != 0;
    }

    std::optional<float> ReplayRigPort::move_input()
    {
        std::uint8_t has = 0;
        float magnitude = 0.0f;
        if (!expect(RigCall::MoveInput) || !answer(has) || (has != 0 && !answer(magnitude)))
        {
            return std::nullopt;
        }
        return has != 0 ? std::optional<float>{magnitude} : std::nullopt;
    }

    std::optional<float> ReplayRigPort::game_move(std::uintptr_t c_player)
    {
        std::uint8_t has = 0;
        float magnitude = 0.0f;
        if (!expect(RigCall::GameMove) || !expect_value(c_player, "game move player") || !answer(has) ||
            (has != 0 && !answer(magnitude)))
        {
            return std::nullopt;
        }
        return has != 0 ? std::optional<float>{magnitude} : std::nullopt;
    }

    bool ReplayRigPort::cursor_shown()
    {
        std::uint8_t shown = 0;
        return expect(RigCall::CursorShown) && answer(shown) && shown != 0;
    }

    std::uint32_t ReplayRigPort::game_state()
    {
        std::uint32_t state = 0;
        if (!expect(RigCall::GameState) || !answer(state))
        {
            return 0;
        }
        return state;
    }

    bool ReplayRigPort::aim_control(float pitch_ease, float pitch_target, bool set_yaw, float yaw)
    {
        std::uint8_t wrote = 0;
        return expect(RigCall::AimControl) && expect_value(pitch_ease, "aim pitch ease") &&
               expect_value(pitch_target, "aim pitch target") &&
               expect_value(static_cast<std::uint8_t>(set_yaw), "aim set yaw") && expect_value(yaw, "aim yaw") &&
               answer(wrote) && wrote != 0;
    }

    void ReplayRigPort::body_turn(float yaw)
    {
        (void)(expect(RigCall::BodyTurn) && expect_value(yaw, "body turn yaw"));
    }

    void ReplayRigPort::publish_move_press(float heading, float body_yaw)
    {
        (void)(expect(RigCall::PublishMovePress) && expect_value(heading, "move press heading") &&
               expect_value(body_yaw, "move press body yaw"));
    }

    void ReplayRigPort::withdraw_move_press()
    {
        (void)expect(RigCall::WithdrawMovePress);
    }

    void ReplayRigPort::event(RigEvent event, float a, float b, float c, std::uint32_t mask)
    {
        (void)(expect(RigCall::Event) && expect_value(event, "event") && expect_value(a, "event a") &&
               expect_value(b, "event b") && expect_value(c, "event c") && expect_value(mask, "event mask"));
    }

    bool ReplayRigPort::finish()
    {
        if (diverged())
        {
            return false;
        }
        RigCall recorded = RigCall::ZoomHeld;
        if (!m_in.get(recorded) || recorded != RigCall::End)
        {
            diverge(std::format("the build ended, the recording holds {} more", rig_call_name(recorded)));
            return false;
        }
        return true;
    }

    bool record_rig(std::vector<std::byte> &out, RigState &state, const RigParams &params, const RigInput &in,
                    RigPort &port, RigFrame &frame)
    {
        Writer writer{out};
        writer.put(FrameKind::Rig);
        writer.put(params);
        writer.put(in);
        put_carry(writer, state);
        RecordingRigPort recording{port, out};
        const bool built = run_rig(state, params, in, recording, frame);
        writer.put(RigCall::End);
        writer.put(static_cast<std::uint8_t>(built));
        writer.put(frame);
        put_carry(writer, state);
        return built;
    }

    std::string replay_rig_frame(Reader &reader, long long &calls)
    {
        RigParams params{};
        RigInput in{};
        RigState state{};
        if (!reader.get(params) || !reader.get(in) || !restore_carry(reader, state))
        {
            return "truncated";
        }
        ReplayRigPort port{reader};
        RigFrame frame{};
        const bool built = run_rig(state, params, in, port, frame);
        calls += port.calls();
        std::uint8_t recorded_built = 0;
        RigFrame recorded{};
        const bool finished = port.finish();
        if (!finished || !reader.get(recorded_built) || !reader.get(recorded))
        {
            return port.diverged() ? port.divergence() : std::string{"truncated"};
        }
        if (built != (recorded_built != 0))
        {
            return "the built flag differs";
        }
        if (const char *what = frame_difference(frame, recorded); what != nullptr)
        {
            return std::format("the frame differs ({})", what);
        }
        if (const char *what = carry_difference(reader, state); what != nullptr)
        {
            return std::format("the state after differs ({})", what);
        }
        return {};
    }
} // namespace TPVCamera::ProbeRecord
