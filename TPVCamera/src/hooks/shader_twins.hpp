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
 *
 * The same module opens a dithered window in the trees between the third-person camera and the character. The trees
 * the camera names draw with twins of the stock Vegetation and Illum shaders (leaves and most trunks, and the bark,
 * trunks and decals some trees draw with Illum; TwinScope::ListedPlants in the generated header). A plant twin
 * discards, with the stock 4x4 dither, the pixels inside a window around the character that are nearer than him, so
 * the rest of the tree and its shadow stay: the item of a listed tree whose shader is a Vegetation or Illum instance
 * goes on with that instance's twin, and nodes that are not listed (props, buildings) keep their stock shaders. A
 * plant twin also discards the pixels close to the lens (the near cut), so a branch right in front of the camera thins
 * away while the rest of the tree stays. The bare frames near the lens (tent beams and poles) draw with a twin of the
 * stock Illum shader that applies only the near cut (TwinScope::ListedFrames), so a beam across the lens thins away and
 * a pole in front of the character stays. The window and the near cut ride in the four rows of the main view's
 * per-view constant buffer that the engine leaves zero, written at its upload. The CreatePipelineStates detour keeps a
 * swapped tree or frame on its stock PSOs until the twin's have compiled, so it never drops out of view while they do;
 * it only opens once they are ready. Trees and frames are never faded whole.
 */
#ifndef TPVCAMERA_HOOKS_SHADER_TWINS_HPP
#define TPVCAMERA_HOOKS_SHADER_TWINS_HPP

#include "hook_set.hpp"
#include "math_utils.hpp"

#include <DetourModKit/error.hpp>

#include <cstdint>
#include <span>

namespace TPVCamera
{

    /**
     * @brief Installs the render swaps, loading-only shader preparation, and per-view window upload.
     * @details Best-effort: on any miss the feature stays off, the close-up fade hides the head and the trees keep
     *          the stock shaders. The hooks go in here, from the ShaderTwins anchors, and the per-view upload hook from
     *          the TreeHoleWindow ones: without it the trees keep the stock shaders and the character's twins, which
     *          need no window, go on. Material loading and actual render items retain bounded shader/resource
     *          candidates. The files, stock-binary checks and shader manager are prepared only on the verified main
     *          thread before a level or engine-save loading operation returns. Render-command barriers finish stock
     *          parses before twin creation and twin parses before returning to the engine. Missing variants during
     *          gameplay stay stock until the game next stands still (service_shader_twins_paused()) or a later loading
     *          screen. Hot reload uses an optional, cache-only lookup to recover loaded current-version twins from the
     *          previous DLL; it never creates missing variants during play, and the level inventory runs again at the
     *          first pause. A missing lifecycle anchor disables the whole twin feature; missing recovery anchors only
     *          skip cache recovery.
     *          Development builds also keep up to eight non-merged plants swapped by their native render-data
     *          association, independently of the camera's target list, in capacity of their own. Each frame checks
     *          the association, and a level or save load releases them. They own no native object and keep every PSO
     *          readiness check.
     * @param hooks The mod's hook set. Each handle is stored before its arm.
     * @return An empty value when the hooks were installed (with or without the per-view upload hook), else the
     *         library Error (ErrorCode::NoMatch when the gate failed).
     * @note Call after resolve_all_anchors() and init_game_types().
     */
    [[nodiscard]] DMK::Result<void> initialize_shader_twins(HookSet &hooks);

    /** @brief True once the hooks are armed and the feature has not been turned off for the session. */
    [[nodiscard]] bool shader_twins_available() noexcept;

    /**
     * @brief Whether a feature that draws with twins is on: the tree see-through (UseTreeSeeThrough), the lens cut
     *        (UseLensCut) or the close-up fade (CloseUpFade).
     * @details While none is, no twin is created: a loading screen drops what it captured and creates none, a pause
     *          creates none, and the materials loaded during play are not captured. The camera stops advancing the
     *          twins once every swapped node has gone back to its stock shader. The level's inventory is then left to
     *          the first pause after one of them turns on (service_shader_twins_paused()). Any thread.
     */
    [[nodiscard]] bool shader_twins_wanted() noexcept;

