/**
 * @file config.hpp
 * @brief KCD2_AnimationFramework.ini: which parts of the framework run.
 */
#ifndef ANIMFW_CONFIG_HPP
#define ANIMFW_CONFIG_HPP

#include <string>

namespace AnimationFramework
{
    /** @brief The loaded settings. Each switch is read once at startup (a change applies on the next start). */
    struct Settings
    {
        /// Mods' delta .adb files, clip swaps and whole-file database copies merge into what the game loads.
        bool animation_patches{true};
        /// Mods' .caf files and clip indexes register in the game's clip index, replacing clips they ship again.
        bool mod_clips{true};
        /// Skeletons (.chr) several mods ship whole merge bone by bone instead of the last one winning.
        bool skeleton_merge{true};
        /// The game's human:PlayAnim / StopAnim, Henry-only, also play on NPCs (npc_actions.hpp).
        bool npc_actions{true};
        /**
         * @brief Options tagged `thirdPerson` play for Henry while mod_camera_view says third person (needs
         *        animation_patches, which adds the tag).
         */
        bool third_person_variants{true};
        /// Tags mods declare in their patches to Henry's tag definition (custom_tags.hpp) count.
        bool custom_tags{true};
        /// [CustomTags] Priority: tag names, highest first, ranked above every other tag.
        std::string custom_tag_priority;
        /// [CustomTags] Disabled: tag names that never turn on.
        std::string custom_tag_disabled;
    };

    /** @brief Registers every INI item with DMK::config. Call before DMK::config::load(). */
    void register_config_items();

    /** @brief The settings as last loaded. Safe from any thread. */
    [[nodiscard]] Settings settings_snapshot();
} // namespace AnimationFramework

#endif // ANIMFW_CONFIG_HPP
