/**
 * @file camera_hook.cpp
 * @brief Third-person camera implemented at the camera frustum builder.
 *
 * Hooks CCamera::UpdateFrustumPlanes, the function that turns a camera's 3x4 matrix
 * into world-space cull planes. By offsetting the game view camera's matrix HERE,
 * before the cull planes are computed from it, the camera renders behind the player
 * AND the frustum culls from the same offset position, so nearby geometry is not
 * wrongly hidden. Offsetting only the rendered matrix afterwards (at the view
 * convergence point on return) leaves the frustum culling from the eye, which was the
 * source of the "items not shown up close" artifact.
 *
 * Every gameplay camera (first person, combat through its private inner camera, mount)
 * funnels into this builder for the active CView, so one hook covers them all. The
 * builder is shared by shadow, reflection, and portal cameras too, so the detour gates
 * to the game view by checking that the CView embedding the camera carries the CView
 * vtable.
 *
 * Gameplay still reads the player look-dir/eye channel, not this render camera, so aim
 * and interaction keep working off the native frame; only the rendered sink moves. A
 * companion hook on the head-visibility setter keeps the player head rendered from
 * behind. The offset is suppressed while the game is in any state listed in SuppressTPVState (by
 * default the pause menu and blocking overlays), so that context renders from the game's own camera.
 */

#include "camera_hook.hpp"
#include "../rtti_types.hpp"
#include "core/blend/blend.hpp"
#include "camera/close_fade.hpp"
#include "camera/collision.hpp"
#include "camera/commit.hpp"
#include "camera/lens.hpp"
#include "camera/npc_lens_fade.hpp"
#include "camera/see_through_driver.hpp"
#include "core/input/move_input.hpp"
#include "core/math/angle.hpp"
#include "core/rig/rig_stage.hpp"
#include "core/view/view_blend.hpp"
#include "aob_resolver.hpp"
#include "character_attachments.hpp"
#include "constants.hpp"
#include "dev/hook_profile.hpp"
#include "config.hpp"
#include "global_state.hpp"
#include "game_state.hpp"
#include "game_structures.hpp"
#include "indoor_state.hpp"
#include "offset_heal.hpp"
#include "math_utils.hpp"
#include "physics_raycast.hpp"
#include "query_ledger.hpp"
#include "render_occlusion.hpp"
#include "hooks/ai_observer.hpp"
#include "hooks/ranged_hook.hpp"
#include "hooks/vtable_slots.hpp"
#include "hooks/character_fade.hpp"
#include "hooks/head_visibility.hpp"
#include "hooks/input_hook.hpp"
#include "hooks/native_turn_hooks.hpp"
#include "hooks/occluder_fade.hpp"
#include "hooks/shader_twins.hpp"
#include "hooks/ui_menu_hooks.hpp"
#include "hooks/player_onaction_hook.hpp"
#include "presets/preset_runtime.hpp"
#include "presets/switch_log.hpp"
#ifdef TPVCAMERA_DEV_BUILD
#include "dev/frame_recorder.hpp"
#endif

#include <DetourModKit.hpp>

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <array>

namespace TPVCamera
{

    // CCamera::UpdateFrustumPlanes(camera /*rcx*/): camera object starts with the 3x4 matrix,
    // followed by the projection params; this builds the world cull planes. Offsetting the matrix
    // here, before the cull planes are computed, moves the rendered view AND its culling together.
    using FrustumBuildFunc = uintptr_t(__fastcall *)(uintptr_t camera);

    // Where the occluder fade aims the hip sightline: this far above the feet when the body is known, else this far
    // below the eye. The head and a chest point between the two complete the three sightlines.
    constexpr float k_fade_hip_height = 0.9f;
    constexpr float k_fade_hip_drop = 0.75f;
    // The camera collision's and the occluder fade's carry-over between frames (see camera/collision.hpp). Main thread
    // only (the camera detour).
    static CollisionState s_collision{};
    // The first-person <-> third-person switch's yaw latch (see core/view/view_blend.hpp), cleared when the offset
    // turns off. Main thread only (the camera detour).
    static ViewBlendLatch s_view_latch{};

    // The lens distance from the eye (meters) within which the switch hides the head like first person does, and the
    // farther one it shows it again past, so a lens hovering at the edge of the head does not flicker it.
    constexpr float k_switch_head_hide_on = 0.25f;
    constexpr float k_switch_head_hide_off = 0.3f;

    // The render nodes of the character and of what it carries (what is bound to its attachments, a bow's nocked
    // arrow included), walked every third-person frame while CloseUpFade, UseOccluderFade or collision is on, and
    // empty otherwise. The occluder scan classes them as the character (set_occluder_player_nodes), so the camera
    // passes through what he carries, and the close-up fade dissolves the carried ones with him. Main thread only (the
    // camera detour).
    static CharacterNodes s_character_nodes{};

    // Trampoline slots. Each install stores its slot with release order before the hook arms. Each detour reads its
    // slot with acquire order, so a detour never chains through a slot it cannot see.
    static std::atomic<FrustumBuildFunc> s_frustum_build_original{nullptr};

    /**
     * @brief The zoom hold bindings resolved to BindingTokens for the per-frame query.
     * @details Acquired on the control plane (refresh_zoom_binding_tokens: after the input engine starts and after
     *          every INI reload, the only places their bindings reshape) and published whole, so the main thread
     *          never resolves a binding name itself. A token left stale by a reshape reads "not held" (fail closed)
     *          until the reload callback republishes, a window of a few milliseconds.
     */
    struct ZoomBindingTokens
    {
        DMK::input::BindingToken zoom_in;
        DMK::input::BindingToken zoom_out;
    };
    static std::atomic<std::shared_ptr<const ZoomBindingTokens>> s_zoom_tokens;

    // Resolved SSystemGlobalEnvironment (g_env) base, set once at init by the AOB cascade (0 when it missed).
    // Reused by the player/animchar walks. p_physical_world is g_env + PHYSICAL_WORLD_OFFSET.
    static uintptr_t s_genv_runtime = 0;

    // CView vtable address, cached lazily on the first frustum-builder camera whose embedding object
    // passes the CView RTTI check. The steady-state game-view gate is then a single qword compare;
    // using the RTTI type name (not a hardcoded address) keeps the gate working across game patches.
    static uintptr_t s_cview_vtable_runtime = 0;

    // The offset's effective active state (effective_tpv && should_apply_view), published by the
    // frustum detour each frame. The head-visibility detour and the free-look input gate read it so
    // they mirror the live offset state without recomputing the whole forced-view policy themselves.
    static std::atomic<bool> s_offset_active{false};

    bool camera_offset_active() noexcept
    {
        return s_offset_active.load(std::memory_order_relaxed);
    }

    // Published by the frustum detour each game-view frame: true while the game is showing the OS cursor
    // (a UI is up). The free-look input gate reads it to FREEZE the orbit - hold its angles and ignore
    // mouse-look - while any cursor UI is open (menu, inventory, loot/trade, dialogue), so the camera
    // does not turn from cursor motion and resumes from the same angle when the cursor hides. The orbit
    // hook captures raw mouse UPSTREAM of the engine's own input freeze, so it needs this explicit gate.
    static std::atomic<bool> s_cursor_shown{false};

    bool game_cursor_shown() noexcept
    {
        return s_cursor_shown.load(std::memory_order_relaxed);
    }

    // Situations in which the game sets which way the character faces, so the aim hand-off leaves the facing alone:
    // riding a horse or a cart, sitting, lying or kneeling, a conversation, and the minigames that put the character at
    // a lock, a table, an anvil and the like. The throwing and shooting minigames are aimed by the player.
    constexpr uint32_t k_game_held_facing_states =
        state_bit(GameState::Mount) | state_bit(GameState::Cart) | state_bit(GameState::Lying) |
        state_bit(GameState::Sitting) | state_bit(GameState::Kneel) | state_bit(GameState::Dialogue) |
        state_bit(GameState::MinigameSharpening) | state_bit(GameState::MinigameReading) |
        state_bit(GameState::MinigameAlchemy) | state_bit(GameState::MinigameHerbGathering) |
        state_bit(GameState::MinigameLockpicking) | state_bit(GameState::MinigameHoleDigging) |
        state_bit(GameState::MinigameDice) | state_bit(GameState::MinigamePickpocketing) |
        state_bit(GameState::MinigameBlacksmithing) | state_bit(GameState::MinigameForgeBuilder);

    // The states read from the live UI signals rather than the debounced mask: the suppression gate and the
    // orbit-exclude policy both react on the frame a menu or an overlay opens or closes.
    constexpr uint32_t k_live_ui_states = state_bit(GameState::Menu) | state_bit(GameState::Overlay);

    bool should_apply_view() noexcept
    {
        const uint32_t suppress = state_policy().suppress();
        if (suppress == 0)
        {
            return true;
        }
        // Menu / Overlay use the live UI signals so suppression is instant (no debounce frame of TPV in a UI).
        if ((suppress & state_bit(GameState::Menu)) != 0 && is_game_menu_open())
        {
            return false;
        }
        if ((suppress & state_bit(GameState::Overlay)) != 0 && overlay_state().active.load(std::memory_order_relaxed))
        {
            return false;
        }
        // Any other listed state is matched against the debounced mask published each game-view frame.
        const uint32_t gameplay = suppress & ~k_live_ui_states;
        if (gameplay != 0 && (gameplay & game_state_mask().load(std::memory_order_relaxed)) != 0)
        {
            return false;
        }
        return true;
    }

    // ITimer::GetFrameStartTime and ITimer::GetFrameTime (see constants.hpp): the engine frame clock.
    using TimerGetFrameStartTimeFunc = const std::int64_t *(__fastcall *)(uintptr_t timer, int which);
    using TimerGetFrameTimeFunc = float(__fastcall *)(uintptr_t timer, int which);
    using TimerGetRealFrameTimeFunc = float(__fastcall *)(uintptr_t timer);

    /**
     * @brief One engine frame as the frame clock reports it.
     */
    struct EngineFrame
    {
        /// UI-clock frame-start stamp, identical for every read within one frame.
        std::int64_t start_stamp;
        /// Seconds the world advanced this frame.
        float seconds;
        /// Real seconds this frame took, whatever the game's time scale; seconds when the real clock cannot be read.
        float real_seconds;
        /// The game clock is stopped (the pause menu) and seconds is the UI clock's.
        bool paused;
    };

    /**
     * @brief Reads the engine frame clock through the g_env timer.
     * @details The timer is confirmed by its RTTI name and both vtable slots must point into the game image
     *          before either is called; the frame time must come back finite and non-negative. The game clock
     *          reads 0 while the game timer is paused (the pause menu), and the UI clock's frame time stands in
     *          then: nothing in the world moves, so its lack of smoothing cannot show, and the view still eases
     *          into and out of the menu. The real frame time is read beside it; without that slot in the image the
     *          game frame time stands in. Runs under the frustum detour's SEH frame, which contains a fault in any of
     *          the engine calls.
     * @return The frame, or std::nullopt when the clock cannot be trusted.
     */
    [[nodiscard]] static std::optional<EngineFrame> read_engine_frame()
    {
        const ModuleInfo &mod = module_info();
        if (s_genv_runtime == 0 || mod.base == 0)
        {
            return std::nullopt;
        }
        const auto timer = DMK::memory::read<uintptr_t>(DMK::Address{s_genv_runtime + Constants::GENV_TIMER_OFFSET});
        if (!timer || !DMK::memory::is_plausible_ptr(DMK::Address{*timer}))
        {
            return std::nullopt;
        }
        const auto vtable = DMK::memory::read<uintptr_t>(DMK::Address{*timer});
        if (!vtable || !vtable_is(GameClass::Timer, *vtable))
        {
            return std::nullopt;
        }
        const auto start_fn =
            DMK::memory::read<uintptr_t>(DMK::Address{*vtable + Constants::ITIMER_VTABLE_GET_FRAME_START_TIME_OFFSET});
        const auto time_fn =
            DMK::memory::read<uintptr_t>(DMK::Address{*vtable + Constants::ITIMER_VTABLE_GET_FRAME_TIME_OFFSET});
        const auto real_time_fn =
            DMK::memory::read<uintptr_t>(DMK::Address{*vtable + Constants::ITIMER_VTABLE_GET_REAL_FRAME_TIME_OFFSET});
        const auto in_image = [&mod](const auto &fn) { return fn && *fn >= mod.base && *fn < mod.base + mod.size; };
        if (!in_image(start_fn) || !in_image(time_fn))
        {
            return std::nullopt;
        }

        const std::int64_t *stamp_slot =
            reinterpret_cast<TimerGetFrameStartTimeFunc>(*start_fn)(*timer, Constants::ETIMER_UI);
        const auto stamp = DMK::memory::read<std::int64_t>(DMK::Address{reinterpret_cast<uintptr_t>(stamp_slot)});
        if (!stamp)
        {
            return std::nullopt;
        }
        const auto get_frame_time = reinterpret_cast<TimerGetFrameTimeFunc>(*time_fn);
        float seconds = get_frame_time(*timer, Constants::ETIMER_GAME);
        const bool paused = !(seconds > 0.0f);
        if (paused)
        {
            seconds = get_frame_time(*timer, Constants::ETIMER_UI);
        }
        if (!std::isfinite(seconds) || seconds < 0.0f)
        {
            return std::nullopt;
        }
        float real_seconds = seconds;
        if (in_image(real_time_fn))
        {
            const float real = reinterpret_cast<TimerGetRealFrameTimeFunc>(*real_time_fn)(*timer);
            if (std::isfinite(real) && real > 0.0f)
            {
                real_seconds = real;
            }
        }
        return EngineFrame{*stamp, seconds, real_seconds, paused};
    }

