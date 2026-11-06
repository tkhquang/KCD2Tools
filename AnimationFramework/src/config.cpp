/**
 * @file config.cpp
 * @brief Configuration registration through DMK::config.
 */

#include "config.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>

namespace AnimationFramework
{
    namespace
    {
        std::atomic<bool> s_animation_patches{true};
        std::atomic<bool> s_mod_clips{true};
        std::atomic<bool> s_skeleton_merge{true};
        std::atomic<bool> s_npc_actions{true};
        std::atomic<bool> s_third_person_variants{true};
        std::atomic<bool> s_custom_tags{true};
        // Written by the INI load (setup), read after it.
        std::mutex s_lists_mutex;
        std::string s_custom_tag_priority;
        std::string s_custom_tag_disabled;
    } // namespace

    void register_config_items()
    {
        const DMK::config::SectionBinder settings = DMK::config::section("Settings");
        settings.bind_log_level("LogLevel", constants::DEFAULT_LOG_LEVEL);

        const DMK::config::SectionBinder features = DMK::config::section("Features");
        features.bind<bool>("AnimationPatches", "Animation Patches", s_animation_patches, true);
        features.bind<bool>("ModClips", "Mod Clips", s_mod_clips, true);
        features.bind<bool>("SkeletonMerge", "Skeleton Merge", s_skeleton_merge, true);
        features.bind<bool>("NpcActions", "NPC Actions", s_npc_actions, true);
        features.bind<bool>("ThirdPersonVariants", "Third Person Variants", s_third_person_variants, true);
        features.bind<bool>("CustomTags", "Custom Tags", s_custom_tags, true);

        const DMK::config::SectionBinder tags = DMK::config::section("CustomTags");
        tags.bind_string(
            "Priority",
            "Custom Tag Priority",
            [](std::string_view value) -> void
            {
                std::lock_guard lock(s_lists_mutex);
                s_custom_tag_priority = value;
            },
            ""
        );
        tags.bind_string(
            "Disabled",
            "Disabled Custom Tags",
            [](std::string_view value) -> void
            {
                std::lock_guard lock(s_lists_mutex);
                s_custom_tag_disabled = value;
            },
            ""
        );
    }

    Settings settings_snapshot()
    {
        // One hold, so both lists come from the same load.
        std::lock_guard lock(s_lists_mutex);
        return Settings{
            .animation_patches = s_animation_patches.load(std::memory_order_relaxed),
            .mod_clips = s_mod_clips.load(std::memory_order_relaxed),
            .skeleton_merge = s_skeleton_merge.load(std::memory_order_relaxed),
            .npc_actions = s_npc_actions.load(std::memory_order_relaxed),
            .third_person_variants = s_third_person_variants.load(std::memory_order_relaxed),
            .custom_tags = s_custom_tags.load(std::memory_order_relaxed),
            .custom_tag_priority = s_custom_tag_priority,
            .custom_tag_disabled = s_custom_tag_disabled,
        };
    }
} // namespace AnimationFramework
