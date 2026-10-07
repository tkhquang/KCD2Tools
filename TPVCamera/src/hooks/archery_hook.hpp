/**
 * @file hooks/archery_hook.hpp
 * @brief Third-person arrows and distraction stones aimed at the crosshair, the arrow flight trail and the aim preview.
 * @details While the hooks are installed, the player's arrows, bolts, firearm shots and thrown distraction stones fly
 *          at the crosshair point in third person, and each of the player's shots falls by its weapon's configured
 *          gravity scale. Each frame the archery step follows, logs and draws the player's shots and previews where
 *          the next one comes down. archery_hook.cpp owns the mechanism.
 */
#ifndef TPVCAMERA_HOOKS_ARCHERY_HOOK_HPP
#define TPVCAMERA_HOOKS_ARCHERY_HOOK_HPP

#include "hook_set.hpp"
#include "math_utils.hpp"

#include <DetourModKit/error.hpp>

#include <cstdint>

namespace TPVCamera
{

    /**
     * @brief Installs the FireProjectile, Launch and arrow-collision hooks from the pre-resolved anchors.
     * @param g_env The resolved SSystemGlobalEnvironment base, or 0 (the trail then stays off).
     * @param hooks The mod's hook set. Each handle is stored before its arm.
     * @return An empty value when the shot hooks are installed, or the Error that refused them (ErrorCode::NoMatch
     *         when the ArcheryAim gate failed). The trail is best-effort within this unit and only warns.
     * @throws std::bad_alloc If a hook name or the hook set cannot allocate.
     * @note Call after initialize_camera(), so the world raycast is ready.
     */
    [[nodiscard]] DMK::Result<void> initialize_archery_hook(std::uintptr_t g_env, HookSet &hooks);

    /**
     * @brief The archery step of one game frame: publishes the crosshair target, follows the player's arrows, logs
     *        their impacts and draws the trail and the aim previews.
     * @param c_player The live C_Player, or 0.
     * @param eye The first-person eye. The crosshair target ray starts at its depth along the ray.
     * @param eye_valid Whether @p eye was read this frame.
     * @note Main thread only. Call once per engine frame from the frustum detour, after the camera pose is published.
     */
    void archery_frame(std::uintptr_t c_player, const Vector3 &eye, bool eye_valid) noexcept;

    /**
     * @brief True while the archery step has work this frame: a shot not yet picked up, an arrow in flight, a trail
     *        still on screen, or, with ShowAimPreview on, a distraction stone in the player's hand.
     * @details The camera's idle path skips its per-frame body in plain first person, so it asks this first. An arrow
     *          loosed there is then still followed, logged and drawn, and a held stone's landing is still shown.
     * @param c_player The live C_Player, or 0 (no stone is then looked for).
     * @note Main thread only (the frustum detour).
     */
    [[nodiscard]] bool archery_wants_frame(std::uintptr_t c_player) noexcept;

    /**
     * @brief Switches the shot re-aim and the flight trail off before the hooks retire.
     * @details Afterwards the detours launch every shot as the game aims it. Pooled projectiles keep the last gravity
     *          set until their next Launch, which resets it while the hook is installed.
     */
    void release_archery() noexcept;

} // namespace TPVCamera

#endif // TPVCAMERA_HOOKS_ARCHERY_HOOK_HPP