    // The heading a movement press captures (radians), when it was published (GetTickCount64, 0 = none) and
    // the thread the camera ran on, for orbit_move_pressed. A heading older than k_move_press_max_age_ms is not used.
    static std::atomic<float> s_move_press_heading{0.0f};
    static std::atomic<std::uint64_t> s_move_press_stamp_ms{0};
    static std::atomic<DWORD> s_move_press_thread{0};
    static std::atomic<float> s_move_press_body_yaw{0.0f};
    constexpr float k_move_press_min_turn = 0.087f; // rad (5 deg)

    /** @brief What one game-view frustum build advances by, as frame_delta reports it. */
    struct FrameStep
    {
        /// Seconds the camera's per-frame integrators and easings advance on this build, clamped.
        float seconds{0.0f};
        /**
         * @brief Real seconds this build advances, clamped: seconds, except while the game scales its time (the
         *        inventory runs the game clock at 1/1000, a perfect block at 0.4).
         * @details For what has to finish in real time whatever the world does, the switch between first and third
         *          person most of all: an overlay that leaves third person must not wait on a clock that has stopped.
         */
        float real_seconds{0.0f};
        /**
         * @brief The frame's second build.
         * @details Work that must run exactly once per engine frame (the ranged step) keys on this flag, not on a
         *          zero seconds value: the wall-clock fallback returns the small gap for the second build, not 0.
         */
        bool repeat_build{false};
        /// The game clock stands still (the pause menu); false when the engine clock cannot be read.
        bool paused{false};
    };

    /**
     * @brief Measures one game-view frustum build: the seconds it advances, whether it repeats the frame, and whether
     *        the game clock is paused.
     * @details The game view's frustum is built twice per frame (the projection setup, then the final rebuild).
     *          The frame's time is spent on the first build and the second advances by 0; the engine's
     *          frame-start stamp tells the two apart.
     *
     *          The duration is the engine's own game frame time, not the wall-clock gap between two builds. The
     *          engine averages its frame time over the last quarter second and moves the whole world by that
     *          average, so everything it animates advances by a near-constant step per frame. The raw gap between
     *          two builds carries the full frame-to-frame variance instead (about a tenth of a frame on average,
     *          up to half a frame). A rate-driven motion paced on it, the gamepad orbit most of all, then turns by
     *          an uneven step each frame against an evenly moving world, which reads as micro-stutter in a slow
     *          pan. Pacing on the engine clock keeps the camera in step with the world. The wall-clock gap is the
     *          fallback whenever the engine clock cannot be read, and it keeps running every call so that
     *          fallback always measures from the previous build.
     * @return This build's step.
     */
    [[nodiscard]] static FrameStep frame_delta()
    {
        static std::chrono::steady_clock::time_point s_last_time = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        const float wall_delta = std::chrono::duration<float>(now - s_last_time).count();
        s_last_time = now;
        FrameStep step{};
        // Without the engine clock, a build within a couple of milliseconds of the previous one is the same frame's
        // second build.
        step.repeat_build = wall_delta < 0.002f;

        const std::optional<EngineFrame> frame = read_engine_frame();
        step.paused = frame.has_value() && frame->paused;

        // Report which clock paces the camera once per switch, so a log shows whether the engine clock resolved.
        enum class ClockSource
        {
            None,
            Engine,
            Wall,
        };
        static ClockSource s_clock_source = ClockSource::None;
        const ClockSource clock_source = frame.has_value() ? ClockSource::Engine : ClockSource::Wall;
        if (clock_source != s_clock_source)
        {
            s_clock_source = clock_source;
            const char *const clock_name =
                frame.has_value() ? "engine frame clock" : "wall clock (engine clock unavailable)";
            (void)DMK::log().try_log(DMK::LogLevel::Debug, "Camera: pacing on the {}", clock_name);
        }

        if (frame.has_value())
        {
            static std::int64_t s_last_stamp = 0;
            step.repeat_build = frame->start_stamp == s_last_stamp;
            if (step.repeat_build)
            {
                return step; // second build of the same frame: the frame's time was already spent
            }
            s_last_stamp = frame->start_stamp;
            step.seconds = std::clamp(frame->seconds, 0.0f, 0.1f);
            step.real_seconds = std::clamp(frame->real_seconds, 0.0f, 0.1f);
            return step;
        }
        step.seconds = std::clamp(wall_delta, 0.0f, 0.1f);
        step.real_seconds = step.seconds;
        return step;
    }

    /**
     * @brief This frame's pause signals for the shader twins' pause-time creation (service_shader_twins_paused()).
     * @details The game clock comes from frame_delta(), the Esc game menu and the UI overlays from their hooks. A
     *          dialogue matters only while the clock stands still (it never lets a twin be created), with or without a
     *          menu or an overlay seen (after a DEV hot reload a menu open across it is not), so without @p state the
     *          game state is polled here only then.
     * @param step This build's FrameStep.
     * @param state This frame's game-state mask (the raw and the debounced one together), or std::nullopt.
     */
    [[nodiscard]] static TwinPauseSignals twin_pause_signals(const FrameStep &step, std::optional<uint32_t> state)
    {
        TwinPauseSignals signals{};
        signals.clock_paused = step.paused;
        signals.menu_open = is_game_menu_open();
        signals.overlay_up = overlay_state().active.load(std::memory_order_relaxed);
        if (signals.clock_paused)
        {
            // The dialogue bit comes from the active camera, which needs no player.
            const uint32_t mask = state.has_value() ? *state : poll_game_state(0);
            signals.dialogue = (mask & state_bit(GameState::Dialogue)) != 0u;
        }
        return signals;
    }

    /**
     * @brief The Indoors bit of this frame's raw game state: whether the player's mid-body is inside a building.
     * @details Asked of the engine only while some preset binds the `indoors` token, and once a frame (the frame's
     *          second build reuses the answer). The point is the first-person eye of the untouched view pose, lowered
     *          by half the eye height the last third-person frame measured above the feet, so it follows a crouch or
     *          the saddle.
     * @param cview The game view's CView.
     * @param repeat_build True on the frame's second build of the game view.
     */
    [[nodiscard]] static uint32_t poll_indoors(uintptr_t cview, bool repeat_build) noexcept
    {
        // The highest real eye height the probe drops by (meters): the saddle's eye is about 2.6 m above the feet.
        constexpr float k_max_eye_height = 3.0f;
        static uint32_t s_indoors = 0;
        if ((Presets::bound_state_bits() & state_bit(GameState::Indoors)) == 0)
        {
            s_indoors = 0;
            return 0;
        }
        if (repeat_build)
        {
            return s_indoors;
        }
        const auto eye = DMK::memory::read<Vector3>(DMK::Address{cview + Constants::SVIEWPARAMS_POSITION_OFFSET});
        if (!eye)
        {
            s_indoors = 0;
            return 0;
        }
        const float eye_height =
            std::clamp(camera_state().real_eye_height.load(std::memory_order_relaxed), 0.0f, k_max_eye_height);
        s_indoors =
            point_indoors(Vector3{eye->x, eye->y, eye->z - 0.5f * eye_height}) ? state_bit(GameState::Indoors) : 0u;
        return s_indoors;
    }

    /**
     * @brief Resolves the live C_Player via g_env each frame (validated by its vtable); 0 on failure.
     * @details Walks g_env -> p_game -> CCryAction (cached, resolved once via a virtual GetIGameFramework
     *          call) -> p_action_game -> C_Player, then confirms C_Player by its main vtable. Resolving
     *          FRESH every frame - rather than trusting a mirrored pointer that goes null/stale across
     *          view transitions and reloads - is what keeps the move-detection and body-turn locked onto
     *          the CURRENT player. Always called from within an SEH frame (the frustum detour, the
     *          body-turn wrapper, or the action dispatcher's guarded capture through orbit_move_pressed), so a fault
     *          during the walk is contained.
     */
    static uintptr_t resolve_c_player()
    {
        const ModuleInfo &mod = module_info();
        if (s_genv_runtime == 0 || mod.base == 0)
        {
            return 0;
        }
        const uintptr_t g_env_addr = s_genv_runtime;
        const auto p_game = DMK::memory::read<uintptr_t>(DMK::Address{g_env_addr + Constants::GENV_PGAME_OFFSET});
        if (!p_game || !DMK::memory::is_plausible_ptr(DMK::Address{*p_game}))
        {
            return 0;
        }

        static uintptr_t s_cry_action = 0;
        if (!DMK::memory::is_plausible_ptr(DMK::Address{s_cry_action}))
        {
            const auto vtable = DMK::memory::read<uintptr_t>(DMK::Address{*p_game});
            if (!vtable || !DMK::memory::is_plausible_ptr(DMK::Address{*vtable}))
            {
                return 0;
            }
            const auto fn =
                DMK::memory::read<uintptr_t>(DMK::Address{*vtable + Constants::IGAME_GET_FRAMEWORK_VTABLE_OFFSET});
            if (!fn || !DMK::memory::is_plausible_ptr(DMK::Address{*fn}))
            {
                return 0;
            }
            using GetFrameworkFn = uintptr_t(__fastcall *)(uintptr_t);
            s_cry_action = reinterpret_cast<GetFrameworkFn>(*fn)(*p_game);
            if (!DMK::memory::is_plausible_ptr(DMK::Address{s_cry_action}))
            {
                s_cry_action = 0;
                return 0;
            }
        }

        // Publish the chain root for the CCryAction -> CActionGame heal group. The scheduler owns the cadence
        // and the latch: its gate stays shut (silently) until the slot holds a constructed CActionGame, so the
        // offset is nominal until then and fixed afterwards. A layout shift here would silently break every
        // walk below, which is why it is the first group registered.
        note_framework_base(s_cry_action);

        const auto p_action_game = DMK::memory::read<uintptr_t>(
            DMK::Address{s_cry_action + runtime_offsets().ccryaction_actiongame.load().value});
        if (!p_action_game || !DMK::memory::is_plausible_ptr(DMK::Address{*p_action_game}))
        {
            return 0;
        }
        note_action_game_base(*p_action_game);

        // Read the local actor through the self-healed CActionGame offset, then confirm it is a real C_Player
        // by its vtable. C_Player is found THROUGH this offset, so the offset cannot be healed from a resolved
        // C_Player; instead, when the cached slot holds a populated object that is NOT a C_Player (the
        // signature of a CActionGame layout drift), attempt a bounded recovery scan and re-read. The nominal
        // slot is probed first inside the heal, so an undrifted build never scans.
        RuntimeOffsets &offsets = runtime_offsets();
        auto player =
            DMK::memory::read<uintptr_t>(DMK::Address{*p_action_game + offsets.cactiongame_local_actor.load().value});
        const auto is_c_player = [](const DMK::Result<uintptr_t> &candidate)
        {
            if (!candidate || !DMK::memory::is_plausible_ptr(DMK::Address{*candidate}))
            {
                return false;
            }
            const auto vt = DMK::memory::read<uintptr_t>(DMK::Address{*candidate});
            return vt.has_value() && vtable_is(GameClass::Player, *vt);
        };
        bool player_valid = is_c_player(player);
        if (!player_valid && player && DMK::memory::is_plausible_ptr(DMK::Address{*player}))
        {
            // The cached slot holds a populated object that is NOT a C_Player: the signature of a CActionGame
            // layout drift. Open the local-actor heal group's gate and let the scheduler recover the offset on
            // its own cadence; it scans CActionGame for the C_Player slot and latches once it resolves. The read
            // above then succeeds on a later frame and this branch stops being entered (the natural stop). The
            // request is idempotent, so a frame that re-enters here simply re-asserts it: the scheduler, not
            // this call site, owns the "do not scan every frame" rate limit and the never-give-up retry.
            request_local_actor_recovery();
        }
        if (!player_valid)
        {
            return 0;
        }

        // A real C_Player is in hand (vtable validated above): publish it for the player-rooted heal groups, so
        // the per-frame aim and body-turn walks read the recovered offsets on a drifted build.
        note_player_base(*player);

        // Log the resolved C_Player and cached CCryAction only when the player pointer CHANGES (once on first
        // resolve, and again after a reload / new player) so the address is available for external tooling
        // without flooding the log.
        {
            static uintptr_t s_logged_player{0};
            if (*player != s_logged_player)
            {
                s_logged_player = *player;
                (void)DMK::log().try_log(DMK::LogLevel::Debug, "C_Player resolved={} (CCryAction={})",
                                         DMK::format::format_address(*player),
                                         DMK::format::format_address(s_cry_action));
            }
        }

        // The player resolves once the game is in-world (window at its final resolution); let the overlay
        // wait on this so it never sizes itself to the transient loading window.
        game_world_ready().store(true, std::memory_order_relaxed);

        return *player;
    }

