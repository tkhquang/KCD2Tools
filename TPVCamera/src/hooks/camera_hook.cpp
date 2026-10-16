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
#include "camera/lens.hpp"
#include "camera/npc_lens_fade.hpp"
#include "camera/see_through_driver.hpp"
#include "core/input/stick.hpp"
#include "core/rig/rig_math.hpp"
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

    // Camera-relative movement (toggle orbit). A move starts when the device-agnostic movement-input magnitude
    // (~0..1.4) rises above MOVE_INPUT_START from idle, which aligns the heading to the camera once, and ends only
    // below the lower k_orbit_move_input_stop (hysteresis, so a momentary dip cannot re-trigger the align). The input
    // stays nonzero while a key is held against a wall, so the heading is not falsely released on a collision stop.
    // k_orbit_aim_level_speed eases the look pitch toward level per second while orbiting.
    constexpr float k_orbit_move_input_stop = 0.05f;
    // A move armed by the game's own move input (see the re-arm guard) also ends once that input has stayed idle this
    // long, which covers the game's few frames of lag behind the keys.
    constexpr float k_orbit_move_game_stop_delay = 0.1f; // seconds
    constexpr float k_orbit_aim_level_speed = 8.0f;

    // Orbit angle low-pass. The engine smooths native look DOWNSTREAM of the input dispatch the orbit
    // hook captures and blocks, so free-look applies the raw per-frame mouse deltas and reads as jittery.
    // orbit_smoothing (0..1) maps to a frame-rate-independent catch-up speed between these bounds: higher
    // smoothing -> lower speed -> more lag (smoother); 0 disables the filter (snap to the raw target).
    constexpr float k_orbit_smooth_min_speed = 6.0f;  // strength 1.0: heavy smoothing
    constexpr float k_orbit_smooth_max_speed = 40.0f; // strength near 0: barely-there smoothing

    // Aim hand-off: the least turn or rise off the plain rig worth a hand-off, and the heading gap at which the look
    // counts as turned; how long it waits for the look; the body step between two frames that counts as a load or a
    // teleport rather than a run or a hitch; and the bound on the look pitch it writes.
    constexpr float k_aim_handoff_min_angle = 0.0175f; // rad (1 deg)
    constexpr float k_aim_handoff_timeout = 0.5f;      // seconds
    constexpr float k_aim_handoff_jump = 3.0f;         // meters per frame
    constexpr float k_aim_handoff_max_pitch = 1.4f;    // rad (80 deg)
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

    // Aim-basis low-pass (AimBasisSmoothing). Maps the 0..1 strength to a frame-rate-independent slerp
    // catch-up speed: higher strength -> lower speed -> more damping (and slightly laggier aim). The window
    // is faster than the orbit low-pass so even at full strength the camera still tracks genuine aim turns
    // closely while shedding the high-frequency view-shake.
    constexpr float k_basis_smooth_min_speed = 8.0f;  // strength 1.0: heaviest damping
    constexpr float k_basis_smooth_max_speed = 60.0f; // strength near 0: barely-there damping

    // SwapShoulderKey: seconds the camera takes to slide over to the other shoulder. A linear ramp read through a
    // smoothstep, so a press starts and lands without a kick, and a second press mid-slide turns back from where the
    // camera is.
    constexpr float k_shoulder_swap_seconds = 0.3f;

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

    // Turn-in-place pivot hold tuning. A native turn step is the body rotating TOWARD the look (faster than
    // k_turn_hold_yaw_rate) while it trails the look by more than k_turn_hold_min_gap, moving no faster than
    // k_turn_hold_max_speed and with no movement input. Its displacement, and that of the step's foot settle for
    // k_turn_hold_settle after the rotation stops, is held off the pivot. A body that moves faster than
    // k_turn_hold_follow_speed without being held is really moving, and the hold eases back onto it at
    // k_turn_hold_release_rate. A per-frame jump beyond k_turn_hold_teleport is a teleport or a load, not a step, and
    // drops the hold. The hold never exceeds k_turn_hold_max. With the turn steps kept in place
    // (detour_phys_ent_movement) there is normally nothing to hold; this keeps the camera still when that hook is
    // unavailable.
    constexpr float k_turn_hold_yaw_rate = 0.26f;    // rad/s (15 deg/s)
    constexpr float k_turn_hold_min_gap = 0.17f;     // rad (10 deg)
    constexpr float k_turn_hold_max_speed = 1.2f;    // m/s
    constexpr float k_turn_hold_follow_speed = 0.3f; // m/s
    constexpr float k_turn_hold_settle = 0.35f;      // seconds
    constexpr float k_turn_hold_teleport = 0.25f;    // meters per frame
    constexpr float k_turn_hold_max = 0.5f;          // meters
    constexpr float k_turn_hold_release_rate = 8.0f; // 1/s

    /**
     * @brief Keeps the pivot still while the body turns in place, returning the horizontal offset to hold off it.
     * @details A native turn-in-place animation carries root motion: the body steps 5-20 cm while it turns, and the
     *          rig anchors to the body, so the camera shifts with every turn unless the step is held off. The
     *          displacement of a turn step is absorbed into a hold the caller subtracts from its anchor, so the camera
     *          stays where it was during and after the turn. A turn step is recognised by its signature: the body
     *          rotates toward the look it trails, at step speed, with no movement input. Running to a stop, a
     *          scripted move or a push does not match it, so the camera follows them, and any such movement also
     *          eases the hold back onto the body. Standing still keeps the hold. Without the look the rotation alone
     *          is used. Normally the steps are kept in place (detour_phys_ent_movement) and the hold stays near zero;
     *          it is the fallback for when that hook is unavailable.
     * @param cam Camera state holding the tracker and the hold.
     * @param body_origin The entity world origin this frame.
     * @param body_yaw The entity world yaw this frame (radians).
     * @param look_yaw The look yaw this frame (radians), used only when @p look_valid.
     * @param look_valid Whether @p look_yaw was read.
     * @param delta_time Frame time in seconds.
     * @return The horizontal offset (z = 0) to subtract from the anchor.
     */
    static Vector3 hold_turn_in_place_pivot(CameraState &cam, const Vector3 &body_origin, float body_yaw,
                                            float look_yaw, bool look_valid, float delta_time)
    {
        if (!cam.turn_track_valid)
        {
            cam.turn_track_x = body_origin.x;
            cam.turn_track_y = body_origin.y;
            cam.turn_track_yaw = body_yaw;
            cam.turn_track_valid = true;
            cam.turn_track_step = 0.0f;
            cam.turn_hold_x = 0.0f;
            cam.turn_hold_y = 0.0f;
            cam.turn_hold_timer = 0.0f;
            return Vector3{0.0f, 0.0f, 0.0f};
        }
        const float step_x = body_origin.x - cam.turn_track_x;
        const float step_y = body_origin.y - cam.turn_track_y;
        const float step = std::sqrt(step_x * step_x + step_y * step_y);
        cam.turn_track_step = step;
        const float turn = std::remainder(body_yaw - cam.turn_track_yaw, 2.0f * DirectX::XM_PI);
        cam.turn_track_x = body_origin.x;
        cam.turn_track_y = body_origin.y;
        cam.turn_track_yaw = body_yaw;
        if (step > k_turn_hold_teleport)
        {
            cam.turn_hold_x = 0.0f;
            cam.turn_hold_y = 0.0f;
            cam.turn_hold_timer = 0.0f;
            return Vector3{0.0f, 0.0f, 0.0f};
        }

        const float speed = delta_time > 1e-4f ? step / delta_time : 0.0f;
        const bool native_turn = native_turn_engaged();
        const bool move_input =
            !player_onaction_available() || player_onaction_move_magnitude() > k_orbit_move_input_stop;
        const bool in_place = native_turn && !move_input && speed < k_turn_hold_max_speed;
        bool toward_look = true;
        if (look_valid)
        {
            const float gap = std::remainder(look_yaw - body_yaw, 2.0f * DirectX::XM_PI);
            toward_look = std::fabs(gap) > k_turn_hold_min_gap && turn * gap > 0.0f;
        }
        if (in_place && toward_look && std::fabs(turn) > k_turn_hold_yaw_rate * delta_time)
        {
            cam.turn_hold_timer = k_turn_hold_settle;
        }
        else
        {
            cam.turn_hold_timer = std::max(0.0f, cam.turn_hold_timer - delta_time);
        }

        if (in_place && cam.turn_hold_timer > 0.0f)
        {
            cam.turn_hold_x += step_x;
            cam.turn_hold_y += step_y;
        }
        else if (!native_turn || speed > k_turn_hold_follow_speed)
        {
            // Real movement (or the feature off). Keyed on the body's speed rather than the movement-input latch, which
            // a combat action-map swap can strand above zero with the keys up.
            const float keep = std::exp(-k_turn_hold_release_rate * delta_time);
            cam.turn_hold_x *= keep;
            cam.turn_hold_y *= keep;
        }
        const float held = std::sqrt(cam.turn_hold_x * cam.turn_hold_x + cam.turn_hold_y * cam.turn_hold_y);
        if (held > k_turn_hold_max)
        {
            const float scale = k_turn_hold_max / held;
            cam.turn_hold_x *= scale;
            cam.turn_hold_y *= scale;
        }
        return Vector3{cam.turn_hold_x, cam.turn_hold_y, 0.0f};
    }

    // Mount follow (OrbitFollowSpeed): how long the follow takes to come back once free-look input ends, and the
    // smallest change of the free-look yaw accumulator (degrees) that counts as input.
    constexpr float k_mount_follow_ramp_seconds = 2.0f;
    constexpr float k_mount_follow_input_deg = 1e-3f;

    /**
     * @brief Turns free-look back behind the mount while riding (OrbitFollowSpeed), a flex of the rule that free-look
     *        stays where the player left it.
     * @details Riding a horse or a cart with free-look on, the camera's heading eases toward the mount's (the rider's
     *          body yaw) by OrbitReturnSpeed x min(step / OrbitFollowSpeed, dt) per frame, where step is the body's
     *          horizontal move since the last frame: at OrbitReturnSpeed once the mount covers OrbitFollowSpeed meters
     *          a second, proportionally slower below that, never while it stands, and not on the frame's second build.
     *          Free-look input (the mouse or the stick moving the orbit) stops it at once; it comes back over
     *          k_mount_follow_ramp_seconds (smoothstep) after the input ends, and ramps in the same way when it turns
     *          on. With a captured heading (camera-relative movement) the heading turns, and the look and the body
     *          written to it with it; otherwise the orbit angle turns. Main thread only.
     * @param forward_yaw The rig's forward yaw this frame (radians).
     * @param body_yaw The rider's body yaw (radians), the mount's heading.
     * @param active Free-look is on, a horse or a cart is ridden, OrbitFollowSpeed is set and the body was read.
     * @param move_orbit Moving with a captured heading.
     * @param orbit_yaw_deg The frame's orbit yaw (degrees), turned in place.
     */
    static void follow_mount_heading(CameraState &cam, const LiveSettings &cfg, float forward_yaw, float body_yaw,
                                     bool active, bool move_orbit, float delta_time, float &orbit_yaw_deg)
    {
        const float follow_speed = cfg.orbit_follow_speed.load(std::memory_order_relaxed);
        if (!active || !(follow_speed > 0.0f))
        {
            cam.mount_follow_seen_valid = false;
            cam.mount_follow_idle = 0.0f;
            return;
        }
        const float accumulator = cam.orbit_yaw.load(std::memory_order_relaxed);
        const bool input = (cam.mount_follow_seen_valid &&
                            std::fabs(accumulator - cam.mount_follow_seen_yaw) > k_mount_follow_input_deg) ||
                           cam.orbit_pad_yaw.load(std::memory_order_relaxed) != 0.0f ||
                           cam.orbit_pad_pitch.load(std::memory_order_relaxed) != 0.0f;
        cam.mount_follow_idle = input ? 0.0f : cam.mount_follow_idle + delta_time;
        cam.mount_follow_seen_yaw = accumulator;
        cam.mount_follow_seen_valid = true;

        const float ramp = smoothstep(std::clamp(cam.mount_follow_idle / k_mount_follow_ramp_seconds, 0.0f, 1.0f));
        const float rate = cfg.orbit_return_speed.load(std::memory_order_relaxed) *
                           std::min(cam.turn_track_step / follow_speed, delta_time) * ramp;
        if (!(rate > 0.0f))
        {
            return;
        }
        const float camera_yaw = forward_yaw + DMK::math::degrees_to_radians(orbit_yaw_deg);
        const float turn = std::remainder(body_yaw - camera_yaw, 2.0f * DirectX::XM_PI) * (1.0f - std::exp(-rate));
        const float turn_deg = DMK::math::radians_to_degrees(turn);
        if (move_orbit)
        {
            cam.orbit_target_yaw += turn;
        }
        else
        {
            // The input thread adds the mouse's deltas to the same accumulator, so the turn is added atomically too,
            // and what the follow saw is its own result: a delta landing in between shows as input next frame.
            cam.mount_follow_seen_yaw = cam.orbit_yaw.fetch_add(turn_deg, std::memory_order_relaxed) + turn_deg;
        }
        orbit_yaw_deg = std::remainder(orbit_yaw_deg + turn_deg, 360.0f);
    }

    /**
     * @brief Offsets the game view camera matrix behind the player and advances zoom/smoothing.
     * @details Reads the eye anchor and orientation from the untouched CView pose (position at
     *          SVIEWPARAMS_POSITION_OFFSET, quat at SVIEWPARAMS_ROTATION_OFFSET), derives the look
     *          basis from the quat (idempotent across same-frame rebuilds), computes the
     *          third-person pose, then writes the camera matrix translation and (when the stable basis,
     *          follow angle or orbit shapes the look) basis. The caller is about to compute the cull
     *          planes from this matrix, so culling matches the rendered view.
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

        // Follow distance = configured base (hot-reloadable INI FollowDistance, re-read every
        // frame so an edit applies live) plus the accumulated zoom offset from the hold
        // keys, clamped to the configured window. The holds are queried through the BindingTokens the control
        // plane publishes (refresh_zoom_binding_tokens), the library's per-frame form of the query. Before the
        // first publish the name-based query stands in.
        const DMK::input::Input &input = DMK::input::Input::instance();
        const std::shared_ptr<const ZoomBindingTokens> zoom_tokens = s_zoom_tokens.load(std::memory_order_acquire);
        const bool zoom_in_held =
            zoom_tokens ? input.is_active(zoom_tokens->zoom_in) : input.is_active(k_zoom_in_binding);
        const bool zoom_out_held =
            zoom_tokens ? input.is_active(zoom_tokens->zoom_out) : input.is_active(k_zoom_out_binding);
        float zoom_offset = cam.zoom_offset.load(std::memory_order_relaxed);
        const float zoom_step = cfg.zoom_step.load(std::memory_order_relaxed);
        if (zoom_in_held)
        {
            zoom_offset -= zoom_step * delta_time; // zoom in pulls the camera closer
        }
        if (zoom_out_held)
        {
            zoom_offset += zoom_step * delta_time;
        }
        const float base_distance = cfg.follow_distance.load(std::memory_order_relaxed);
        // A preset can carry Min above Max, and std::clamp is undefined when its upper bound is below the lower.
        const float distance_min = cfg.follow_distance_min.load(std::memory_order_relaxed);
        const float distance_max = std::max(distance_min, cfg.follow_distance_max.load(std::memory_order_relaxed));
        const float distance = std::clamp(base_distance + zoom_offset, distance_min, distance_max);
        // A requested reset keeps this frame's distance and hands the clamped offset to the preset ease, which
        // carries it from the next frame on and eases it out.
        if (cam.zoom_reset_requested.exchange(false, std::memory_order_relaxed))
        {
            Presets::ease_out_zoom_offset(distance - base_distance);
            cam.zoom_offset.store(0.0f, std::memory_order_relaxed);
        }
        else
        {
            cam.zoom_offset.store(distance - base_distance, std::memory_order_relaxed);
        }

        // Read the eye anchor and the orientation quat from the untouched CView pose. The basis is
        // derived from the quat (CryEngine column-vector convention: right = quat * +X, forward =
        // quat * +Y, up = quat * +Z). Reading the quat rather
        // than the matrix columns keeps the basis idempotent across the frustum builder's same-frame
        // rebuilds (we overwrite the matrix, never the pose, so the pose stays the clean eye input).
        // The matrix to offset is the camera's 3x4 at offset 0 (cview + SVIEWPARAMS_VIEWMATRIX_OFFSET).
        // The eye pose is read through memory::read (memcpy semantics into a trivially-copyable Vector3 /
        // Quaternion): it screens the source against the low-address floor and a wrap guard, swallows an
        // access fault via its own SEH frame, and avoids the type-pun UB of a placement reinterpret_cast
        // read, so a stale/torn cview fails closed here rather than depending solely on the outer detour
        // SEH frame.
        const auto eye_position_read =
            DMK::memory::read<Vector3>(DMK::Address{cview + Constants::SVIEWPARAMS_POSITION_OFFSET});
        const auto eye_rotation_read =
            DMK::memory::read<Quaternion>(DMK::Address{cview + Constants::SVIEWPARAMS_ROTATION_OFFSET});
        if (!eye_position_read || !eye_rotation_read)
        {
            return; // CView pose read faulted; leave the view untouched this frame
        }
        const Vector3 eye_position = *eye_position_read;
        const Quaternion eye_rotation = *eye_rotation_read;
        // FPV-side basis (the engine eye orientation). Kept as the view-blend SOURCE so view_blend 0 leaves
        // the untouched first-person view; the third-person rig basis below is built from a STABLE source.
        const Vector3 eye_forward = eye_rotation.rotate(Vector3{0.0f, 1.0f, 0.0f});
        const Vector3 eye_up = eye_rotation.rotate(Vector3{0.0f, 0.0f, 1.0f});

        // Stable rig basis. The third-person rig (camera = pivot - forward * distance) amplifies any rotation of
        // the basis into a position swing. The EyeHeight body anchor removes the POSITIONAL bob, and this removes
        // the ROTATIONAL component the engine bakes into the eye quat during animations (head-bob and weapon-sway
        // rotation, combat / hit / landing shake). StableAimBasis follows the player's clean look-controller AIM
        // instead, leveled by level_look_rotation. That aim carries none of the engine view-shake and equals the eye
        // quat at rest. AimBasisSmoothing low-passes it. basis_overridden records whether the basis was shaped at
        // all. When neither feature is active the basis stays the raw eye quat, and the orientation path below is
        // byte-identical to the follow-angle/orbit-only behavior (a fail-safe default).
        Quaternion basis_rotation = eye_rotation;
        bool basis_overridden = false;
        Quaternion look_rotation = eye_rotation;
        bool look_valid = false;
        if (cfg.stable_aim_basis.load(std::memory_order_relaxed) && c_player != 0)
        {
            // Read the look controller through the self-healed offset (seeded from the constant, recovered if
            // the C_Player layout drifts) so the basis tracks the same controller the orbit aim control writes.
            // Read-only here (the basis is derived from the controller's quat and pitch), so the retained nominal is
            // acceptable: worst case on a drifted layout it reads neighboring fields and the sanity checks below
            // reject them. The WRITE path (apply_orbit_aim_control) demands a Confirmed heal.
            const std::ptrdiff_t c_player_look_controller_offset =
                runtime_offsets().c_player_look_controller.load().value;
            const auto controller =
                DMK::memory::read<uintptr_t>(DMK::Address{c_player + c_player_look_controller_offset});
            if (controller && DMK::memory::is_plausible_ptr(DMK::Address{*controller}))
            {
                const auto look_quat =
                    DMK::memory::read<Quaternion>(DMK::Address{*controller + Constants::LOOK_CONTROLLER_QUAT_OFFSET});
                const auto look_pitch =
                    DMK::memory::read<float>(DMK::Address{*controller + Constants::LOOK_CONTROLLER_PITCH2_OFFSET});
                if (look_quat && look_pitch)
                {
                    const Quaternion q = *look_quat;
                    const float n2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
                    // Accept only a finite, ~unit quaternion and a finite pitch within the engine's +/-1.5608 rad
                    // look clamp, plus a small margin. A layout drift that feeds garbage falls back to the eye quat
                    // and never rotates the rig by a denormal.
                    if (std::isfinite(n2) && n2 > 0.9f && n2 < 1.1f && std::isfinite(*look_pitch) &&
                        std::fabs(*look_pitch) <= 1.6f)
                    {
                        look_rotation = level_look_rotation(q, *look_pitch);
                        look_valid = true;
                    }
                }
            }
        }
        // Low-pass the basis quaternion toward the target: the leveled look orientation when StableAimBasis resolved
        // it, else the raw eye quat. Snaps on the first engaged frame (basis_quat_valid false) so the camera does not
        // swing in from a stale orientation across a first-person gap. AimBasisSmoothing 0 disables the filter
        // (the target is used directly).
        const float basis_smoothing = std::clamp(cfg.aim_basis_smoothing.load(std::memory_order_relaxed), 0.0f, 1.0f);
        if (look_valid || basis_smoothing > 1e-4f)
        {
            basis_overridden = true;
            const DirectX::XMVECTOR target = (look_valid ? look_rotation : eye_rotation).to_xm_vector();
            DirectX::XMVECTOR result;
            if (!cam.basis_quat_valid)
            {
                result = target; // snap on the first engaged frame so the camera does not swing in from stale
                cam.basis_quat_valid = true;
            }
            else if (basis_smoothing > 1e-4f)
            {
                const DirectX::XMVECTOR current =
                    DirectX::XMVectorSet(cam.basis_quat_x, cam.basis_quat_y, cam.basis_quat_z, cam.basis_quat_w);
                const float speed =
                    k_basis_smooth_max_speed - basis_smoothing * (k_basis_smooth_max_speed - k_basis_smooth_min_speed);
                const float t = 1.0f - std::exp(-speed * delta_time);
                // XMQuaternionSlerp takes the shortest arc, so a quaternion double-cover sign flip never spins the
                // basis the long way.
                result = DirectX::XMQuaternionNormalize(DirectX::XMQuaternionSlerp(current, target, t));
            }
            else
            {
                result = target; // StableAimBasis on, smoothing off: use the look target directly
            }
            cam.basis_quat_x = DirectX::XMVectorGetX(result);
            cam.basis_quat_y = DirectX::XMVectorGetY(result);
            cam.basis_quat_z = DirectX::XMVectorGetZ(result);
            cam.basis_quat_w = DirectX::XMVectorGetW(result);
            basis_rotation = Quaternion{cam.basis_quat_x, cam.basis_quat_y, cam.basis_quat_z, cam.basis_quat_w};
        }
        else
        {
            cam.basis_quat_valid = false; // basis not shaped: the next engage snaps the low-pass
        }

        const Vector3 right = basis_rotation.rotate(Vector3{1.0f, 0.0f, 0.0f});
        const Vector3 forward = basis_rotation.rotate(Vector3{0.0f, 1.0f, 0.0f});
        // The look pitch and heading the rig is built from (engine convention: forward = (-sin yaw, cos yaw)).
        const float forward_pitch = std::asin(std::clamp(forward.z, -1.0f, 1.0f));
        const float forward_yaw = std::atan2(-forward.x, forward.y);
        const Vector3 up = basis_rotation.rotate(Vector3{0.0f, 0.0f, 1.0f});
        GameStructures::Matrix34f *matrix = reinterpret_cast<GameStructures::Matrix34f *>(camera);
        // Per-preset FOV, smoothly crossing the "off" boundary. SetFrustum (sub_1805392FC) writes the render
        // CCamera's FOV scalar at camera+0x30 right before this builder, plus the cull-frustum terms at camera+0x50 /
        // +0x58 / +0x68 / +0x70 (the near and far corner x and z, proportional to tan(fov/2)) and +0x60 (the
        // projection plane distance, proportional to 1/tan(fov/2)). So camera+0x30 read here is the live GAME FOV
        // (radians). cfg.fov is the per-preset TARGET in degrees (0 = use the game FOV). A rendered FOV eases toward
        // (target>0 ? target : game FOV) with a 2-stage critically-damped ease at the preset-blend rate, so turning a
        // preset's FOV on/off GLIDES through the game FOV instead of snapping across 0. It is written (the frustum
        // terms rescaled by tan(new/2)/tan(game/2) so culling matches) only while a preset wants a FOV or is still
        // easing back; once settled at the game FOV this passes through, so the game's own dynamic FOV keeps working.
        // TPV only.
        const float game_fov = *reinterpret_cast<const float *>(
            camera + Constants::CCAMERA_PROJECTION_FOV_OFFSET); // radians, freshly set by SetFrustum
        if (view_blend > 0.0f && game_fov > 0.05f && game_fov < 3.0f)
        {
            const float fov_target_degrees = cfg.fov.load(std::memory_order_relaxed);
            const float desired_fov =
                (fov_target_degrees > 0.0f) ? DMK::math::degrees_to_radians(fov_target_degrees) : game_fov;
            const float blend_seconds = cfg.preset_blend_in_seconds.load(std::memory_order_relaxed);
            if (!cam.fov_ease_valid) // first engaged frame after a first-person gap: snap, no ease across the gap
            {
                cam.fov_ease.snap(desired_fov);
                cam.fov_ease_valid = true;
            }
            else
            {
                // The preset blend's filter in the active preset's blend time; 0 snaps.
                cam.fov_ease.step(desired_fov, spring_alpha(blend_seconds, delta_time));
            }
            // Override while a preset wants a FOV or while still easing back toward the game FOV; otherwise pass
            // through (preset off + settled) so the game keeps its own FOV (including any dynamic FOV).
            if ((fov_target_degrees > 0.0f || std::fabs(cam.fov_ease.applied - game_fov) > 0.005f) &&
                cam.fov_ease.applied > 0.05f && cam.fov_ease.applied < 3.0f)
            {
                const float ratio = std::tan(cam.fov_ease.applied * 0.5f) / std::tan(game_fov * 0.5f);
                *reinterpret_cast<float *>(camera + Constants::CCAMERA_PROJECTION_FOV_OFFSET) =
                    cam.fov_ease.applied; // projection FOV (radians)
                *reinterpret_cast<float *>(camera + Constants::CCAMERA_CULL_EDGE_0_OFFSET) *=
                    ratio; // near/far corner x and z (proportional to tan)
                *reinterpret_cast<float *>(camera + Constants::CCAMERA_CULL_EDGE_1_OFFSET) *= ratio;
                *reinterpret_cast<float *>(camera + Constants::CCAMERA_CULL_TAN_OFFSET) /=
                    ratio; // projection plane distance (proportional to 1/tan)
                *reinterpret_cast<float *>(camera + Constants::CCAMERA_CULL_EDGE_3_OFFSET) *= ratio;
                *reinterpret_cast<float *>(camera + Constants::CCAMERA_CULL_EDGE_4_OFFSET) *= ratio;
            }
        }
        // First-person camera position - the blend source for the view-switch ease. Sourced from the
        // untouched eye pose (cview + SVIEWPARAMS_POSITION_OFFSET, same as eye_position above), NOT the
        // matrix translation this function overwrites, so a second same-frame frustum rebuild does not
        // compound the FPV->TPV lerp (the same idempotency the rotation basis and the TPV anchor rely on).
        // The engine builds the matrix translation from this eye pose, so at view_blend 0 the matrix is
        // left exactly first person.
        const Vector3 fpv_position = eye_position;

        // Over-the-shoulder lateral offset along the eye-right axis, applied to the non-orbit follow
        // pose. During free-look it is folded into the orbit start offset (below) so it revolves with
        // the ring, which keeps engaging free-look continuous. SwapShoulderKey mirrors it to the other side: the
        // offset is scaled from +1 to -1 through the smoothstepped swap progress, so it passes through the look line
        // instead of jumping across it. The first engaged frame takes the chosen side at once.
        const float swap_target = cam.shoulder_swapped.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        cam.shoulder_swap = cam.shoulder_swap_valid
                                ? ramp_weight(cam.shoulder_swap, swap_target, k_shoulder_swap_seconds, delta_time)
                                : swap_target;
        cam.shoulder_swap_valid = true;
        const float offset_right = cfg.offset_right.load(std::memory_order_relaxed) * shoulder_side(cam.shoulder_swap);
        const Vector3 lateral_offset = right * offset_right;

        // Anchor the rig to a STABLE point. The first-person eye (eye_position, read from CView+0x14)
        // carries head-bob, breathing and weapon-sway, so anchoring the third-person camera to it
        // shakes the whole view. Instead anchor to the player BODY origin - column 3 of the entity
        // world matrix at OFFSET_ENTITY_WORLD_MATRIX_MEMBER (Matrix34, translation at m[*][3]) - which
        // does not bob - lifted by EyeHeight to roughly eye level along world up. Falls back to the eye
        // anchor when the feature is off (EyeHeight <= 0) or the entity/world matrix is unavailable, so
        // a missing player or a layout drift degrades to the previous behaviour rather than misplacing
        // the camera.
        const Vector3 world_up{0.0f, 0.0f, 1.0f};
        const float eye_height = cfg.eye_height.load(std::memory_order_relaxed);
        // Read the player body world origin once. It feeds the stable anchor (below) AND the
        // device-agnostic movement speed used by the camera-relative move alignment; body_valid gates both.
        Vector3 body_origin{0.0f, 0.0f, 0.0f};
        float body_yaw = 0.0f;
        bool body_valid = false;
        uintptr_t entity_addr = 0;
        {
            // Resolve the entity FRESH from the live C_Player every frame (rather than trusting a mirrored
            // pointer that goes null/stale across view transitions and reloads), which is what keeps the
            // move-detection (hence the camera-relative body-turn and the body anchor) locked onto the
            // CURRENT player. On a failed walk body_valid stays false and the camera degrades to the eye anchor.
            if (c_player != 0)
            {
                const std::ptrdiff_t c_player_entity_offset = runtime_offsets().c_player_entity.load().value;
                const auto ent = DMK::memory::read<uintptr_t>(DMK::Address{c_player + c_player_entity_offset});
                if (ent && DMK::memory::is_plausible_ptr(DMK::Address{*ent}))
                {
                    entity_addr = *ent;
                }
            }
            if (entity_addr != 0 && DMK::memory::is_plausible_ptr(DMK::Address{entity_addr}))
            {
                const DMK::Address matrix_addr =
                    DMK::Address{entity_addr}.offset(Constants::OFFSET_ENTITY_WORLD_MATRIX_MEMBER);
                const auto world_matrix = DMK::memory::read<GameStructures::Matrix34f>(matrix_addr);
                if (world_matrix)
                {
                    body_origin = Vector3{world_matrix->m[0][3], world_matrix->m[1][3], world_matrix->m[2][3]};
                    // Forward is column 1; yaw in the engine convention forward = (-sin yaw, cos yaw).
                    body_yaw = std::atan2(-world_matrix->m[0][1], world_matrix->m[1][1]);
                    body_valid = true;
                }
            }
        }
        // Held off the anchor while the body steps through a native turn-in-place animation, so the camera does not
        // shift with the turn. The look yaw comes from the look quat, or from the eye quat, whose yaw is the look yaw
        // in first person.
        Vector3 turn_hold{0.0f, 0.0f, 0.0f};
        if (body_valid)
        {
            const Vector3 look_fwd = (look_valid ? look_rotation : eye_rotation).rotate(Vector3{0.0f, 1.0f, 0.0f});
            const bool look_yaw_valid = look_fwd.x * look_fwd.x + look_fwd.y * look_fwd.y > 1e-6f;
            const float look_yaw = look_yaw_valid ? std::atan2(-look_fwd.x, look_fwd.y) : 0.0f;
            turn_hold = hold_turn_in_place_pivot(cam, body_origin, body_yaw, look_yaw, look_yaw_valid, delta_time);
        }
        const Vector3 anchor_origin = body_origin - turn_hold;
        // Publish the live REAL eye height (FP eye above the body root) for the overlay read-out and as the
        // dynamic-eye-sync source. eye_position carries head bob; body_origin is the bob-free feet origin.
        if (body_valid)
        {
            cam.real_eye_height.store(eye_position.z - body_origin.z, std::memory_order_relaxed);
        }
        // The eye anchor (EyeHeight 0, or no body) takes the turn hold too, so switching anchor mode with a hold in
        // place does not jump; the hold then eases out as usual.
        Vector3 anchor_base = eye_position - turn_hold;
        if (body_valid && eye_height > 0.0f)
        {
            float effective_eye_height = eye_height;
            bool sync_engaged = false; // true while actively re-anchoring (vs. just using the configured height)
            if (cfg.dynamic_eye_sync.load(std::memory_order_relaxed))
            {
                // Re-anchor the eye HEIGHT to the real FP eye when a low pose (kneel / pray, and other poses
                // EyeHeight does not model) drops it OUT OF RANGE of the configured height - where a fixed
                // height floats the camera too high. Within the threshold (head bob / a minor pose change) keep
                // the steady bob-free height; in the re-anchored low poses we accept the bob (they are mostly
                // stationary). Ease so the swap slides; eye_sync_valid resets on suppression so re-engaging snaps.
                constexpr float k_oor_threshold = 0.25f; // meters: a genuine pose drop, not head bob
                constexpr float k_sync_rate = 9.0f;      // ease rate, 1/sec (frame-rate independent below)
                const float real_eye_height = eye_position.z - body_origin.z;
                sync_engaged = std::fabs(real_eye_height - eye_height) > k_oor_threshold;
                const float target_eye_height = sync_engaged ? real_eye_height : eye_height;
                if (!cam.eye_sync_valid)
                {
                    cam.eye_sync_applied = target_eye_height;
                    cam.eye_sync_valid = true;
                }
                else
                {
                    const float k = 1.0f - std::exp(-k_sync_rate * delta_time);
                    cam.eye_sync_applied += (target_eye_height - cam.eye_sync_applied) * k;
                }
                effective_eye_height = cam.eye_sync_applied;
            }
            else
            {
                cam.eye_sync_valid = false; // feature off -> the next engage snaps to the current pose
            }
            // Publish for the overlay status line (actively re-anchoring vs. using the configured height).
            cam.eye_sync_engaged.store(sync_engaged, std::memory_order_relaxed);
            cam.eye_sync_effective.store(effective_eye_height, std::memory_order_relaxed);
            anchor_base = anchor_origin + world_up * effective_eye_height;
        }
        else
        {
            cam.eye_sync_engaged.store(false, std::memory_order_relaxed);
        }
        const Vector3 pivot = anchor_base + world_up * cfg.offset_up.load(std::memory_order_relaxed);

        // Free-look orbit angles (degrees), accumulated by the input hook while the key is
        // held. On release they ease back to center. Read here so the render reflects them.
        const bool orbit_held = cam.orbit_active.load(std::memory_order_relaxed);
        // Gamepad right-stick orbit: integrate the latched stick DEFLECTION by RATE into the SAME orbit
        // accumulator the mouse writes, so the stick orbits the camera (yaw) and raises/lowers it (pitch) like
        // the mouse. Done here (not in the input hook) because the stick reports a HELD position, so it needs
        // delta_time to be a frame-rate-independent turn rate and to keep moving while held steady. Yaw matches
        // the mouse (stick-right orbits right); the right-stick Y reads OPPOSITE the mouse look delta, so it is
        // negated below (stick-up raises the camera, like the mouse). GamepadOrbitSpeed X/Y (deg/sec at full stick,
        // one rate per axis so each can be inverted with a negative value) sets the rate - a SEPARATE knob from the
        // mouse's OrbitSensitivity X/Y, because a relative mouse (delta) and an absolute stick (rate) cannot share
        // one linear scale; the pitch is clamped to the same limits.
        // Gated on the cursor-shown freeze (a UI up holds
        // still, mirroring the mouse path); when not orbiting or frozen the latch is cleared so re-engaging with
        // the stick centred does not jump.
        if (orbit_held && !s_cursor_shown.load(std::memory_order_relaxed))
        {
            // The preset's response curve, then its smoothing: a curve of 1 and 0 s use the latched deflection as it
            // is. A deflection easing back to a centred stick lands on 0 below k_pad_rest instead of turning the
            // orbit ever more slowly (at 720 deg/s it is under 0.1 deg/s).
            constexpr float k_pad_rest = 1e-4f;
            const StickDeflection shaped =
                shape_stick({cam.orbit_pad_yaw.load(std::memory_order_relaxed),
                             cam.orbit_pad_pitch.load(std::memory_order_relaxed)},
                            std::clamp(cfg.gamepad_orbit_curve.load(std::memory_order_relaxed), 1.0f, 3.0f));
            const float pad_alpha =
                spring_alpha(cfg.gamepad_orbit_smoothing.load(std::memory_order_relaxed), delta_time);
            const auto ease_axis = [pad_alpha](Spring<float> &eased, float target) noexcept
            {
                eased.step(target, pad_alpha);
                if (target == 0.0f && std::fabs(eased.applied) < k_pad_rest && std::fabs(eased.stage1) < k_pad_rest)
                {
                    eased.snap(0.0f);
                }
                return eased.applied;
            };
            const float pad_yaw = ease_axis(cam.orbit_pad_yaw_eased, shaped.x);
            const float pad_pitch = ease_axis(cam.orbit_pad_pitch_eased, shaped.y);
            if (pad_yaw != 0.0f || pad_pitch != 0.0f)
            {
                const float step_x = cfg.gamepad_orbit_speed_x.load(std::memory_order_relaxed) * delta_time;
                const float step_y = cfg.gamepad_orbit_speed_y.load(std::memory_order_relaxed) * delta_time;
                cam.orbit_yaw.store(cam.orbit_yaw.load(std::memory_order_relaxed) - pad_yaw * step_x,
                                    std::memory_order_relaxed);
                const float old_pitch = cam.orbit_pitch.load(std::memory_order_relaxed);
                cam.orbit_pitch.store(
                    std::clamp(old_pitch - pad_pitch * step_y,
                               std::min(cam.orbit_pitch_lo.load(std::memory_order_relaxed), old_pitch),
                               std::max(cam.orbit_pitch_hi.load(std::memory_order_relaxed), old_pitch)),
                    std::memory_order_relaxed);
            }
        }
        else
        {
            cam.orbit_pad_yaw.store(0.0f, std::memory_order_relaxed);
            cam.orbit_pad_pitch.store(0.0f, std::memory_order_relaxed);
            cam.orbit_pad_yaw_eased.snap(0.0f);
            cam.orbit_pad_pitch_eased.snap(0.0f);
        }

        // Wrap the accumulated yaw into [-180, 180] for the orbit math. The orbit is periodic in yaw,
        // so this is invisible while orbiting, but on release it lets the camera ease back to centre
        // along the SHORTEST path (<= 180 deg) in one smooth pass instead of un-spinning every full
        // revolution the player made. The re-based value is only stored back on release (below), so the wrap
        // here stays main-thread-local while held. The raw accumulator itself is written by the mouse path
        // on the input thread and by the gamepad integration above on the main thread; only one input device
        // drives orbit at a time in practice, so the relaxed load here does not race a meaningful write (and a
        // dropped sub-degree delta in the unlikely simultaneous case is harmless, never torn).
        float orbit_yaw_deg = std::remainder(cam.orbit_yaw.load(std::memory_order_relaxed), 360.0f);
        float orbit_pitch_deg = cam.orbit_pitch.load(std::memory_order_relaxed);

        // While moving with a captured heading, the camera holds its WORLD yaw and the body turns UNDER it.
        // Derive the rig's orbit angle as (camera_world_yaw - current_body_forward_yaw), where camera_world_yaw =
        // the captured heading plus any orbit the player has added since capture. On the capture frame the body
        // has not turned yet, so this equals the pre-move orbit (no snap); as the eye+body rotate to the heading
        // the angle eases to the residual orbit, so the camera POSITION is identical across the turn (no pop).
        // forward is this frame's rig basis forward (the stable-aim or smoothed basis when active, else the raw
        // eye look); char_forward_yaw below derives from the SAME forward that positions the camera, so the two
        // cancel and the held world yaw is independent of which source forward tracks. cam.orbit_yaw stays the
        // raw input accumulator throughout; the rendered angle is baked back only on release/stop so free-look resumes
        // from exactly where the moving camera was. orbit_moving implies the key was held, so this also runs on
        // the release frame (orbit_moving is cleared at end of frame), giving the bake-on-release below.
        const bool move_orbit = cam.orbit_moving && cam.orbit_target_valid;
        if (move_orbit)
        {
            const float char_forward_yaw = forward_yaw;
            const float raw_user_since_deg =
                cam.orbit_yaw.load(std::memory_order_relaxed) - cam.orbit_yaw_at_capture_deg;
            // In continuous-align (GTA) mode the user's orbit-since-capture STEERS the run: it drives
            // body_target_yaw (the look + body heading) below, and the camera follows that heading (the
            // derived rig angle cancels to ~0, keeping the camera behind). Smooth that steering input when
            // OrbitSmoothing is on so the turn is fluid instead of tracking the raw per-frame mouse deltas;
            // the SAME smoothed value is reused for body_target_yaw below so the camera stays behind the body.
            // Hold mode keeps the raw value (its orbit-around rides the snapped rig offset, left instant) -
            // only continuous-align steering is smoothed.
            const float orbit_smoothing = std::clamp(cfg.orbit_smoothing.load(std::memory_order_relaxed), 0.0f, 1.0f);
            const bool steer_smooth =
                cfg.orbit_continuous_align.load(std::memory_order_relaxed) && orbit_smoothing > 1e-4f;
            if (!steer_smooth || !cam.orbit_steer_valid)
            {
                cam.orbit_steer_smooth = raw_user_since_deg; // snap: smoothing off / hold mode, or first move frame
                cam.orbit_steer_valid = true;
            }
            else
            {
                const float speed =
                    k_orbit_smooth_max_speed - orbit_smoothing * (k_orbit_smooth_max_speed - k_orbit_smooth_min_speed);
                const float ease = 1.0f - std::exp(-speed * delta_time);
                cam.orbit_steer_smooth += std::remainder(raw_user_since_deg - cam.orbit_steer_smooth, 360.0f) * ease;
            }
            const float user_orbit_since_deg = steer_smooth ? cam.orbit_steer_smooth : raw_user_since_deg;
            orbit_yaw_deg = std::remainder(
                DMK::math::radians_to_degrees(cam.orbit_target_yaw - char_forward_yaw) + user_orbit_since_deg, 360.0f);
        }
        else
        {
            cam.orbit_steer_valid = false; // not move-orbiting: the next engage snaps the steer low-pass
        }

        // Riding with free-look on, the camera turns back behind the mount as it moves (OrbitFollowSpeed).
        const bool riding = (game_state_mask().load(std::memory_order_relaxed) &
                             (state_bit(GameState::Mount) | state_bit(GameState::Cart))) != 0u;
        follow_mount_heading(cam, cfg, forward_yaw, body_yaw, orbit_held && riding && body_valid, move_orbit,
                             delta_time, orbit_yaw_deg);

        if (!orbit_held)
        {
            // Orbit toggle released. When a move-orbit was active, orbit_yaw_deg already holds the rendered
            // (world-stable) angle derived above, not the raw input accumulator, so the ease-back below starts
            // from where the camera actually is rather than jumping by the orbit held at capture. The single
            // store at the end of this block persists the re-based (and, with a return speed, eased) angle.
            const float return_speed = cfg.orbit_return_speed.load(std::memory_order_relaxed);
            if (return_speed > 0.0f)
            {
                // Ease back to the centre: directly behind and level (hardcoded 0, 0).
                const float init_yaw = 0.0f;
                const float init_pitch = 0.0f;
                const float ease = 1.0f - std::exp(-return_speed * delta_time);
                orbit_yaw_deg -= std::remainder(orbit_yaw_deg - init_yaw, 360.0f) * ease; // shortest path to centre
                orbit_pitch_deg -= (orbit_pitch_deg - init_pitch) * ease;
            }
            // Store the re-based (and, with a return speed, eased) angles so the next frame resumes from the
            // short-path value; also runs with return_speed == 0 so the held-spin accumulator is bounded the
            // moment the key is released.
            cam.orbit_yaw.store(orbit_yaw_deg, std::memory_order_relaxed);
            cam.orbit_pitch.store(orbit_pitch_deg, std::memory_order_relaxed);
        }

        // Temporal low-pass on the orbit angle. orbit_yaw_deg / orbit_pitch_deg above are the raw target
        // (the accumulated mouse input, or the eased return / world-stable move derivation); the rig is
        // built from a smoothed copy that eases toward it each frame so free-look is not as jittery as the
        // unfiltered per-frame deltas (the engine's own look smoothing runs past the dispatch we block).
        // orbit_smoothing 0 = off (snap, raw). Snap on the first engaged frame (orbit_render_valid false) so
        // the camera does not slide in from centre. SNAP ALSO while move-orbiting (a movement key is driving
        // the camera-relative turn): there orbit_yaw_deg is the WORLD-STABLE derivation that cancels the body
        // turn to keep the camera planted, so it must track the body 1:1 - smoothing it would lag the
        // cancellation and swing the camera on the move-start, when it should be instant. The target writes
        // above always use the unsmoothed value, so the accumulator and the return/move logic are unaffected;
        // only the idle (standing free-look) rendered angle is low-passed.
        const float orbit_smoothing = std::clamp(cfg.orbit_smoothing.load(std::memory_order_relaxed), 0.0f, 1.0f);
        if (orbit_smoothing <= 1e-4f || !cam.orbit_render_valid || move_orbit)
        {
            cam.orbit_yaw_render = orbit_yaw_deg;
            cam.orbit_pitch_render = orbit_pitch_deg;
            cam.orbit_render_valid = true;
        }
        else
        {
            const float smooth_speed =
                k_orbit_smooth_max_speed - orbit_smoothing * (k_orbit_smooth_max_speed - k_orbit_smooth_min_speed);
            const float smooth_ease = 1.0f - std::exp(-smooth_speed * delta_time);
            // Shortest-path ease on yaw so a wrap (e.g. -179 -> +179) eases the short way, not all the way round.
            cam.orbit_yaw_render += std::remainder(orbit_yaw_deg - cam.orbit_yaw_render, 360.0f) * smooth_ease;
            cam.orbit_pitch_render += (orbit_pitch_deg - cam.orbit_pitch_render) * smooth_ease;
        }
        const float render_yaw_deg = cam.orbit_yaw_render;
        const float render_pitch_deg = cam.orbit_pitch_render;

        // "Orbiting" while EITHER the raw target or the still-easing rendered angle is off-centre, so the
        // orbit block keeps running until the smoothed angle has fully settled back (otherwise a release
        // would cut the rig straight to centre before the low-pass finishes).
        const bool orbiting = orbit_yaw_deg < -0.05f || orbit_yaw_deg > 0.05f || orbit_pitch_deg < -0.05f ||
                              orbit_pitch_deg > 0.05f || render_yaw_deg < -0.05f || render_yaw_deg > 0.05f ||
                              render_pitch_deg < -0.05f || render_pitch_deg > 0.05f;

        // While free-look is on, the orbit is built from a held base: the follow rig for the look pitch it had when
        // free-look took over, so the camera stays put whatever the look pitch does meanwhile. Until the base is held
        // its pitch follows the look, so on the frame free-look takes over the held base is the pose on screen and is
        // taken at once; easing it in lets the live look (leveled by OrbitLevelAim meanwhile) drag the camera.
        // When free-look turns off, the held pitch eases back onto the look by angle, riding on the look at a
        // decaying offset, so the rig is always a real follow rig: free-look turned back on during the ease holds it
        // where it is, and the live rig takes over once the two meet. The hold keys on orbit_held, not on
        // "orbiting": with OrbitReturnSpeed 0 the angles stay and "orbiting" latches true, which pins the camera
        // to the held base and lock out the look pitch. Easing back on release lets a retained orbit angle ride as a
        // rigid offset on top of the real look, so vertical control is kept in every preset.
        {
            constexpr float k_orbit_base_speed = 8.0f;
            // The ease back ends once the held pitch is this close to the look's.
            constexpr float k_orbit_base_settled = 1e-3f; // rad
            if (!cam.orbit_base_held)
            {
                cam.orbit_base_pitch = forward_pitch;
                cam.orbit_base_offset = 0.0f;
                cam.orbit_base_held = orbit_held;
            }
            else if (orbit_held)
            {
                cam.orbit_base_offset = cam.orbit_base_pitch - forward_pitch;
            }
            else
            {
                cam.orbit_base_offset *= std::exp(-k_orbit_base_speed * delta_time);
                cam.orbit_base_pitch = forward_pitch + cam.orbit_base_offset;
                cam.orbit_base_held = std::fabs(cam.orbit_base_offset) >= k_orbit_base_settled;
            }
        }

        // The real aim is driven AFTER the orbit look is finalized (below), so the heading can be aligned
        // to the camera's forward direction on the idle -> moving edge.

        // The follow rig is a spring arm on the look: the camera sits `distance` back from the pivot along the look and
        // the shoulder offset to its right, and it looks along the look itself, like the first-person view carried back
        // and to the side. The screen-centre crosshair is therefore parallel to the player's aim line, a shoulder
        // offset beside it. A walk the way the player looks moves the camera along its own view axis, so what sits
        // under the crosshair stays there as it does in first person, and a change of the follow distance slides the
        // camera straight along its look. Ranged shots and the interaction ray are re-aimed onto the crosshair ray by
        // their own hooks, so the view is not toed in toward the aim line: a toe-in turns the view as the follow
        // distance changes and slides the crosshair off a target the player walks toward. The orbit rotation is
        // applied on top, not folded into this base, so the orbit stays instantly responsive and never accumulates
        // into the follow position. The follow is instant: the anchor is the de-bobbed body origin, so there is no
        // shake to smooth out.
        Vector3 camera_position = pivot - forward * distance + lateral_offset;
        Vector3 look_forward = forward;
        Vector3 look_up = up;
        bool rebuild_basis = false;

        // Static follow angle: turn the RESTING rig about the pivot by FollowYaw / FollowPitch (degrees, both 0 =
        // directly behind and level) with the same spring-arm turn as the free-look orbit below. Positive yaw swings
        // the camera to one side, positive pitch raises it and tilts the look down. The offset and the look turn
        // together, so the over-the-shoulder framing holds while the camera circles you. The free-look orbit then turns
        // further on top of this resting angle, and its held base is turned the same way.
        const float follow_yaw = cfg.follow_yaw.load(std::memory_order_relaxed);
        const float follow_pitch = cfg.follow_pitch.load(std::memory_order_relaxed);
        const bool follow_turned =
            follow_yaw < -0.05f || follow_yaw > 0.05f || follow_pitch < -0.05f || follow_pitch > 0.05f;
        const float follow_yaw_rad = DMK::math::degrees_to_radians(follow_yaw);
        const float follow_pitch_rad = DMK::math::degrees_to_radians(follow_pitch);
        if (follow_turned)
        {
            Vector3 off = camera_position - pivot;
            turn_rig(off, look_forward, look_up, follow_yaw_rad, follow_pitch_rad);
            camera_position = pivot + off;
            rebuild_basis = true;
        }

        // Aim hand-off. When a situation in OrbitExcludeState (aiming, a distraction throw, a drawn weapon, a menu)
        // turns free-look off while the camera is turned away from the look, the camera swings back behind the old
        // look, and what starts there (a shot, a throw) starts where the character faced instead of where the
        // camera points. So the look and the body are turned to the camera's direction (the follow-up below the orbit
        // rig), and the orbit rig holds the camera where it was until the look shows it. The direction comes from the
        // previous frame, the one on screen, measured against that frame's own plain rig.
        if (cam.aim_handoff_requested && !orbit_held && !cam.aim_handoff_active && cam.rig_last_orbited &&
            cam.orbit_suspended.load(std::memory_order_relaxed) &&
            (std::fabs(cam.rig_last_turn) > k_aim_handoff_min_angle ||
             std::fabs(cam.rig_last_rise) > k_aim_handoff_min_angle))
        {
            cam.aim_handoff_active = true;
            cam.aim_handoff_azimuth = cam.rig_last_azimuth;
            cam.aim_handoff_elevation = cam.rig_last_elevation;
            cam.aim_handoff_yaw = cam.rig_last_yaw;
            cam.aim_handoff_pitch = cam.rig_last_pitch;
            cam.aim_handoff_seconds = 0.0f;
            cam.aim_handoff_written = false;
            cam.aim_handoff_player = c_player;
            // The plain rig takes over at once; the hold keeps the camera in place across the switch.
            cam.orbit_base_held = false;
            (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                     "Orbit: free-look turned off by a situation; the look takes the camera "
                                     "direction, turning {:.1f} deg, pitch {:.1f} to {:.1f} deg",
                                     DMK::math::radians_to_degrees(cam.rig_last_turn),
                                     DMK::math::radians_to_degrees(forward_pitch),
                                     DMK::math::radians_to_degrees(cam.rig_last_pitch));
        }
        const Vector3 plain_look = look_forward;

        // Free-look orbit: turn the WHOLE non-orbit rig (the camera's offset from the pivot, its look and its up)
        // rigidly about the pivot with turn_rig - yaw about world up, then pitch about the look heading's horizontal
        // axis. The offset (shoulder + height + distance) turns with the look, so the over-the-shoulder framing and the
        // crosshair keep their screen positions while the camera circles the player, and the turned rig is exactly the
        // follow rig of the turned look: a zoom while orbited still slides the camera straight along its look. Yaw is
        // unbounded so full spins work. The pitch is tracked as the arm's elevation (how far the rig is raised: the
        // negative of its look pitch), clamped away from the pole. At zero angle the rotation is identity, so the orbit
        // engages without a jump. Mouse up/down raises/lowers the camera.
        const bool orbit_rig = orbiting || cam.orbit_base_held || cam.aim_handoff_active;
        if (orbit_rig)
        {
            // The base the orbit turns: the live follow pose (the player's look pitch included), or the held base. The
            // held base is the follow rig built the same way as above for orbit_base_pitch, with this frame's heading,
            // distance, shoulder and follow angle, so a zoom or a preset blend still reshapes it. The height stays in
            // the pivot.
            Vector3 offset0 = camera_position - pivot;
            Vector3 base_look = look_forward;
            Vector3 base_up = look_up;
            if (cam.orbit_base_held)
            {
                Vector3 heading{forward.x, forward.y, 0.0f};
                if (heading.magnitude_squared() > 1e-6f)
                {
                    heading = heading.normalized();
                }
                else
                {
                    heading = world_up.cross(right).normalized(); // straight down/up: recover heading from right
                }
                const Vector3 held_fwd =
                    heading * std::cos(cam.orbit_base_pitch) + world_up * std::sin(cam.orbit_base_pitch);
                const Vector3 held_right = heading.cross(world_up).normalized();
                offset0 = held_fwd * (-distance) + held_right * offset_right;
                base_look = held_fwd;
                base_up = held_right.cross(held_fwd);
                if (follow_turned)
                {
                    turn_rig(offset0, base_look, base_up, follow_yaw_rad, follow_pitch_rad);
                }
            }

            const float base_elevation = -std::asin(std::clamp(base_look.z, -1.0f, 1.0f));
            // The elevation stays off the overhead pole; a held base already past that limit (a steep look) is kept
            // but cannot go further.
            const float elevation_min = std::min(-1.45f, base_elevation);
            const float elevation_max = std::max(1.45f, base_elevation);
            float elevation = std::clamp(base_elevation + DMK::math::degrees_to_radians(render_pitch_deg),
                                         elevation_min, elevation_max);
            // The pitch range for the next input, from level: OrbitPitchMin / OrbitPitchMax less the held base's
            // elevation, widened to include the base itself (reachable even when it starts outside the range) and
            // kept within the elevation the rig can show.
            if (orbit_held && !cam.aim_handoff_active)
            {
                const float base_deg = DMK::math::radians_to_degrees(base_elevation);
                const float lo =
                    std::max(std::min(cfg.orbit_pitch_min.load(std::memory_order_relaxed) - base_deg, 0.0f),
                             DMK::math::radians_to_degrees(elevation_min) - base_deg);
                const float hi =
                    std::min(std::max(cfg.orbit_pitch_max.load(std::memory_order_relaxed) - base_deg, 0.0f),
                             DMK::math::radians_to_degrees(elevation_max) - base_deg);
                cam.orbit_pitch_lo.store(lo, std::memory_order_relaxed);
                cam.orbit_pitch_hi.store(hi, std::memory_order_relaxed);
            }
            float yaw_delta = DMK::math::degrees_to_radians(render_yaw_deg);
            if (cam.aim_handoff_active)
            {
                // Aim hand-off: keep the camera in the direction it had when the situation took over, whatever the look
                // and the basis do meanwhile, and leave the matching orbit angles behind so the ease back, or free-look
                // turned back on before the look has turned (a paused menu closed), resumes here.
                yaw_delta =
                    std::remainder(cam.aim_handoff_azimuth - std::atan2(-base_look.x, base_look.y), DirectX::XM_2PI);
                elevation = std::clamp(cam.aim_handoff_elevation, elevation_min, elevation_max);
                const float held_yaw_deg = DMK::math::radians_to_degrees(yaw_delta);
                const float held_pitch_deg = DMK::math::radians_to_degrees(elevation - base_elevation);
                cam.orbit_yaw.store(held_yaw_deg, std::memory_order_relaxed);
                cam.orbit_pitch.store(held_pitch_deg, std::memory_order_relaxed);
                cam.orbit_yaw_render = held_yaw_deg;
                cam.orbit_pitch_render = held_pitch_deg;
                cam.orbit_render_valid = true;
            }

            turn_rig(offset0, base_look, base_up, yaw_delta, elevation - base_elevation);
            camera_position = pivot + offset0;
            look_forward = base_look;
            look_up = base_up;
            rebuild_basis = true;
        }

        // While the orbit rig shapes the camera, record for the next frame's aim hand-off its look heading and arm
        // elevation, its turn and rise off this frame's plain rig, and the player look that puts the plain rig on it.
        // All are measured within one frame, so a preset blend or a basis turn between frames does not count as orbit.
        // The orbit and the follow angle both turn the rig as a spring arm, which adds to the look's yaw and pitch, so
        // that player look is this frame's look turned by the yaw and pitch of the orbited look off the plain look.
        cam.rig_last_orbited = false;
        if (orbit_rig)
        {
            const float rig_pitch = std::asin(std::clamp(look_forward.z, -1.0f, 1.0f));
            const float plain_pitch = std::asin(std::clamp(plain_look.z, -1.0f, 1.0f));
            cam.rig_last_azimuth = std::atan2(-look_forward.x, look_forward.y);
            cam.rig_last_elevation = -rig_pitch;
            cam.rig_last_turn =
                std::remainder(cam.rig_last_azimuth - std::atan2(-plain_look.x, plain_look.y), DirectX::XM_2PI);
            cam.rig_last_rise = plain_pitch - rig_pitch;
            cam.rig_last_pitch =
                std::clamp(forward_pitch + rig_pitch - plain_pitch, -k_aim_handoff_max_pitch, k_aim_handoff_max_pitch);
            cam.rig_last_yaw = std::remainder(forward_yaw + cam.rig_last_turn, DirectX::XM_2PI);
            cam.rig_last_orbited = true;
        }

        // Aim hand-off follow-up. While aiming the game re-derives the look heading from the body every frame (the body
        // follows the look), so the look alone cannot be turned: the body is turned with it, as camera-relative
        // movement does, and both are written every frame until the look shows the new heading. They are written at
        // least once, so a hand-off that only raises or lowers the camera still sets the pitch, which sticks, and it
        // can end only on a later frame than its first write: on the same frame's second build the eye pose does not
        // show the write yet. Until then the next frame's basis snaps to the look, so the rig lands on the new
        // direction at once instead of easing across the turn under AimBasisSmoothing.
        if (cam.aim_handoff_active)
        {
            const bool lost = cam.aim_handoff_cancel || c_player == 0 || c_player != cam.aim_handoff_player;
            const bool jumped = body_valid && cam.turn_track_step > k_aim_handoff_jump;
            const Vector3 look_now = (look_valid ? look_rotation : eye_rotation).rotate(Vector3{0.0f, 1.0f, 0.0f});
            const float yaw_gap =
                std::remainder(std::atan2(-look_now.x, look_now.y) - cam.aim_handoff_yaw, DirectX::XM_2PI);
            if (lost)
            {
                cam.aim_handoff_active = false;
                cam.aim_handoff_cancel = false;
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "Orbit: the hand-off ended without turning the look (the game sets the "
                                         "facing here, or the player changed)");
            }
            else if (cam.aim_handoff_written && !step.repeat_build && std::fabs(yaw_gap) < k_aim_handoff_min_angle)
            {
                cam.aim_handoff_active = false;
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "Orbit: the look took the camera direction after {:.2f} s",
                                         cam.aim_handoff_seconds);
            }
            else if (jumped)
            {
                cam.aim_handoff_active = false;
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "Orbit: the hand-off ended without turning the look (the player was moved)");
            }
            else if (cam.aim_handoff_seconds >= k_aim_handoff_timeout)
            {
                cam.aim_handoff_active = false;
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "Orbit: the look did not take the camera direction ({:.1f} deg left after "
                                         "{:.2f} s); the camera eases back",
                                         DMK::math::radians_to_degrees(yaw_gap), cam.aim_handoff_seconds);
            }
            else if (!apply_orbit_aim_control(1.0f, cam.aim_handoff_pitch, true, cam.aim_handoff_yaw))
            {
                cam.aim_handoff_active = false;
                (void)DMK::log().try_log(DMK::LogLevel::Debug, "Orbit: the look write failed; the camera eases back");
            }
            else
            {
                apply_orbit_body_turn(cam.aim_handoff_yaw);
                cam.aim_handoff_written = true;
                cam.basis_quat_valid = false;
                // The pause menu stops the player update, so the look shows the new heading only once play resumes;
                // the wait does not count paused time.
                if (!step.paused)
                {
                    cam.aim_handoff_seconds += delta_time;
                }
            }
        }

        // Camera-relative movement (toggle orbit): on the idle -> moving edge CAPTURE the camera heading, then
        // HOLD it while moving. The body turn (apply_orbit_body_turn) pins the body to that heading, which is
        // what makes locomotion camera-relative in EVERY direction (KCD2 moves relative to the body rotation):
        // W runs away from the camera, A/D strafe to the sides, S backs toward it. The look-yaw write
        // (apply_orbit_aim_control) faces the head/aim the same way and feeds the camera-position derivation.
        // Both are re-applied every frame because the engine reverts/consumes a single write - like the pitch
        // leveling. Released when movement stops.
        bool do_align = false;
        // Camera-relative movement, keyed on the device-agnostic action-input intent
        // (player_onaction_move_magnitude, from the action dispatcher). The input is nonzero the instant a movement
        // key is pressed - BEFORE the body accelerates - so the heading captures immediately and the
        // character turns to the camera direction without first moving the old way; and it stays nonzero while
        // a key is held against a wall, so the heading is not falsely released on a collision arrest. The
        // body-position speed is deliberately NOT used (it lags the intent and reads zero when arrested), so
        // the feature requires the action hook - if it did not resolve, camera-relative movement stays off
        // (free-look still works).
        if (orbit_held && body_valid && player_onaction_available())
        {
            bool moving = cam.orbit_moving;
            const float move_magnitude = player_onaction_move_magnitude();
            // Re-arm guard: a genuine release (magnitude below the stop threshold) must be observed since orbit
            // engaged before a move-start is honored. A stranded latch (a held-move release swallowed on a
            // combat action-map swap, see player_onaction_reset) reads > 0 with the keys up; without this guard
            // it re-trips orbit_moving the instant orbit restores and drives the body-turn with no input (the
            // post-combat self-rotation). Arming on a sub-stop reading means a fresh, observed press engages it.
            // A key held while orbit restores (running on right after leaving crouch) is no stranded latch: the
            // game's own move input shows it too, so that also arms the move. A stranded latch has the keys up and
            // the game's input at 0, so the guard still holds it. A move armed that way also ends when the game's
            // input goes idle, as another key's slot can still be stranded and hold the latch up.
            static bool s_stale_suppress_logged = false;
            const std::optional<float> game_move = player_move_input(c_player);
            if (move_magnitude < k_orbit_move_input_stop)
            {
                cam.orbit_move_armed = true;
                cam.orbit_move_armed_by_game = false;
                s_stale_suppress_logged = false;
            }
            else if (!cam.orbit_move_armed && game_move && *game_move > MOVE_INPUT_START)
            {
                cam.orbit_move_armed = true;
                cam.orbit_move_armed_by_game = true;
                cam.orbit_move_game_idle = 0.0f;
                s_stale_suppress_logged = false;
                (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                         "Orbit: move held as orbit engaged (magnitude {:.2f}, the game's own move "
                                         "input agrees); armed",
                                         move_magnitude);
            }
            // A hand-off still turning the look and the body (free-look back on before it ends) is not interrupted.
            if (!moving && !cam.aim_handoff_active && move_magnitude > MOVE_INPUT_START)
            {
                if (cam.orbit_move_armed)
                {
                    moving = true;
                    do_align = true; // idle -> moving edge: capture the camera heading
                    // Diagnostic: what tripped the body-turn. move_magnitude near 1.0 with a movement key/stick =
                    // genuine locomotion; a smaller or unexpected value while the player is only free-looking
                    // points at game-driven movement (finishing-move / combat footwork) being mistaken for intent.
                    // state is the debounced GameState mask (see game_state.hpp) so we can tell whether a combat /
                    // aiming / cinematic state was active when the body-turn engaged.
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Trace,
                        "Orbit: move-orbit START (body-turn engaging) - move_magnitude={:.2f}, continuous_align={}, "
                        "state_mask=0x{:X}",
                        move_magnitude, cfg.orbit_continuous_align.load(std::memory_order_relaxed),
                        game_state_mask().load(std::memory_order_relaxed));
                }
                else if (!s_stale_suppress_logged)
                {
                    // Unarmed (no release seen since orbit engaged) yet reading as movement: a stranded latch.
                    // Suppress the body-turn re-trip and log it once - the diagnostic for the post-combat case.
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Trace,
                        "Orbit: suppressed a stale move re-trip (magnitude {:.2f}, no release observed since orbit "
                        "engaged; likely a movement latch stranded by a combat action-map swap)",
                        move_magnitude);
                    s_stale_suppress_logged = true;
                }
            }
            else if (moving && move_magnitude < k_orbit_move_input_stop)
            {
                // Release the latch the instant the movement keys are let go (no debounce tail), so stopping
                // and then rotating is immediately pure free-look and the character does not keep turning to
                // the camera. No bridge is needed the way body-speed required: the action input stays nonzero
                // while any key is held (diagonals, overlapping key-switches), so a drop to zero is a genuine
                // stop; a full release-then-repress simply re-captures the heading on the new press, which is
                // the correct behaviour.
                moving = false;
                // Bake the world-stable orbit angle back into the accumulator so free-look resumes from
                // exactly where the moving camera was, with no jump the instant movement stops.
                cam.orbit_yaw.store(orbit_yaw_deg, std::memory_order_relaxed);
                (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                         "Orbit: move-orbit STOP (body-turn releasing) - move_magnitude={:.2f}",
                                         move_magnitude);
            }
            else if (moving && cam.orbit_move_armed_by_game)
            {
                // Armed by the game's own input: it ends once that input has stayed idle, even while another latch
                // slot is stranded. Disarmed rather than reset, so a stranded slot stays held off by the guard and a
                // key still held re-arms through the game's input on the next frame.
                cam.orbit_move_game_idle =
                    (game_move && *game_move < k_orbit_move_input_stop) ? cam.orbit_move_game_idle + delta_time : 0.0f;
                if (cam.orbit_move_game_idle >= k_orbit_move_game_stop_delay)
                {
                    moving = false;
                    cam.orbit_move_armed = false;
                    cam.orbit_move_armed_by_game = false;
                    cam.orbit_yaw.store(orbit_yaw_deg, std::memory_order_relaxed);
                    (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                             "Orbit: move-orbit STOP (the game's own move input is idle; magnitude "
                                             "{:.2f} is a stranded latch)",
                                             move_magnitude);
                }
            }
            cam.orbit_moving = moving;
        }
        else
        {
            cam.orbit_moving = false;
            cam.orbit_move_armed = false; // re-armed by a release, or by a held move the game's input shows
            cam.orbit_move_armed_by_game = false;
        }

        // Capture the heading on move-start: the eye-look yaw plus the orbit applied this frame, the yaw the
        // world-stable orbit derivation above renders, so the camera stays put as the body turns. It is built from
        // the angles rather than atan2(look_forward), which also carries the static follow yaw.
        if (do_align)
        {
            cam.orbit_target_yaw = forward_yaw + DMK::math::degrees_to_radians(cam.orbit_yaw_render);
            cam.orbit_target_valid = true;
            // Snapshot the orbit input so further orbiting while moving is measured from here. We do NOT
            // reset the orbit to 0: the world-stable derivation above holds the camera in place while the
            // body turns to this heading, so there is no snap-behind pop.
            cam.orbit_yaw_at_capture_deg = cam.orbit_yaw.load(std::memory_order_relaxed);
            // The look is turned to this heading below, so the next frame's basis snaps to it instead of easing
            // across the turn (see orbit_move_pressed).
            cam.basis_quat_valid = false;
        }
        if (!cam.orbit_moving)
        {
            cam.orbit_target_valid = false;
        }
        // The heading a move press captures, for orbit_move_pressed: the camera's yaw on screen (with the
        // orbit as rendered, so a smoothed angle still catching up does not snap), published only while a press
        // starts a camera-relative move with the body turn, and stamped so a stale one is never used.
        if (orbit_held && body_valid && player_onaction_available() && !cam.orbit_moving && cam.orbit_move_armed &&
            !cam.aim_handoff_active && cfg.orbit_body_turn.load(std::memory_order_relaxed))
        {
            s_move_press_heading.store(forward_yaw + DMK::math::degrees_to_radians(cam.orbit_yaw_render),
                                       std::memory_order_relaxed);
            s_move_press_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
            s_move_press_body_yaw.store(body_yaw, std::memory_order_relaxed);
            s_move_press_stamp_ms.store(GetTickCount64(), std::memory_order_release);
        }
        else
        {
            s_move_press_stamp_ms.store(0, std::memory_order_release);
        }

        const float pitch_ease = (orbit_held && cfg.orbit_level_aim.load(std::memory_order_relaxed))
                                     ? (1.0f - std::exp(-k_orbit_aim_level_speed * delta_time))
                                     : 0.0f;
        const bool hold_yaw = orbit_held && cam.orbit_moving && cam.orbit_target_valid;
        // Heading fed to the body + movement while moving. CONTINUOUS-ALIGN (GTA style): follow the CURRENT
        // camera world yaw every frame (captured heading + the orbit added since), so orbiting STEERS the run
        // and the rig stays behind the character. Otherwise HOLD the heading captured on move-start (orbit
        // looks around freely while the character keeps its line). The world-stable camera derivation above is
        // the same either way - only this target differs - so in continuous mode the derived orbit angle
        // cancels to ~0 (camera behind the character) while in hold mode it carries the orbit-around offset.
        float body_target_yaw = cam.orbit_target_yaw;
        if (hold_yaw && cfg.orbit_continuous_align.load(std::memory_order_relaxed))
        {
            // Reuse the smoothed steer angle computed in the move-orbit derivation above (when OrbitSmoothing
            // is on) so the body/look heading and the rig agree - the camera follows the smoothed steering
            // and stays behind. With smoothing off this is the raw orbit-since-capture. The smoothed value is
            // only current once the move-orbit block above has run for it (cam.orbit_steer_valid); on the
            // idle->moving capture frame that block did NOT run (move_orbit read the pre-update orbit_moving,
            // which was still false), so fall back to the raw orbit-since-capture - ~0 on the capture frame
            // because orbit_yaw_at_capture_deg was just snapshotted - instead of a stale smoothed value from a
            // prior move. orbit_smoothing was read for the orbit-angle low-pass above and is constant across
            // the frame, so it is reused here.
            const float user_orbit_since_deg =
                (orbit_smoothing > 1e-4f && cam.orbit_steer_valid)
                    ? cam.orbit_steer_smooth
                    : (cam.orbit_yaw.load(std::memory_order_relaxed) - cam.orbit_yaw_at_capture_deg);
            body_target_yaw = cam.orbit_target_yaw + DMK::math::degrees_to_radians(user_orbit_since_deg);
        }
        // The hand-off writes the look itself while it runs.
        if (orbit_held && !cam.aim_handoff_active && (pitch_ease > 0.0f || hold_yaw))
        {
            (void)apply_orbit_aim_control(pitch_ease, 0.0f, hold_yaw, body_target_yaw);
        }
        // Pin the BODY to the camera heading while moving so the character runs camera-relative in EVERY
        // direction. KCD2 moves the player relative to the ENTITY (body) rotation, NOT the look, so this body
        // override - not the look-yaw write above - is what redirects locomotion: with the body faced at the
        // camera heading, W runs away from the camera, A/D strafe to the sides and S backs toward it. The body
        // must hold the CAMERA heading, not the input direction: facing it at the movement direction would
        // rotate the move frame and re-apply the input on top of it (world_move = body + input), sending every
        // key the wrong way (A/D -> backward, S -> forward). Held every frame while moving (the override is
        // consume-once); released when movement stops.
        static bool s_body_turn_engaged = false;
        const bool body_turn_active = hold_yaw && cfg.orbit_body_turn.load(std::memory_order_relaxed);
        if (body_turn_active != s_body_turn_engaged)
        {
            s_body_turn_engaged = body_turn_active;
            if (body_turn_active)
            {
                (void)DMK::log().log_noexcept(DMK::LogLevel::Trace,
                                              "Camera: orbit body-turn ENGAGED (moving; heading locked to camera)");
            }
            else
            {
                (void)DMK::log().log_noexcept(DMK::LogLevel::Trace, "Camera: orbit body-turn released");
            }
        }
        if (body_turn_active)
        {
            apply_orbit_body_turn(body_target_yaw);
        }

        // Camera collision and the occluder fade (see camera/collision.hpp).
        const bool fade_on = cfg.use_occluder_fade.load(std::memory_order_relaxed) && occluder_fade_available();
        const bool close_fade = cfg.close_up_fade.load(std::memory_order_relaxed) && character_fade_available();
        if (!step.repeat_build)
        {
            // One attachment walk serves the occluder scan's exclusion of what the character carries and the close-up
            // fade. Bone-bound items need not have an IEntity parent, so the actor registry alone cannot identify them.
            if (entity_addr != 0 && (close_fade || fade_on || cfg.enable_collision.load(std::memory_order_relaxed)))
            {
                (void)collect_character_nodes(entity_addr, s_character_nodes);
            }
            else if (s_character_nodes.entity != 0)
            {
                clear_character_nodes(s_character_nodes);
            }
            set_occluder_player_nodes(std::span<const std::uintptr_t>(
                s_character_nodes.nodes.data(), static_cast<std::size_t>(s_character_nodes.count + 1)));
        }
        // Looking up, Arm Follow Up and Lens Drop Up bend the arm instead of swinging it down on the sphere toward the
        // ground behind the character (see aim_arm_shift()). The shift depends on the rendered look alone, so the aim
        // hand-off and free-look engage do not step.
        camera_position = camera_position + aim_arm_shift(look_forward, look_up, distance,
                                                          cfg.arm_follow_up.load(std::memory_order_relaxed),
                                                          cfg.lens_drop_up.load(std::memory_order_relaxed));

        // Shoulder collision: a wall at the shoulder narrows the shoulder offset first, so the arm below runs from the
        // pivot to a camera nearer the look line and keeps its distance. The offset lies along the finished rig's right
        // (the follow angle and the orbit turn it with the look), toward the side its sign puts it.
        {
            const Vector3 rig_right = look_forward.cross(look_up);
            const float lateral = std::fabs(offset_right);
            if (rig_right.magnitude_squared() > 1e-6f && lateral > 0.0f)
            {
                const Vector3 lateral_dir = rig_right.normalized() * (offset_right < 0.0f ? -1.0f : 1.0f);
                const ShoulderInput shoulder{
                    .pivot = pivot,
                    .direction = lateral_dir,
                    .lateral = lateral,
                    .delta_time = delta_time,
                    .enabled = cfg.shoulder_collision.load(std::memory_order_relaxed) &&
                               cfg.enable_collision.load(std::memory_order_relaxed),
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
            .pivot = pivot,
            .desired = camera_position,
            .world_up = world_up,
            .head = anchor_base,
            .hip = body_valid ? anchor_origin + world_up * k_fade_hip_height : anchor_base - world_up * k_fade_hip_drop,
            .delta_time = delta_time,
            .fade_on = fade_on,
            .wall_class_changed = wall_class_changed,
            .repeat_build = step.repeat_build,
        };
        camera_position = resolve_camera_collision(cam, s_collision, collision_input);

        // Hand the fade set to the dissolve hook once per frame (the frame's second build changes nothing).
        if (!step.repeat_build)
        {
            submit_fade_set(s_collision, delta_time);
        }

        // The arm after collision, measured from the head where it really is: the pivot plus the turn hold, which
        // keeps the pivot off the body while it steps through a turn. The close-up fade and the lens near plane keep
        // their distance from the head by it.
        const float arm_after_collision = (camera_position - (pivot + turn_hold)).magnitude();

        // The trees, bushes and grass, which never fade whole for hiding the character, open a window around him
        // instead (see run_tree_see_through), from the camera the fade's sightlines were cast from and the scan the
        // collision just refreshed. Shut in dialogue and while a menu or an overlay is up.
        if (!step.repeat_build)
        {
            const bool ui_up =
                is_game_menu_open() || overlay_state().active.load(std::memory_order_relaxed) ||
                (game_state_mask().load(std::memory_order_relaxed) & state_bit(GameState::Dialogue)) != 0u;
            run_tree_see_through(camera_position, eye_position, body_origin, body_valid, cam.turn_track_step, !ui_up,
                                 view_blend, delta_time, s_character_nodes);
        }

        // Too close to the character: the close-up fade, or the first-person fallback (see camera/close_fade.hpp).
        const CloseUpInput close_up{
            .camera = camera_position,
            .eye = fpv_position,
            .arm = arm_after_collision,
            .entity = entity_addr,
            .delta_time = delta_time,
            .close_fade = close_fade,
            .repeat_build = step.repeat_build,
        };
        camera_position = apply_close_up(cam, s_character_nodes, close_up);

        // Write the offset position into the camera matrix translation column. The basis columns are
        // left as the engine built them (from the eye quat) unless the follow angle/orbit changed the look,
        // so the camera otherwise stays parallel to the player's view. The caller computes the cull
        // planes from this matrix next, so culling follows the rendered view.
        // Blend the final pose between first person (the engine's eye matrix) and the third-person pose
        // by view_blend so toggling the view eases instead of snapping. At view_blend 1 this is the full
        // third-person pose; at 0 it is the untouched first-person camera.
        const Vector3 final_position = fpv_position + (camera_position - fpv_position) * view_blend;
        matrix->m[0][3] = final_position.x;
        matrix->m[1][3] = final_position.y;
        matrix->m[2][3] = final_position.z;

        // Orientation: only when the follow angle/orbit changed the look (otherwise the engine-built eye basis
        // is already correct at every blend value, so the camera just sits behind the player looking where
        // they look). Ease the look and its up from the eye basis to the rig's by view_blend, then rebuild
        // the column basis (right = forward x up, up = right x forward; col0 = right, col1 = forward,
        // col2 = up) so the orientation transitions smoothly too. The rig's up turned with its look, so an
        // orbit away from a pitched look does not roll the view.
        // Screen-centre forward (the crosshair direction): the turned look when we rebuilt the
        // basis, else the engine eye forward. Published below for the camera-space interaction hook.
        // When the stable / smoothed basis is active, the rendered ORIENTATION must be rebuilt from it too:
        // otherwise the engine eye orientation (which carries the view-shake) would still drive the view
        // rotation even though the position is now stable, leaving the view tilting during an action. Forcing
        // the rebuild here routes the orientation through the same eye->stable blend below. When the basis was
        // NOT overridden, eye_forward == forward and eye_up == up, so this is a no-op and the path stays
        // byte-identical to the follow-angle/orbit-only behaviour.
        if (basis_overridden)
        {
            rebuild_basis = true;
        }
        Vector3 screen_forward = forward;
        if (rebuild_basis)
        {
            // Blend the orientation from the engine EYE basis (FPV side, so view_blend 0 leaves the untouched
            // first-person view) to the stable look (TPV side). At view_blend 0 this reconstructs the engine
            // eye basis exactly (eye_forward x eye_up == eye_right); at view_blend 1 it is the stable look.
            Vector3 blended_forward = eye_forward + (look_forward - eye_forward) * view_blend;
            blended_forward =
                (blended_forward.magnitude_squared() > 1e-6f) ? blended_forward.normalized() : look_forward;
            screen_forward = blended_forward;
            const Vector3 ref_up = eye_up + (look_up - eye_up) * view_blend;
            const Vector3 right_axis = blended_forward.cross(ref_up);
            if (right_axis.magnitude_squared() > 1e-6f)
            {
                const Vector3 new_right = right_axis.normalized();
                const Vector3 new_up = new_right.cross(blended_forward);
                matrix->m[0][0] = new_right.x;
                matrix->m[1][0] = new_right.y;
                matrix->m[2][0] = new_right.z;
                matrix->m[0][1] = blended_forward.x;
                matrix->m[1][1] = blended_forward.y;
                matrix->m[2][1] = blended_forward.z;
                matrix->m[0][2] = new_up.x;
                matrix->m[1][2] = new_up.y;
                matrix->m[2][2] = new_up.z;
            }
        }

        // The lens near plane (see camera/lens.hpp), measured from the final pose written above.
        apply_lens_near_plane(camera, *matrix, arm_after_collision, view_blend, delta_time, step.repeat_build);

        // People and animals at the lens fade instead of being cut open by the near plane (see
        // camera/npc_lens_fade.hpp), once per frame from the final pose. The near corner's ratios are the frustum's
        // half-angle tangents, whatever scale the near plane gave it.
        if (!step.repeat_build)
        {
            const auto *near_corner = reinterpret_cast<const float *>(camera + Constants::CCAMERA_NEAR_CORNER_OFFSET);
            NpcLensFadeInput npc{};
            npc.view.position = {matrix->m[0][3], matrix->m[1][3], matrix->m[2][3]};
            npc.view.right = {matrix->m[0][0], matrix->m[1][0], matrix->m[2][0]};
            npc.view.forward = {matrix->m[0][1], matrix->m[1][1], matrix->m[2][1]};
            npc.view.up = {matrix->m[0][2], matrix->m[1][2], matrix->m[2][2]};
            npc.view.tan_half_width = near_corner[1] > 1e-6f ? std::fabs(near_corner[0] / near_corner[1]) : 0.0f;
            npc.view.tan_half_height = near_corner[1] > 1e-6f ? std::fabs(near_corner[2] / near_corner[1]) : 0.0f;
            npc.player_entity = entity_addr;
            npc.player_origin = body_valid ? body_origin : eye_position;
            npc.real_seconds = step.real_seconds;
            npc.mounted = (game_state_mask().load(std::memory_order_relaxed) & state_bit(GameState::Mount)) != 0u;
            npc.enabled = cfg.npc_lens_fade.load(std::memory_order_relaxed) && character_fade_available() &&
                          view_blend > 0.0f && npc.view.tan_half_width > 0.0f;
            update_npc_lens_fade(npc);
        }

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
                                     stranded, stranded > k_orbit_move_input_stop ? ", WAS STRANDED" : "");
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
        reassert_head_visibility(offset_engaged, c_player);
        // The body's native turn-in-place animation follows the offset too (it follows the look again once the view is
        // back at the eye), and is off while the view rides the first-person eye (the head fallback, or an Eye Height 0
        // preset anchored to it). No turn starts under free-look.
        const bool eye_anchored = head_fallback() || cfg.eye_height.load(std::memory_order_relaxed) <= 0.0f;
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
                if (stranded > k_orbit_move_input_stop)
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Trace,
                        "Orbit: TPV disengaged; cleared a stranded move-latch (magnitude {:.2f})", stranded);
                }
                s_orbit_was_engaged = false;
            }
            reset_collision_carry(cam);
            reset_shoulder_carry(s_collision);
            cam.head_fallback = false;
            cam.head_fallback_blend = 0.0f;
            set_head_fallback(false);
            set_head_close_fade(false);
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
