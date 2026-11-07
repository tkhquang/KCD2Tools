/**
 * @file live_bridge.hpp
 * @brief The Blender add-on's way into the game: a local pipe whose requests run on the main thread.
 *
 * The add-on's "Play in game" and "Stop" send their requests (core/bridge_protocol.hpp) to the pipe
 * `\\.\pipe\KCD2_AnimationFramework`. A worker thread owns the pipe and only reads, checks and answers. Each request
 * that touches the game waits for the frame tick, which runs its Lua through the engine's script system and hands the
 * answer back. The worker never calls the game.
 *
 * The pipe takes local clients only, and its access list admits the user the game runs as, at medium integrity, so a
 * Blender started normally reaches a game started as administrator too.
 */
#ifndef ANIMFW_LIVE_BRIDGE_HPP
#define ANIMFW_LIVE_BRIDGE_HPP

#include <cstdint>

namespace AnimationFramework
{
    /**
     * @brief Starts the pipe's worker. Without a checked script system vtable (anchors.hpp) it starts nothing.
     * @param genv The resolved gEnv.
     * @param script_system_vtable CScriptSystem's primary vtable, checked before every run.
     * @note Setup only. A failure goes to the log, and the rest of the framework runs on.
     */
    void start_live_bridge(std::uintptr_t genv, std::uintptr_t script_system_vtable) noexcept;

    /** @brief Runs the request that waits for the main thread, if any. Main thread, from the frame tick. */
    void run_live_bridge_frame_work() noexcept;

    /**
     * @brief Stops and joins the worker. A request that still waits gets no answer: its client sees the pipe close.
     * @note Teardown only, after the hooks retired, so no frame tick runs a request any more.
     */
    void stop_live_bridge() noexcept;
} // namespace AnimationFramework

#endif // ANIMFW_LIVE_BRIDGE_HPP