    /**
     * @brief Resolves the player look controller and drives the real aim: eases the PITCH toward a target (level
     *        while orbiting) and/or sets the YAW (heading).
     * @details Walks the player look chain (g_env -> p_game -> CCryAction -> p_action_game -> C_Player ->
     *          look controller; see constants.hpp) and validates C_Player by its vtable. The look
     *          quaternion the cameras read is RE-DERIVED from the controller's scalar pitch+yaw every
     *          frame, so writing those scalars (not the derived quat, which is overwritten) is what
     *          actually moves the eye, the character head AND the movement heading. While orbiting the mod
     *          redirects the look input, so the writes stick. Outside orbit (the aim hand-off) the game re-derives
     *          the look yaw from the body heading while aiming, so a yaw write sticks only together with a body
     *          turn (apply_orbit_body_turn). When the controller is unreachable nothing is written (the
     *          camera-side held base is unaffected); a false return can also follow a partial write.
     *          CCryAction is process-lifetime and resolved once via a virtual GetIGameFramework call, then
     *          cached; the rest is a guarded walk.
     * @param pitch_ease Per-frame fraction to move the look pitch toward pitch_target, in [0, 1] (0 = leave it,
     *        1 = set it).
     * @param pitch_target Look pitch to ease toward, in radians (0 = level, negative = down).
     * @param set_yaw When true, the look yaw is set to yaw_value to align the heading to the camera.
     * @param yaw_value Target look yaw in radians (engine convention: forward = (-sin yaw, cos yaw)).
     * @return True when every requested write landed; false when the controller is unreachable, a value
     *         failed its sanity check or a write was refused.
     */
    [[nodiscard]] static bool apply_orbit_aim_control(float pitch_ease, float pitch_target, bool set_yaw,
                                                      float yaw_value)
    {
        const ModuleInfo &mod = module_info();
        if (s_genv_runtime == 0 || mod.base == 0)
        {
            return false;
        }
        const uintptr_t g_env_addr = s_genv_runtime;
        const auto p_game = DMK::memory::read<uintptr_t>(DMK::Address{g_env_addr + Constants::GENV_PGAME_OFFSET});
        if (!p_game || !DMK::memory::is_plausible_ptr(DMK::Address{*p_game}))
        {
            return false;
        }

        // CCryAction is process-lifetime; resolve it once via p_game->IGame::GetIGameFramework() (a
        // trivial member getter) and cache, so the per-frame path is a pure guarded pointer walk.
        static uintptr_t s_cry_action = 0;
        if (!DMK::memory::is_plausible_ptr(DMK::Address{s_cry_action}))
        {
            const auto vtable = DMK::memory::read<uintptr_t>(DMK::Address{*p_game});
            if (!vtable || !DMK::memory::is_plausible_ptr(DMK::Address{*vtable}))
            {
                return false;
            }
            const auto fn =
                DMK::memory::read<uintptr_t>(DMK::Address{*vtable + Constants::IGAME_GET_FRAMEWORK_VTABLE_OFFSET});
            if (!fn || !DMK::memory::is_plausible_ptr(DMK::Address{*fn}))
            {
                return false;
            }
            using GetFrameworkFn = uintptr_t(__fastcall *)(uintptr_t);
            s_cry_action = reinterpret_cast<GetFrameworkFn>(*fn)(*p_game);
            if (!DMK::memory::is_plausible_ptr(DMK::Address{s_cry_action}))
            {
                s_cry_action = 0;
                return false;
            }
        }

        // Publish the chain root for the heal group, as the resolver does.
        note_framework_base(s_cry_action);

        // CCryAction -> CActionGame -> local actor in one guarded walk. walk() issues one guarded read per hop,
        // screens each result against that hop's validity floor, and reports the failing hop index in Error::detail.
        const RuntimeOffsets &offsets = runtime_offsets();
        const std::array<std::ptrdiff_t, 3> actor_chain{offsets.ccryaction_actiongame.load().value,
                                                        offsets.cactiongame_local_actor.load().value, 0};
        const auto c_player = DMK::memory::walk(DMK::Address{s_cry_action}, actor_chain);
        if (!c_player || !DMK::memory::is_plausible_ptr(*c_player))
        {
            return false;
        }
        // Confirm this is really C_Player (its main vtable) before trusting the controller offset.
        const auto vt = DMK::memory::read<uintptr_t>(*c_player);
        if (!vt || !vtable_is(GameClass::Player, *vt))
        {
            return false;
        }
        // This offset decides where a WRITE lands, so demand a Confirmed heal rather than accepting the
        // retained nominal: on a genuinely drifted C_Player the store would land in whatever member now
        // occupies the slot. A miss simply skips the aim control for the frame (fail closed).
        const auto look_controller_offset = offsets.c_player_look_controller.authorized();
        if (!look_controller_offset)
        {
            return false;
        }
        const auto controller = DMK::memory::read<uintptr_t>(c_player->offset(*look_controller_offset));
        if (!controller || !DMK::memory::is_plausible_ptr(DMK::Address{*controller}))
        {
            return false;
        }
        // Ease the SCALAR pitch toward pitch_target. Both synchronized copies are written so any internal
        // current/target smoothing also settles there. A sane pitch is within about +/- 1.6 rad; a
        // wild or non-finite value means the layout drifted, so that write is skipped.
        //
        // Every store below goes through memory::write_in_place, the library's per-frame data write: a guarded
        // copy that changes NO page protection and fails closed with WriteFaulted when the target is not
        // already writable. That is the right primitive for a value written every frame through a RESOLVED
        // address (a scanned base plus a pointer chain that can go stale between frames), and it is why this
        // function needs no SEH wrapper of its own: a stale chain reports a fault instead of raising one, and
        // a chain that drifted onto a read-only page is rejected rather than silently unprotected. It is also
        // an out-of-line library call, so the compiler cannot elide the consume-once store the engine reads on
        // its own thread.
        bool wrote = true;
        if (pitch_ease > 0.0f)
        {
            const DMK::Address pitch_addr = DMK::Address{*controller + Constants::LOOK_CONTROLLER_PITCH_OFFSET};
            const auto pitch_value = DMK::memory::read<float>(pitch_addr);
            if (pitch_value && *pitch_value > -3.2f && *pitch_value < 3.2f && std::isfinite(pitch_target))
            {
                const float eased = *pitch_value + (pitch_target - *pitch_value) * pitch_ease;
                wrote = DMK::memory::write_in_place<float>(pitch_addr, eased) &&
                        DMK::memory::write_in_place<float>(
                            DMK::Address{*controller + Constants::LOOK_CONTROLLER_PITCH2_OFFSET}, eased);
            }
            else
            {
                wrote = false;
            }
        }

        // Set the look YAW (heading) to face the camera direction. The character moves along this heading,
        // so this is what turns the body and makes movement camera-relative on the idle -> moving edge.
        // Both synchronized copies are written. yaw_value is finite-checked first: a non-finite heading (a
        // layout drift feeding NaN through the derivation) must never be written into the engine's look state.
        if (set_yaw)
        {
            const bool yaw_wrote = std::isfinite(yaw_value) &&
                                   DMK::memory::write_in_place<float>(
                                       DMK::Address{*controller + Constants::LOOK_CONTROLLER_YAW_OFFSET}, yaw_value) &&
                                   DMK::memory::write_in_place<float>(
                                       DMK::Address{*controller + Constants::LOOK_CONTROLLER_YAW2_OFFSET}, yaw_value);
            wrote = wrote && yaw_wrote;
        }
        return wrote;
    }

    /**
     * @brief Forces the player BODY to face a world yaw, making movement+facing camera-relative.
     * @details The look controller (apply_orbit_aim_control) is aim-only: writing its yaw steers the
     *          movement frame but does not turn the body. The body heading is owned by the engine's
     *          animated-character layer. This replicates CAnimatedCharacter::ForceOverrideRotation
     *          (main vtable slot 95): set the override-active byte and store a yaw-only world quat
     *          (XYZW, rotation about +Z); the animated-character update copies it into the entity
     *          rotation for the frame, replacing the animation-derived facing, then clears the active
     *          byte. It is consume-once, so this is called every frame while the heading is held.
     *
     *          Resolution mirrors apply_orbit_aim_control: g_env -> p_game -> CCryAction (cached) ->
     *          p_action_game -> C_Player (validated vtable) -> C_AnimatedHuman -> CAnimatedCharacter,
     *          validated by the animchar vtable before any write. The quat is written before the active
     *          byte so the game never observes active==1 with a torn/stale quat.
     *          See constants.hpp (ANIMCHAR_*, C_PLAYER_ANIMATED_HUMAN_OFFSET) for the offsets.
     * @return The player's animated character the turn was written to, or 0 when nothing was written.
     */
    static uintptr_t apply_orbit_body_turn(float target_yaw)
    {
        const ModuleInfo &mod = module_info();
        const uintptr_t c_player = resolve_c_player();
        if (mod.base == 0 || c_player == 0)
        {
            return 0;
        }

        // C_Player -> C_AnimatedHuman (+0x268) -> CAnimatedCharacter (+0x20). Both offsets decide where a
        // WRITE lands, so each must be a Confirmed heal rather than a retained nominal; on a drifted layout
        // the quat would otherwise be stored over whatever member now occupies the slot. A miss skips the
        // body turn for the frame (fail closed).
        const RuntimeOffsets &offsets = runtime_offsets();
        const auto animated_human_offset = offsets.c_player_animated_human.authorized();
        const auto animchar_offset = offsets.animated_human_animchar.authorized();
        if (!animated_human_offset || !animchar_offset)
        {
            return 0;
        }

        // One guarded walk instead of a read-and-screen per hop: walk() reads each hop under the same fault
        // guard, screens it against that hop's validity floor, and reports the failing hop index on a miss.
        const std::array<std::ptrdiff_t, 3> animchar_chain{*animated_human_offset, *animchar_offset, 0};
        const auto anim_char = DMK::memory::walk(DMK::Address{c_player}, animchar_chain);
        if (!anim_char || !DMK::memory::is_plausible_ptr(*anim_char))
        {
            return 0;
        }
        // Validate the animchar vtable before touching its override fields; a mismatch means the layout
        // drifted, so skip this frame.
        const auto avt = DMK::memory::read<uintptr_t>(*anim_char);
        if (!avt || !vtable_is(GameClass::AnimatedCharacter, *avt))
        {
            return 0;
        }

        // Yaw-only world quat (XYZW): rotation about +Z by target_yaw == {0, 0, sin(y/2), cos(y/2)}. The four
        // components are contiguous and written as ONE guarded store, so the quat cannot straddle a protection
        // boundary mid-write and there is no per-component call. memory::write_in_place is the per-frame data
        // write: guarded, no protection change, and it fails closed instead of faulting the host when the
        // resolved chain has gone stale - which is why this function needs no SEH wrapper of its own.
        const float half = target_yaw * 0.5f;
        const std::array<float, 4> quat{0.0f, 0.0f, std::sin(half), std::cos(half)};
        static_assert(Constants::QUAT_X_OFFSET == 0 && Constants::QUAT_Y_OFFSET == 4 &&
                          Constants::QUAT_Z_OFFSET == 8 && Constants::QUAT_W_OFFSET == 12,
                      "The quat components must be contiguous XYZW floats for the single-store write.");
        if (!DMK::memory::write_in_place<std::array<float, 4>>(
                anim_char->offset(Constants::ANIMCHAR_OVERRIDE_ROT_QUAT_OFFSET), quat))
        {
            return 0;
        }
        // Set the active byte LAST so the animated-character update (a separate consumer) never reads
        // active==1 with a half-written quat. The release fence orders the quat store before the active
        // store explicitly rather than relying on the target's store-store ordering; on x86 it lowers to a
        // compiler barrier with no runtime cost.
        std::atomic_thread_fence(std::memory_order_release);
        if (!DMK::memory::write_in_place<unsigned char>(
                anim_char->offset(Constants::ANIMCHAR_OVERRIDE_ROT_ACTIVE_OFFSET), 1))
        {
            return 0;
        }
        return anim_char->raw();
    }

