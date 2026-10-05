#include "ui/mini_window.h"
#include "overlay/overlay_window.h"
#include "audio/phase_detector.h"
#include "audio/wasapi_capture.h"
#include "audio/fft_analyzer.h"
#include "core/hotkey_manager.h"
#include "ui/theme.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <cmath>
#include <cstdio>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace nonstop {

static MiniWindow* g_miniSelf = nullptr;

static std::string wideToUtf8Mini(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, wstr.data(),
        static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
        s.data(), n, nullptr, nullptr);
    return s;
}

// Own caption drag: the OS modal move loop would freeze FX rendering
// (DispatchMessage never returns while dragging), so we move the window
// ourselves with SetCapture + SetWindowPos. Close/minimize buttons untouched.
static LRESULT CALLBACK MiniWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_miniSelf && g_miniSelf->context()) {
        if (msg == WM_CLOSE) {
            // X kills the process (no tray hiding per user request).
            PostQuitMessage(0);
            return 0;
        }
        if (msg == WM_NCLBUTTONDOWN && wParam == HTCAPTION) {
            g_miniSelf->beginOwnDrag();
            return 0;
        }
        if (msg == WM_MOUSEMOVE && g_miniSelf->isOwnDragging()) {
            g_miniSelf->updateOwnDrag();
            return 0;
        }
        if (msg == WM_LBUTTONUP && g_miniSelf->isOwnDragging()) {
            g_miniSelf->endOwnDrag();
            return 0;
        }
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(g_miniSelf->context());
        LRESULT handled = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
        ImGui::SetCurrentContext(prev);
        if (handled) return true;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static const char* kMiniHints[kFxModuleCount] = {
    "color channels split apart on bass",
    "torn rows and blocks on highs",
    "waves breathing with mids",
    "screen melts after the drop",
    "shockwave rings on beats",
    "rain on glass, frost, prism",
    "camera punch and shake on beats",
    "whispering phrases at the edges",
    "elastic whole-screen wobble [BETA]",
    "bright pixels smear downward [BETA]",
    "copies of the screen drift apart",
};

MiniWindow::MiniWindow() = default;

MiniWindow::~MiniWindow() {
    shutdown();
}

bool MiniWindow::create(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context) return false;
    shutdown();
    m_device = device;
    m_context = context;

    HINSTANCE hi = GetModuleHandleW(nullptr);
    const wchar_t* cls = L"NonstopMiniClass";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MiniWndProc;
    wc.hInstance = hi;
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);

    const int sw = GetSystemMetrics(SM_CXSCREEN);
    m_hwnd = CreateWindowExW(
        0, cls, L"NONSTOP",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        sw - m_width - 40, 80, m_width, m_height,
        nullptr, nullptr, hi, nullptr);
    if (!m_hwnd) return false;

    if (!createSwapchain()) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
        return false;
    }

    // Own ImGui context on the shared device.
    ImGuiContext* prev = ImGui::GetCurrentContext();
    m_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(m_ctx);
    applyDarkTheme();
    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(device, context);
    ImGui::SetCurrentContext(prev);

    g_miniSelf = this;
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(m_hwnd);
    return true;
}

bool MiniWindow::createSwapchain() {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = m_width;
    sd.BufferDesc.Height = m_height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(m_device->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) return false;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDev->GetAdapter(&adapter))) return false;
    ComPtr<IDXGIFactory> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;
    if (FAILED(factory->CreateSwapChain(m_device.Get(), &sd, &m_swapChain))) return false;

    ComPtr<ID3D11Texture2D> back;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    if (FAILED(m_device->CreateRenderTargetView(back.Get(), nullptr, &m_rtv))) return false;
    return true;
}

void MiniWindow::destroySwapchain() {
    m_rtv.Reset();
    m_swapChain.Reset();
}

void MiniWindow::shutdown() {
    closeHellWarning();    if (m_ctx) {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(m_ctx);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        m_ctx = nullptr;
        ImGui::SetCurrentContext(prev);
    }
    destroySwapchain();
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (g_miniSelf == this) g_miniSelf = nullptr;
    m_device.Reset();
    m_context = nullptr;
}