    /**
     * @brief The window the trees open, around the character and on his vertical plane facing the camera, and the near
     *        cut the trees and the frames make at the lens.
     */
    struct TreeHoleWindow
    {
        Vector3 anchor{};            // world centre of the character's box
        float strength = 0.0f;       // 0 (off) to 1 (window fully open, dithered rim)
        float half_width = 0.0f;     // metres, on the character's vertical plane
        float half_height = 0.0f;    // metres
        float depth_margin = 0.30f;  // metres in front of the character where nothing is cut
        float depth_feather = 0.30f; // metres past the margin over which the cut fades in
        float softness = 0.35f;      // rim width, share of the window
        // World height below which nothing opens (a pixel whose ray meets the character's plane lower stays), and the
        // metres above it over which the cut fades in.
        float floor_z = -1.0e6f;
        float floor_feather = 0.25f;
        // 0: the window narrows to a point at the lens (a cone from the camera); 1: it keeps its size from the
        // character back to the lens (a tube), so what hides him right in front of the lens goes too.
        float tunnel = 1.0f;
        // The near cut, on every swapped tree and frame wherever it is on the screen, whether or not the window is
        // open: 0 (off) to 1. A pixel nearer to the lens than near_full (view depth, metres) goes, one beyond
        // near_clear stays, and in between it fades out in the dither.
        float near_strength = 0.0f;
        float near_full = 0.6f;
        float near_clear = 1.2f;
    };

    /**
     * @brief Advances the swap set and the window by one camera frame.
     * @details Main thread, once per camera frame. Polls and publishes already-prepared twins; it performs no shader
     *          file preparation or shader creation. With @p character_nodes the character's Eye, Hair and transparent
     *          Illum items on those nodes draw with their twins (while they dissolve, only once the twin is warm for
     *          the fade), and eye_fade_ready() and character_fade_ready() say how much of him that covers. Every call,
     *          with or without @p character_nodes, also polls the twins' sets for the fade until they can draw.
     *          @p frames and @p trees are the frame and tree render nodes that should draw with the hole now (at most
     *          TREE_HOLE_MAX_NODES together, the frames first; extras are dropped). The module keeps a node swapped
     *          for TREE_HOLE_HOLD_SECONDS after it stops being listed, invalidates its persistent render object when it
     *          is activated or released (and again when a twin it may use becomes ready, or when the twin's PSOs a
     *          compiled object of it waited for are ready), and publishes the window and the near cut to the per-view
     *          hook.
     *          Targets without a ready twin of their scope are tracked passively, without these invalidations; no
     *          window rows are published until a listed scope has a ready twin. Neither a window strength nor a near
     *          cut strength, or no node with nothing held, turns the hole off.
     * @param character_nodes The player character's render node, then those of what he carries (CharacterNodes), while
     *        the close-up fade can run (CloseUpFade on, third person), else empty. At most
     *        Constants::CHARACTER_TWIN_MAX_NODES are kept; a zero entry is skipped.
     * @param window The window and the near cut this frame.
     * @param trees The IRenderNode pointers of the trees to swap.
     * @param frames The IRenderNode pointers of the bare frames to swap (the near cut only).
     * @param delta_time Seconds this frame advances.
     */
    void update_shader_twins(std::span<const std::uintptr_t> character_nodes, const TreeHoleWindow &window,
                             std::span<void *const> trees, std::span<void *const> frames, float delta_time) noexcept;

