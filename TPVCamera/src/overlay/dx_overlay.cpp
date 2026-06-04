/**
 * @file dx_overlay.cpp
 * @brief WARP + GDI ImGui host implementation (private overlay backend).
 *
 * @details Swap-chain-free transparent overlay, ported from
 *          CrimsonDesertLiveTransmog with the ReShade function-table layer
 *          removed. The pipeline is:
 *            1. D3D11 WARP device (CreateDevice only, no swap chain)
 *            2. Offscreen Texture2D render target (BGRA for GDI)
 *            3. ImGui renders to the texture via ImGui_ImplDX11
 *            4. Texture pixels copied to a DIB section (premultiplied alpha)
 *            5. UpdateLayeredWindowIndirect composites the DIB over the game
 *
 *          No DXGI swap chain exists and the game swapchain is never hooked, so
 *          this is compatible with any DX wrapper (ReShade, SpecialK, etc.) and
 *          cannot fault the game render thread.
 */

#include "dx_overlay.hpp"
#include "overlay.hpp"

#include "global_state.hpp"

#include <DetourModKit.hpp>

#pragma warning(push, 0)
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#pragma warning(pop)

#include <d3d11.h>

#include <Windows.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "winmm.lib")

// Forward decl of the ImGui Win32 backend message handler (the backend header
// declares it as extern but does not always pull it in for the subclass site).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace TPVCamera::Overlay::Detail
{
namespace
{

std::atomic<bool> s_overlay_visible{false};
std::atomic<bool> s_shutdown_requested{false};
std::atomic<bool> s_ready{false};

// Input-capture intent, published by the render thread each frame and read by the game thread via
// dx_wants_input(). Mirroring the ImGui WantCapture* flags into atomics keeps the game thread from
// touching the shared ImGui IO concurrently with the render thread's NewFrame/Render.
std::atomic<bool> s_want_capture_mouse{false};
std::atomic<bool> s_want_capture_keyboard{false};

HWND s_overlay_hwnd = nullptr;
HWND s_game_hwnd = nullptr;

// D3D11 WARP device, no swap chain.
ID3D11Device *s_device = nullptr;
ID3D11DeviceContext *s_context = nullptr;

// Offscreen render target + CPU-readable staging copy.
ID3D11Texture2D *s_rt_tex = nullptr;
ID3D11RenderTargetView *s_rtv = nullptr;
ID3D11Texture2D *s_staging_tex = nullptr;
UINT s_width = 0;
UINT s_height = 0;

// GDI compositing surface.
HDC s_mem_dc = nullptr;
HBITMAP s_dib = nullptr;
// Bitmap the memory DC had selected before s_dib; restored before s_dib is deleted so the DIB is
// not still selected when DeleteObject runs (a GDI object selected into a DC cannot be freed).
HBITMAP s_mem_dc_old_bitmap = nullptr;
void *s_dib_pixels = nullptr;

HANDLE s_render_thread = nullptr;

constexpr wchar_t k_overlay_class[] = L"TPVCameraOverlay";

/**
 * @brief Releases the offscreen target, staging texture and GDI surface.
 */
void release_targets()
{
    if (s_rtv)         { s_rtv->Release();         s_rtv = nullptr; }
    if (s_rt_tex)      { s_rt_tex->Release();       s_rt_tex = nullptr; }
    if (s_staging_tex) { s_staging_tex->Release();  s_staging_tex = nullptr; }
    // Deselect the DIB before deleting it: a GDI object still selected into a DC cannot be freed
    // (DeleteObject fails and the DIB plus its pixel buffer leak). Restore the DC's original bitmap
    // while the DC is still alive, then delete the DIB, then the DC.
    if (s_mem_dc && s_mem_dc_old_bitmap)
    {
        SelectObject(s_mem_dc, s_mem_dc_old_bitmap);
        s_mem_dc_old_bitmap = nullptr;
    }
    if (s_dib)         { DeleteObject(s_dib);       s_dib = nullptr; }
    if (s_mem_dc)      { DeleteDC(s_mem_dc);        s_mem_dc = nullptr; }
    s_dib_pixels = nullptr;
}

/**
 * @brief (Re)creates the render target, staging texture and DIB at w x h.
 * @param w Surface width in pixels.
 * @param h Surface height in pixels.
 * @return True on success; false if any D3D/GDI resource failed to allocate.
 */
[[nodiscard]] bool create_targets(UINT w, UINT h)
{
    release_targets();
    s_width = w;
    s_height = h;

    // Render target texture (BGRA matches the DIB layout GDI expects).
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(s_device->CreateTexture2D(&td, nullptr, &s_rt_tex)))
        { release_targets(); return false; }

    if (FAILED(s_device->CreateRenderTargetView(s_rt_tex, nullptr, &s_rtv)))
        { release_targets(); return false; }

    // Staging texture (CPU-readable copy for the GDI blit).
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(s_device->CreateTexture2D(&td, nullptr, &s_staging_tex)))
        { release_targets(); return false; }

    // DIB section for UpdateLayeredWindowIndirect (top-down 32bpp BGRA).
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(w);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(h); // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screen_dc = GetDC(nullptr);
    s_mem_dc = CreateCompatibleDC(screen_dc);
    s_dib = CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS,
                             &s_dib_pixels, nullptr, 0);
    ReleaseDC(nullptr, screen_dc);

    if (!s_dib || !s_dib_pixels)
        { release_targets(); return false; }

    // Keep the DC's original bitmap so release_targets can deselect the DIB before freeing it.
    s_mem_dc_old_bitmap = static_cast<HBITMAP>(SelectObject(s_mem_dc, s_dib));
    return true;
}

