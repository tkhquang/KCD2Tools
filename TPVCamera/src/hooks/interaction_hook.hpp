/**
 * @file hooks/interaction_hook.hpp
 * @brief Camera-space interaction: redirect the player look-ray onto the render camera + crosshair.
 *
 * KCD2's player interactor casts its "what am I looking at / press to use" look ray from the EYE along the
 * look direction; it never consults the render camera. In third person the over-the-shoulder camera is
 * offset from the eye, so the screen-centre crosshair and the use-target diverge (worst at close range).
 * This hook intercepts the ray-query builder (sub_180530584) and, when InteractFromCamera is set and the
 * offset is engaged, rewrites the look-ray origin + direction to the rendered camera + crosshair so the
 * use-target follows the screen centre at all ranges. A companion hook on the on-screen reticle projection
 * gate (sub_18093C170) lets crosshair-pointed static usables (beds/shrines/doors) survive the offset camera.
 */
#ifndef TPVCAMERA_HOOKS_INTERACTION_HOOK_HPP
#define TPVCAMERA_HOOKS_INTERACTION_HOOK_HPP

#include <DetourModKit/error.hpp>
#include <DetourModKit/hook.hpp>

namespace TPVCamera
{

    /**
     * @brief Installs the interaction look-ray redirect (sub_180530584) and the on-screen reticle gate
     *        (sub_18093C170) from the pre-resolved anchors.
     * @details Best-effort: on a pattern miss the feature simply no-ops (interaction stays vanilla) and the
     *          rest of the mod is unaffected. The detours are also a no-op at runtime unless InteractFromCamera
     *          is enabled AND the third-person offset is engaged, so first person is never touched.
     * @param hooks The mod's hook stack; each installed handle is pushed in install order.
     * @return An empty value when the look-ray builder hook was installed, or the library Error that refused
     *         it (ErrorCode::NoMatch when an anchor cascade did not resolve). The companion on-screen reticle
     *         gate is best-effort within this unit and only warns.
     * @note Call after resolve_all_anchors(); the hook targets are read via anchor_address().
     */
    [[nodiscard]] DMK::Result<void> initialize_interaction_hook(DMK::hook::HookStack &hooks);

} // namespace TPVCamera

#endif // TPVCAMERA_HOOKS_INTERACTION_HOOK_HPP
