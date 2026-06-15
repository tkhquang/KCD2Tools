/**
 * @file camera_preset_fields.hpp
 * @brief Single source of truth pairing each CameraPreset payload field with its UI
 *        label, editing range, JSON key and grouping.
 *
 * @details Both the ImGui editor (slider/checkbox loop) and the JSON (de)serializer
 *          iterate fields() so a new tunable is added in exactly one place. Each entry
 *          carries a pointer-to-member into CameraPreset; floats use @ref PresetField::f
 *          and bools use @ref PresetField::b (the other is null for that type).
 */
#ifndef TPVCAMERA_PRESETS_CAMERA_PRESET_FIELDS_HPP
#define TPVCAMERA_PRESETS_CAMERA_PRESET_FIELDS_HPP

#include "camera_preset.hpp"

#include <span>

namespace TPVCamera::Presets
{

    /** @brief Editor widget kind for a preset field. */
    enum class FieldType
    {
        Float,
        Bool,
    };

    /**
     * @struct PresetField
     * @brief Metadata for one editable CameraPreset field.
     */
    struct PresetField
    {
        /// Stable JSON key (also the persisted identifier).
        const char *key;
        /// ImGui label.
        const char *label;
        /// Hover help (may be empty).
        const char *tooltip;
        /// UI section: "Framing" | "Orbit" | "Collision".
        const char *group;
        /// Float or Bool.
        FieldType type;
        /// Member pointer when type == Float (else nullptr).
        float CameraPreset::*f;
        /// Member pointer when type == Bool (else nullptr).
        bool CameraPreset::*b;
        /// Slider lower bound (Float only).
        float min_value;
        /// Slider upper bound (Float only).
        float max_value;
        /// Single-arrow increment (Float only); the << / >> double arrows step 10x this.
        float fine_step;
    };

    /** @brief Returns the ordered table of editable preset fields. */
    [[nodiscard]] std::span<const PresetField> fields() noexcept;

} // namespace TPVCamera::Presets

#endif // TPVCAMERA_PRESETS_CAMERA_PRESET_FIELDS_HPP
