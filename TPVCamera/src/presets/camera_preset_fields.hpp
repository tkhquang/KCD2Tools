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
    const char *key;          ///< Stable JSON key (also the persisted identifier).
    const char *label;        ///< ImGui label.
    const char *tooltip;      ///< Hover help (may be empty).
    const char *group;        ///< UI section: "Framing" | "Orbit" | "Collision".
    FieldType type;           ///< Float or Bool.
    float CameraPreset::*f;   ///< Member pointer when type == Float (else nullptr).
    bool CameraPreset::*b;    ///< Member pointer when type == Bool (else nullptr).
    float min_value;          ///< Slider lower bound (Float only).
    float max_value;          ///< Slider upper bound (Float only).
    float fine_step;          ///< Single-arrow increment (Float only); the << / >> double arrows step 10x this.
};

/** @brief Returns the ordered table of editable preset fields. */
[[nodiscard]] std::span<const PresetField> fields() noexcept;

} // namespace TPVCamera::Presets

#endif // TPVCAMERA_PRESETS_CAMERA_PRESET_FIELDS_HPP