/**
 * @brief Computes the union of this frame's non-empty draw-cmd clip rects.
 * @param dd ImGui draw data for the current frame.
 * @param out Receives the clamped bounding box when the function returns true.
 * @return True if ImGui drew anything; false (e.g. collapsed window) so the
 *         caller falls back to the full surface.
 */
[[nodiscard]] bool compute_draw_bounds(ImDrawData *dd, RECT &out)
{
    if (!dd || dd->CmdListsCount == 0)
        return false;
    const float f_w = static_cast<float>(s_width);
    const float f_h = static_cast<float>(s_height);
    float mnx = f_w, mny = f_h, mxx = 0.0f, mxy = 0.0f;
    bool any = false;
    for (int i = 0; i < dd->CmdListsCount; ++i)
    {
        const ImDrawList *cl = dd->CmdLists[i];
        for (int c = 0; c < cl->CmdBuffer.Size; ++c)
        {
            const ImDrawCmd &cmd = cl->CmdBuffer[c];
            if (cmd.ElemCount == 0)
                continue;
            // Parenthesized names defeat the Windows.h min/max macros.
            const float x0 = (std::max)(0.0f, cmd.ClipRect.x);
            const float y0 = (std::max)(0.0f, cmd.ClipRect.y);
            const float x1 = (std::min)(f_w, cmd.ClipRect.z);
            const float y1 = (std::min)(f_h, cmd.ClipRect.w);
            if (x1 <= x0 || y1 <= y0)
                continue;
            if (x0 < mnx) mnx = x0;
            if (y0 < mny) mny = y0;
            if (x1 > mxx) mxx = x1;
            if (y1 > mxy) mxy = y1;
            any = true;
        }
    }
    if (!any)
        return false;
    out.left = static_cast<LONG>(std::floor(mnx));
    out.top = static_cast<LONG>(std::floor(mny));
    out.right = static_cast<LONG>(std::ceil(mxx));
    out.bottom = static_cast<LONG>(std::ceil(mxy));
    return true;
}

/**
 * @brief Copies the dirty region of the render target into the DIB and
 *        composites it onto the screen via UpdateLayeredWindowIndirect.
 * @param dirty The rect (target-space) to refresh; pixels outside it keep the
 *        previous frame's layered-surface contents (ULWI prcDirty ignores them).
 * @details Limiting the staging readback, the per-pixel premultiply and the GDI
 *          composite to the union of the previous and current drawn rects is the
 *          bulk of the responsiveness win on high-res displays.
 */