void MiniWindow::setVisible(bool v) {
    m_visible = v;
    if (m_hwnd) {
        ShowWindow(m_hwnd, v ? (IsIconic(m_hwnd) ? SW_RESTORE : SW_SHOWNOACTIVATE) : SW_HIDE);
    }
}

void MiniWindow::beginOwnDrag() {
    if (!m_hwnd || m_dragging) return;
    POINT pt;
    GetCursorPos(&pt);
    RECT rc;
    GetWindowRect(m_hwnd, &rc);
    m_dragOffX = pt.x - rc.left;
    m_dragOffY = pt.y - rc.top;
    m_dragging = true;
    SetCapture(m_hwnd);
}

void MiniWindow::updateOwnDrag() {
    if (!m_dragging || !m_hwnd) return;
    POINT pt;
    GetCursorPos(&pt);
    SetWindowPos(m_hwnd, nullptr, pt.x - m_dragOffX, pt.y - m_dragOffY,
        0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void MiniWindow::endOwnDrag() {
    m_dragging = false;
    ReleaseCapture();
}

// ---- HELL MODE epilepsy warning (fullscreen, shown on every enable) ----
static const wchar_t* kHellWarnText =
    L"EPILEPSY WARNING / ПРЕДУПРЕЖДЕНИЕ ОБ ЭПИЛЕПСИИ\r\n"
    L"\r\n"
    L"HELL MODE strobes the whole screen up to 10 flashes per second "
    L"and pushes contrast to maximum. This can trigger seizures in people "
    L"with photosensitive epilepsy.\r\n"
    L"\r\n"
    L"HELL MODE даёт вспышки до 10 раз в секунду и максимальный контраст. "
    L"Может вызвать приступ при фоточувствительной эпилепсии.\r\n"
    L"\r\n"
    L"Do not enable if you or anyone watching has epilepsy. / "
    L"Не включайте при эпилепсии у вас или зрителей.";

static LRESULT CALLBACK HellWarnProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MiniWindow* self = reinterpret_cast<MiniWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        FillRect(hdc, &rc, black);
        // Red frame.
        HPEN red = CreatePen(PS_SOLID, 6, RGB(255, 0, 0));
        HGDIOBJ oldPen = SelectObject(hdc, red);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, rc.left + 20, rc.top + 20, rc.right - 20, rc.bottom - 20);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(red);
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctrl = reinterpret_cast<HWND>(lParam);
        if (GetDlgCtrlID(ctrl) == 100) {
            SetTextColor(hdc, RGB(255, 30, 30));
        } else {
            SetTextColor(hdc, RGB(255, 255, 255));
        }
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetStockObject(BLACK_BRUSH));
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if (!self) break;
        if (id == 101 && code == BN_CLICKED) {
            // Checkbox toggled: arm OK only when checked.
            const BOOL checked = (IsDlgButtonChecked(hwnd, 101) == BST_CHECKED);
            EnableWindow(GetDlgItem(hwnd, 102), checked);
        } else if (id == 102 && code == BN_CLICKED) {
            self->confirmHellWarning();
        }
        break;
    }
    case WM_CLOSE: {
        if (self) self->cancelHellWarning();
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void MiniWindow::showHellWarning(OverlayWindow* target) {
    if (m_hellWarn) {
        SetForegroundWindow(m_hellWarn);
        return;
    }
    m_hellTarget = target;

    HINSTANCE hi = GetModuleHandleW(nullptr);
    const wchar_t* cls = L"NonstopHellWarnClass";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HellWarnProc;
    wc.hInstance = hi;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);

    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    m_hellWarn = CreateWindowExW(
        WS_EX_TOPMOST, cls, L"HELL MODE - EPILEPSY WARNING",
        WS_POPUP | WS_VISIBLE,
        0, 0, sw, sh,
        nullptr, nullptr, hi, nullptr);
    if (!m_hellWarn) return;
    SetWindowLongPtrW(m_hellWarn, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    // Centered content block.
    const int bw = 780;
    const int bx = (sw - bw) / 2;
    int by = sh / 2 - 260;
    m_hellWarnText = CreateWindowExW(0, L"STATIC",
        L"EPILEPSY WARNING",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        bx, by - 70, bw, 50, m_hellWarn, reinterpret_cast<HMENU>(100), hi, nullptr);
    HWND body = CreateWindowExW(0, L"STATIC", kHellWarnText,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        bx, by, bw, 340, m_hellWarn, nullptr, hi, nullptr);
    (void)body;
    m_hellCheck = CreateWindowExW(0, L"BUTTON",
        L"I have read and understand the risk / Я прочитал и понимаю риск",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        bx, by + 350, bw, 30, m_hellWarn, reinterpret_cast<HMENU>(101), hi, nullptr);
    m_hellOk = CreateWindowExW(0, L"BUTTON", L"ENABLE HELL",
        WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON,
        bx + (bw - 240) / 2, by + 395, 240, 52,
        m_hellWarn, reinterpret_cast<HMENU>(102), hi, nullptr);

    // Big red title font.
    m_hellFont = CreateFontW(-44, 0, 0, 0, FW_BLACK, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
    if (m_hellFont && m_hellWarnText) {
        SendMessageW(m_hellWarnText, WM_SETFONT, reinterpret_cast<WPARAM>(m_hellFont), TRUE);
    }

    SetForegroundWindow(m_hellWarn);
    UpdateWindow(m_hellWarn);
}

void MiniWindow::confirmHellWarning() {
    if (m_hellTarget) m_hellTarget->setHellMode(true);
    closeHellWarning();
}

void MiniWindow::cancelHellWarning() {
    // Dismissed = NOT confirmed: make sure HELL stays off.
    if (m_hellTarget) m_hellTarget->setHellMode(false);
    closeHellWarning();
}

void MiniWindow::closeHellWarning() {
    if (m_hellWarn) {
        DestroyWindow(m_hellWarn);
        m_hellWarn = nullptr;
    }
    if (m_hellFont) {
        DeleteObject(m_hellFont);
        m_hellFont = nullptr;
    }
    m_hellWarnText = nullptr;
    m_hellCheck = nullptr;
    m_hellOk = nullptr;
    m_hellTarget = nullptr;
}

void MiniWindow::renderFxTab(OverlayWindow& overlay) {
    // ---- HELL MODE (separate option, epilepsy warning attached) ----
    {
        bool hell = overlay.isHellMode() || isHellWarnOpen();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.25f, 0.25f, 1.0f));
        const bool changed = ImGui::Checkbox("HELL MODE (epilepsy!)", &hell);
        ImGui::PopStyleColor();
        if (changed) {
            if (hell) {
                showHellWarning(&overlay);
            } else {
                overlay.setHellMode(false);
                closeHellWarning();
            }
        }
        if (overlay.isHellMode()) {
            ImGui::TextColored(ImVec4(1.0f, 0.15f, 0.15f, 1.0f), "HELL ENGAGED - strobing");
            float hi = overlay.getHellIntensity();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##hellk", &hi, 1.0f, 5.0f, "x%.1f (100-500%%)")) {
                overlay.setHellIntensity(hi);
            }
            const float el = overlay.getHellElapsedSec();
            char elb[48];
            std::snprintf(elb, sizeof(elb), "%02d:%02d in hell",
                static_cast<int>(el) / 60, static_cast<int>(el) % 60);
            ImGui::TextDisabled("%s | auto-off min:", elb);
            ImGui::SameLine();
            float ao = overlay.getHellAutoOffMin();
            ImGui::SetNextItemWidth(90);
            if (ImGui::SliderFloat("##hellao", &ao, 0.0f, 30.0f, "%.0f")) {
                overlay.setHellAutoOffMin(ao);
            }
        }
        if (overlay.isRepeatActive()) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.1f, 1.0f), "REPEAT %.1f Hz x%.2f",
                overlay.getRepeatHz(), overlay.getRepeatStrength());
        }
    }
    ImGui::Separator();

    float power = overlay.getFxPower();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##pwr", &power, 0.5f, 2.5f, "POWER x%.2f")) {
        overlay.setFxPower(power);
    }
    if (ImGui::Button("All on", ImVec2((ImGui::GetContentRegionAvail().x - 8) * 0.5f, 0))) {
        for (int i = 0; i < kFxModuleCount; ++i)
            overlay.setModuleEnabled(static_cast<FxModule>(i), true);
    }
    ImGui::SameLine();
    if (ImGui::Button("All off", ImVec2(-1, 0))) {
        for (int i = 0; i < kFxModuleCount; ++i)
            overlay.setModuleEnabled(static_cast<FxModule>(i), false);
    }

    for (int i = 0; i < kFxModuleCount; ++i) {
        const FxModule m = static_cast<FxModule>(i);
        bool on = overlay.isModuleEnabled(m);
        char id[64];
        std::snprintf(id, sizeof(id), "%s##m%d", fxModuleName(m), i);
        if (ImGui::Checkbox(id, &on)) {
            overlay.setModuleEnabled(m, on);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(kMiniHints[i]);
            ImGui::EndTooltip();
        }
        ImGui::SameLine(190);
        float amt = overlay.getModuleAmount(m);
        char sl[64];
        std::snprintf(sl, sizeof(sl), "##a%d", i);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat(sl, &amt, 0.0f, 1.5f, "x%.2f")) {
            overlay.setModuleAmount(m, amt);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(kMiniHints[i]);
            ImGui::EndTooltip();
        }
    }

    bool lot = overlay.isLotteryOn();
    if (ImGui::Checkbox("Surprise me on drops", &lot)) overlay.setLotteryOn(lot);
    bool drf = overlay.isDriftOn();
    if (ImGui::Checkbox("Drift between sounds", &drf)) overlay.setDriftOn(drf);
    ImGui::TextDisabled("starring: %s", overlay.getAccentString().c_str());
    ImGui::SameLine();
    if (ImGui::Button("Shuffle")) overlay.rerollSeed();

    if (ImGui::Button("Flash", ImVec2((ImGui::GetContentRegionAvail().x - 8) * 0.5f, 0))) {
        overlay.triggerRgbTestFlash();
    }
    ImGui::SameLine();
    if (ImGui::Button("Word", ImVec2(-1, 0))) {
        overlay.spawnTestWord();
    }
    if (overlay.wasLastFrameDrawn()) {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "FX: ON AIR | draws=%llu",
            overlay.getCapturedFrames());
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "FX: quiet (%s)",
            overlay.getLastSkipReason().c_str());
    }
}

