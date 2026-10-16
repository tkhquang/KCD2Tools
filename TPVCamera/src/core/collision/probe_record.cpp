/**
 * @file core/collision/probe_record.cpp
 * @brief Recording and replay of the camera collision (see probe_record.hpp).
 */

#include "core/collision/probe_record.hpp"
#include "core/rig/rig_record.hpp"

#include <format>

namespace TPVCamera::ProbeRecord
{
    namespace
    {
        [[nodiscard]] bool same_bits(float a, float b) noexcept
        {
            return std::memcmp(&a, &b, sizeof(float)) == 0;
        }

        [[nodiscard]] bool same_bits(const Vector3 &a, const Vector3 &b) noexcept
        {
            return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z);
        }

        /**
         * @brief Whether two collision states hold the same values, field by field (the structs' padding bytes are
         *        not part of the state).
         * @return The first field that differs, or nullptr.
         */
        [[nodiscard]] const char *state_difference(const CollisionState &a, const CollisionState &b) noexcept
        {
            if (a.arm.collision_valid != b.arm.collision_valid ||
                !same_bits(a.arm.collision_distance, b.arm.collision_distance) ||
                !same_bits(a.arm.collision_desired, b.arm.collision_desired) ||
                !same_bits(a.arm.collision_speed, b.arm.collision_speed) ||
                !same_bits(a.arm.hold_timer, b.arm.hold_timer))
            {
                return "arm ease";
            }
            const ShoulderCollision &sa = a.shoulder;
            const ShoulderCollision &sb = b.shoulder;
            if (sa.collision_valid != sb.collision_valid || !same_bits(sa.collision_distance, sb.collision_distance) ||
                !same_bits(sa.collision_desired, sb.collision_desired) ||
                !same_bits(sa.collision_speed, sb.collision_speed) || !same_bits(sa.hold_timer, sb.hold_timer) ||
                sa.throttle_valid != sb.throttle_valid || !same_bits(sa.throttle_pivot, sb.throttle_pivot) ||
                !same_bits(sa.throttle_focal, sb.throttle_focal) || !same_bits(sa.cached_allowed, sb.cached_allowed) ||
                sa.cached_held_in != sb.cached_held_in || sa.stamp_ms != sb.stamp_ms ||
                sa.traced_blocked != sb.traced_blocked || sa.traced_node != sb.traced_node)
            {
                return "shoulder";
            }
            if (a.fade_count != b.fade_count || a.arm_fade_count != b.arm_fade_count || a.fade_stale != b.fade_stale)
            {
                return "fade set size";
            }
            for (int i = 0; i < a.fade_count; ++i)
            {
                const FadeEntry &ea = a.fade_nodes[static_cast<std::size_t>(i)];
                const FadeEntry &eb = b.fade_nodes[static_cast<std::size_t>(i)];
                if (ea.node != eb.node || ea.source != eb.source || ea.buried != eb.buried || ea.inside != eb.inside)
                {
                    return "fade set entry";
                }
            }
            if (a.sight_count != b.sight_count || !same_bits(a.sight_age, b.sight_age) ||
                !same_bits(a.sight_cam, b.sight_cam))
            {
                return "sightline set";
            }
            for (int i = 0; i < a.sight_count; ++i)
            {
                const SightlineOccluder &oa = a.sight_nodes[static_cast<std::size_t>(i)];
                const SightlineOccluder &ob = b.sight_nodes[static_cast<std::size_t>(i)];
                if (oa.node != ob.node || oa.buried != ob.buried)
                {
                    return "sightline entry";
                }
            }
            if (a.throttle_valid != b.throttle_valid || a.throttle_fade_on != b.throttle_fade_on ||
                !same_bits(a.throttle_pivot, b.throttle_pivot) || !same_bits(a.throttle_cam, b.throttle_cam) ||
                !same_bits(a.cached_allowed, b.cached_allowed) || a.cached_held_in != b.cached_held_in ||
                a.stamp_ms != b.stamp_ms)
            {
                return "throttle";
            }
            if (a.traced_blocked != b.traced_blocked || !same_bits(a.traced_distance, b.traced_distance) ||
                a.traced_mesh != b.traced_mesh || !same_bits(a.last_camera, b.last_camera) ||
                a.pull_in_traced != b.pull_in_traced)
            {
                return "trace memory or last camera";
            }
            return nullptr;
        }

