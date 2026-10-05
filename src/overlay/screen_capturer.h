#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>
#include <cstdint>

namespace nonstop {

using Microsoft::WRL::ComPtr;

// Grabs the desktop through DXGI duplication, plain D3D11, no WinRT.
// Same device as the overlay. Skips the overlay window itself when
// it's flagged, so no feedback loop. Exclusive fullscreen games still
// show through the capture, but the overlay won't sit on top of those.
class ScreenCapturer {
public:
    ScreenCapturer();
    ~ScreenCapturer();

    // Hook up to an existing D3D11 device (the overlay's).
    // Takes the primary screen (index 0).
    bool init(ID3D11Device* device);
    void shutdown();

    bool isActive() const { return m_active; }

    // Pull the newest desktop picture into a shader-readable texture.
    // Doesn't wait (0ms): call once per overlay frame.
    // Gives back the last good frame, or nothing if there isn't one yet.
    // Same thread that owns `context`, please.
    // Heads up: it can hand you a STALE frame (admin prompt screen,
    // mode switch) - check secondsSinceFresh() before drawing it.
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
    // When the last fresh pixels arrived + the marker telling
    // new frames apart from empty polls.
    double m_lastFreshSec = -1e9;
    long long m_prevPresent = 0;
    static double nowSeconds();
};

} // namespace nonstop