void MiniWindow::renderTuneTab(OverlayWindow& overlay) {
    float v = overlay.getRgbStrengthPx();
    if (ImGui::SliderFloat("Split width (px)", &v, 0.0f, 60.0f, "%.1f")) overlay.setRgbStrengthPx(v);
    v = overlay.getRgbDecay();
    if (ImGui::SliderFloat("Flash length", &v, 0.005f, 0.30f, "%.3f")) overlay.setRgbDecay(v);
    v = overlay.getGhostMix();
    if (ImGui::SliderFloat("Echo trails", &v, 0.0f, 1.0f, "%.2f")) overlay.setGhostMix(v);
    v = overlay.getSatBoost();
    if (ImGui::SliderFloat("Color juice", &v, 0.0f, 1.0f, "%.2f")) overlay.setSatBoost(v);
    v = overlay.getDriftAmp();
    if (ImGui::SliderFloat("Drift", &v, 0.0f, 0.006f, "%.4f")) overlay.setDriftAmp(v);
}

void MiniWindow::renderMusicTab(OverlayWindow& overlay, WasapiCapture& capture,
                                FftAnalyzer& analyzer, PhaseDetector& detector) {
    const AudioAnalysisSnapshot snap = analyzer.getSnapshot();
    const PhaseState st = overlay.getPhaseState();
    const char* pname = "?";
    ImVec4 lamp = ImVec4(0.5f, 0.5f, 0.55f, 1.0f);
    switch (st.phase) {
    case TrackPhase::CALM: pname = "quiet"; break;
    case TrackPhase::BUILD: pname = "building"; lamp = ImVec4(0.95f, 0.75f, 0.2f, 1.0f); break;
    case TrackPhase::DROP: pname = "DROP"; lamp = ImVec4(1.0f, 0.25f, 0.45f, 1.0f); break;
    case TrackPhase::RELEASE: pname = "cooling"; lamp = ImVec4(0.2f, 0.85f, 1.0f, 1.0f); break;
    }
    ImGui::TextColored(lamp, "%s", pname);
    ImGui::SameLine();
    ImGui::TextDisabled("(%.1fs)", st.timeInPhase);
    ImGui::ProgressBar(std::clamp(overlay.getPhaseMaster(), 0.0f, 1.0f), ImVec2(-1, 12), "rave energy");

    // Levels.
    char txt[48];
    float rmsDb = (snap.rms > 0.0001f) ? (20.0f * std::log10(snap.rms)) : -60.0f;
    std::snprintf(txt, sizeof(txt), "in %.1f dB", rmsDb);
    ImGui::ProgressBar(std::clamp(snap.rms * 2.0f, 0.0f, 1.0f), ImVec2(-1, 12), txt);
    auto band = [&](const char* name, float val, ImVec4 c) {
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, c);
        ImGui::ProgressBar(std::clamp(val, 0.0f, 1.0f), ImVec2(-1, 10), name);
        ImGui::PopStyleColor();
    };
    band("sub", snap.bandsSmoothed.subBass, ImVec4(0.71f, 0.09f, 0.62f, 1.0f));
    band("bass", snap.bandsSmoothed.bass, ImVec4(0.45f, 0.04f, 0.72f, 1.0f));
    band("mids", snap.bandsSmoothed.mids, ImVec4(0.26f, 0.38f, 0.93f, 1.0f));
    band("highs", snap.bandsSmoothed.highs, ImVec4(0.00f, 0.96f, 0.83f, 1.0f));

    if (snap.isOnset) {
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.1f, 1.0f), "BEAT");
    } else {
        ImGui::TextDisabled("listening...");
    }
    float sens = analyzer.getSensitivity();
    if (ImGui::SliderFloat("Ear sensitivity", &sens, 0.2f, 3.0f, "%.1fx")) {
        analyzer.setSensitivity(sens);
    }
    float th = analyzer.getOnsetThreshold();
    if (ImGui::SliderFloat("Beat threshold", &th, 1.0f, 3.0f, "%.2fx")) {
        analyzer.setOnsetThreshold(th);
    }

    if (ImGui::CollapsingHeader("Detector tuning")) {
        ImGui::TextDisabled("slope %.3f | onsets %.1f/s", st.bassSlope, st.onsetDensity);
        ImGui::BeginChild("PhaseLog", ImVec2(0, 70), true);
        for (const auto& line : detector.getLog()) {
            ImGui::TextDisabled("%s", line.c_str());
        }
        ImGui::EndChild();
        PhaseParams& p = detector.params();
        ImGui::SliderFloat("drop jump", &p.dropJump, 0.05f, 0.60f, "%.3f");
        ImGui::SliderFloat("build slope", &p.buildSlopeThresh, 0.005f, 0.150f, "%.3f");
        ImGui::SliderFloat("onset dens", &p.onsetDensityThresh, 1.0f, 8.0f, "%.1f");
        ImGui::SliderFloat("rel ratio", &p.releaseRatio, 0.30f, 0.80f, "%.2f");
        ImGui::SliderFloat("rel time", &p.releaseTime, 0.5f, 6.0f, "%.1f");
        ImGui::SliderFloat("calm floor", &p.calmFloor, 0.03f, 0.30f, "%.3f");
    }
}