        /** @brief Whether two lens states hold the same values, field by field. */
        [[nodiscard]] bool same_lens(const LensState &a, const LensState &b) noexcept
        {
            const LensSample &sa = a.sample;
            const LensSample &sb = b.sample;
            return same_bits(a.near, b.near) && sa.valid == sb.valid && same_bits(sa.position, sb.position) &&
                   same_bits(sa.forward, sb.forward) && same_bits(sa.up, sb.up) &&
                   same_bits(sa.tan_half_width, sb.tan_half_width) &&
                   same_bits(sa.tan_half_height, sb.tan_half_height) && sa.rules_generation == sb.rules_generation &&
                   sa.stamp_ms == sb.stamp_ms && same_bits(sa.solid, sb.solid);
        }

        void put_lines(Writer &out, const Sightline *lines, int n_lines)
        {
            out.put(n_lines);
            if (n_lines > 0)
            {
                out.put_bytes(lines, sizeof(Sightline) * static_cast<std::size_t>(n_lines));
            }
        }
    } // namespace

    const char *call_name(Call call) noexcept
    {
        switch (call)
        {
        case Call::End:
            return "end";
        case Call::Ray:
            return "ray";
        case Call::RefreshScan:
            return "refresh_scan";
        case Call::NodeForCollider:
            return "node_for_collider";
        case Call::StructureDrawnOn:
            return "structure_drawn_on";
        case Call::StructureOnArm:
            return "structure_on_arm";
        case Call::SpansTube:
            return "spans_tube";
        case Call::OccludersOnSightlines:
            return "occluders_on_sightlines";
        case Call::PrefetchMeshes:
            return "prefetch_meshes";
        case Call::StoppingNodeNear:
            return "stopping_node_near";
        case Call::NowMs:
            return "now_ms";
        }
        return "?";
    }

    // --- RecordingProbe ---

    std::optional<RayHit> RecordingProbe::ray(QueryLedger::Tag tag, const Vector3 &origin, const Vector3 &sweep,
                                              int objtypes, unsigned int flags, const std::uintptr_t *skip, int n_skip)
    {
        const std::optional<RayHit> hit = inner_.ray(tag, origin, sweep, objtypes, flags, skip, n_skip);
        out_.put(Call::Ray);
        out_.put(tag);
        out_.put(origin);
        out_.put(sweep);
        out_.put(objtypes);
        out_.put(flags);
        out_.put(n_skip);
        if (n_skip > 0)
        {
            out_.put_bytes(skip, sizeof(std::uintptr_t) * static_cast<std::size_t>(n_skip));
        }
        out_.put(static_cast<std::uint8_t>(hit.has_value()));
        if (hit.has_value())
        {
            out_.put(*hit);
        }
        return hit;
    }

    void RecordingProbe::refresh_scan(const Vector3 *points, int n_points)
    {
        inner_.refresh_scan(points, n_points);
        out_.put(Call::RefreshScan);
        out_.put(n_points);
        if (n_points > 0)
        {
            out_.put_bytes(points, sizeof(Vector3) * static_cast<std::size_t>(n_points));
        }
    }

    void *RecordingProbe::node_for_collider(std::uintptr_t collider, Rules::Behaviour *cls)
    {
        void *node = inner_.node_for_collider(collider, cls);
        out_.put(Call::NodeForCollider);
        out_.put(collider);
        out_.put(reinterpret_cast<std::uintptr_t>(node));
        out_.put(*cls);
        return node;
    }

    bool RecordingProbe::structure_drawn_on(QueryLedger::Tag tag, void *node, const Sightline *lines, int n_lines)
    {
        const bool drawn = inner_.structure_drawn_on(tag, node, lines, n_lines);
        out_.put(Call::StructureDrawnOn);
        out_.put(tag);
        out_.put(reinterpret_cast<std::uintptr_t>(node));
        put_lines(out_, lines, n_lines);
        out_.put(static_cast<std::uint8_t>(drawn));
        return drawn;
    }

    float RecordingProbe::structure_on_arm(QueryLedger::Tag tag, const Sightline *lines, int n_lines, int min_lines,
                                           float max_fraction, void **node_out, bool trace)
    {
        const float crossing = inner_.structure_on_arm(tag, lines, n_lines, min_lines, max_fraction, node_out, trace);
        out_.put(Call::StructureOnArm);
        out_.put(tag);
        put_lines(out_, lines, n_lines);
        out_.put(min_lines);
        out_.put(max_fraction);
        out_.put(static_cast<std::uint8_t>(node_out != nullptr));
        out_.put(static_cast<std::uint8_t>(trace));
        out_.put(crossing);
        out_.put(node_out != nullptr ? reinterpret_cast<std::uintptr_t>(*node_out) : std::uintptr_t{0});
        return crossing;
    }

    bool RecordingProbe::spans_tube(void *node, const Sightline *tube)
    {
        const bool spans = inner_.spans_tube(node, tube);
        out_.put(Call::SpansTube);
        out_.put(reinterpret_cast<std::uintptr_t>(node));
        out_.put_bytes(tube, sizeof(Sightline) * 5);
        out_.put(static_cast<std::uint8_t>(spans));
        return spans;
    }

    int RecordingProbe::occluders_on_sightlines(const Sightline *lines, int n_lines, std::span<void *const> skip,
                                                SightlineOccluder *out, int cap)
    {
        const int count = inner_.occluders_on_sightlines(lines, n_lines, skip, out, cap);
        out_.put(Call::OccludersOnSightlines);
        put_lines(out_, lines, n_lines);
        out_.put(static_cast<int>(skip.size()));
        for (void *const node : skip)
        {
            out_.put(reinterpret_cast<std::uintptr_t>(node));
        }
        out_.put(cap);
        out_.put(count);
        for (int i = 0; i < count; ++i)
        {
            out_.put(reinterpret_cast<std::uintptr_t>(out[i].node));
            out_.put(static_cast<std::uint8_t>(out[i].buried));
        }
        return count;
    }

    void RecordingProbe::prefetch_meshes(const Sightline *lines, int n_lines)
    {
        inner_.prefetch_meshes(lines, n_lines);
        out_.put(Call::PrefetchMeshes);
        put_lines(out_, lines, n_lines);
    }

    bool RecordingProbe::stopping_node_near(const float *box, float pad)
    {
        const bool near = inner_.stopping_node_near(box, pad);
        out_.put(Call::StoppingNodeNear);
        out_.put_bytes(box, sizeof(float) * 6);
        out_.put(pad);
        out_.put(static_cast<std::uint8_t>(near));
        return near;
    }

    std::uint64_t RecordingProbe::now_ms()
    {
        const std::uint64_t now = inner_.now_ms();
        out_.put(Call::NowMs);
        out_.put(now);
        return now;
    }

    // --- ReplayProbe ---

    void ReplayProbe::diverge(const std::string &why)
    {
        if (divergence_.empty())
        {
            divergence_ = std::format("call {}: {}", calls_, why);
        }
    }

    bool ReplayProbe::expect(Call expected)
    {
        if (diverged())
        {
            return false;
        }
        ++calls_;
        Call recorded = Call::End;
        if (!in_.get(recorded))
        {
            diverge(std::format("{} asked, the recording ended", call_name(expected)));
            return false;
        }
        if (recorded != expected)
        {
            diverge(std::format("{} asked, {} recorded", call_name(expected), call_name(recorded)));
            return false;
        }
        return true;
    }

    bool ReplayProbe::expect_bytes(const void *actual, std::size_t size, const char *what)
    {
        std::vector<std::byte> recorded(size);
        if (!in_.get_bytes(recorded.data(), size))
        {
            diverge(std::format("{}: the recording ended", what));
            return false;
        }
        if (std::memcmp(recorded.data(), actual, size) != 0)
        {
            diverge(std::format("{} differs", what));
            return false;
        }
        return true;
    }

    std::optional<RayHit> ReplayProbe::ray(QueryLedger::Tag tag, const Vector3 &origin, const Vector3 &sweep,
                                           int objtypes, unsigned int flags, const std::uintptr_t *skip, int n_skip)
    {
        if (!expect(Call::Ray) || !expect_value(tag, "ray tag") || !expect_value(origin, "ray origin") ||
            !expect_value(sweep, "ray sweep") || !expect_value(objtypes, "ray objtypes") ||
            !expect_value(flags, "ray flags") || !expect_value(n_skip, "ray skip count") ||
            (n_skip > 0 && !expect_bytes(skip, sizeof(std::uintptr_t) * static_cast<std::size_t>(n_skip), "ray skip")))
        {
            return std::nullopt;
        }
        std::uint8_t has = 0;
        RayHit hit{};
        if (!in_.get(has) || (has != 0 && !in_.get(hit)))
        {
            diverge("ray answer: the recording ended");
            return std::nullopt;
        }
        return has != 0 ? std::optional<RayHit>{hit} : std::nullopt;
    }

    void ReplayProbe::refresh_scan(const Vector3 *points, int n_points)
    {
        if (expect(Call::RefreshScan) && expect_value(n_points, "scan point count") && n_points > 0)
        {
            (void)expect_bytes(points, sizeof(Vector3) * static_cast<std::size_t>(n_points), "scan points");
        }
    }

    void *ReplayProbe::node_for_collider(std::uintptr_t collider, Rules::Behaviour *cls)
    {
        *cls = Rules::Behaviour::Collide;
        std::uintptr_t node = 0;
        if (!expect(Call::NodeForCollider) || !expect_value(collider, "collider") || !in_.get(node) || !in_.get(*cls))
        {
            *cls = Rules::Behaviour::Collide;
            return nullptr;
        }
        return reinterpret_cast<void *>(node);
    }

    bool ReplayProbe::structure_drawn_on(QueryLedger::Tag tag, void *node, const Sightline *lines, int n_lines)
    {
        std::uint8_t drawn = 0;
        if (!expect(Call::StructureDrawnOn) || !expect_value(tag, "drawn-on tag") ||
            !expect_value(reinterpret_cast<std::uintptr_t>(node), "drawn-on node") ||
            !expect_value(n_lines, "drawn-on line count") ||
            (n_lines > 0 &&
             !expect_bytes(lines, sizeof(Sightline) * static_cast<std::size_t>(n_lines), "drawn-on lines")) ||
            !in_.get(drawn))
        {
            return false;
        }
        return drawn != 0;
    }

    float ReplayProbe::structure_on_arm(QueryLedger::Tag tag, const Sightline *lines, int n_lines, int min_lines,
                                        float max_fraction, void **node_out, bool trace)
    {
        float crossing = 2.0f;
        std::uintptr_t node = 0;
        if (!expect(Call::StructureOnArm) || !expect_value(tag, "arm-mesh tag") ||
            !expect_value(n_lines, "arm-mesh line count") ||
            (n_lines > 0 &&
             !expect_bytes(lines, sizeof(Sightline) * static_cast<std::size_t>(n_lines), "arm-mesh lines")) ||
            !expect_value(min_lines, "arm-mesh min lines") || !expect_value(max_fraction, "arm-mesh max fraction") ||
            !expect_value(static_cast<std::uint8_t>(node_out != nullptr), "arm-mesh node out") ||
            !expect_value(static_cast<std::uint8_t>(trace), "arm-mesh trace") || !in_.get(crossing) || !in_.get(node))
        {
            return 2.0f;
        }
        if (node_out != nullptr)
        {
            *node_out = reinterpret_cast<void *>(node);
        }
        return crossing;
    }

    bool ReplayProbe::spans_tube(void *node, const Sightline *tube)
    {
        std::uint8_t spans = 0;
        if (!expect(Call::SpansTube) || !expect_value(reinterpret_cast<std::uintptr_t>(node), "tube node") ||
            !expect_bytes(tube, sizeof(Sightline) * 5, "tube lines") || !in_.get(spans))
        {
            return false;
        }
        return spans != 0;
    }

    int ReplayProbe::occluders_on_sightlines(const Sightline *lines, int n_lines, std::span<void *const> skip,
                                             SightlineOccluder *out, int cap)
    {
        if (!expect(Call::OccludersOnSightlines) || !expect_value(n_lines, "sightline count") ||
            (n_lines > 0 &&
             !expect_bytes(lines, sizeof(Sightline) * static_cast<std::size_t>(n_lines), "sightlines")) ||
            !expect_value(static_cast<int>(skip.size()), "sightline skip count"))
        {
            return 0;
        }
        for (void *const node : skip)
        {
            if (!expect_value(reinterpret_cast<std::uintptr_t>(node), "sightline skip"))
            {
                return 0;
            }
        }
        int count = 0;
        if (!expect_value(cap, "sightline cap") || !in_.get(count) || count < 0 || count > cap)
        {
            diverge("sightline answer");
            return 0;
        }
        for (int i = 0; i < count; ++i)
        {
            std::uintptr_t node = 0;
            std::uint8_t buried = 0;
            if (!in_.get(node) || !in_.get(buried))
            {
                diverge("sightline answer: the recording ended");
                return 0;
            }
            out[i] = SightlineOccluder{reinterpret_cast<void *>(node), buried != 0};
        }
        return count;
    }

    void ReplayProbe::prefetch_meshes(const Sightline *lines, int n_lines)
    {
        if (expect(Call::PrefetchMeshes) && expect_value(n_lines, "prefetch line count") && n_lines > 0)
        {
            (void)expect_bytes(lines, sizeof(Sightline) * static_cast<std::size_t>(n_lines), "prefetch lines");
        }
    }

    bool ReplayProbe::stopping_node_near(const float *box, float pad)
    {
        std::uint8_t near = 0;
        if (!expect(Call::StoppingNodeNear) || !expect_bytes(box, sizeof(float) * 6, "near box") ||
            !expect_value(pad, "near pad") || !in_.get(near))
        {
            return false;
        }
        return near != 0;
    }

    std::uint64_t ReplayProbe::now_ms()
    {
        std::uint64_t now = 0;
        if (!expect(Call::NowMs) || !in_.get(now))
        {
            return 0;
        }
        return now;
    }

    bool ReplayProbe::finish()
    {
        if (diverged())
        {
            return false;
        }
        Call recorded = Call::Ray;
        if (!in_.get(recorded) || recorded != Call::End)
        {
            diverge(std::format("the frame ended, the recording holds {} more", call_name(recorded)));
            return false;
        }
        return true;
    }

    // --- Frames ---

    Vector3 record_arm(std::vector<std::byte> &out, CollisionState &state, const CollisionInput &in,
                       const CollisionParams &params, WorldProbe &probe, CollisionTrace &trace)
    {
        Writer writer{out};
        writer.put(FrameKind::Arm);
        writer.put(in);
        writer.put(params);
        writer.put(state);
        RecordingProbe recording{probe, out};
        const Vector3 camera = resolve_camera_collision(state, in, params, recording, trace);
        writer.put(Call::End);
        writer.put(camera);
        writer.put(state);
        return camera;
    }

    float record_shoulder(std::vector<std::byte> &out, CollisionState &state, const ShoulderInput &in,
                          const CollisionParams &params, WorldProbe &probe, CollisionTrace &trace)
    {
        Writer writer{out};
        writer.put(FrameKind::Shoulder);
        writer.put(in);
        writer.put(params);
        writer.put(state);
        RecordingProbe recording{probe, out};
        const float allowed = resolve_shoulder_collision(state, in, params, recording, trace);
        writer.put(Call::End);
        writer.put(allowed);
        writer.put(state);
        return allowed;
    }

    std::optional<float> record_lens(std::vector<std::byte> &out, LensState &state, const LensInput &in,
                                     WorldProbe &probe)
    {
        Writer writer{out};
        writer.put(FrameKind::Lens);
        writer.put(in);
        writer.put(state);
        RecordingProbe recording{probe, out};
        const std::optional<float> near = plan_lens_near(state, in, recording);
        writer.put(Call::End);
        writer.put(static_cast<std::uint8_t>(near.has_value()));
        writer.put(near.value_or(0.0f));
        writer.put(state);
        return near;
    }

    ReplayReport replay(std::span<const std::byte> data)
    {
        ReplayReport report;
        Reader reader{data};
        Header header{};
        if (!reader.get(header) || !header.matches_build())
        {
            report.failure = "the header does not match this build (format or struct sizes)";
            return report;
        }
        QuietTrace trace;
        while (!reader.at_end())
        {
            FrameKind kind{};
            if (!reader.get(kind))
            {
                report.failure = std::format("frame {}: truncated", report.frames);
                return report;
            }
            CollisionParams params{};
            CollisionState state{};
            ReplayProbe probe{reader};
            if (kind == FrameKind::Arm)
            {
                CollisionInput in{};
                if (!reader.get(in) || !reader.get(params) || !reader.get(state))
                {
                    report.failure = std::format("frame {}: truncated", report.frames);
                    return report;
                }
                const Vector3 camera = resolve_camera_collision(state, in, params, probe, trace);
                Vector3 recorded{};
                CollisionState after{};
                const bool finished = probe.finish();
                if (!finished || !reader.get(recorded) || !reader.get(after))
                {
                    report.failure = std::format("frame {} (arm): {}", report.frames,
                                                 probe.diverged() ? probe.divergence() : std::string{"truncated"});
                    return report;
                }
                if (!same_bits(camera, recorded))
                {
                    report.failure = std::format("frame {} (arm): the camera differs", report.frames);
                    return report;
                }
                if (const char *what = state_difference(state, after); what != nullptr)
                {
                    report.failure = std::format("frame {} (arm): the state after differs ({})", report.frames, what);
                    return report;
                }
                ++report.arm_frames;
            }
            else if (kind == FrameKind::Shoulder)
            {
                ShoulderInput in{};
                if (!reader.get(in) || !reader.get(params) || !reader.get(state))
                {
                    report.failure = std::format("frame {}: truncated", report.frames);
                    return report;
                }
                const float allowed = resolve_shoulder_collision(state, in, params, probe, trace);
                float recorded = 0.0f;
                CollisionState after{};
                const bool finished = probe.finish();
                if (!finished || !reader.get(recorded) || !reader.get(after))
                {
                    report.failure = std::format("frame {} (shoulder): {}", report.frames,
                                                 probe.diverged() ? probe.divergence() : std::string{"truncated"});
                    return report;
                }
                if (!same_bits(allowed, recorded))
                {
                    report.failure = std::format("frame {} (shoulder): the offset differs", report.frames);
                    return report;
                }
                if (const char *what = state_difference(state, after); what != nullptr)
                {
                    report.failure =
                        std::format("frame {} (shoulder): the state after differs ({})", report.frames, what);
                    return report;
                }
                ++report.shoulder_frames;
            }
            else if (kind == FrameKind::Lens)
            {
                LensInput in{};
                LensState lens{};
                if (!reader.get(in) || !reader.get(lens))
                {
                    report.failure = std::format("frame {}: truncated", report.frames);
                    return report;
                }
                const std::optional<float> near = plan_lens_near(lens, in, probe);
                std::uint8_t has = 0;
                float recorded = 0.0f;
                LensState after{};
                const bool finished = probe.finish();
                if (!finished || !reader.get(has) || !reader.get(recorded) || !reader.get(after))
                {
                    report.failure = std::format("frame {} (lens): {}", report.frames,
                                                 probe.diverged() ? probe.divergence() : std::string{"truncated"});
                    return report;
                }
                if (near.has_value() != (has != 0) || !same_bits(near.value_or(0.0f), recorded))
                {
                    report.failure = std::format("frame {} (lens): the near plane differs", report.frames);
                    return report;
                }
                if (!same_lens(lens, after))
                {
                    report.failure = std::format("frame {} (lens): the state after differs", report.frames);
                    return report;
                }
                ++report.lens_frames;
            }
            else if (kind == FrameKind::Rig)
            {
                if (const std::string failure = replay_rig_frame(reader, report.calls); !failure.empty())
                {
                    report.failure = std::format("frame {} (rig): {}", report.frames, failure);
                    return report;
                }
                ++report.rig_frames;
                ++report.frames;
                continue;
            }
            else
            {
                report.failure = std::format("frame {}: unknown kind {}", report.frames, static_cast<int>(kind));
                return report;
            }
            report.calls += probe.calls();
            ++report.frames;
        }
        report.ok = true;
        return report;
    }
} // namespace TPVCamera::ProbeRecord