    /** @brief What a camera frame says about the game standing still, for service_shader_twins_paused(). */
    struct TwinPauseSignals
    {
        bool clock_paused = false; // the engine's game clock stands still this frame (it advanced the world by 0)
        bool menu_open = false;    // the Esc game menu is open
        bool overlay_up = false;   // a game UI overlay is up (inventory, map, codex, or the dialog screen)
        // A dialogue runs, which is gameplay whatever else is up; read whenever the clock stands still.
        bool dialogue = false;
    };

    /**
     * @brief Creates, while the game stands still, the twins of the plant and frame variants (and the character's
     *        drawn items) that came after the last loading boundary, one at a time.
     * @details A twin's parse stalls the render thread for about 30 to 190 ms, so outside a loading screen a twin is
     *          created only while the game clock stands still (under the Esc game menu, a UI overlay, or neither, as
     *          when the Esc menu was open across a DEV hot reload), never on a menu or overlay over a running clock,
     *          never in a dialogue (both are real time), and only once that has lasted
     *          Constants::SHADER_TWIN_PAUSE_DWELL_MS. Each creation is finished in its own paused frame: the twins'
     *          mutex is released and a render-command barrier waits for the parse, as at a loading screen, and the
     *          next comes Constants::SHADER_TWIN_PAUSE_SPACING_MS after it. The variants come from the materials
     *          loaded during play (plant and frame kinds only), the items a render job found without a twin and the
     *          level inventory's pairs whose stock shader still parsed at the loading screen; the first such pause
     *          after a DEV hot reload also takes the level inventory the loading screen before it did not. A barrier
     *          that fails stops the pause-time creation for the session (loading screens go on). Every call that is not
     *          inside such a barrier also drops the candidates a render job or a loader evicted from the pool, whose
     *          engine Release runs only on the main thread. When the game does not stand still this is a few compares
     *          and returns without a lock.
     * @param signals This frame's pause signals.
     * @note Main thread, once per camera frame (not on the frame's second build), whatever view the camera shows,
     *       before update_shader_twins().
     */
    void service_shader_twins_paused(const TwinPauseSignals &signals) noexcept;

    /**
     * @brief Names the render nodes of the people and animals the NPC lens fade dissolves, for the next twin update.
     * @details Their eyes, eyelashes, hair and eye film then draw with a twin while they dissolve, when one is ready
     *          and warm for the fade (the player's close-up fade decides which twins exist and warms them); without
     *          one they keep the stock shader, which ignores the dissolve. Main thread only, once per camera frame;
     *          the first NPC_FADE_MAX_ENTITIES are kept.
     */
    void set_npc_twin_nodes(std::span<const std::uintptr_t> nodes) noexcept;

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
     * @brief Swaps every tree back (invalidating it) so no render object keeps a plant twin, unpublishes the swap set
     *        and the window, and drops retained candidate and PSO references.
     * @details Called from shutdown before the hooks retire. The twin shader references are kept on purpose: render
     *          objects hold them by id. Main thread or the teardown thread, after the camera stopped calling
     *          update_shader_twins().
     */
    void shutdown_shader_twins() noexcept;

    /**
     * @brief Drops the PSO waits and the sets for the fade a render thread handed over while shutdown_shader_twins()
     *        ran.
     * @details A render thread that checked the module was up just before the shutdown can still hand one over until
     *          the hooks retire, and nothing else would drop the references it holds. The same applies to the
     *          candidates a callback retained or evicted meanwhile. It also drops the extra references a hot-reload
     *          cache recovery took and frees the asset-scope TLS index. Called from shutdown once the hook set
     *          retired, when no detour runs.
     */
    void release_shader_twin_records() noexcept;

    /** @brief Swapped trees now, twin instances created (trees and the character), and the twins ready, for logs. */
    struct ShaderTwinStats
    {
        int swapped = 0;
        int twins = 0;
        int twins_ready = 0;
    };

    /** @brief The current ShaderTwinStats. Any thread. */
    [[nodiscard]] ShaderTwinStats shader_twin_stats() noexcept;

} // namespace TPVCamera

#endif // TPVCAMERA_HOOKS_SHADER_TWINS_HPP
