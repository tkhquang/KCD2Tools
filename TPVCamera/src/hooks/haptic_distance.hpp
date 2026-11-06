/**
 * @file hooks/haptic_distance.hpp
 * @brief Measures the range of the game's controller vibration from the first-person eye instead of the camera.
 *
 * KCD2 vibrates the pad through audio events: an ATL trigger with an `osh_preset` plays that haptic preset when its
 * FMOD event starts (libs/GameAudio/oldschool_haptics.xml). The event start measures the distance from the event's 3D
 * attributes to the audio listener. The preset play drops the preset when that distance exceeds its max_player_dist
 * (1.6 m for footsteps, sprint and thuds, 1.8 to 2.0 m for hits and clashes, 2.2 m for doors). The listener follows
 * the view camera, so the third-person camera, a few meters behind the head, puts every hit, step and landing of the
 * player out of range. NPC events carry the same presets, and only the range keeps them off the pad, so a larger range
 * in the data makes a nearby fight vibrate too.
 *
 * A mid hook on the argument setup of the start's call of the play measures the event from the eye the camera
 * publishes. The play's entry hook hands that distance to the range test, as in first person. The 3D sound and its
 * culling keep the listener.
 */
#ifndef TPVCAMERA_HOOKS_HAPTIC_DISTANCE_HPP
#define TPVCAMERA_HOOKS_HAPTIC_DISTANCE_HPP

#include <DetourModKit/hook.hpp>

namespace TPVCamera
{
    /**
     * @brief Hooks the haptic preset play and the event start's call of it (Feature::HapticsFromEye).
     * @details Best-effort: when the gate fails or a hook does not install, the presets keep the listener's distance.
     * @note Setup only, after resolve_all_anchors().
     */
    void install_haptic_distance_hooks(DMK::hook::HookStack &hooks);

    /**
     * @brief Sets the first-person eye (@p x, @p y, @p z, world coordinates) the haptic range is measured from.
     * @details The eye lapses on its own soon after the last call (FramePublication), so a camera that stops building
     *          leaves the range to the listener again.
     * @note Any thread. The camera calls it every frame it builds the third-person view.
     */
    void publish_haptic_eye(float x, float y, float z) noexcept;

    /**
     * @brief Withdraws the eye publish_haptic_eye() set: the haptic range is measured from the listener again.
     * @note Any thread.
     */
    void withdraw_haptic_eye() noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_HOOKS_HAPTIC_DISTANCE_HPP
