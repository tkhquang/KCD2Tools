/**
 * @file rtti_types.hpp
 * @brief Cached identity handles for the WHGame.dll class vtables the mod tests against.
 *
 * Several per-frame paths ask whether a vtable belongs to a given engine class. The direct form,
 * DMK::rtti::vtable_is_type(vtable, name), answers by walking the RTTI descriptors behind that
 * vtable on every call: a guarded read of the `[-1]` meta slot, a 24-byte COL read, and a name
 * compare. That cost repeats per question, per frame.
 *
 * DMK::rtti::TypeIdentity inverts the query. It resolves the class primary vtable once through a
 * reverse-RTTI sweep, caches it with the resolving image generation token, and answers later
 * questions with a pointer compare. The generation stamp is what makes the cache safe: an unload or
 * a detectable same-base remap drops it and forces a cold resolve, where a plain "last vtable seen"
 * memo keeps answering from an image that is no longer mapped.
 *
 * A caller names a class and gets an answer. Whether the table is built, and whether a given class
 * resolved, stays inside this module.
 */
#ifndef TPVCAMERA_RTTI_TYPES_HPP
#define TPVCAMERA_RTTI_TYPES_HPP

#include "dmk_aliases.hpp"

#include <cstddef>
#include <cstdint>

namespace TPVCamera
{
    /**
     * @enum GameClass
     * @brief The engine classes the mod identifies by vtable.
     */
    enum class GameClass : std::uint8_t
    {
        /// wh::entitymodule::C_Player, the local actor the camera chain resolves.
        Player,
        /// CView, the engine view object that gates the frustum detour to the game view.
        View,
        /// CAnimatedCharacter, the body-rotation layer the orbit body turn writes through.
        AnimatedCharacter,
        /// wh::game::C_CameraCombatDelegate, the active camera during combat.
        CameraCombat,
        /// wh::game::C_CameraDialog, the active camera during dialogue.
        CameraDialog,
        /// wh::game::C_MissileWeaponPlayerController, embedded in C_Player, carries the aim flag.
        MissileController,
        /// wh::game::C_ActorModel, which carries the stance the MOUNT and STEALTH presets key on.
        ActorModel,
        /// Enumerator count. Not a class.
        Count,
    };

    /**
     * @brief Resolves every class identity over the game image.
     * @param image The WHGame.dll range. Region::host() is the game executable, and every class below
     *        lives in WHGame.dll.
     * @details Call exactly once, from init(), after the module base and size resolve and before any
     *          detour arms. Every query before that point answers through the direct RTTI walk, so the
     *          table is never read half-built, and each resolving sweep then runs on the init thread
     *          rather than the render thread.
     * @note Setup and control plane only: allocates the identity table. Not safe to call twice, because
     *       a query thread reads the published table without a lock.
     */
    void init_game_types(Region image);

    /**
     * @brief Checks whether @p vtable is the primary vtable of @p klass.
     * @param klass The class to test against.
     * @param vtable Runtime vtable pointer read from an object. Zero answers false.
     * @return True when the vtable belongs to that class.
     * @details A resolved identity answers with one pointer compare plus a bounded generation re-read.
     *          Before init_game_types() runs, and for a class whose primary vtable is ambiguous or
     *          carries no RTTI, the answer comes from the direct RTTI walk instead. The verdict is the
     *          same either way, only its cost differs.
     * @note Callback-safe once the identity resolves.
     */
    [[nodiscard]] bool vtable_is(GameClass klass, std::uintptr_t vtable) noexcept;

    /**
     * @brief Checks whether @p vtable is the primary vtable of the minigame class at @p index.
     * @param index Row index into the k_minigames table in game_state.hpp.
     * @param vtable Runtime vtable pointer read from an object. Zero answers false.
     * @return True when the vtable belongs to that minigame class, false for an out-of-range index.
     * @details The minigame classes share one shape and are looked up by position, so they are keyed
     *          by that table's index rather than by a second enumerator list that could drift from it.
     * @note Callback-safe once the identity resolves.
     */
    [[nodiscard]] bool minigame_vtable_is(std::size_t index, std::uintptr_t vtable) noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_RTTI_TYPES_HPP
