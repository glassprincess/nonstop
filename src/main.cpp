#include "audio/wasapi_capture.h"
#include "audio/fft_analyzer.h"
#include "audio/phase_detector.h"
#include "overlay/overlay_window.h"
#include "core/hotkey_manager.h"
#include "ui/mini_window.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <timeapi.h>

#include <vector>
#include <memory>
#include <iostream>

using Microsoft::WRL::ComPtr;

// Shared D3D device for the mini remote (it owns its own swapchain).
// Device-only: no big diagnostics window anymore (user asked to drop it).
static ComPtr<ID3D11Device> g_pd3dDevice;
static ComPtr<ID3D11DeviceContext> g_pd3dDeviceContext;

static bool CreateDeviceD3D() {
    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevelArray,
        2,
        D3D11_SDK_VERSION,
        g_pd3dDevice.GetAddressOf(),
        &featureLevel,
        g_pd3dDeviceContext.GetAddressOf()
    );
    return SUCCEEDED(hr);
}

static void CleanupDeviceD3D() {
    g_pd3dDeviceContext.Reset();
    g_pd3dDevice.Reset();
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    (void)hInstance;
    (void)nCmdShow;
    // Enable DPI awareness
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);

    if (!CreateDeviceD3D()) {
        CleanupDeviceD3D();
        return 1;
    }

    // 1ms scheduler granularity for the frame pacer below.
    timeBeginPeriod(1);
    LARGE_INTEGER paceFreq, paceLast;
    QueryPerformanceFrequency(&paceFreq);
    QueryPerformanceCounter(&paceLast);

    // No diagnostics ImGui context here: the mini remote owns the only
    // UI context (big menu deleted per user request).

    // Initialize Audio Subsystems
    auto capture = std::make_unique<nonstop::WasapiCapture>();
    auto analyzer = std::make_unique<nonstop::FftAnalyzer>(48000);
    auto phaseDetector = std::make_unique<nonstop::PhaseDetector>();
    // The mini remote is now the ONLY settings UI (tabbed: FX/Tune/Music/More).
    auto miniWindow = std::make_unique<nonstop::MiniWindow>();
    miniWindow->create(g_pd3dDevice.Get(), g_pd3dDeviceContext.Get());

    // Start capturing default audio endpoint
    if (capture->start(L"")) {
        auto format = capture->getFormatInfo();
        if (format.sampleRate > 0) {
            analyzer->setSampleRate(format.sampleRate);
        }
    }

    // Initialize Stage 2 Transparent Click-Through Overlay
    auto overlay = std::make_unique<nonstop::OverlayWindow>();
    overlay->create(screenW, screenH);

    // Initialize Panic Hotkey Manager (Ctrl + Alt + X) in dedicated high-priority thread.
    // FIX (bind didn't work): the worker thread only RECORDS presses into a
    // lock-free counter; the actual overlay toggle runs HERE on the main
    // thread. The old code called overlay->toggleVisible() (ShowWindow /
    // SetWindowPos on a main-thread window) directly from the hotkey thread,
    // which was racy and never visibly toggled. Polling also keeps all D3D
    // and Win32 window calls on one thread.
    auto hotkeyMgr = std::make_unique<nonstop::HotkeyManager>();
    hotkeyMgr->start();

    std::vector<float> audioBatch(4096);

    // Main loop
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) break;

        // Panic: in HELL MODE always force-hide + disarm (never toggle).
        while (hotkeyMgr->consumePanicPress()) {
            if (overlay->isHellMode()) {
                overlay->setVisible(false);
                overlay->setHellMode(false);
            } else {
                overlay->toggleVisible();
            }
        }

        // Drain audio samples from WASAPI loopback ring buffer and feed into FFT analyzer
        if (capture->isRunning()) {
            // Keep FFT bin mapping in sync when the user switches output
            // device mid-session (device combo calls capture->start(id)).
            const auto fmt = capture->getFormatInfo();
            if (fmt.sampleRate > 0 && fmt.sampleRate != analyzer->getSampleRate()) {
                analyzer->setSampleRate(fmt.sampleRate);
            }
            size_t samplesRead = capture->readSamples(audioBatch.data(), audioBatch.size());
            if (samplesRead > 0) {
                analyzer->processSamples(audioBatch.data(), samplesRead);
            }
        }

        const auto snapshot = analyzer->getSnapshot();

        // Stage 4: track phase follows the analysis snapshot.
        phaseDetector->update(snapshot);
        overlay->setPhaseState(phaseDetector->getState());
        overlay->setReleaseTime(phaseDetector->params().releaseTime);

        // Render transparent overlay frame (audio-reactive effects on top of all windows)
        if (overlay->isVisible()) {
            overlay->renderFrame(snapshot);
        }

        // The mini remote is the whole UI now.
        if (miniWindow->isVisible()) {
            miniWindow->render(*overlay, *capture, *analyzer, *phaseDetector, *hotkeyMgr);
        }

        // Frame pacer: presents don't vsync on these windows (~400+fps
        // observed = pure GPU burn). Cap ~60Hz; audio drain catches up
        // through the ring buffer, all envelopes use real dt anyway.
        LARGE_INTEGER paceNow;
        QueryPerformanceCounter(&paceNow);
        const double frameMs = static_cast<double>(paceNow.QuadPart - paceLast.QuadPart)
            * 1000.0 / static_cast<double>(paceFreq.QuadPart);
        const double budgetMs = 1000.0 / 60.0;
        if (frameMs < budgetMs) {
            Sleep(static_cast<DWORD>(budgetMs - frameMs));
            QueryPerformanceCounter(&paceNow);
        }
        paceLast = paceNow;
    }

    // Stop panic hotkey thread
    hotkeyMgr->stop();

    // Destroy mini remote (own ImGui context).
    miniWindow->shutdown();

    // Destroy overlay
    overlay->shutdown();

    // Stop and cleanup audio
    capture->stop();

    CleanupDeviceD3D();
    timeEndPeriod(1);

    return 0;
}