void MiniWindow::renderMoreTab(OverlayWindow& overlay, WasapiCapture& capture,
                               const HotkeyManager& hotkeyMgr) {
    if (!m_devicesLoaded) {
        m_cachedDevices = capture.enumerateDevices();
        m_devicesLoaded = true;
    }
    std::string preview = "audio device...";
    if (m_selectedDeviceIndex >= 0
        && m_selectedDeviceIndex < static_cast<int>(m_cachedDevices.size())) {
        preview = wideToUtf8Mini(m_cachedDevices[m_selectedDeviceIndex].name);
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##dev", preview.c_str())) {
        for (int i = 0; i < static_cast<int>(m_cachedDevices.size()); ++i) {
            const bool sel = (m_selectedDeviceIndex == i);
            std::string label = wideToUtf8Mini(m_cachedDevices[i].name);
            if (m_cachedDevices[i].isDefault) label += " [default]";
            if (ImGui::Selectable(label.c_str(), sel)) {
                m_selectedDeviceIndex = i;
                capture.start(m_cachedDevices[i].id);
            }
        }
        ImGui::EndCombo();
    }
    if (capture.isRunning()) {
        if (ImGui::Button("Stop sound capture", ImVec2(-1, 0))) capture.stop();
    } else {
        if (ImGui::Button("Start sound capture", ImVec2(-1, 0))) capture.start(L"");
    }
    const AudioFormatInfo fmt = capture.getFormatInfo();
    ImGui::TextDisabled("%u Hz %uch buf %.1f ms", fmt.sampleRate, fmt.channels,
        fmt.bufferLatencyMs);
    ImGui::Separator();

    bool streamer = overlay.isStreamerMode();
    if (ImGui::Checkbox("Streamer mode", &streamer)) overlay.setStreamerMode(streamer);
    ImGui::TextDisabled("OFF: friends see it (share SCREEN). ON: clean stream.");
    ImGui::TextDisabled("capture hidden: %s",
        overlay.isExcludedFromCapture() ? "YES" : "NO");
    ImGui::Separator();

    ImGui::TextDisabled("keys: %d (Ctrl+Alt+X)", hotkeyMgr.getPressCount());
    if (overlay.isVisible()) {
        if (ImGui::Button("Hide rave", ImVec2(-1, 0))) overlay.setVisible(false);
    } else {
        if (ImGui::Button("Show rave", ImVec2(-1, 0))) overlay.setVisible(true);
    }
    if (ImGui::Button("Quit NONSTOP", ImVec2(-1, 0))) {
        PostQuitMessage(0);
    }
}

