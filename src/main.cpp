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

// one D3D device for the mini window (it brings its own swapchain).
// device only, no big window attached.
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
    // DPI matters, text goes blurry otherwise
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);

    if (!CreateDeviceD3D()) {
        CleanupDeviceD3D();
        return 1;
    }

    // 1ms timer ticks for the frame pacer below.
    timeBeginPeriod(1);
    LARGE_INTEGER paceFreq, paceLast;
    QueryPerformanceFrequency(&paceFreq);
    QueryPerformanceCounter(&paceLast);

    // no leftover ImGui context here: the mini owns the only UI.

    // sound in, numbers out
    auto capture = std::make_unique<nonstop::WasapiCapture>();
    auto analyzer = std::make_unique<nonstop::FftAnalyzer>(48000);
    auto phaseDetector = std::make_unique<nonstop::PhaseDetector>();
    // the mini is the whole UI now (FX/Tune/Music/More tabs).
    auto miniWindow = std::make_unique<nonstop::MiniWindow>();
    miniWindow->create(g_pd3dDevice.Get(), g_pd3dDeviceContext.Get());

    // start with whatever Windows plays to
    if (capture->start(L"")) {
        auto format = capture->getFormatInfo();
        if (format.sampleRate > 0) {
            analyzer->setSampleRate(format.sampleRate);
        }
    }

    // the overlay itself: fullscreen, transparent, click-through
    auto overlay = std::make_unique<nonstop::OverlayWindow>();
    overlay->create(screenW, screenH);

    // Panic keys live on their own thread and just count presses.
    // The toggle itself happens down in the main loop, so no window
    // call ever fires from the wrong thread (that used to break binds).
    auto hotkeyMgr = std::make_unique<nonstop::HotkeyManager>();
    hotkeyMgr->start();

    std::vector<float> audioBatch(4096);

    // main loop
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

        // panic: HELL always dies + hides, never toggles.
        while (hotkeyMgr->consumePanicPress()) {
            if (overlay->isHellMode()) {
                overlay->setVisible(false);
                overlay->setHellMode(false);
            } else {
                overlay->toggleVisible();
            }
        }

        // suck samples out of WASAPI, push them through the analyzer
        if (capture->isRunning()) {
            // FFT bins depend on the sample rate, so re-tune
            // when the output device changes mid-session.
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

        // what part of the track is this?
        phaseDetector->update(snapshot);
        overlay->setPhaseState(phaseDetector->getState());
        overlay->setReleaseTime(phaseDetector->params().releaseTime);

        // draw the rave on top of everything
        if (overlay->isVisible()) {
            overlay->renderFrame(snapshot);
        }

        // mini does its thing here.
        if (miniWindow->isVisible()) {
            miniWindow->render(*overlay, *capture, *analyzer, *phaseDetector, *hotkeyMgr);
        }

        // presents don't vsync on these windows (saw 400+fps = GPU
        // melting for nothing). Cap ~60Hz; audio catches up through
        // the ring buffer, envelopes use real dt anyway.
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

    // kill the hotkey thread
    hotkeyMgr->stop();

    // Destroy mini remote (own ImGui context).
    miniWindow->shutdown();

    // overlay down
    overlay->shutdown();

    // sound off
    capture->stop();

    CleanupDeviceD3D();
    timeEndPeriod(1);

    return 0;
}
