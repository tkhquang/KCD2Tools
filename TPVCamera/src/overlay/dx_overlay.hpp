/**
 * @file dx_overlay.hpp
 * @brief Internal WARP + GDI ImGui host for the TPVCamera overlay module.
 *
 * @details This is the private rendering backend behind TPVCamera::Overlay
 *          (overlay.hpp / overlay.cpp). It owns a D3D11 WARP device, an
 *          offscreen render target, the GDI layered-window composite, the
 *          render thread, the input bridge (GetAsyncKeyState + WndProc
 *          subclass) and the game-window discovery. It deliberately creates
 *          no DXGI swap chain and never hooks the game's swapchain, so it is
 *          independent of the game's DX11/DX12 pipeline and cannot crash the
 *          game render thread.
 *
 *          Ported from CrimsonDesertLiveTransmog's dx_overlay with all ReShade
 *          function-table indirection stripped: ImGui is linked directly as a
 *          static lib and ImGui:: / ImGui_ImplWin32_* / ImGui_ImplDX11_* are
 *          called directly.
 *
 *          This header is internal to the Overlay module. External integrators
 *          use overlay.hpp instead.
 */
#ifndef TPVCAMERA_OVERLAY_DX_OVERLAY_HPP
#define TPVCAMERA_OVERLAY_DX_OVERLAY_HPP

namespace TPVCamera::Overlay::Detail
{

/**
 * @brief Spawns the overlay render thread (creates the WARP device, the
 *        offscreen target, the ImGui context and the layered window).
 * @details The thread first waits for the game's top-level window to appear,
 *          then builds all rendering resources and enters the render loop. The
 *          per-frame work (ImGui frame, draw_ui(), render, composite) lives in
 *          the loop body. Safe to call once; a second call while running is a
 *          no-op.
 * @return True if the render thread was started; false if thread creation failed.
 */
[[nodiscard]] bool dx_start();

/**
 * @brief Signals the render thread to exit and waits for it to finish.
 * @details Tears down ImGui and the WARP device on the render thread before it
 *          returns. Safe to call even if dx_start() was never called or failed.
 */
void dx_stop() noexcept;

/**
 * @brief Sets the desired overlay visibility.
 * @param visible True to show the panel and route input to it; false to hide.
 */
void dx_set_visible(bool visible) noexcept;

/**
 * @brief Whether the overlay is currently set visible.
 * @return True if the panel is shown.
 */
[[nodiscard]] bool dx_is_visible() noexcept;

/**
 * @brief Whether the host is past ImGui init and the panel is visible this frame.
 * @details Used by overlay.cpp's wants_input(); the ImGui WantCapture* flags are
 *          read by the caller. Returns false before the render thread finishes
 *          initialization so input is never claimed prematurely.
 */
[[nodiscard]] bool dx_is_ready() noexcept;

/**
 * @brief Whether ImGui wants to capture mouse or keyboard input this frame.
 * @details The render thread publishes the ImGui WantCapture* flags into atomics each frame; this
 *          returns that snapshot so the game thread never touches the shared ImGui IO. A stale read
 *          only costs one frame of input arbitration.
 * @return True if the overlay should consume pointer or keyboard input.
 */
[[nodiscard]] bool dx_wants_input() noexcept;

} // namespace TPVCamera::Overlay::Detail

#endif // TPVCAMERA_OVERLAY_DX_OVERLAY_HPP