void MiniWindow::render(OverlayWindow& overlay, WasapiCapture& capture,
                        FftAnalyzer& analyzer, PhaseDetector& detector,
                        const HotkeyManager& hotkeyMgr) {
    if (!m_hwnd || !m_visible || !m_ctx || IsIconic(m_hwnd)) return;

    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(m_ctx);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2((float)m_width, (float)m_height), ImGuiCond_Always);
    ImGui::Begin("MINI", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    if (ImGui::BeginTabBar("MiniTabs")) {
        if (ImGui::BeginTabItem("FX")) {
            ImGui::BeginChild("FxScroll", ImVec2(0, 0), false,
                ImGuiWindowFlags_AlwaysVerticalScrollbar);
            renderFxTab(overlay);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Tune")) {
            ImGui::BeginChild("TuneScroll", ImVec2(0, 0), false,
                ImGuiWindowFlags_AlwaysVerticalScrollbar);
            renderTuneTab(overlay);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Music")) {
            ImGui::BeginChild("MusicScroll", ImVec2(0, 0), false,
                ImGuiWindowFlags_AlwaysVerticalScrollbar);
            renderMusicTab(overlay, capture, analyzer, detector);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("More")) {
            ImGui::BeginChild("MoreScroll", ImVec2(0, 0), false,
                ImGuiWindowFlags_AlwaysVerticalScrollbar);
            renderMoreTab(overlay, capture, hotkeyMgr);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
    ImGui::Render();

    m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
    const float clear[4] = { 0.07f, 0.07f, 0.09f, 1.0f };
    m_context->ClearRenderTargetView(m_rtv.Get(), clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    m_swapChain->Present(1, 0);

    ImGui::SetCurrentContext(prev);
}

} // namespace nonstop
