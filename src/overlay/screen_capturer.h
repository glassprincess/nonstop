#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>
#include <cstdint>

namespace nonstop {

using Microsoft::WRL::ComPtr;

// Stage 3: desktop frame grabber based on DXGI Desktop Duplication.
// Rationale (recorded for TZ section 2): TZ names Windows.Graphics.Capture,
// but Desktop Duplication is pure D3D11, needs no WinRT/C++/WinRT plumbing,
// gives a GPU-side ID3D11Texture2D on the same device as the overlay and
// respects SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE), so the overlay
// does not capture itself (no feedback loop). If Dota runs in exclusive
// fullscreen, duplication still captures the desktop image; the overlay
// itself stays visible only in borderless windowed (TZ limitation).
class ScreenCapturer {
public:
    ScreenCapturer();
    ~ScreenCapturer();

    // Bind to an existing D3D11 device (the overlay's device).
    // Captures the primary output (index 0).
    bool init(ID3D11Device* device);
    void shutdown();

    bool isActive() const { return m_active; }

    // Grab the newest desktop image into the internal shader-readable
    // texture. Non-blocking (0ms timeout): call once per overlay frame.
    // Returns SRV of the last good frame, or nullptr if no frame yet.
    // Must be called on the render thread that owns `context`.
    // NOTE: a returned SRV may be STALE (UAC secure desktop, mode switch) —
    // check secondsSinceFresh() before compositing it over live content.
    ID3D11ShaderResourceView* acquireFrame(ID3D11DeviceContext* context);

    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }
    uint64_t getCapturedFrames() const { return m_capturedFrames; }
    uint64_t getTimeouts() const { return m_timeouts; }
    uint64_t getErrors() const { return m_errors; }
    std::string getStatusString() const { return m_statusString; }

    // Freshness: seconds since a frame with genuinely new desktop content.
    // >0.4s means secure desktop / mode switch — do NOT composite.
    double secondsSinceFresh() const;
    bool isCaptureFresh() const { return m_active && secondsSinceFresh() < 0.4; }

private:
    bool createDuplication();
    bool ensureCaptureTexture(ID3D11Texture2D* src);

    ComPtr<ID3D11Device> m_device;
    ComPtr<IDXGIOutputDuplication> m_duplication;
    ComPtr<ID3D11Texture2D> m_captureTex;
    ComPtr<ID3D11ShaderResourceView> m_captureSrv;

    int m_width = 0;
    int m_height = 0;
    bool m_active = false;
    std::string m_statusString = "Not initialized";
    uint64_t m_capturedFrames = 0;
    uint64_t m_timeouts = 0;
    uint64_t m_errors = 0;
    // Freshness tracking (UAC secure-desktop fix): QPC timestamp (seconds)
    // of the last frame carrying genuinely new desktop pixels, plus the
    // LastPresentTime marker to tell new frames from empty polls.
    double m_lastFreshSec = -1e9;
    long long m_prevPresent = 0;
    static double nowSeconds();
};

} // namespace nonstop
