/**
 * @file hooks/shader_twins.hpp
 * @brief Swaps the character's forward-pass items to twins of the stock shaders that apply the close-up dissolve.
 *
 * The stock Eye shader draws the eyes again in a forward pass that ignores the dissolve, the stock Hair shader the
 * eyelashes, beard, hair and a hood's hair cards (alpha-blended after the G-buffer), and the stock Illum shader a
 * transparent item such as the wet film over the eyes, so in the close-up fade they stayed solid on a dithered
 * character. Their twins, built offline from the game's own shader binaries (generated/shader_twin_blobs.hpp) and
 * written to the user shader cache, apply the dissolve in those pixel shaders and nothing else, and draw exactly like
 * the stock shaders while nothing dissolves. A shader-name detour on the .ext loader gives a twin its stock shader's
 * gen flags. The swap happens per render item in CRenderView::AddRenderObject, in the main view only: in third person
 * an item of a temporary render object that names one of the character's render nodes (his own, or a carried item's)
 * draws with its twin (an Illum item only when it is transparent), and every other item (other characters' too) keeps
 * the stock shader. A detour on CreatePipelineStates makes a build of such an item at rest keep the stock PSOs and
 * build the fade's PSOs ahead, so the engine compiles them before the first close-up; while he dissolves, an item keeps
 * the twin only once those have compiled.
 */
#ifndef TPVCAMERA_HOOKS_SHADER_TWINS_HPP
#define TPVCAMERA_HOOKS_SHADER_TWINS_HPP

#include "hook_set.hpp"

#include <DetourModKit/error.hpp>

#include <cstdint>
#include <span>

namespace TPVCamera
{

    /**
     * @brief Installs the AddRenderObject swap, the .ext alias and the CreatePipelineStates detour.
     * @details Best-effort: on any miss the feature stays off and the close-up fade hides the head as before. The
     *          shader files, the check of the stock shaders the twins were built from and the shader manager need the
     *          engine up, so they are prepared on the main thread by the first update_shader_twins() call that lists a
     *          character node (the first third-person frame with CloseUpFade on), before anything is swapped; that is
     *          also when the files and compiled entries of twins an older build shipped are deleted from the user
     *          shader cache. A session that never needs a twin prepares nothing.
     * @param hooks The mod's hook set. Each handle is stored before its arm.
     * @return An empty value when the hooks were installed, else the library Error (ErrorCode::NoMatch when the gate
     *         failed).
     * @note Call after resolve_all_anchors() and init_game_types().
     */
    [[nodiscard]] DMK::Result<void> initialize_shader_twins(HookSet &hooks);

    /** @brief True once the hooks are armed and the feature has not been turned off for the session. */
    [[nodiscard]] bool shader_twins_available() noexcept;

    /**
     * @brief Advances the swap set by one camera frame.
     * @details Main thread, once per camera frame. Creates the twin shaders that render jobs asked for and publishes
     *          the swap set: with @p character_nodes the character's Eye, Hair and transparent Illum items on those
     *          nodes draw with their twins (while they dissolve, only once the twin is warm for the fade), and
     *          eye_fade_ready() and character_fade_ready() say how much of him that now covers. Every call, with or
     *          without @p character_nodes, also polls the twins' sets for the fade until they can draw.
     * @param character_nodes The player character's render node, then those of what he carries (CharacterNodes), while
     *        the close-up fade can run (CloseUpFade on, third person), else empty. At most
     *        Constants::CHARACTER_TWIN_MAX_NODES are kept; a zero entry is skipped.
     */
    void update_shader_twins(std::span<const std::uintptr_t> character_nodes) noexcept;

    /**
     * @brief True while the eyes fade with the character: the twins run, an Eye twin on him is warm for the fade, and
     *        so is every Eye item's twin seen on him in the last moment, and every Hair and transparent Illum item's
     *        twin that has not been given up.
     * @details The close-up fade then leaves the head shown until the face is gone, since the face and the eyes
     *          dither out with the body; while this is false it hides the head most of the way along instead. A twin
     *          is warm for the fade once every set of its PSOs for the character's dissolving objects can draw. Those
     *          sets are built ahead in third person, while he does not dissolve, so the engine compiles them in the
     *          background before the first close-up (the first run after installing can take tens of seconds of play,
     *          after which the user shader cache holds them); a dissolving item swaps to its twin only once it is warm.
     *          A later fading build that asks for a permutation not compiled yet keeps the stock PSOs (solid eyes or
     *          hair, never none), the twin turns cold, its items go back to the stock shader for the fade and it warms
     *          up again, and this is false meanwhile. It keeps its value while none of his items is drawn, and in
     *          first person. A twin is given up when its shader does not load or does not match its source's, or when
     *          a set of its PSOs for the fade cannot draw for good (the engine stopped trying to compile its
     *          permutation); a given-up Hair or Illum twin does not hold this back (its items stay solid, which the
     *          head hide at the top of the fade covers), an Eye twin does. Main thread, after this frame's
     *          update_shader_twins().
     */
    [[nodiscard]] bool eye_fade_ready() noexcept;

    /**
     * @brief True while everything of the character that redraws in a forward pass fades with him: eye_fade_ready(),
     *        and the twin of every Eye, Hair and transparent Illum item seen on him in the last moment (his eyes,
     *        eyelashes, beard, hair and eye film) is warm for the fade, none of them given up.
     * @details The close-up fade then never hides the head: the 4x4 dither discards every pixel at the top of the
     *          fade, so he is gone without it. Main thread, after this frame's update_shader_twins().
     */
    [[nodiscard]] bool character_fade_ready() noexcept;

    /**
     * @brief Unpublishes the swap set and drops the PSO references the main thread holds.
     * @details Called from shutdown before the hooks retire. The twin shader references are kept on purpose: render
     *          objects hold them by id. Main thread or the teardown thread, after the camera stopped calling
     *          update_shader_twins().
     */
    void shutdown_shader_twins() noexcept;

    /**
     * @brief Drops the sets for the fade a render thread handed over while shutdown_shader_twins() ran.
     * @details A render thread that checked the module was up just before the shutdown can still hand one over until
     *          the hooks retire, and nothing else would drop the references it holds. Called from shutdown once the
     *          hook set retired, when no detour runs any more.
     */
    void release_shader_twin_records() noexcept;

} // namespace TPVCamera

#endif // TPVCAMERA_HOOKS_SHADER_TWINS_HPP