    namespace
    {
        /**
         * @brief The rig's port onto the live game view (see core/rig/rig_stage.hpp).
         * @details Made per build for the game view's CView, the frustum the build found and the camera writes it
         *          collects (camera/commit.hpp). Main thread only: the frustum detour, whose SEH frame contains a fault
         *          in the engine reads and writes below.
         */
        class LiveRigPort final : public RigPort
        {
        public:
            LiveRigPort(uintptr_t cview, const CameraFrustum &frustum, ViewWrite &write) noexcept
                : m_cview(cview), m_frustum(frustum), m_write(write)
            {
            }

            [[nodiscard]] ZoomHeld zoom_held() override
            {
                // The holds are queried through the BindingTokens the control plane publishes
                // (refresh_zoom_binding_tokens), the library's per-frame form of the query. Before the first publish
                // the name-based query stands in.
                const DMK::input::Input &input = DMK::input::Input::instance();
                const std::shared_ptr<const ZoomBindingTokens> zoom_tokens =
                    s_zoom_tokens.load(std::memory_order_acquire);
                return ZoomHeld{
                    .in = zoom_tokens ? input.is_active(zoom_tokens->zoom_in) : input.is_active(k_zoom_in_binding),
                    .out = zoom_tokens ? input.is_active(zoom_tokens->zoom_out) : input.is_active(k_zoom_out_binding),
                };
            }

            void ease_out_zoom(float offset) override { Presets::ease_out_zoom_offset(offset); }

            [[nodiscard]] std::optional<EyePose> eye_pose() override
            {
                // memory::read (memcpy semantics into a trivially-copyable Vector3 / Quaternion) screens the source
                // against the low-address floor and a wrap guard, swallows an access fault via its own SEH frame, and
                // avoids the type-pun UB of a placement reinterpret_cast read, so a stale/torn cview fails closed here
                // rather than depending solely on the outer detour SEH frame.
                const auto position =
                    DMK::memory::read<Vector3>(DMK::Address{m_cview + Constants::SVIEWPARAMS_POSITION_OFFSET});
                const auto rotation =
                    DMK::memory::read<Quaternion>(DMK::Address{m_cview + Constants::SVIEWPARAMS_ROTATION_OFFSET});
                if (!position || !rotation)
                {
                    return std::nullopt;
                }
                return EyePose{*position, *rotation};
            }

            [[nodiscard]] std::optional<LookReading> look(std::uintptr_t c_player) override
            {
                // Through the self-healed offset (seeded from the constant, recovered if the C_Player layout drifts).
                // Read-only here, so the retained nominal is acceptable: worst case on a drifted layout it reads
                // neighboring fields and the rig's sanity checks reject them. The WRITE path (apply_orbit_aim_control)
                // demands a Confirmed heal.
                const std::ptrdiff_t look_controller_offset = runtime_offsets().c_player_look_controller.load().value;
                const auto controller = DMK::memory::read<uintptr_t>(DMK::Address{c_player + look_controller_offset});
                if (!controller || !DMK::memory::is_plausible_ptr(DMK::Address{*controller}))
                {
                    return std::nullopt;
                }
                const auto quat =
                    DMK::memory::read<Quaternion>(DMK::Address{*controller + Constants::LOOK_CONTROLLER_QUAT_OFFSET});
                const auto pitch =
                    DMK::memory::read<float>(DMK::Address{*controller + Constants::LOOK_CONTROLLER_PITCH2_OFFSET});
                if (!quat || !pitch)
                {
                    return std::nullopt;
                }
                return LookReading{*quat, *pitch};
            }

            [[nodiscard]] float game_fov() override { return m_frustum.fov; }

            void override_fov(float fov, float ratio) override
            {
                m_write.fov = fov;
                m_write.fov_ratio = ratio;
                m_write.fov_override = true;
            }

            [[nodiscard]] BodyReading body(std::uintptr_t c_player) override
            {
                // The entity world matrix (Matrix34, translation at m[*][3]) is the body origin, which does not bob.
                BodyReading body{};
                const std::ptrdiff_t entity_offset = runtime_offsets().c_player_entity.load().value;
                const auto entity = DMK::memory::read<uintptr_t>(DMK::Address{c_player + entity_offset});
                if (!entity || !DMK::memory::is_plausible_ptr(DMK::Address{*entity}))
                {
                    return body;
                }
                body.entity = *entity;
                const auto world = DMK::memory::read<GameStructures::Matrix34f>(
                    DMK::Address{*entity}.offset(Constants::OFFSET_ENTITY_WORLD_MATRIX_MEMBER));
                if (world)
                {
                    body.origin = Vector3{world->m[0][3], world->m[1][3], world->m[2][3]};
                    // Forward is column 1; yaw in the engine convention forward = (-sin yaw, cos yaw).
                    body.yaw = std::atan2(-world->m[0][1], world->m[1][1]);
                    body.valid = true;
                }
                return body;
            }

            [[nodiscard]] bool native_turn() override { return native_turn_engaged(); }

            [[nodiscard]] std::optional<float> move_input() override
            {
                if (!player_onaction_available())
                {
                    return std::nullopt;
                }
                return player_onaction_move_magnitude();
            }

            [[nodiscard]] std::optional<float> game_move(std::uintptr_t c_player) override
            {
                return player_move_input(c_player);
            }

            [[nodiscard]] bool cursor_shown() override { return s_cursor_shown.load(std::memory_order_relaxed); }

            [[nodiscard]] std::uint32_t game_state() override
            {
                return game_state_mask().load(std::memory_order_relaxed);
            }

            [[nodiscard]] bool aim_control(float pitch_ease, float pitch_target, bool set_yaw, float yaw) override
            {
                return apply_orbit_aim_control(pitch_ease, pitch_target, set_yaw, yaw);
            }

            void body_turn(float yaw) override { (void)apply_orbit_body_turn(yaw); }

            void publish_move_press(float heading, float body_yaw) override
            {
                s_move_press_heading.store(heading, std::memory_order_relaxed);
                s_move_press_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
                s_move_press_body_yaw.store(body_yaw, std::memory_order_relaxed);
                s_move_press_stamp_ms.store(GetTickCount64(), std::memory_order_release);
            }

            void withdraw_move_press() override { s_move_press_stamp_ms.store(0, std::memory_order_release); }