void blit_to_screen(const RECT &dirty)
{
    const LONG w = static_cast<LONG>(s_width);
    const LONG h = static_cast<LONG>(s_height);
    const LONG x0 = std::clamp<LONG>(dirty.left, 0, w);
    const LONG y0 = std::clamp<LONG>(dirty.top, 0, h);
    const LONG x1 = std::clamp<LONG>(dirty.right, 0, w);
    const LONG y1 = std::clamp<LONG>(dirty.bottom, 0, h);
    if (x1 <= x0 || y1 <= y0)
        return;

    // GPU -> staging: copy only the dirty box.
    D3D11_BOX box{};
    box.left = static_cast<UINT>(x0);
    box.top = static_cast<UINT>(y0);
    box.front = 0;
    box.right = static_cast<UINT>(x1);
    box.bottom = static_cast<UINT>(y1);
    box.back = 1;
    s_context->CopySubresourceRegion(
        s_staging_tex, 0,
        static_cast<UINT>(x0), static_cast<UINT>(y0), 0,
        s_rt_tex, 0, &box);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(s_context->Map(s_staging_tex, 0, D3D11_MAP_READ, 0, &mapped)))
        return;

    const UINT row_bytes = s_width * 4;
    const UINT span_bytes = static_cast<UINT>(x1 - x0) * 4;
    const auto *src_base = static_cast<const uint8_t *>(mapped.pData);
    auto *dst_base = static_cast<uint8_t *>(s_dib_pixels);
    // memcpy + premultiply each dirty row in one pass.
    for (LONG y = y0; y < y1; ++y)
    {
        const uint8_t *src = src_base + static_cast<size_t>(y) * mapped.RowPitch +
                             static_cast<size_t>(x0) * 4;
        uint8_t *dst = dst_base + static_cast<size_t>(y) * row_bytes +
                       static_cast<size_t>(x0) * 4;
        memcpy(dst, src, span_bytes);
        for (LONG xi = 0; xi < x1 - x0; ++xi)
        {
            uint8_t *px = dst + xi * 4;
            const uint8_t a = px[3];
            if (a == 0)
            {
                px[0] = px[1] = px[2] = 0;
            }
            else if (a < 255)
            {
                px[0] = static_cast<uint8_t>((px[0] * a + 127) / 255);
                px[1] = static_cast<uint8_t>((px[1] * a + 127) / 255);
                px[2] = static_cast<uint8_t>((px[2] * a + 127) / 255);
            }
        }
    }
    s_context->Unmap(s_staging_tex, 0);

    // Composite onto the screen with a dirty-rect hint.
    RECT gr{};
    GetWindowRect(s_game_hwnd, &gr);
    POINT pt_pos = {gr.left, gr.top};
    SIZE sz = {w, h};
    POINT pt_src = {0, 0};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    RECT dirty_clamped{x0, y0, x1, y1};
    UPDATELAYEREDWINDOWINFO ulwi{};
    ulwi.cbSize = sizeof(ulwi);
    ulwi.pptDst = &pt_pos;
    ulwi.psize = &sz;
    ulwi.hdcSrc = s_mem_dc;
    ulwi.pptSrc = &pt_src;
    ulwi.pblend = &blend;
    ulwi.dwFlags = ULW_ALPHA;
    ulwi.prcDirty = &dirty_clamped;
    UpdateLayeredWindowIndirect(s_overlay_hwnd, &ulwi);
}

/**
 * @brief Finds this process's top-level game window.
 * @details Skips windows owned by other processes, hidden windows, sub-640x480
 *          surfaces and the launcher's "SplashWindow", matching CDLT's filter.
 * @return The game HWND, or nullptr if none matched.
 */
