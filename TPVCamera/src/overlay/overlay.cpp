/**
 * @file overlay.cpp
 * @brief Implements the public TPVCamera::Overlay API on the WARP + GDI host.
 *
 * @details Thin adapter over the dx_overlay backend (Detail namespace): start()
 *          spawns the render thread, stop() tears it down, and the visibility
 *          helpers forward to the backend's atomic state. The per-frame render
 *          loop (begin ImGui frame -> draw_ui() -> render -> composite) lives in
 *          dx_overlay.cpp; this file only owns lifecycle and the input-intent
 *          query. The overlay never sets any GameState/overlay bit so the
 *          third-person camera keeps rendering live while the panel is open.
 */

#include "overlay.hpp"
#include "dx_overlay.hpp"

#include <DetourModKit.hpp>

namespace TPVCamera::Overlay
{

bool start()
{
    DMK::Logger &logger = DMK::Logger::get_instance();
    if (!Detail::dx_start())
    {
        logger.error("[overlay] Failed to start render thread");
        return false;
    }
    logger.info("[overlay] Render thread started");
    return true;
}

void stop()
{
    Detail::dx_stop();
}

void toggle()
{
    Detail::dx_set_visible(!Detail::dx_is_visible());
}

void set_visible(bool visible)
{
    Detail::dx_set_visible(visible);
}

bool is_visible() noexcept
{
    return Detail::dx_is_visible();
}

bool wants_input() noexcept
{
    if (!is_visible() || !Detail::dx_is_ready())
        return false;
    return Detail::dx_wants_input();
}

} // namespace TPVCamera::Overlay