            void event(RigEvent event, float a, float b, float c, std::uint32_t mask) override
            {
                switch (event)
                {
                case RigEvent::HandoffStart:
                    (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                             "Orbit: free-look turned off by a situation; the look takes the camera "
                                             "direction, turning {:.1f} deg, pitch {:.1f} to {:.1f} deg",
                                             a, b, c);
                    return;
                case RigEvent::HandoffTurned:
                    (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                             "Orbit: the look took the camera direction after {:.2f} s", a);
                    return;
                case RigEvent::HandoffLost:
                    (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                             "Orbit: the hand-off ended without turning the look (the game sets the "
                                             "facing here, or the player changed)");
                    return;
                case RigEvent::HandoffMoved:
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Debug,
                        "Orbit: the hand-off ended without turning the look (the player was moved)");
                    return;
                case RigEvent::HandoffTimeout:
                    (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                             "Orbit: the look did not take the camera direction ({:.1f} deg left after "
                                             "{:.2f} s); the camera eases back",
                                             a, b);
                    return;
                case RigEvent::HandoffWriteFailed:
                    (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                             "Orbit: the look write failed; the camera eases back");
                    return;
                case RigEvent::MoveArmedByGame:
                    (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                             "Orbit: move held as orbit engaged (magnitude {:.2f}, the game's own move "
                                             "input agrees); armed",
                                             a);
                    return;
                case RigEvent::MoveStart:
                    // Diagnostic: what tripped the body-turn. A magnitude near 1.0 with a movement key/stick is genuine
                    // locomotion; a smaller or unexpected value while the player is only free-looking points at
                    // game-driven movement (finishing-move / combat footwork) being mistaken for intent. The mask is
                    // the debounced GameState mask (see game_state.hpp), which tells whether a combat / aiming /
                    // cinematic state was active when the body-turn engaged.
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Trace,
                        "Orbit: move-orbit START (body-turn engaging) - move_magnitude={:.2f}, continuous_align={}, "
                        "state_mask=0x{:X}",
                        a, b != 0.0f, mask);
                    return;
                case RigEvent::MoveStaleSuppressed:
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Trace,
                        "Orbit: suppressed a stale move re-trip (magnitude {:.2f}, no release observed since orbit "
                        "engaged; likely a movement latch stranded by a combat action-map swap)",
                        a);
                    return;
                case RigEvent::MoveStop:
                    (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                             "Orbit: move-orbit STOP (body-turn releasing) - move_magnitude={:.2f}", a);
                    return;
                case RigEvent::MoveStopGameIdle:
                    (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                             "Orbit: move-orbit STOP (the game's own move input is idle; magnitude "
                                             "{:.2f} is a stranded latch)",
                                             a);
                    return;
                case RigEvent::BodyTurnEngaged:
                    (void)DMK::log().log_noexcept(DMK::LogLevel::Trace,
                                                  "Camera: orbit body-turn ENGAGED (moving; heading locked to camera)");
                    return;
                case RigEvent::BodyTurnReleased:
                    (void)DMK::log().log_noexcept(DMK::LogLevel::Trace, "Camera: orbit body-turn released");
                    return;
                }
            }

        private:
            uintptr_t m_cview;
            const CameraFrustum &m_frustum;
            ViewWrite &m_write;
        };

        /** @brief The settings the rig reads, taken once per build: the live preset's and two INI options. */
        [[nodiscard]] RigParams rig_params(const Presets::CameraPreset &preset, const LiveSettings &cfg) noexcept
        {
            constexpr auto rel = std::memory_order_relaxed;
            return RigParams{
                .follow_distance = preset.follow_distance,
                .follow_distance_min = preset.follow_distance_min,
                .follow_distance_max = preset.follow_distance_max,
                .zoom_step = preset.zoom_step,
                .offset_up = preset.offset_up,
                .eye_height = preset.eye_height,
                .offset_right = preset.offset_right,
                .follow_yaw = preset.follow_yaw,
                .follow_pitch = preset.follow_pitch,
                .arm_follow_up = preset.arm_follow_up,
                .lens_drop_up = preset.lens_drop_up,
                .fov = preset.fov,
                .preset_blend_in_seconds = preset.blend_in_seconds,
                .aim_basis_smoothing = cfg.aim_basis_smoothing.load(rel),
                .gamepad_orbit_speed_x = preset.gamepad_orbit_speed_x,
                .gamepad_orbit_speed_y = preset.gamepad_orbit_speed_y,
                .gamepad_orbit_curve = preset.gamepad_orbit_curve,
                .gamepad_orbit_smoothing = preset.gamepad_orbit_smoothing,
                .orbit_pitch_min = preset.orbit_pitch_min,
                .orbit_pitch_max = preset.orbit_pitch_max,
                .orbit_return_speed = preset.orbit_return_speed,
                .orbit_follow_speed = preset.orbit_follow_speed,
                .orbit_smoothing = preset.orbit_smoothing,
                .dynamic_eye_sync = preset.dynamic_eye_sync,
                .stable_aim_basis = cfg.stable_aim_basis.load(rel),
                .orbit_level_aim = preset.orbit_level_aim,
                .orbit_body_turn = preset.orbit_body_turn,
                .orbit_continuous_align = preset.orbit_continuous_align,
            };
        }
    } // namespace

    /**
     * @brief Hides the head like first person while the switch has the lens within reach of it (see
     *        set_head_transition()).
     * @param switching The first-person <-> third-person switch is under way (0 < view_blend < 1).
     * @param lens_to_eye The rendered lens's distance from the first-person eye (meters).
     */
    static void update_head_transition(bool switching, float lens_to_eye) noexcept
    {
        if (!switching)
        {
            set_head_transition(false);
            return;
        }
        set_head_transition(lens_to_eye < k_switch_head_hide_on ||
                            (head_transition() && lens_to_eye < k_switch_head_hide_off));
    }

    /**
     * @brief Offsets the game view camera matrix behind the player and advances zoom/smoothing.
     * @details Builds the rig from the untouched CView pose (core/rig/rig_stage.hpp), runs the shoulder and arm
     *          collision, the close-up fade and the see-through on it, then writes the camera matrix translation and
     *          (when the stable basis, follow angle or orbit shapes the look) basis. The caller is about to compute the
     *          cull planes from this matrix, so culling matches the rendered view.
     * @param camera The game view's embedded render camera (matrix at offset 0).
     * @param cview The CView embedding the camera (camera - SVIEWPARAMS_VIEWMATRIX_OFFSET).
     * @param c_player Live C_Player address for the body anchor, or 0 to fall back to the eye anchor.
     * @param step This build's step (shared with the state poll).
     * @param view_blend Smoothstepped first-person(0) -> third-person(1) blend for the view-switch ease.
     */
    static void offset_game_view_camera(uintptr_t camera, uintptr_t cview, uintptr_t c_player, const FrameStep &step,
                                        float view_blend)
    {
        const float delta_time = step.seconds;
        LiveSettings &cfg = settings();
        CameraState &cam = camera_state();

        // What this build found in the camera and what it will write there (camera/commit.hpp). The basis starts as
        // the engine's eye basis, which stays unless the rig or the switch rebuilds it.
        const CameraFrustum frustum = read_camera_frustum(camera);
        ViewWrite write{};
        write.right = frustum.right;
        write.forward = frustum.forward;
        write.up = frustum.up;
        LiveRigPort port{cview, frustum, write};
        const RigParams params = rig_params(Presets::live_params(), cfg);
        const RigInput rig_input{
            .c_player = c_player,
            .view_blend = view_blend,
            .seconds = delta_time,
            .repeat_build = step.repeat_build,
            .paused = step.paused,
        };
        RigFrame rig{};
#ifdef TPVCAMERA_DEV_BUILD
        const bool built = FrameRecorder::recording() ? FrameRecorder::rig(cam, params, rig_input, port, rig)
                                                      : run_rig(cam, params, rig_input, port, rig);
#else
        const bool built = run_rig(cam, params, rig_input, port, rig);
#endif
        if (!built)
        {
            return; // the CView pose did not read; leave the view untouched this frame
        }
        const Vector3 world_up{0.0f, 0.0f, 1.0f};

        // The first-person <-> third-person switch blends the eye and the rig around the pivot before collision, so
        // the collision below runs once on the camera this build renders (see core/view/view_blend.hpp). At
        // view_blend 1 the rig passes through unchanged. The eye is the untouched CView pose, not the matrix this
        // function overwrites, so the frame's second build blends from the same eye.
        const ViewBlendOutput view = blend_view(s_view_latch, ViewBlendInput{
                                                                  .eye_position = rig.eye_position,
                                                                  .eye_forward = rig.eye_forward,
                                                                  .eye_up = rig.eye_up,
                                                                  .pivot = rig.pivot,
                                                                  .camera = rig.camera_position,
                                                                  .look_forward = rig.look_forward,
                                                                  .look_up = rig.look_up,
                                                                  .s = view_blend,
                                                                  .rebuild_basis = rig.rebuild_basis,
                                                                  .repeat_build = step.repeat_build,
                                                              });
        const bool switching = view_blend < 1.0f;
        Vector3 camera_position = view.camera;

        // Camera collision and the occluder fade (see camera/collision.hpp).
        const bool fade_on = cfg.use_occluder_fade.load(std::memory_order_relaxed) && occluder_fade_available();
        const bool close_fade = cfg.close_up_fade.load(std::memory_order_relaxed) && character_fade_available();
        if (!step.repeat_build)
        {
            // One attachment walk serves the occluder scan's exclusion of what the character carries and the close-up
            // fade. Bone-bound items need not have an IEntity parent, so the actor registry alone cannot identify them.
            if (rig.entity != 0 && (close_fade || fade_on || Presets::live_collision_enabled()))
            {
                (void)collect_character_nodes(rig.entity, s_character_nodes);
            }
            else if (s_character_nodes.entity != 0)
            {
                clear_character_nodes(s_character_nodes);
            }
            set_occluder_player_nodes(std::span<const std::uintptr_t>(
                s_character_nodes.nodes.data(), static_cast<std::size_t>(s_character_nodes.count + 1)));
        }

        // Shoulder collision: a wall at the shoulder narrows the shoulder offset first, so the arm below runs from the
        // pivot to a camera nearer the look line and keeps its distance. The offset lies along the blended rig's right
        // (the follow angle and the orbit turn it with the look), toward the side its sign puts it, and grows with the
        // switch like the rest of the arm.
        {
            const Vector3 rig_right = view.forward.cross(view.up);
            const float lateral = std::fabs(rig.offset_right) * view_blend;
            if (rig_right.magnitude_squared() > 1e-6f && lateral > 0.0f)
            {
                const Vector3 lateral_dir = rig_right.normalized() * (rig.offset_right < 0.0f ? -1.0f : 1.0f);
                const ShoulderInput shoulder{
                    .pivot = view.pivot,
                    .direction = lateral_dir,
                    .lateral = lateral,
                    .delta_time = delta_time,
                    .enabled = Presets::live_params().shoulder_collision && Presets::live_collision_enabled(),
                    .fade_on = fade_on,
                };
                const float allowed = resolve_shoulder_collision(s_collision, shoulder);
                if (allowed < lateral)
                {
                    camera_position = camera_position - lateral_dir * (lateral - allowed);
                }
            }
        }
        const bool wall_class_changed = !step.repeat_build && advance_fence_classification();
        const CollisionInput collision_input{
            .pivot = view.pivot,
            .desired = camera_position,
            .world_up = world_up,
            .head = rig.anchor_base,
            .hip = rig.body_valid ? rig.anchor_origin + world_up * k_fade_hip_height
                                  : rig.anchor_base - world_up * k_fade_hip_drop,
            .delta_time = delta_time,
            .fade_on = fade_on,
            .wall_class_changed = wall_class_changed,
            .repeat_build = step.repeat_build,
        };
        camera_position = resolve_camera_collision(s_collision, collision_input);

        // Hand the fade set to the dissolve hook once per frame (the frame's second build changes nothing).
        if (!step.repeat_build)
        {
            submit_fade_set(s_collision, delta_time);
        }

        // The arm after collision, measured from the head where it really is: the pivot plus the turn hold, which
        // keeps the pivot off the body while it steps through a turn (both blended in from the eye by the switch). The
        // close-up fade and the lens near plane keep their distance from the head by it.
        const float arm_after_collision = (camera_position - (view.pivot + rig.turn_hold * view_blend)).magnitude();

        // The trees, bushes and grass, which never fade whole for hiding the character, open a window around him
        // instead (see run_tree_see_through), from the camera the fade's sightlines were cast from and the scan the
        // collision just refreshed. Shut in dialogue and while a menu or an overlay is up.
        if (!step.repeat_build)
        {
            const bool ui_up =
                is_game_menu_open() || overlay_state().active.load(std::memory_order_relaxed) ||
                (game_state_mask().load(std::memory_order_relaxed) & state_bit(GameState::Dialogue)) != 0u;
            run_tree_see_through(camera_position, rig.eye_position, rig.body_origin, rig.body_valid,
                                 cam.turn_track_step, !ui_up, view_blend, delta_time, s_character_nodes);
        }

        // Too close to the character: the close-up fade, or the first-person fallback (see camera/close_fade.hpp).
        const CloseUpInput close_up{
            .camera = camera_position,
            .eye = rig.eye_position,
            .arm = arm_after_collision,
            .entity = rig.entity,
            .delta_time = delta_time,
            .close_fade = close_fade,
            .switching = switching,
            .repeat_build = step.repeat_build,
        };
        camera_position = apply_close_up(cam, s_character_nodes, close_up);

        // While the switch runs, the head is hidden like in first person once the lens comes within reach of it, from
        // the frame the switch leaves the eye (the detour seeds the hide) until the lens is clear of the head, or the
        // switch back reaches the eye.
        if (!step.repeat_build)
        {
            const float lens_to_eye = (camera_position - rig.eye_position).magnitude();
            update_head_transition(switching, lens_to_eye);
            if (switching)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                         "Camera: view switch s={:.3f}, yaw {:.1f} deg, arm {:.2f} -> {:.2f} m, lens "
                                         "{:.2f} m from the eye, head {}",
                                         view_blend, radians_to_degrees(s_view_latch.yaw_accumulated),
                                         (view.camera - view.pivot).magnitude(), arm_after_collision, lens_to_eye,
                                         head_transition() ? "hidden" : "shown");
            }
        }

        // The camera goes into the matrix translation column. The caller computes the cull planes from this matrix
        // next, so culling follows the rendered view.
        const Vector3 final_position = camera_position;
        write.position = final_position;

        // Orientation: rebuilt when the rig shaped the look (the stable or smoothed basis, the follow angle or the
        // orbit), as the column basis right = forward x up, up = right x forward (col0 = right, col1 = forward,
        // col2 = up), through the switch's turn while it runs. Otherwise the engine-built eye basis stays, so the
        // camera sits behind the player looking where they look. The rig's up turned with its look, so an orbit away
        // from a pitched look does not roll the view. Screen-centre forward (the crosshair direction) is the rebuilt
        // forward, else the engine eye forward; it is published below for the camera-space interaction hook.
        Vector3 screen_forward = rig.forward;
        if (view.rebuild)
        {
            const Vector3 forward =
                (view.forward.magnitude_squared() > 1e-6f) ? view.forward.normalized() : rig.look_forward;
            screen_forward = forward;
            const Vector3 right_axis = forward.cross(view.up);
            if (right_axis.magnitude_squared() > 1e-6f)
            {
                const Vector3 new_right = right_axis.normalized();
                write.right = new_right;
                write.forward = forward;
                write.up = new_right.cross(forward);
                write.rebuild_basis = true;
            }
        }

        // The lens near plane (see camera/lens.hpp), measured from the final pose decided above; the near corner then
        // scales by it as a whole.
        if (const std::optional<float> near_plane =
                plan_live_lens_near(write, committed_near_corner(frustum, write), arm_after_collision, view_blend,
                                    delta_time, step.repeat_build))
        {
            write.near_scale = *near_plane / frustum.near_corner[1];
            write.near_override = true;
        }

        // People and animals at the lens fade instead of being cut open by the near plane (see
        // camera/npc_lens_fade.hpp), once per frame from the final pose. The near corner's ratios are the frustum's
        // half-angle tangents, whatever scale the near plane gave it.
        if (!step.repeat_build)
        {
            const std::array<float, 3> near_corner = committed_near_corner(frustum, write);
            NpcLensFadeInput npc{};
            npc.view.position = {write.position.x, write.position.y, write.position.z};
            npc.view.right = {write.right.x, write.right.y, write.right.z};
            npc.view.forward = {write.forward.x, write.forward.y, write.forward.z};
            npc.view.up = {write.up.x, write.up.y, write.up.z};
            npc.view.tan_half_width = near_corner[1] > 1e-6f ? std::fabs(near_corner[0] / near_corner[1]) : 0.0f;
            npc.view.tan_half_height = near_corner[1] > 1e-6f ? std::fabs(near_corner[2] / near_corner[1]) : 0.0f;
            npc.player_entity = rig.entity;
            npc.player_origin = rig.body_valid ? rig.body_origin : rig.eye_position;
            npc.real_seconds = step.real_seconds;
            npc.mounted = (game_state_mask().load(std::memory_order_relaxed) & state_bit(GameState::Mount)) != 0u;
            npc.enabled = cfg.npc_lens_fade.load(std::memory_order_relaxed) && character_fade_available() &&
                          view_blend > 0.0f && npc.view.tan_half_width > 0.0f;
            update_npc_lens_fade(npc);
        }

        // The build's one write into the camera: the matrix, the FOV and the cull edges, the near corner.
        commit_view(camera, write);

        // Publish the rendered camera pose + crosshair direction for the camera-space interaction hook
        // (interaction_hook.cpp). The player use-cone is eye-anchored and never reads the render camera, so
        // in third person the screen-centre crosshair and the use-target diverge by the shoulder offset
        // (worst at close range - you cannot loot/use what the crosshair appears to be on). The hook
        // re-origins the cone onto this pose when InteractFromCamera is set. valid is true only here (while
        // the offset is engaged); the suppression path below clears it, so first person leaves the game alone.
        interaction_aim_pose().store(final_position.x, final_position.y, final_position.z, screen_forward.x,
                                     screen_forward.y, screen_forward.z);
        if (!step.repeat_build)
        {
            QueryLedger::note_view({
                .position = {final_position.x, final_position.y, final_position.z},
                .forward = {screen_forward.x, screen_forward.y, screen_forward.z},
            });
        }
    }

    /**
     * @brief Applies the forced-view policy on STATE-CHANGE EDGES, not every frame.
     * @details A continuous per-frame override would fight the player: they could never toggle the view
     *          while a forced state was active, because the next frame would re-force it. Instead this
     *          forces the view ONCE when a forced state begins, lets the player toggle freely while it
     *          lasts, and restores the pre-force view when the state ends - unless the player changed
     *          the view themselves meanwhile, in which case their choice stands. Forced-FPV wins over
     *          forced-TPV on overlap. Runs on the main thread only (the detour), so the latch state is
     *          plain file-scope static; cam.applying is atomic because the hotkeys write it too.
     * @param cam Camera state whose applying flag (the live view) is forced and restored.
     * @param state Current debounced GameState mask.
     * @param policy The situations' rows: which force first or third person on entry (first person wins).
     */
    static void apply_forced_view_policy(CameraState &cam, uint32_t state, const StatePolicyTable &policy)
    {
        // The view the policy wants this frame: -1 none, 0 force FPV, 1 force TPV (forced-FPV wins).
        const ViewForce force = policy.view(state);
        const int forced_view = force == ViewForce::FirstPerson ? 0 : force == ViewForce::ThirdPerson ? 1 : -1;

        // s_prev_forced_view detects the edge; s_saved_applying is the view to restore on exit;
        // s_policy_owns is cleared once the player toggles the view while a forced state is active, so
        // the policy then stops re-forcing and skips the restore on exit (their manual choice stands).
        static int s_prev_forced_view = -1;
        static bool s_saved_applying = false;
        static bool s_policy_owns = false;

        if (forced_view != s_prev_forced_view)
        {
            const bool view_before = cam.applying.load(std::memory_order_relaxed);
            Presets::SwitchEvent event{
                .kind = Presets::SwitchKind::Policy,
                .mask = state & policy.forcing(force),
            };
            Presets::copy_switch_text(event.from, view_before ? "third person" : "first person");
            if (forced_view == -1)
            {
                // Left every forced state: restore the pre-force view, but only if the player did not
                // switch the view themselves during the state (otherwise their choice stands).
                Presets::copy_switch_text(event.reason, s_policy_owns ? "forced view restore" : "forced view release");
                if (s_policy_owns)
                {
                    cam.applying.store(s_saved_applying, std::memory_order_relaxed);
                    s_policy_owns = false;
                }
            }
            else
            {
                // Entered a forced state (or switched directly from one forced view to another). Save
                // the pre-force view on the first entry from an unforced state so it can be restored on
                // exit, then apply the forced view exactly once.
                if (s_prev_forced_view == -1)
                {
                    s_saved_applying = cam.applying.load(std::memory_order_relaxed);
                }
                cam.applying.store(forced_view == 1, std::memory_order_relaxed);
                s_policy_owns = true;
                Presets::copy_switch_text(event.reason, forced_view == 1 ? "forced tpv" : "forced fpv");
            }
            Presets::copy_switch_text(event.to,
                                      cam.applying.load(std::memory_order_relaxed) ? "third person" : "first person");
            Presets::record_switch(event);
            s_prev_forced_view = forced_view;
        }
        else if (forced_view != -1 && s_policy_owns &&
                 cam.applying.load(std::memory_order_relaxed) != (forced_view == 1))
        {
            // Same forced state as last frame, but the player toggled the view away from the forced
            // value: release ownership so the policy stops forcing and does not restore on exit.
            s_policy_owns = false;
        }
    }

    /**
     * @brief Suspends free-look on entering an OrbitExcludeState and RESTORES it on exit.
     * @details Edge-triggered, like the forced-view policy: free-look is turned off once when a listed
     *          state begins and re-enabled when it ends, so a state that interrupts free-look (a
     *          dialogue, a minigame) does not permanently cancel it. It only re-enables if the policy
     *          was the one that turned it off (free-look was on at entry). Main-thread only.
     *
     *          orbit_active is also written by the input thread (the toggle and OrbitHoldKey). The policy turns it off
     *          and on only by compare-exchange, and an OrbitHoldKey release posts orbit_hold_released before it clears
     *          orbit_active. The exit branch restores free-look, then consumes the flag and turns free-look back off
     *          when a release was posted, so every interleaving with a release ends with free-look off. These steps
     *          stay sequentially consistent because the protocol spans two variables.
     * @param cam Camera state whose orbit_active flag is suspended and restored.
     * @param state Current GameState mask: debounced, except Menu and Overlay, which come from the live UI signals.
     * @param orbit_exclude_mask States in which free-look is disabled.
     * @return True on the frame it turns free-look off.
     */
    [[nodiscard]] static bool apply_orbit_exclude_policy(CameraState &cam, uint32_t state, uint32_t orbit_exclude_mask)
    {
        const bool excluded = (state & orbit_exclude_mask) != 0;
        static bool s_prev_excluded = false;

        if (excluded == s_prev_excluded)
        {
            // Still in the same state. If the policy suspended free-look on entry but the player has since
            // turned it back on by hand, release the suspension so the exit branch does not later re-assert
            // it against a manual choice (symmetric to apply_forced_view_policy's ownership release).
            if (excluded && cam.orbit_suspended.load(std::memory_order_relaxed) &&
                cam.orbit_active.load(std::memory_order_relaxed))
            {
                cam.orbit_suspended.store(false, std::memory_order_relaxed);
            }
            return false;
        }
        bool suspended_now = false;
        Presets::SwitchEvent event{
            .kind = Presets::SwitchKind::Policy,
            .mask = state & orbit_exclude_mask,
        };
        Presets::copy_switch_text(event.from,
                                  cam.orbit_active.load(std::memory_order_relaxed) ? "free-look on" : "free-look off");
        Presets::copy_switch_text(event.reason, excluded ? "orbit exclude" : "orbit exclude end");
        if (excluded)
        {
            // Entering an excluded state (combat, dialogue, minigame) swaps the action map, which can swallow a
            // held-move release and strand the movement-input latch > 0. Drop it now so when free-look restores on
            // exit it cannot re-trip the body-turn with the keys released (the post-combat self-rotation). Logged
            // with the cleared magnitude so the trace shows whether the latch WAS actually stranded.
            const float stranded = player_onaction_reset();
            (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                     "Orbit: exclude-state entered; cleared move-latch (had magnitude {:.2f}{})",
                                     stranded, stranded > k_move_input_stop ? ", WAS STRANDED" : "");
            // If free-look was on, turn it off and remember to restore it.
            bool on = true;
            const bool suspend = cam.orbit_active.compare_exchange_strong(on, false);
            cam.orbit_suspended.store(suspend, std::memory_order_relaxed);
            suspended_now = suspend;
        }
        else if (cam.orbit_suspended.load(std::memory_order_relaxed))
        {
            // Leaving the excluded state: re-enable free-look. Like a toggle-on, it takes over from the pose on
            // screen with the orbit angles it has, so the camera does not move. An OrbitHoldKey let go of meanwhile
            // keeps it off.
            cam.orbit_suspended.store(false, std::memory_order_relaxed);
            bool off = false;
            const bool restored = cam.orbit_active.compare_exchange_strong(off, true);
            if (cam.orbit_hold_released.exchange(false) && restored)
            {
                cam.orbit_active.store(false);
            }
        }
        Presets::copy_switch_text(event.to,
                                  cam.orbit_active.load(std::memory_order_relaxed) ? "free-look on" : "free-look off");
        Presets::record_switch(event);
        s_prev_excluded = excluded;
        return suspended_now;
    }

    /**
     * @brief Runs the ranged step once per engine frame (the first of the frame's two builds).
     * @param cview The game view, whose pose holds the first-person eye the crosshair target ray starts beside.
     * @param c_player The live C_Player, or 0.
     * @param repeat_build True on the frame's second build, which skips the step.
     */
    static void run_ranged_frame(uintptr_t cview, uintptr_t c_player, bool repeat_build)
    {
        if (repeat_build)
        {
            return;
        }
        const auto eye = DMK::memory::read<Vector3>(DMK::Address{cview + Constants::SVIEWPARAMS_POSITION_OFFSET});
        ranged_frame(c_player, eye ? *eye : Vector3{}, eye.has_value());
    }

    /**
     * @brief Times the frustum detour around the original engine call without filling the profiler ring with
     *        non-game cameras.
     * @details Two records per identified game-view build separate all mod work from the original engine call; the
     *          camera.frustum_build scope of the body nests inside frustum_mod. Other cameras record only individual
     *          calls of at least 1 ms. Empty when profiling is off; no logger calls, allocations, or shared timing
     *          counters.
     */
    struct FrustumBuildProfile
    {
#ifdef DMK_ENABLE_PROFILING
        LARGE_INTEGER mod_started{};
        LARGE_INTEGER original_started{};
        bool game_view = false;
        bool repeat = false;
#endif

        void begin_mod() noexcept
        {
#ifdef DMK_ENABLE_PROFILING
            QueryPerformanceCounter(&mod_started);
#endif
        }

        void identify_game_view() noexcept
        {
#ifdef DMK_ENABLE_PROFILING
            game_view = true;
#endif
        }

        void mark_repeat([[maybe_unused]] bool repeat_build) noexcept
        {
#ifdef DMK_ENABLE_PROFILING
            repeat = repeat_build;
#endif
        }

        void begin_original() noexcept
        {
#ifdef DMK_ENABLE_PROFILING
            QueryPerformanceCounter(&original_started);
#endif
        }

        void finish() noexcept
        {
#ifdef DMK_ENABLE_PROFILING
            LARGE_INTEGER ended{};
            QueryPerformanceCounter(&ended);
            auto &profiler = DMK::Profiler::get_instance();
            const auto thread = GetCurrentThreadId();
            if (game_view)
            {
                HookProfile::record_game_frustum(mod_started.QuadPart, original_started.QuadPart, ended.QuadPart,
                                                 repeat);
                profiler.record(repeat ? "camera.frustum_mod_repeat" : "camera.frustum_mod", mod_started.QuadPart,
                                original_started.QuadPart, thread);
                profiler.record(repeat ? "camera.frustum_original_repeat" : "camera.frustum_original",
                                original_started.QuadPart, ended.QuadPart, thread);
                return;
            }
            // Rejected or unidentified cameras must not evict the ring with a sample on every frustum call. Keep only
            // individual calls taking at least 1 ms; DEV totals go into the separate bounded aggregate export.
            HookProfile::record_other_frustum(mod_started.QuadPart, original_started.QuadPart, ended.QuadPart);
            const auto slow_ticks = std::max<std::int64_t>(profiler.qpc_frequency() / 1000, 1);
            if (original_started.QuadPart - mod_started.QuadPart >= slow_ticks)
            {
                profiler.record("camera.other_frustum_mod_slow", mod_started.QuadPart, original_started.QuadPart,
                                thread);
            }
            if (ended.QuadPart - original_started.QuadPart >= slow_ticks)
            {
                profiler.record("camera.other_frustum_original_slow", original_started.QuadPart, ended.QuadPart,
                                thread);
            }
#endif
        }
    };

    /**
     * @brief Gate and matrix-offset body, outside the SEH wrapper so local C++ objects can unwind.
     * @param camera The camera handed to the frustum builder (matrix at offset 0).
     * @param profile Per-call diagnostic state, marked after identifying the game view.
     */
    static void detour_frustum_build_impl(uintptr_t camera, FrustumBuildProfile &profile)
    {
        CameraState &cam = camera_state();
        LiveSettings &cfg = settings();

        // The [StateBehavior] lists, one row per situation, read once for this build.
        const StatePolicyTable policy = state_policy();

        // The camera is embedded in its CView at SVIEWPARAMS_VIEWMATRIX_OFFSET, so the CView is that
        // far below the camera. Confirm it by checking the CView vtable: shadow/reflection/portal
        // cameras handed to the same builder are not embedded in a CView and fail this guard. Checked before the
        // fast path, so the about fifty other frustum builds of a frame pay only for this compare.
        if (!DMK::memory::is_plausible_ptr(DMK::Address{camera}))
        {
            return;
        }
        const uintptr_t cview = camera - Constants::SVIEWPARAMS_VIEWMATRIX_OFFSET;
        const auto vtable = DMK::memory::read<uintptr_t>(DMK::Address{cview});
        if (!vtable || !DMK::memory::is_plausible_ptr(DMK::Address{*vtable}))
        {
            return;
        }
        // Identify the CView by RTTI on the first hit and cache its vtable address, so the
        // steady-state gate is a single qword compare. Using the RTTI type name rather than a
        // hardcoded vtable address keeps this resilient across game patches.
        if (s_cview_vtable_runtime != 0)
        {
            if (*vtable != s_cview_vtable_runtime)
            {
                return;
            }
        }
        else if (vtable_is(GameClass::View, *vtable))
        {
            s_cview_vtable_runtime = *vtable;
        }
        else
        {
            return;
        }

        profile.identify_game_view();
        // Named timing scope for the camera body of the game view; the other frustum builds of a frame returned
        // above. The game view normally builds twice per frame. Compiles to nothing unless the build sets
        // DMK_ENABLE_PROFILING, and the collision/occlusion queries below carry their own nested scopes, so a
        // profiling build attributes a frame spike to a specific query instead of "the camera hook".
        DMK_PROFILE_SCOPE("camera.frustum_build");

        // Every game-view build takes its step, the idle ones included, so the wall-clock fallback always measures
        // from the previous build and the frame's second build is told apart on either path.
        const FrameStep step = frame_delta();
        const float delta_time = step.seconds;
        const bool repeat_build = step.repeat_build;
        profile.mark_repeat(repeat_build);
        // The frame's first build of the game view opens a new engine frame for the query ledger.
        if (!repeat_build)
        {
            QueryLedger::close_frame();
        }

        // Advance the self-heal scheduler once per frame, before anything below reads a healed offset. Its retry
        // interval counts ticks as frames, so neither the other frustum builds (dozens a frame) nor the frame's second
        // build tick it. The idle path needs it too: the bases a group waits on come up while the offset is still
        // disengaged. A resolved group latches, so only groups still waiting for their live bases can scan here.
        if (!repeat_build)
        {
            offset_heal_tick();
        }

        // Fast path: nothing can drive the offset, so there is nothing to do. When any forced state is
        // configured the policy must run every game-view frame to catch the state-change EDGES that
        // trigger a one-time forced switch, so only skip when the player is in manual first person with
        // no forced states configured and the head has already been restored (the head-restore branch
        // keeps running for one frame after a toggle-off, while head_was_active() still holds).
        // The native turn animation must also be off (it lets the body follow the look again on the frame the view
        // disengages).
        // A pending arrow keeps the full path too, so a first-person shot is still followed and drawn, and so does a
        // distraction stone in hand while the aim preview is on, so its landing is still shown.
        if (!cam.applying.load(std::memory_order_relaxed) && !policy.forces_any_view() && !head_was_active() &&
            cam.view_blend <= 1e-3f && !native_turn_engaged() &&
            !ranged_wants_frame(cfg.ranged_show_aim_preview.load(std::memory_order_relaxed) ? resolve_c_player() : 0))
        {
            // Even while idle (first person, no forced state), probe for the player until the world is
            // first seen so game_world_ready becomes true in-world REGARDLESS of the third-person view
            // being on - the overlay waits on it. resolve_c_player sets the flag on success and returns
            // 0 at the menu, so this costs a few guarded reads per frame only until load-in, then never.
            if (!game_world_ready().load(std::memory_order_relaxed))
            {
                resolve_c_player();
            }
            // Offset disengaged: the orbit cannot be capturing, and the cursor flag is only refreshed on
            // the game-view path below, so clear it here to keep it from latching true across the gap.
            s_cursor_shown.store(false, std::memory_order_relaxed);
            // The trees the tree see-through still holds swapped go back to the stock shader once its hold runs out,
            // so it keeps getting its frames while any is left. The twins a pause creates come in first person too.
            if (!repeat_build && shader_twins_available())
            {
                service_shader_twins_paused(twin_pause_signals(step, std::nullopt));
                if (shader_twin_stats().swapped > 0)
                {
                    stop_tree_see_through(delta_time);
                }
            }
            return;
        }

        // Game-view camera: resolve the player once here and reuse it and the frame step in the matrix offset
        // below. The game state is derived from the discrete engine signals and published for the input-dispatch
        // thread.
        const uintptr_t c_player = resolve_c_player();
        // Presets are always active, so the debounced game state is always needed here (to select the
        // active preset and, when state behaviour is on, to drive the forced-view / orbit-exclude policies).
        const uint32_t raw_state = poll_game_state(c_player) | poll_indoors(cview, repeat_build);
        const uint32_t state =
            debounce_game_state(raw_state, delta_time, cfg.state_exit_hold_seconds.load(std::memory_order_relaxed));
        // The frame the orbit-exclude policy turns free-look off raises the aim hand-off (see offset_game_view_camera),
        // unless the game sets the character's facing in that situation. The policy reads the menu and the overlays
        // live, as should_apply_view does, so free-look comes back on the frame third person does. The raw state
        // counts for the facing too, since such a situation can still be inside its hold delay while the policy
        // already reacts to a screen. A same-frame rebuild reads the same state, so it is raised once.
        const uint32_t policy_state = (state & ~k_live_ui_states) | (raw_state & k_live_ui_states);
        const bool facing_held = ((state | raw_state) & k_game_held_facing_states) != 0u;
        cam.aim_handoff_requested = false;
        if (policy.edge_rules())
        {
            // Both policies are edge-triggered (they act on state-change edges, not every frame) so the
            // player keeps manual control while a state lasts. The forced-view policy writes cam.applying
            // on entry and restores it on exit; the orbit-exclude policy suspends free-look on entry and
            // restores it on exit.
            apply_forced_view_policy(cam, state, policy);
            cam.aim_handoff_requested =
                apply_orbit_exclude_policy(cam, policy_state, policy.orbit_exclude()) && !facing_held;
        }
        else
        {
            // No policy restores free-look while state behavior is off, so nothing stays suspended.
            cam.orbit_suspended.store(false, std::memory_order_relaxed);
        }
        // One that shows up while a hand-off runs ends it after this frame's rig, and the camera eases back.
        if (facing_held && cam.aim_handoff_active)
        {
            cam.aim_handoff_cancel = true;
        }
        game_state_mask().store(state, std::memory_order_relaxed);
        // While the game stands still, the shader twins create the twins of the variants that came after the loading
        // screen: once per frame, before this frame's update of them.
        if (!repeat_build && shader_twins_available())
        {
            service_shader_twins_paused(twin_pause_signals(step, raw_state | state));
        }

        // Publish whether the game is showing the OS cursor (a UI is up) so the free-look input gate can
        // freeze the orbit while menus / loot / trade / dialogue are open. The hardware-mouse reference
        // counter (g_env -> p_hardware_mouse -> + count) reads > 0 whenever any UI requests the cursor.
        // Every link is screened (memory::walk + memory::read), and any failed link - or the feature
        // being disabled - publishes false, so a layout miss or a load degrades to normal orbit rather
        // than a stuck-frozen one. Read here on the main thread; the input thread only reads the
        // published flag (same producer/consumer split as game_state_mask, so no new cross-thread race).
        // Non-game-view frustum frames (shadow/reflection) skip this publish, but cannot strand the orbit
        // frozen: s_offset_active is co-published on this same game-view path and the input gate tests it
        // first, so a stale true here is moot whenever the offset is not actively rendering the view.
        bool cursor_shown = false;
        if (cfg.freeze_orbit_on_cursor.load(std::memory_order_relaxed) && s_genv_runtime != 0)
        {
            const std::array<std::ptrdiff_t, 2> cursor_chain{Constants::GENV_HARDWARE_MOUSE_OFFSET,
                                                             Constants::HARDWARE_MOUSE_CURSOR_COUNT_OFFSET};
            const auto count_addr = DMK::memory::walk(DMK::Address{s_genv_runtime}, cursor_chain);
            if (count_addr)
            {
                const auto count = DMK::memory::read<int32_t>(*count_addr);
                cursor_shown = count.has_value() && *count > 0;
            }
        }
        s_cursor_shown.store(cursor_shown, std::memory_order_relaxed);

        // Drive the player head from the offset state every frame (the engine only sets it on its own
        // transitions, so toggling the offset would otherwise leave the head stuck), then offset the
        // matrix while active. The offset follows cam.applying directly - both the manual toggle and
        // the edge-triggered policy write it - and should_apply_view() applies the SuppressTPVState hard
        // gate.
        // Ease the first-person <-> third-person blend toward the desired view so toggling (and UI
        // suppression) slides instead of snapping. ViewTransitionDuration 0 makes the switch instant. It runs on real
        // time: the inventory slows the game clock to 1/1000 as it opens, and a switch to first person paced on that
        // clock would leave the offset on the inventory's own view of the character.
        const bool want_tpv = cam.applying.load(std::memory_order_relaxed) && should_apply_view();
        // A situation that also leaves third person (an overlay under SuppressTPVState) does not hand the look off:
        // the camera eases to the eye, whatever ViewTransitionDuration is.
        if (!want_tpv)
        {
            cam.aim_handoff_requested = false;
        }
        const float view_dur = cfg.view_transition_duration.load(std::memory_order_relaxed);
        const float view_target = want_tpv ? 1.0f : 0.0f;
        cam.view_blend = ramp_weight(cam.view_blend, view_target, view_dur, step.real_seconds);

        // The offset is rendered while heading TO or holding third person, so the ease-OUT renders too.
        const bool offset_engaged = want_tpv || cam.view_blend > 1e-3f;
        s_offset_active.store(offset_engaged, std::memory_order_relaxed);
        // A switch into third person starts with the lens at the eye, so the head stays hidden from its first frame;
        // the offset clears the hide once the lens is clear of the head. The re-assert below applies it this frame.
        const bool switch_starts = offset_engaged && !head_was_active();
        if (switch_starts && view_dur > 1e-4f)
        {
            set_head_transition(true);
        }
        {
            static bool s_switch_logged = false;
            const bool switching = offset_engaged && cam.view_blend < 1.0f;
            if (switching != s_switch_logged)
            {
                s_switch_logged = switching;
                const char *const person = want_tpv ? "third" : "first";
                if (switching)
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Debug, "Camera: view switch toward {} person over {:.2f} s",
                                             person, view_dur);
                }
                else
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Debug, "Camera: view switch done ({} person)", person);
                }
            }
        }
        reassert_head_visibility(offset_engaged, c_player);
        // The body's native turn-in-place animation follows the offset too (it follows the look again once the view is
        // back at the eye), and is off while the view rides the first-person eye (the head fallback, or an Eye Height 0
        // preset anchored to it). No turn starts under free-look.
        const bool eye_anchored = head_fallback() || Presets::live_params().eye_height <= 0.0f;
        update_native_turn(c_player, offset_engaged, state, eye_anchored,
                           !cam.orbit_active.load(std::memory_order_relaxed), delta_time);

        // Edge tracker for the disengage cleanup below: true while the offset is rendering, so the cleanup runs
        // ONCE on the third-person -> first-person transition, not every first-person frame.
        static bool s_orbit_was_engaged = false;
        if (!offset_engaged)
        {
            if (s_orbit_was_engaged)
            {
                // Just left third person (toggled to FPV, or a state suppressed the offset). Drop the orbit
                // run-state so re-engaging is fresh: clear the move re-arm latch and force-clear any
                // movement-input latch the game may have stranded on an action-map swap. Edge-gated so a key
                // held in first person does not spam the reset/log every frame.
                cam.orbit_move_armed = false;
                cam.orbit_move_armed_by_game = false;
                const float stranded = player_onaction_reset();
                if (stranded > k_move_input_stop)
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Trace,
                        "Orbit: TPV disengaged; cleared a stranded move-latch (magnitude {:.2f})", stranded);
                }
                s_orbit_was_engaged = false;
            }
            reset_collision_carry(s_collision);
            reset_shoulder_carry(s_collision);
            cam.head_fallback = false;
            cam.head_fallback_blend = 0.0f;
            set_head_fallback(false);
            set_head_close_fade(false);
            set_head_transition(false);
            s_view_latch = ViewBlendLatch{};
            // Faded objects go solid at once (they are behind the first-person eye), and the next third-person frame
            // starts from an empty fade set instead of node addresses from before the gap.
            reset_occluder_fade();
            set_character_fade(0, 0.0f);
            reset_npc_lens_fade();
            reset_close_up();
            // The carried list goes too, and the next third-person walk logs its set afresh.
            if (s_character_nodes.entity != 0)
            {
                clear_character_nodes(s_character_nodes);
            }
            s_collision.fade_stale = true;
            // And the tree see-through window shuts; its trees go back to the stock shader after the module's hold.
            if (!repeat_build)
            {
                stop_tree_see_through(delta_time);
            }
            // First person (or suppressed): the camera is the eye, so the interaction hook must NOT redirect.
            interaction_aim_pose().invalidate();
            // Nor does the AI's camera observer need the eye substituted.
            withdraw_ai_observer_eye();
            // The held base outlives a first-person gap while a situation holds free-look off, so free-look comes
            // back on the base it had.
            if (!cam.orbit_suspended.load(std::memory_order_relaxed))
            {
                cam.orbit_base_held = false;
            }
            cam.orbit_render_valid =
                false; // next engaged frame snaps the orbit low-pass instead of easing across the gap
            cam.orbit_steer_valid = false; // and the continuous-align steer low-pass
            cam.eye_sync_valid = false;    // and the dynamic eye-height low-pass
            cam.fov_ease_valid = false;    // and the per-preset FOV override ease
            cam.basis_quat_valid = false;  // and the aim-basis low-pass
            cam.turn_track_valid = false;  // and the turn-in-place pivot hold
            cam.shoulder_swap_valid = false; // and the shoulder swap's slide
            // A press in first person does not start a free-look move.
            s_move_press_stamp_ms.store(0, std::memory_order_release);
            clear_move_press_turn();
            // A move under way keeps the angle the camera last showed, as stopping does.
            if (cam.orbit_moving && cam.orbit_target_valid)
            {
                cam.orbit_yaw.store(cam.orbit_yaw_render, std::memory_order_relaxed);
            }
            cam.orbit_moving = false;
            cam.orbit_target_valid = false;
            // A hand-off that turned the look left angles measured against the old look; the look takes the camera
            // direction once play resumes, so they are centered.
            if (cam.aim_handoff_active && cam.aim_handoff_written)
            {
                cam.orbit_yaw.store(0.0f, std::memory_order_relaxed);
                cam.orbit_pitch.store(0.0f, std::memory_order_relaxed);
                cam.orbit_yaw_render = 0.0f;
                cam.orbit_pitch_render = 0.0f;
            }
            cam.aim_handoff_active = false; // and the aim hand-off, which has no camera to hold
            cam.aim_handoff_cancel = false;
            cam.rig_last_orbited = false;
            // Back to first person: the next engaged frame should snap the preset, not ease across the gap.
            Presets::reset_transition();
            // Shots loosed in first person are still followed and drawn (the pose is invalid, so none is re-aimed).
            run_ranged_frame(cview, c_player, repeat_build);
            return;
        }
        s_orbit_was_engaged = true;

        // Resolve the active preset (by debounced state, or the overlay's editing pin) and ease the
        // live framing toward it BEFORE the matrix offset reads those settings this frame.
        Presets::resolve_and_apply(state, delta_time);

        // Publish the untouched first-person eye for the camera-observer detour before the matrix is offset: the AI
        // judges which NPCs to update from the observer, and in first person that observer stands at the eye.
        if (const auto eye = DMK::memory::read<Vector3>(DMK::Address{cview + Constants::SVIEWPARAMS_POSITION_OFFSET});
            eye)
        {
            publish_ai_observer_eye(*eye);
        }

        // Smoothstep the linear view blend for an ease-in/out feel, then offset with it.
        const float view_s = smoothstep(cam.view_blend);
        offset_game_view_camera(camera, cview, c_player, step, view_s);
        // After the offset, so the crosshair target is cast from the pose this frame renders.
        run_ranged_frame(cview, c_player, repeat_build);
    }

    /**
     * @brief Runs the per-frame camera offset under SEH, so an outdated offset or layout never crashes the game.
     * @details Apart from the detour, because the detour's DetourScope needs C++ object unwinding, which a __try
     *          frame cannot hold.
     */
    static void frustum_build_offset_guarded(uintptr_t camera, FrustumBuildProfile &profile) noexcept
    {
        __try
        {
            detour_frustum_build_impl(camera, profile);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    /**
     * @brief Per-frame frustum-builder detour.
     * @details The matrix offset runs BEFORE the original, which then computes the cull planes from
     *          the offset matrix, so geometry culling matches the rendered third-person view. A
     *          layout drift degrades the offset to a no-op. The original always runs and its return
     *          value is forwarded so the frustum is still built whether or not the offset applied.
     */
    static uintptr_t __fastcall detour_frustum_build(uintptr_t camera) noexcept
    {
        const DetourScope in_flight;
        FrustumBuildProfile profile;
        profile.begin_mod();
        frustum_build_offset_guarded(camera, profile);
        const FrustumBuildFunc original = s_frustum_build_original.load(std::memory_order_acquire);
        profile.begin_original();
        const uintptr_t result = original ? original(camera) : 0;
        profile.finish();
        return result;
    }

    /**
     * @brief Resolves the SSystemGlobalEnvironment (g_env) base, patch-resiliently.
     * @details Reads the g_env base through the Engine gate. The Genv anchor is a 2-of-5 quorum of
     *          RIP-relative lea/mov [rip+g_env] reference sites, so the address survives game
     *          patches that shift the RVA. The result is screened as a plausible user-space pointer
     *          before it is accepted. There is no fixed-address fallback: the Steam, GOG and Game Pass
     *          builds each keep g_env at a different address, so a fixed one names the wrong slot
     *          on every build but one, and the engine calls made through it run arbitrary code.
     * @return The g_env base address, or 0 when the gate failed. Every consumer then stays off.
     */
    static uintptr_t resolve_genv()
    {
        DMK::Logger &logger = DMK::log();

        // The Genv quorum resolved up front in resolve_all_anchors(), when at least two independent
        // lea/mov [rip+g_env] reference sites agreed. The result is screened as a plausible pointer.
        const uintptr_t resolved = gated_anchor_address(Feature::Engine, AnchorId::Genv);
        if (resolved != 0 && DMK::memory::is_plausible_ptr(DMK::Address{resolved}))
        {
            logger.info("Camera: g_env resolved at {}", DMK::format::format_address(resolved));
            return resolved;
        }

        logger.warning("Camera: g_env did not resolve; collision, the ranged crosshair ray, render occlusion and the "
                       "player look features are unavailable");
        return 0;
    }

    void orbit_move_pressed() noexcept
    {
        // Taken once: the camera publishes again on its next frame only while no move is under way. The move state
        // below belongs to the camera's thread, so the press acts only when it arrives there. Acquire pairs with the
        // publish's release, so the heading and the thread read below are that frame's.
        const std::uint64_t stamp = s_move_press_stamp_ms.exchange(0, std::memory_order_acquire);
        if (stamp == 0 || GetTickCount64() - stamp > k_move_press_max_age_ms ||
            GetCurrentThreadId() != s_move_press_thread.load(std::memory_order_relaxed))
        {
            return;
        }
        // Third person and free-look must still be on: a view switch or a released hold key since the publish
        // leaves the heading unused.
        CameraState &cam = camera_state();
        if (!cam.applying.load(std::memory_order_relaxed) || !cam.orbit_active.load(std::memory_order_relaxed) ||
            !s_offset_active.load(std::memory_order_relaxed))
        {
            return;
        }
        const float heading = s_move_press_heading.load(std::memory_order_relaxed);
        // The move starts here as the camera starts it, so this frame's rig already holds the camera's world yaw
        // while the look turns under it. The look and the body both turn at the press: the body override sets the
        // facing at the end of the animated-character update, and the look-driven release of the walk start
        // (LockBodyTurn) copies the look into the body one frame later, so an old look turns the body back.
        cam.orbit_target_yaw = heading;
        cam.orbit_target_valid = true;
        cam.orbit_yaw_at_capture_deg = cam.orbit_yaw.load(std::memory_order_relaxed);
        cam.orbit_moving = true;
        // The look jumps to the new heading, so the basis snaps to it rather than easing across the turn. A short
        // press then ends with the rig already on the new heading, and the angle baked on release stays put.
        cam.basis_quat_valid = false;
        (void)apply_orbit_aim_control(0.0f, 0.0f, true, heading);
        const std::uintptr_t anim_char = apply_orbit_body_turn(heading);
        const float turn =
            std::remainder(heading - s_move_press_body_yaw.load(std::memory_order_relaxed), DirectX::XM_2PI);
        if (anim_char != 0 && std::fabs(turn) > k_move_press_min_turn)
        {
            note_move_press_turn(anim_char);
        }
        (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                 "Orbit: move-orbit START at the press (the body and the look turn to the camera "
                                 "heading {:.1f} deg)",
                                 DMK::math::radians_to_degrees(heading));
    }

    DMK::Result<void> initialize_camera(uintptr_t module_base, size_t module_size, HookSet &hooks)
    {
        HookProfile::initialize();
        DMK::Logger &logger = DMK::log();

#ifdef DMK_ENABLE_PROFILING
        // Allocate the diagnostic ring on the initialization thread before any camera callback can record.
        (void)DMK::Profiler::get_instance();
#endif

        // Start in first-person (offset off) so the game looks normal on load; the view
        // hotkeys toggle/force the third-person offset on. The hooks below fast-path out
        // while the offset is off, so installing them unconditionally costs almost nothing.
        CameraState &cam = camera_state();
        cam.applying.store(false);
        cam.zoom_offset.store(0.0f);

        // Resolve g_env once (patch-resilient AOB). The CView vtable is identified lazily by RTTI on
        // the first game-view camera, so nothing is resolved here.
        s_genv_runtime = resolve_genv();

        // Resolve the engine ray helper for collision + the ranged crosshair ray. Best-effort: on a miss
        // those features no-op (the camera still renders), so the result is intentionally discarded.
        (void)initialize_physics_raycast(s_genv_runtime);

        // Resolve the 3DEngine render-octree query the occluder scan uses to find the objects between the camera and
        // the character. Best-effort: a miss leaves plain physics collision, so the result is intentionally
        // discarded.
        (void)initialize_render_occlusion(module_base, module_size, s_genv_runtime);

        // The Indoors state presets can bind to. Best-effort: on a miss such a preset never applies.
        initialize_indoor_state();

        // What the character carries fades with it, found through the entity system g_env holds. Handed over before
        // any hook below is armed, so no camera frame walks the attachments without it.
        initialize_character_attachments(s_genv_runtime);

        // The default hook::Options prologue policy is Fail, which is what this mod wants: refuse the
        // install when the resolved entry leads with a call or breakpoint byte, the shape a cascade
        // mis-resolution or a foreign int3 stub produces. A sibling mod's E9 jump hook decodes as a
        // relocatable branch rather than a refusal, so layering still works.
        //
        // The frustum hook is mandatory and returns the library Error verbatim on a refusal, so the caller
        // sees the real ErrorCode. The head-visibility and input-dispatcher hooks are best-effort WITHIN
        // this unit (each costs one cosmetic/optional feature, not the camera), so they warn in place.

        // Hook the camera frustum builder - the matrix-offset point; without it the feature
        // does nothing. The module-scoped cascade resolves to the function entry inside the game
        // image or returns 0, so no separate bounds check is needed here.
        const uintptr_t frustum_addr = gated_anchor_address(Feature::Camera, AnchorId::Frustum);
        if (frustum_addr == 0)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "camera_hook/frustum_anchor"});
        }
        auto frustum_result = DMK::hook::inline_at(
            DMK::hook::InlineRequest{.name = "CameraFrustumBuild", .target = DMK::Address{frustum_addr}},
            detour_frustum_build);
        if (!frustum_result.has_value())
        {
            return std::unexpected(frustum_result.error());
        }
        // Publish the trampoline and store the handle BEFORE enable() arms the patch. The detour then cannot observe a
        // null original on its first frame, and the set owns a hook whose arm fails with the patch live. The backend
        // allocates the trampoline at install, so it is readable off the still-disabled handle.
        s_frustum_build_original.store(frustum_result->original<FrustumBuildFunc>(), std::memory_order_release);
        if (auto armed = hooks.push(std::move(*frustum_result)).enable(); !armed.has_value())
        {
            return std::unexpected(armed.error());
        }

        install_head_visibility_hook(hooks);

        // The input-dispatcher hook powers free-look orbit. Best-effort: a miss only disables orbit, the offset camera
        // still works.
        install_input_dispatch_hook(hooks);

        // Keep the AI judging which NPCs to update (and which to hide and pause) from the first-person eye while the
        // third-person offset renders. Best-effort, like the two hooks above.
        install_camera_observer_hook(module_base, module_size, hooks);

        // Let the body play the game's own turn-in-place animations while the third-person view is engaged.
        // Best-effort, like the hooks above.
        install_native_turn_hook(module_base, module_size, hooks);
        // Keeps native turn steps on the spot (with the decision hook above) and drops the stale first step of a
        // free-look move started at the press. After the native turn hook, which resets the state this one sets.
        // Best-effort.
        install_turn_in_place_hook(hooks);

        // Fade the character instead of switching to first person when the camera has no room behind it. Best-effort:
        // without it the first-person switch stays.
        if (auto character = initialize_character_fade(hooks); !character.has_value())
        {
            logger.warning(
                "Camera: character fade unavailable ({}); the camera switches to first person when too close",
                character.error().message());
        }
        // Fade the objects between the camera and the character instead of pulling the camera in front of them.
        // Best-effort: without it the camera collides with them.
        if (auto fade = initialize_occluder_fade(hooks); !fade.has_value())
        {
            logger.warning("Camera: occluder fade unavailable ({}); objects in front of the character block the camera",
                           fade.error().message());
        }

        logger.info("Camera: Third-person camera hooks installed");
        return {};
    }

    void refresh_zoom_binding_tokens() noexcept
    {
        try
        {
            const DMK::input::Input &input = DMK::input::Input::instance();
            s_zoom_tokens.store(std::make_shared<const ZoomBindingTokens>(ZoomBindingTokens{
                                    .zoom_in = input.acquire_token(k_zoom_in_binding),
                                    .zoom_out = input.acquire_token(k_zoom_out_binding),
                                }),
                                std::memory_order_release);
        }
        catch (...)
        {
            // An allocation failure leaves no tokens published, so the main thread falls back to the name-based
            // query and never reads a stale pair.
            s_zoom_tokens.store(nullptr, std::memory_order_release);
        }
    }

    void release_zoom_binding_tokens() noexcept
    {
        s_zoom_tokens.store(nullptr, std::memory_order_release);
    }

} // namespace TPVCamera