HWND find_game_hwnd()
{
    struct Ctx
    {
        HWND result;
    } ctx{nullptr};
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd))
                return TRUE;
            RECT rc{};
            GetClientRect(hwnd, &rc);
            if ((rc.right - rc.left) < 640 || (rc.bottom - rc.top) < 480)
                return TRUE;
            char cls[128]{};
            GetClassNameA(hwnd, cls, sizeof(cls));
            if (strcmp(cls, "SplashWindow") == 0)
                return TRUE;
            reinterpret_cast<Ctx *>(lp)->result = hwnd;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

/**
 * @brief WndProc for the layered overlay window.
 * @details Routes input to ImGui while the panel is visible. Esc hides the
 *          panel and returns focus to the game. We do NOT register any game
 *          hotkey here; the show/hide toggle is driven externally via toggle().
 */
LRESULT CALLBACK overlay_wndproc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (s_ready.load(std::memory_order_relaxed) &&
        s_overlay_visible.load(std::memory_order_relaxed))
    {
        if (msg == WM_KEYDOWN && wParam == VK_ESCAPE)
        {
            dx_set_visible(false);
            return 0;
        }
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
            return 1;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

/**
 * @brief Waits for the game window to resolve, returning it.
 * @return The game HWND, or nullptr if shutdown was requested while waiting.
 */
HWND wait_for_game_window(DMK::Logger &logger)
{
    logger.info("[overlay] Waiting for game window...");
    for (int tick = 0;; ++tick)
    {
        if (s_shutdown_requested.load(std::memory_order_relaxed))
            return nullptr;
        if (HWND hwnd = find_game_hwnd())
        {
            // Do NOT snapshot the client size yet. The game opens at a transient loading-window size
            // and only later resizes to the real resolution, so sizing the overlay surface + DPI font
            // to that early size leaves it tiny. The overlay is an in-world tool, so wait until the
            // game is IN-WORLD: game_world_ready is set once C_Player first resolves, by which point
            // the window is at its final resolution. (Deliberately NO size-based fallback -- a menu /
            // loading window is "stable" too, which is exactly the size we must not lock onto.)
            while (!TPVCamera::game_world_ready().load(std::memory_order_relaxed))
            {
                if (s_shutdown_requested.load(std::memory_order_relaxed))
                    return nullptr;
                Sleep(150);
            }
            Sleep(250); // let the final resize fully land before snapshotting
            if (HWND cur = find_game_hwnd())
                hwnd = cur;
            return hwnd;
        }
        // 600 * 100ms == 60s. Heartbeat each minute so a user on a long boot
        // can confirm the overlay thread is alive.
        if (tick > 0 && (tick % 600) == 0)
            logger.info("[overlay] Still waiting for game window ({} min)", tick / 600);
        Sleep(100);
    }
}

/**
 * @brief Render thread entry: builds resources, runs the per-frame loop, tears down.
 */
DWORD WINAPI render_thread(LPVOID)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    s_game_hwnd = wait_for_game_window(logger);
    if (!s_game_hwnd)
        return 0;

    RECT gr{};
    GetClientRect(s_game_hwnd, &gr);
    const UINT gw = static_cast<UINT>(gr.right);
    const UINT gh = static_cast<UINT>(gr.bottom);
    logger.info("[overlay] Game window {}x{}", gw, gh);

    // Layered overlay window: pure GDI composite target, no D3D/DXGI objects.
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = overlay_wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = k_overlay_class;
    RegisterClassExW(&wc);

    GetWindowRect(s_game_hwnd, &gr);
    s_overlay_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"", WS_POPUP,
        gr.left, gr.top, static_cast<int>(gw), static_cast<int>(gh),
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!s_overlay_hwnd)
    {
        logger.error("[overlay] Window creation failed");
        UnregisterClassW(k_overlay_class, wc.hInstance);
        return 0;
    }

    ShowWindow(s_overlay_hwnd, SW_SHOWNOACTIVATE);

    // D3D11 WARP device (NO swap chain). Private to this module: we never touch
    // the game's device or swapchain.
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            &fl, 1, D3D11_SDK_VERSION,
            &s_device, nullptr, &s_context)))
    {
        logger.error("[overlay] WARP device creation failed");
        DestroyWindow(s_overlay_hwnd);
        s_overlay_hwnd = nullptr;
        UnregisterClassW(k_overlay_class, wc.hInstance);
        return 0;
    }

    if (!create_targets(gw, gh))
    {
        logger.error("[overlay] Render target creation failed");
        s_context->Release();
        s_context = nullptr;
        s_device->Release();
        s_device = nullptr;
        DestroyWindow(s_overlay_hwnd);
        s_overlay_hwnd = nullptr;
        UnregisterClassW(k_overlay_class, wc.hInstance);
        return 0;
    }

    // ImGui (direct, no ReShade function table).
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(static_cast<float>(gw), static_cast<float>(gh));

    // DPI-aware scaling: 14px base targets 1080p, scaled linearly for higher
    // resolutions. Build the font at the target size rather than stretching the
    // built-in bitmap, then scale style sizes by the same ratio so padding and
    // frame heights stay proportional to the glyph size.
    const float dpi_scale = static_cast<float>(gh) / 1080.0f;
    ImGui::StyleColorsDark();
    ImFontConfig font_cfg;
    font_cfg.SizePixels = 14.0f * dpi_scale;
    io.Fonts->AddFontDefault(&font_cfg);
    // 13px is Dear ImGui's built-in proggy-clean font height, so target/13 scales the style metrics
    // (padding, frame heights) by the same ratio as the font. FontGlobalScale is left at its 1.0
    // default here: draw_ui() drives it from the user UI-scale preference every frame.
    constexpr float k_imgui_default_font_px = 13.0f;
    ImGui::GetStyle().ScaleAllSizes(font_cfg.SizePixels / k_imgui_default_font_px);

    ImGui_ImplWin32_Init(s_overlay_hwnd);
    ImGui_ImplDX11_Init(s_device, s_context);

    s_ready.store(true, std::memory_order_release);
    logger.info("[overlay] Ready (WARP + GDI blit, no swap chain)");

    // Raise the system timer resolution to 1ms so the adaptive Sleep() below is
    // honored rather than rounding up to the ~15.6ms scheduler tick.
    timeBeginPeriod(1);

    // Previous frame's drawn rect, so we union it with this frame's rect and
    // clear pixels ImGui vacated (closed popup, dismissed tooltip).
    RECT prev_dirty{0, 0, 0, 0};

    while (!s_shutdown_requested.load(std::memory_order_relaxed))
    {
        MSG msg{};
        while (PeekMessageW(&msg, s_overlay_hwnd, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!IsWindow(s_game_hwnd))
            break;

        // Only show the panel when the game (or our overlay) is foreground, so
        // it does not linger over other apps when alt-tabbed.
        const HWND fg = GetForegroundWindow();
        const bool game_active = (fg == s_game_hwnd || fg == s_overlay_hwnd);
        const bool want_visible =
            game_active && s_overlay_visible.load(std::memory_order_relaxed);

        if (want_visible)
        {
            // Reposition only when the game window actually moved/resized. Only the POSITION/extent of
            // the overlay window tracks the game here; the render targets, ImGui DisplaySize and font
            // scale are sized once at startup (create_targets above). A mid-session RESOLUTION change is
            // therefore not re-detected -- the panel keeps the startup size until it is closed and
            // reopened. This is intentional: rebuilding the device targets and font atlas on every resize
            // is not worth the complexity for a tuning panel that is normally opened briefly, and a stale
            // size only misaligns the blit (GDI clips it; there is no out-of-bounds access).
            static RECT s_last_gr{};
            RECT ngr{};
            GetWindowRect(s_game_hwnd, &ngr);
            if (ngr.left != s_last_gr.left || ngr.top != s_last_gr.top ||
                ngr.right != s_last_gr.right || ngr.bottom != s_last_gr.bottom)
            {
                SetWindowPos(s_overlay_hwnd, HWND_TOP,
                             ngr.left, ngr.top,
                             ngr.right - ngr.left, ngr.bottom - ngr.top,
                             SWP_NOACTIVATE);
                s_last_gr = ngr;
            }
            if (!IsWindowVisible(s_overlay_hwnd))
                ShowWindow(s_overlay_hwnd, SW_SHOWNOACTIVATE);
        }
        else
        {
            if (IsWindowVisible(s_overlay_hwnd))
                ShowWindow(s_overlay_hwnd, SW_HIDE);
        }

        // Toggle click-through: while visible the window accepts the cursor and
        // is brought forward; while hidden it is click-through and focus goes
        // back to the game.
        {
            static bool s_was_visible = false;
            if (want_visible && !s_was_visible)
            {
                const LONG_PTR ex = GetWindowLongPtrW(s_overlay_hwnd, GWL_EXSTYLE);
                SetWindowLongPtrW(s_overlay_hwnd, GWL_EXSTYLE, ex & ~WS_EX_TRANSPARENT);
                SetForegroundWindow(s_overlay_hwnd);
            }
            else if (!want_visible && s_was_visible)
            {
                const LONG_PTR ex = GetWindowLongPtrW(s_overlay_hwnd, GWL_EXSTYLE);
                SetWindowLongPtrW(s_overlay_hwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
                SetForegroundWindow(s_game_hwnd);
            }
            s_was_visible = want_visible;
        }

        if (!want_visible)
        {
            Sleep(50);
            continue;
        }

        // Clear the render target to transparent black.
        const float clear[4] = {0, 0, 0, 0};
        s_context->OMSetRenderTargets(1, &s_rtv, nullptr);
        s_context->ClearRenderTargetView(s_rtv, clear);

        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(s_width);
        vp.Height = static_cast<float>(s_height);
        vp.MaxDepth = 1.0f;
        s_context->RSSetViewports(1, &vp);

        // Bridge mouse-button state into ImGui via polling. The game uses
        // SetCapture / RawInput so WM_*BUTTON* route to the game window even
        // over our overlay; ImGui_ImplWin32 polls cursor position but not
        // button state. Edge-detect the press so only down transitions that
        // started over the overlay count, suppressing drag-in / drag-out
        // phantom clicks.
        bool over_overlay = false;
        {
            POINT pt{};
            GetCursorPos(&pt);
            RECT wr{};
            GetWindowRect(s_overlay_hwnd, &wr);
            over_overlay = pt.x >= wr.left && pt.x < wr.right &&
                           pt.y >= wr.top && pt.y < wr.bottom;

            static bool s_l_latch = false, s_l_was = false;
            static bool s_r_latch = false, s_r_was = false;
            static bool s_m_latch = false, s_m_was = false;
            auto poll = [&](int idx, int vk, bool &latch, bool &was) -> void
            {
                const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
                if (!now)
                    latch = false;
                else if (!was && over_overlay)
                    latch = true;
                was = now;
                io.AddMouseButtonEvent(idx, latch);
            };
            poll(0, VK_LBUTTON, s_l_latch, s_l_was);
            poll(1, VK_RBUTTON, s_r_latch, s_r_was);
            poll(2, VK_MBUTTON, s_m_latch, s_m_was);
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Populate the panel. draw_ui() is the Overlay module's public hook,
        // implemented in overlay_ui.cpp; it issues its own Begin/End.
        TPVCamera::Overlay::draw_ui();

        // Snapshot interactivity inside the frame (IsAnyItem* and WantTextInput
        // are only valid between NewFrame and Render).
        const bool ui_active = ImGui::IsAnyItemActive() ||
                               ImGui::IsAnyItemHovered() ||
                               io.WantTextInput;

        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Publish the input-capture intent for the game thread (Overlay::wants_input) without sharing
        // the ImGui IO across threads. WantCapture* are finalized by NewFrame and stable for the frame.
        s_want_capture_mouse.store(io.WantCaptureMouse, std::memory_order_relaxed);
        s_want_capture_keyboard.store(io.WantCaptureKeyboard, std::memory_order_relaxed);

        // Tight dirty rect = union of the previous and current drawn rects, with
        // a small border for anti-aliased edges. Full surface when ImGui drew
        // nothing (collapsed window).
        RECT cur{};
        if (!compute_draw_bounds(ImGui::GetDrawData(), cur))
        {
            cur.left = 0;
            cur.top = 0;
            cur.right = static_cast<LONG>(s_width);
            cur.bottom = static_cast<LONG>(s_height);
        }
        RECT dirty = cur;
        if (prev_dirty.right > prev_dirty.left && prev_dirty.bottom > prev_dirty.top)
            UnionRect(&dirty, &cur, &prev_dirty);
        constexpr LONG k_edge_pad = 4;
        dirty.left = (std::max<LONG>)(0, dirty.left - k_edge_pad);
        dirty.top = (std::max<LONG>)(0, dirty.top - k_edge_pad);
        dirty.right = (std::min<LONG>)(static_cast<LONG>(s_width), dirty.right + k_edge_pad);
        dirty.bottom = (std::min<LONG>)(static_cast<LONG>(s_height), dirty.bottom + k_edge_pad);
        blit_to_screen(dirty);
        prev_dirty = cur;

        // Adaptive sleep. This thread renders with WARP (software) on the CPU, so an uncapped rate
        // here steals a core from the game. Cap it to ~60Hz while actively interacting (an item is
        // active/hovered, text input is wanted, or the mouse is moving) and ~30Hz when idle. NOTE:
        // over_overlay is deliberately NOT a trigger -- the overlay window spans the entire game window,
        // so the cursor is "over" it almost the whole time the panel is open; including it pinned the
        // thread at the interactive rate for the whole session, which hit game fps.
        const bool mouse_moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
        const bool interactive = ui_active || mouse_moved;
        Sleep(interactive ? 16 : 33);
    }

    timeEndPeriod(1);

    // Teardown on the render thread (every D3D/ImGui object was created here).
    s_ready.store(false, std::memory_order_release);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    release_targets();
    if (s_context) { s_context->Release(); s_context = nullptr; }
    if (s_device)  { s_device->Release();  s_device = nullptr; }
    if (s_overlay_hwnd)
    {
        DestroyWindow(s_overlay_hwnd);
        s_overlay_hwnd = nullptr;
    }
    UnregisterClassW(k_overlay_class, GetModuleHandleW(nullptr));
    return 0;
}

} // namespace

bool dx_start()
{
    if (s_render_thread)
        return true;
    s_shutdown_requested.store(false, std::memory_order_relaxed);
    s_render_thread = CreateThread(nullptr, 0, render_thread, nullptr, 0, nullptr);
    return s_render_thread != nullptr;
}

void dx_stop() noexcept
{
    s_shutdown_requested.store(true, std::memory_order_release);
    if (s_render_thread)
    {
        // Only reclaim the handle if the render thread actually exited. On a timeout the thread is still
        // running (e.g. a hung driver/GDI call); closing its handle then, or letting it run teardown after
        // the module begins unloading, is the worse hazard, so leak the handle instead (teardown runs on a
        // worker thread off the loader lock, so the wait itself is safe to take).
        if (WaitForSingleObject(s_render_thread, 5000) == WAIT_OBJECT_0)
        {
            CloseHandle(s_render_thread);
            s_render_thread = nullptr;
        }
    }
    s_overlay_visible.store(false, std::memory_order_relaxed);
}

void dx_set_visible(bool visible) noexcept
{
    s_overlay_visible.store(visible, std::memory_order_relaxed);
}

bool dx_is_visible() noexcept
{
    return s_overlay_visible.load(std::memory_order_relaxed);
}

bool dx_is_ready() noexcept
{
    return s_ready.load(std::memory_order_acquire);
}

bool dx_wants_input() noexcept
{
    return s_want_capture_mouse.load(std::memory_order_relaxed) ||
           s_want_capture_keyboard.load(std::memory_order_relaxed);
}

} // namespace TPVCamera::Overlay::Detail
