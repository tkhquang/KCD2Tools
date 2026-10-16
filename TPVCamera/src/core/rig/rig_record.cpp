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
        template <typename T> struct FieldValue
        {
            using type = T;
        };
        template <typename T> struct FieldValue<std::atomic<T>>
        {
            using type = T;
        };

        /** @brief The value a CameraState field holds: an atomic one loaded, a plain one as it is. */
        template <typename F> [[nodiscard]] typename FieldValue<F>::type field_value(const F &field) noexcept
        {
            if constexpr (std::is_same_v<F, typename FieldValue<F>::type>)
            {
                return field;
            }
            else
            {
                return field.load(std::memory_order_relaxed);
            }
        }

        /** @brief Sets a CameraState field: an atomic one stored, a plain one assigned. */
        template <typename F> void set_field(F &field, const typename FieldValue<F>::type &value) noexcept
        {
            if constexpr (std::is_same_v<F, typename FieldValue<F>::type>)
            {
                field = value;
            }
            else
            {
                field.store(value, std::memory_order_relaxed);
            }
        }

        /**
         * @brief Calls @p fn(name, field) for every CameraState field the rig reads or writes, in the recording's
         *        order.
         * @details The one list of the rig's carry-over: the recording writes the fields in this order, the replay
         *          restores and compares them in it, and rig_carry_bytes() sizes it for the header. A field the rig
         *          starts to read must be added here, or its replay diverges.
         */
        template <typename Fn> void for_each_carry_field(CameraState &cam, Fn &&fn)
        {
            // Written by the input thread, the hotkeys, the overlay and the detour between builds.
            fn("zoom_offset", cam.zoom_offset);
            fn("zoom_reset_requested", cam.zoom_reset_requested);
            fn("shoulder_swapped", cam.shoulder_swapped);
            fn("orbit_active", cam.orbit_active);
            fn("orbit_yaw", cam.orbit_yaw);
            fn("orbit_pitch", cam.orbit_pitch);
            fn("orbit_pitch_lo", cam.orbit_pitch_lo);
            fn("orbit_pitch_hi", cam.orbit_pitch_hi);
            fn("orbit_pad_yaw", cam.orbit_pad_yaw);
            fn("orbit_pad_pitch", cam.orbit_pad_pitch);
            fn("orbit_suspended", cam.orbit_suspended);
            fn("aim_handoff_requested", cam.aim_handoff_requested);
            fn("aim_handoff_cancel", cam.aim_handoff_cancel);
            // Published by the rig for the overlay.
            fn("real_eye_height", cam.real_eye_height);
            fn("eye_sync_engaged", cam.eye_sync_engaged);
            fn("eye_sync_effective", cam.eye_sync_effective);
            // The rig's own carry-over.
            fn("shoulder_swap", cam.shoulder_swap);
            fn("shoulder_swap_valid", cam.shoulder_swap_valid);
            fn("orbit_base_held", cam.orbit_base_held);
            fn("orbit_base_pitch", cam.orbit_base_pitch);
            fn("orbit_base_offset", cam.orbit_base_offset);
            fn("orbit_moving", cam.orbit_moving);
            fn("orbit_target_yaw", cam.orbit_target_yaw);
            fn("orbit_target_valid", cam.orbit_target_valid);
            fn("orbit_yaw_at_capture_deg", cam.orbit_yaw_at_capture_deg);
            fn("orbit_pad_yaw_eased", cam.orbit_pad_yaw_eased);
            fn("orbit_pad_pitch_eased", cam.orbit_pad_pitch_eased);
            fn("orbit_yaw_render", cam.orbit_yaw_render);
            fn("orbit_pitch_render", cam.orbit_pitch_render);
            fn("orbit_render_valid", cam.orbit_render_valid);
            fn("orbit_steer_smooth", cam.orbit_steer_smooth);
            fn("orbit_steer_valid", cam.orbit_steer_valid);
            fn("mount_follow_idle", cam.mount_follow_idle);
            fn("mount_follow_seen_yaw", cam.mount_follow_seen_yaw);
            fn("mount_follow_seen_valid", cam.mount_follow_seen_valid);
            fn("orbit_move_armed", cam.orbit_move_armed);
            fn("orbit_move_armed_by_game", cam.orbit_move_armed_by_game);
            fn("orbit_move_game_idle", cam.orbit_move_game_idle);
            fn("rig_stale_move_logged", cam.rig_stale_move_logged);
            fn("rig_body_turn_logged", cam.rig_body_turn_logged);
            fn("eye_sync_applied", cam.eye_sync_applied);
            fn("eye_sync_valid", cam.eye_sync_valid);
            fn("fov_ease", cam.fov_ease);
            fn("fov_ease_valid", cam.fov_ease_valid);
            fn("basis_quat_x", cam.basis_quat_x);
            fn("basis_quat_y", cam.basis_quat_y);
            fn("basis_quat_z", cam.basis_quat_z);
            fn("basis_quat_w", cam.basis_quat_w);
            fn("basis_quat_valid", cam.basis_quat_valid);
            fn("turn_hold_x", cam.turn_hold_x);
            fn("turn_hold_y", cam.turn_hold_y);
            fn("turn_hold_timer", cam.turn_hold_timer);
            fn("turn_track_x", cam.turn_track_x);
            fn("turn_track_y", cam.turn_track_y);
            fn("turn_track_yaw", cam.turn_track_yaw);
            fn("turn_track_step", cam.turn_track_step);
            fn("turn_track_valid", cam.turn_track_valid);
            fn("rig_last_orbited", cam.rig_last_orbited);
            fn("rig_last_azimuth", cam.rig_last_azimuth);
            fn("rig_last_elevation", cam.rig_last_elevation);
            fn("rig_last_turn", cam.rig_last_turn);
            fn("rig_last_rise", cam.rig_last_rise);
            fn("rig_last_yaw", cam.rig_last_yaw);
            fn("rig_last_pitch", cam.rig_last_pitch);
            fn("aim_handoff_active", cam.aim_handoff_active);
            fn("aim_handoff_written", cam.aim_handoff_written);
            fn("aim_handoff_player", cam.aim_handoff_player);
            fn("aim_handoff_azimuth", cam.aim_handoff_azimuth);
            fn("aim_handoff_elevation", cam.aim_handoff_elevation);
            fn("aim_handoff_yaw", cam.aim_handoff_yaw);
            fn("aim_handoff_pitch", cam.aim_handoff_pitch);
            fn("aim_handoff_seconds", cam.aim_handoff_seconds);
        }

        /** @brief Appends the rig's carry-over in @p cam to a recording. */
        void put_carry(Writer &out, CameraState &cam)
        {
            for_each_carry_field(cam, [&out](const char *, const auto &field) { out.put(field_value(field)); });
        }

        /** @brief Sets the rig's carry-over in @p cam from a recording. @return False when the recording ended. */
        [[nodiscard]] bool restore_carry(Reader &in, CameraState &cam)
        {
            bool ok = true;
            for_each_carry_field(cam,
                                 [&in, &ok](const char *, auto &field)
                                 {
                                     typename FieldValue<std::remove_cvref_t<decltype(field)>>::type value{};
                                     ok = ok && in.get(value);
                                     if (ok)
                                     {
                                         set_field(field, value);
                                     }
                                 });
            return ok;
        }

        /**
         * @brief Compares the rig's carry-over in @p cam with the one recorded next, bit for bit.
         * @return The first field that differs, "the recording ended", or nullptr when all match.
         */
        [[nodiscard]] const char *carry_difference(Reader &in, CameraState &cam)
        {
            const char *difference = nullptr;
            for_each_carry_field(cam,
                                 [&in, &difference](const char *name, const auto &field)
                                 {
                                     using Value = typename FieldValue<std::remove_cvref_t<decltype(field)>>::type;
                                     Value recorded{};
                                     if (!in.get(recorded))
                                     {
                                         difference = difference != nullptr ? difference : "the recording ended";
                                         return;
                                     }
                                     const Value value = field_value(field);
                                     if (difference == nullptr && std::memcmp(&value, &recorded, sizeof(Value)) != 0)
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
            CameraState cam{};
            std::uint32_t bytes = 0;
            for_each_carry_field(cam, [&bytes](const char *, const auto &field)
                                 { bytes += static_cast<std::uint32_t>(sizeof(field_value(field))); });
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

    // --- RecordingRigPort ---

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

    // --- ReplayRigPort ---

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

    // --- Frames ---

    bool record_rig(std::vector<std::byte> &out, CameraState &cam, const RigParams &params, const RigInput &in,
                    RigPort &port, RigFrame &frame)
    {
        Writer writer{out};
        writer.put(FrameKind::Rig);
        writer.put(params);
        writer.put(in);
        put_carry(writer, cam);
        RecordingRigPort recording{port, out};
        const bool built = run_rig(cam, params, in, recording, frame);
        writer.put(RigCall::End);
        writer.put(static_cast<std::uint8_t>(built));
        writer.put(frame);
        put_carry(writer, cam);
        return built;
    }

    std::string replay_rig_frame(Reader &reader, long long &calls)
    {
        RigParams params{};
        RigInput in{};
        CameraState cam{};
        if (!reader.get(params) || !reader.get(in) || !restore_carry(reader, cam))
        {
            return "truncated";
        }
        ReplayRigPort port{reader};
        RigFrame frame{};
        const bool built = run_rig(cam, params, in, port, frame);
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
        if (const char *what = carry_difference(reader, cam); what != nullptr)
        {
            return std::format("the state after differs ({})", what);
        }
        return {};
    }
} // namespace TPVCamera::ProbeRecord
