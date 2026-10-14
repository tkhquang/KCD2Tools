/**
 * @file camera/collision_trace.cpp
 * @brief The camera's log lines for the collision walk, the fade set and the see-through window (see
 *        collision_trace.hpp).
 */

#include "camera/collision_trace.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <format>
#include <string>

namespace TPVCamera
{
    namespace
    {
        // When the collision last pulled the camera in, and when its latest release (an ease back out) began, as
        // GetTickCount64 values (0 = not since the module loaded), for the walk trace. Main thread only.
        std::uint64_t s_pull_in_ms = 0;
        std::uint64_t s_release_ms = 0;

        /** @brief Seconds since @p stamp_ms (a GetTickCount64 value), or -1 when it is 0 (never). */
        [[nodiscard]] float age_seconds(std::uint64_t stamp_ms) noexcept
        {
            return stamp_ms == 0 ? -1.0f : static_cast<float>(GetTickCount64() - stamp_ms) / 1000.0f;
        }

        /**
         * @brief The file name of @p node's mesh, for the log: render_node_name() into @p path, then the part after its
         *        last slash, or "?" when the path is empty.
         */
        const char *mesh_file_name(void *node, char (&path)[160]) noexcept
        {
            render_node_name(node, path, static_cast<int>(sizeof(path)));
            const char *base = path;
            for (const char *p = path; *p != '\0'; ++p)
            {
                if (*p == '/' || *p == '\\')
                {
                    base = p + 1;
                }
            }
            return *base != '\0' ? base : "?";
        }

        /** @brief The nodes a list last held, so its log line comes only when the list changes. */
        struct ListedNodes
        {
            std::array<void *, Constants::TREE_HOLE_MAX_NODES> last{};
            std::size_t count = 0;

            /** @brief Keeps @p nodes as the latest list; true when they differ from the ones before. */
            bool changed(std::span<void *const> nodes) noexcept
            {
                const std::size_t n = std::min(nodes.size(), last.size());
                if (n == count &&
                    std::equal(nodes.begin(), nodes.begin() + static_cast<std::ptrdiff_t>(n), last.begin()))
                {
                    return false;
                }
                std::copy_n(nodes.begin(), n, last.begin());
                count = n;
                return true;
            }
        };

        /** @brief The file names of @p nodes' meshes, comma-separated, or "none", for the log. */
        std::string listed_node_names(std::span<void *const> nodes)
        {
            if (nodes.empty())
            {
                return "none";
            }
            std::string text;
            for (void *node : nodes)
            {
                char path[160];
                text += text.empty() ? "" : ", ";
                text += mesh_file_name(node, path);
            }
            return text;
        }
    } // namespace

    /** @brief Notes @p motion, the result of one collision ease step, for the pull-in and release ages. */
    void note_collision_motion(EaseMotion motion) noexcept
    {
        const std::uint64_t now = GetTickCount64();
        if (motion == EaseMotion::PullIn)
        {
            s_pull_in_ms = now;
            s_release_ms = 0;
        }
        else if (motion == EaseMotion::Release && s_release_ms == 0)
        {
            s_release_ms = now;
        }
    }

    void log_walk_change(std::span<const WalkStep> steps, float desired_distance) noexcept
    {
        static std::array<WalkStep, 8> s_last{};
        static std::size_t s_last_count = 0;
        const std::size_t n = std::min(steps.size(), s_last.size());
        const auto same = [](const WalkStep &a, const WalkStep &b)
        { return a.collider == b.collider && a.what == b.what; };
        if (n == s_last_count &&
            std::equal(steps.begin(), steps.begin() + static_cast<std::ptrdiff_t>(n), s_last.begin(), same))
        {
            return;
        }
        std::copy_n(steps.begin(), n, s_last.begin());
        s_last_count = n;
        if (!DMK::log().is_enabled(DMK::LogLevel::Trace))
        {
            return;
        }
        try
        {
            std::string text;
            for (std::size_t i = 0; i < n; ++i)
            {
                const WalkStep &s = steps[i];
                char path[160];
                const char *name = mesh_file_name(s.node, path);
                text += std::format("{}{} {}({}) at {:.2f}", i == 0 ? "" : ", ", s.what,
                                    s.node != nullptr ? name : "unmatched",
                                    s.node != nullptr ? Rules::behaviour_name(s.cls) : "-", s.distance);
            }
            (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                     "Camera walk of {:.2f} m: {} (pulled in {:.2f} s ago, released {:.2f} s ago; "
                                     "-1 = never)",
                                     desired_distance, n == 0 ? std::string{"open"} : text, age_seconds(s_pull_in_ms),
                                     age_seconds(s_release_ms));
        }
        catch (...)
        {
        }
    }

    void log_fade_set_change(std::span<const FadeEntry> entries) noexcept
    {
        static std::array<FadeEntry, Constants::OCCLUDER_FADE_MAX_NODES> s_last{};
        static std::size_t s_last_count = 0;
        const std::size_t n = std::min(entries.size(), s_last.size());
        const auto same = [](const FadeEntry &a, const FadeEntry &b)
        { return a.node == b.node && a.source == b.source && a.buried == b.buried && a.inside == b.inside; };
        if (n == s_last_count &&
            std::equal(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(n), s_last.begin(), same))
        {
            return;
        }
        std::copy_n(entries.begin(), n, s_last.begin());
        s_last_count = n;
        if (!DMK::log().is_enabled(DMK::LogLevel::Debug))
        {
            return;
        }
        try
        {
            std::string text;
            for (std::size_t i = 0; i < n; ++i)
            {
                char path[160];
                text += (i == 0) ? "" : ", ";
                text += mesh_file_name(entries[i].node, path);
                text += '[';
                text += entries[i].source;
                text += entries[i].inside ? "!" : "";
                text += entries[i].buried ? "+]" : "]";
            }
            (void)DMK::log().try_log(DMK::LogLevel::Debug, "OccluderFade set ({}): {}", n, text);
        }
        catch (...)
        {
        }
    }

    void log_tree_window_change(std::span<void *const> trees, std::span<void *const> frames,
                                const TreeHoleWindow &window) noexcept
    {
        static ListedNodes s_trees;
        static ListedNodes s_frames;
        const bool trees_changed = s_trees.changed(trees);
        const bool frames_changed = s_frames.changed(frames);
        if (!DMK::log().is_enabled(DMK::LogLevel::Debug))
        {
            return;
        }
        try
        {
            if (trees_changed)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "TreeSeeThrough: {} plant(s) in the window ({:.2f} x {:.2f} m, "
                                         "open from {:.2f} m below its centre, strength {:.2f}): {}",
                                         s_trees.count, 2.0f * window.half_width, 2.0f * window.half_height,
                                         window.anchor.z - window.floor_z, window.strength,
                                         listed_node_names(trees.first(s_trees.count)));
            }
            if (frames_changed)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "NearCut: {} frame(s) near the lens (cut from {:.2f} m, "
                                         "gone within {:.2f} m, strength {:.2f}): {}",
                                         s_frames.count, window.near_clear, window.near_full, window.near_strength,
                                         listed_node_names(frames.first(s_frames.count)));
            }
        }
        catch (...)
        {
        }
    }
} // namespace TPVCamera
