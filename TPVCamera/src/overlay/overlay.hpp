/**
 * @file overlay.hpp
 * @brief Public API for the self-hosted ImGui overlay (preset manager UI).
 *
 * @details The overlay renders ImGui through a private WARP D3D11 device into an
 *          offscreen surface that is composited over the game window with GDI, on its
 *          own thread (ported from CrimsonDesertLiveTransmog's dx_overlay). It does NOT
 *          hook the game's swapchain, so it is independent of the game's DX11/DX12
 *          pipeline and cannot crash the render thread. Input is captured via a WndProc
 *          subclass + GetAsyncKeyState bridge while visible.
 *
 *          Lifecycle: start() once after the game module is resolved; toggle()/set_visible()
 *          from the bound hotkey; stop() on shutdown. The render loop calls draw_ui()
 *          (implemented in overlay_ui.cpp) to populate the preset-manager window.
 */
#ifndef TPVCAMERA_OVERLAY_OVERLAY_HPP
#define TPVCAMERA_OVERLAY_OVERLAY_HPP

namespace TPVCamera::Overlay
{

    /**
     * @brief Starts the overlay host (creates the WARP device, ImGui context and render thread).
     * @return True on success; false if device/ImGui init failed (the mod still runs without UI).
     */
    [[nodiscard]] bool start();

    /** @brief Stops the render thread and tears down ImGui + the WARP device. Safe if not started. */
    void stop();

    /** @brief Toggles overlay visibility (bind to a hotkey). */
    void toggle();

    /** @brief Shows or hides the overlay. */
    void set_visible(bool visible);

    /** @brief Whether the overlay window is currently shown. */
    [[nodiscard]] bool is_visible() noexcept;

    /**
     * @brief Whether the overlay is visible and ImGui wants the mouse/keyboard this frame.
     * @details Lets the rest of the mod gate game input while the user interacts with the panel.
     */
    [[nodiscard]] bool wants_input() noexcept;

    /**
     * @brief Renders the preset-manager window contents.
     * @details Implemented in overlay_ui.cpp; called by the overlay render loop inside an
     *          active ImGui frame. Drives PresetStore CRUD and the field editors.
     */
    void draw_ui();

} // namespace TPVCamera::Overlay

#endif // TPVCAMERA_OVERLAY_OVERLAY_HPP
