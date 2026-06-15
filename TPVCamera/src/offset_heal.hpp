/**
 * @file offset_heal.hpp
 * @brief Self-healing offset cache for the player / context pointer chains.
 *
 * The pointer-chain navigation in camera_hook.cpp and game_state.cpp walks fixed field offsets inside
 * structs whose base is resolved live (g_env / the anchored global context). A game patch that inserts or
 * removes a struct member shifts every field after it, and a hardcoded offset then reads the wrong slot.
 * This unit recovers the shifted offsets at runtime with DetourModKit's reverse-RTTI self-heal
 * (rtti_dissect.hpp): each in-scope offset is keyed to the MSVC mangled name of the object its slot points
 * at, and the heal scans a small window around the nominal offset for the slot that still resolves to that
 * type. Only the offset is cached (a ptrdiff_t), never an absolute address, so the recovered value stays
 * valid across instances and sessions.
 *
 * The cache holds the current nominal offsets until a heal runs, so behaviour is identical to a hardcoded
 * build until a layout actually drifts. Healing is strictly fail-closed: an unrecoverable layout leaves the
 * nominal offset in place (degrades to current behaviour, never a guessed offset, never a crash), exactly
 * as DetourModKit's heal primitives guarantee. The heals are init-time / first-resolve only (the RTTI
 * prelude is syscall-heavy), never per-frame; the per-frame chains just read one relaxed atomic each.
 */
#ifndef TPVCAMERA_OFFSET_HEAL_HPP
#define TPVCAMERA_OFFSET_HEAL_HPP

#include "constants.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace TPVCamera
{

    /**
     * @struct RuntimeOffsets
     * @brief Live (possibly healed) copies of the in-scope pointer-chain field offsets.
     * @details Each field is a lock-free atomic so the render thread can read it per frame while a one-shot
     *          heal writes it once, with no torn read (a single aligned word) and no lock. Every field is
     *          seeded with its constants.hpp nominal, so before any heal the cache reproduces the hardcoded
     *          build exactly. Writes happen only inside the heal functions below (under a one-shot guard);
     *          the chains read with relaxed ordering because the value is a standalone scalar with no
     *          dependent data published through it, and a one-frame-stale nominal-vs-healed read is harmless
     *          (both are valid offsets until the heal completes).
     */
    struct RuntimeOffsets
    {
        std::atomic<std::ptrdiff_t> cactiongame_local_actor{Constants::CACTIONGAME_LOCAL_ACTOR_OFFSET};
        std::atomic<std::ptrdiff_t> c_player_entity{Constants::C_PLAYER_ENTITY_OFFSET};
        std::atomic<std::ptrdiff_t> c_player_look_controller{Constants::C_PLAYER_LOOK_CONTROLLER_OFFSET};
        std::atomic<std::ptrdiff_t> c_player_animated_human{Constants::C_PLAYER_ANIMATED_HUMAN_OFFSET};
        std::atomic<std::ptrdiff_t> c_player_actor_model{Constants::C_PLAYER_ACTOR_MODEL_OFFSET};
        std::atomic<std::ptrdiff_t> c_player_missile_controller{Constants::C_PLAYER_MISSILE_CONTROLLER_OFFSET};
        std::atomic<std::ptrdiff_t> animated_human_animchar{Constants::ANIMATED_HUMAN_ANIMCHAR_OFFSET};
        std::atomic<std::ptrdiff_t> context_manager{Constants::OFFSET_MANAGER_PTR_STORAGE};
        std::atomic<std::ptrdiff_t> context_minigame_subsystem{Constants::OFFSET_MINIGAME_SUBSYSTEM};
    };

    /**
     * @brief Returns the process-wide runtime offset cache.
     * @details Backed by a single function-local static (no static-init-order dependency, like the other
     *          shared state in global_state.hpp). Constructed on first use with every field at its nominal.
     */
    [[nodiscard]] RuntimeOffsets &runtime_offsets() noexcept;

    /**
     * @brief Heals the C_Player-rooted offsets once from a live, RTTI-validated C_Player.
     * @details Independently heals the RTTI-typed members (entity, animated-human, actor-model, missile
     *          controller) and the animated-character pointer one hop further out, then recovers the
     *          look-controller offset (which has no RTTI of its own) from the tight bracket of its two
     *          straddling RTTI neighbours. Each heal that resolves writes its cache field; each that cannot
     *          leaves the nominal in place. One-shot: the first call on a validated C_Player does the work,
     *          every later call returns immediately. Caller must pass a C_Player whose vtable already matched
     *          C_PLAYER_RTTI_NAME (the members are only meaningful for a real C_Player). Render-thread only.
     * @param c_player Live C_Player base address.
     */
    void heal_player_offsets(std::uintptr_t c_player) noexcept;

    /**
     * @brief Recovers the CActionGame local-actor offset when the cached slot no longer holds a C_Player.
     * @details The look-up of C_Player itself navigates through this offset, so it cannot be healed from a
     *          resolved C_Player (there is none until this offset is right). Instead the resolver calls this
     *          when the cached slot holds a populated object that is NOT a C_Player (the signature of a
     *          CActionGame layout drift): it scans a window around the nominal for the slot that resolves to
     *          C_Player and caches it. The nominal slot is checked first, so an undrifted build resolves with
     *          a single probe and no scan. Render-thread only.
     * @param action_game Live CActionGame base address.
     * @return The resulting local-actor offset (healed if recovered, else the unchanged cache value).
     */
    [[nodiscard]] std::ptrdiff_t heal_local_actor_offset(std::uintptr_t action_game) noexcept;

    /**
     * @brief Heals the global-context member offsets once (camera manager, minigame subsystem).
     * @details Independently heals OFFSET_MANAGER_PTR_STORAGE and OFFSET_MINIGAME_SUBSYSTEM from the resolved
     *          context object, keyed on the manager / minigame-subsystem RTTI names. Because the context base
     *          is anchored (not navigated through a possibly-drifted offset), a top-of-context member
     *          insertion is recoverable here. One-shot: the first call after the world is live does the work.
     *          Render-thread only.
     * @param context Resolved global-context object base address.
     */
    void heal_context_offsets(std::uintptr_t context) noexcept;

} // namespace TPVCamera

#endif // TPVCAMERA_OFFSET_HEAL_HPP
