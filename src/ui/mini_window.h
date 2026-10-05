#pragma once

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <imgui.h>

#include <string>
#include <vector>

#include "core/types.h"

namespace nonstop {

using Microsoft::WRL::ComPtr;

class OverlayWindow;
class WasapiCapture;
class FftAnalyzer;
class PhaseDetector;
class HotkeyManager;

// The ONLY user window: tabbed mini remote (FX / Tune / Music / More).
// Own caption drag (no OS modal move loop -> FX never freezes), minimizable.
class MiniWindow {
public:    MiniWindow();
    ~MiniWindow();

    // Shared D3D device; own swapchain + ImGui ctx.
    bool create(ID3D11Device* device, ID3D11DeviceContext* context);
    void shutdown();

    void setVisible(bool v);
    bool isVisible() const { return m_hwnd && m_visible; }
    void toggleVisible() { setVisible(!m_visible); }
    HWND getHwnd() const { return m_hwnd; }
    ImGuiContext* context() const { return m_ctx; }

    // Own-drag helpers used by the window proc.
    void beginOwnDrag();
    void updateOwnDrag();
    void endOwnDrag();
    bool isOwnDragging() const { return m_dragging; }

    // HELL MODE epilepsy warning (fullscreen, every enable).
    void showHellWarning(OverlayWindow* target);
    void closeHellWarning();
    bool isHellWarnOpen() const { return m_hellWarn != nullptr; }
    void confirmHellWarning(); // OK pressed: arm HELL, close
    void cancelHellWarning();  // dismissed: keep HELL off, close

    // Render one frame (call on main thread when visible).
    void render(OverlayWindow& overlay, WasapiCapture& capture,
                FftAnalyzer& analyzer, PhaseDetector& detector,
                const HotkeyManager& hotkeyMgr);

private:
    bool createSwapchain();
    void destroySwapchain();

    void renderFxTab(OverlayWindow& overlay);
    void renderTuneTab(OverlayWindow& overlay);
    void renderMusicTab(OverlayWindow& overlay, WasapiCapture& capture,
                        FftAnalyzer& analyzer, PhaseDetector& detector);
    void renderMoreTab(OverlayWindow& overlay, WasapiCapture& capture,
                       const HotkeyManager& hotkeyMgr);

    HWND m_hwnd = nullptr;
    bool m_visible = true;
    int m_width = 340;
    int m_height = 640;

    // Own caption drag state (avoids the freezing OS move loop).
    bool m_dragging = false;
    int m_dragOffX = 0;
    int m_dragOffY = 0;

    // HELL warning dialog state.
    HWND m_hellWarn = nullptr;
    HWND m_hellWarnText = nullptr;
    HWND m_hellCheck = nullptr;
    HWND m_hellOk = nullptr;
    HFONT m_hellFont = nullptr;
    OverlayWindow* m_hellTarget = nullptr;

    // Cached audio devices (enumeration is COM-heavy, do it once).
    std::vector<AudioDeviceInfo> m_cachedDevices;
    int m_selectedDeviceIndex = 0;
    bool m_devicesLoaded = false;

    ComPtr<ID3D11Device> m_device;
    ID3D11DeviceContext* m_context = nullptr; // shared, not owned
    ComPtr<IDXGISwapChain> m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_rtv;

    ImGuiContext* m_ctx = nullptr;
};

} // namespace nonstop
