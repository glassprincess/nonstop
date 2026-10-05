#include "overlay/overlay_window.h"

#include <dwmapi.h>
#include <d3dcompiler.h>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <sstream>

namespace nonstop {

const char* fxModuleName(FxModule m) {
    switch (m) {
    case FxModule::RGB: return "RGB-split + echoes";
    case FxModule::SLICE: return "Slices + glitch blocks";
    case FxModule::WARP: return "Warp waves";
    case FxModule::MELT: return "Screen melt";
    case FxModule::RIPPLE: return "Ripple shockwaves";
    case FxModule::GLASS: return "Glass (drops/frost/prism)";
    case FxModule::ZOOM: return "Zoom punch + shake";
    case FxModule::TEXT: return "Ghost words";
    case FxModule::JELLY: return "Jelly [BETA]";
    case FxModule::STREAK: return "Sort streaks [BETA]";
    case FxModule::DUP: return "Duplication cascade";
    default: return "?";
    }
}

// whisper lines for the ghost words. short and cryptic, english only.
static const wchar_t* kGhostPhrases[] = {
    L"i am here", L"watch me", L"lost in static", L"stay with me",
    L"do not blink", L"it sees you", L"hear the signal", L"behind your eyes",
    L"no way back", L"between frames", L"sleep no more", L"follow the noise",
    L"you cannot leave", L"breathe with me", L"everywhere and nowhere", L"fade into static",
};
static constexpr int kGhostPhraseCount = 16;

struct OverlayVertex {
    float x, y;
    float r, g, b, a;
};

struct ScreenVertex {
    float x, y;
    float u, v;
};

struct ScreenParams {
    // one uber-shader, 6 float4s of knobs.
    float rgbOx, rgbOy, alpha, time;
    float ghostMix, satBoost, driftAmp, streakAmt;
    float sliceAmp, blockAmp, blockSeed, waveAmp;
    float meltAmp, rippleAmp, rippleT, zoomAmt;
    float shakeAmp, glassAmp, frostAmt, prismAmt;
    float rippleCx, rippleCy, blast, jellyAmt;
};

struct TextParams {
    float alpha;
    float seed;
    float keep;
    float pad;
};

// HELL extras live in their own cbuffer (b1).
struct FxHellParams {
    float invertAmt;  // repeat-synced negative flip (HELL only)
    float mirrorAmt;  // brief asymmetric mirror band on DROP (HELL only)
    float hellMaster; // 1..5 intensity mirror for shader-side boosts
    float hpad;
};

// Duplication cascade (b2): whole copies of the shot drifting apart.
// Not the RGB split - full frames shifted by tens of pixels.
struct FxDupParams {
    float dupN;                 // smoothed active copy count 0..3
    float dupAa, dupAb, dupAc;  // per-copy weights
    float dupAx, dupAy;         // copy A offset (uv): right
    float dupBx, dupBy;         // copy B offset (uv): left-down
    float dupCx, dupCy;         // copy C offset (uv): right-up
    float dupMix, dpad1, dpad2, dpad3;
    float dpad4, dpad5; // 16 floats = 64 bytes total (D3D11 CB alignment)
};

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCHITTEST:
        // let every click fall through to the game
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        // never steal focus
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

OverlayWindow::OverlayWindow() = default;

OverlayWindow::~OverlayWindow() {
    shutdown();
}

bool OverlayWindow::create(int width, int height) {
    shutdown();
    m_statusString = "Init"; // shutdown() leaves "Shutdown" — reset the prefix

    m_width = (width > 0) ? width : GetSystemMetrics(SM_CXSCREEN);
    m_height = (height > 0) ? height : GetSystemMetrics(SM_CYSCREEN);

    HINSTANCE hInstance = GetModuleHandleW(nullptr);
    const wchar_t* className = L"NonstopOverlayTransparentClass";

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = className;
    RegisterClassExW(&wc);

    // topmost, transparent, click-through, no taskbar button blinking
    DWORD exStyle = WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    DWORD style = WS_POPUP | WS_VISIBLE;

    m_hwnd = CreateWindowExW(
        exStyle,
        className,
        L"NONSTOP_OVERLAY_SURFACE",
        style,
        0, 0,
        m_width, m_height,
        nullptr, nullptr,
        hInstance, nullptr
    );

    if (!m_hwnd) {
        DWORD err = GetLastError();
        m_statusString = "CreateWindowEx failed with error code: " + std::to_string(err);
        return false;
    }

    // layered windows need this for DWM to draw them
    SetLayeredWindowAttributes(m_hwnd, RGB(0, 0, 0), 255, LWA_ALPHA);

    // glass over the whole window so alpha blending is real
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);

    // streamer mode keeps us out of every capture.
    // heads up: when everyone can see us, our own capture sees us too
    // (feedback loop) - alpha is damped to keep it calm.
    applyCaptureAffinity();

    if (!initD3D()) {
        shutdown();
        return false;
    }

    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(m_hwnd);

    // stay on top of the game
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, m_width, m_height, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    m_visible.store(true);
    m_rng.seed(m_seed); // deterministic default; EFFECTS menu can reroll
    // don't wipe this: shader errors land here, and without them
    // a dead FX chain looks perfectly fine while showing nothing.
    m_statusString += " | Active (D3D11 HWND: " + std::to_string(reinterpret_cast<uintptr_t>(m_hwnd)) + ", WDA OK: " + (m_excludedFromCapture ? "YES" : "NO") + ")";
    return true;
}

bool OverlayWindow::initD3D() {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = m_width;
    sd.BufferDesc.Height = m_height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 165;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createFlags = 0;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL outFeatureLevel;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &sd,
        m_swapChain.GetAddressOf(),
        m_d3dDevice.GetAddressOf(),
        &outFeatureLevel,
        m_d3dContext.GetAddressOf()
    );

    if (FAILED(hr)) {
        std::stringstream ss;
        ss << "D3D11CreateDeviceAndSwapChain failed (0x" << std::hex << hr << ")";
        m_statusString = ss.str();
        return false;
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) {
        m_statusString = "GetBuffer(0) failed";
        return false;
    }

    hr = m_d3dDevice->CreateRenderTargetView(backBuffer.Get(), nullptr, m_renderTargetView.GetAddressOf());
    if (FAILED(hr)) {
        m_statusString = "CreateRenderTargetView failed";
        return false;
    }

    // normal alpha blending
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = m_d3dDevice->CreateBlendState(&blendDesc, m_blendState.GetAddressOf());
    if (FAILED(hr)) {
        m_statusString = "CreateBlendState failed";
        return false;
    }

    // plain colored-quad shaders
    const char* shaderSource = R"(
        struct VS_INPUT {
            float2 pos : POSITION;
            float4 col : COLOR;
        };
        struct PS_INPUT {
            float4 pos : SV_POSITION;
            float4 col : COLOR;
        };
        PS_INPUT vs_main(VS_INPUT input) {
            PS_INPUT output;
            output.pos = float4(input.pos, 0.0f, 1.0f);
            output.col = input.col;
            return output;
        }
        float4 ps_main(PS_INPUT input) : SV_TARGET {
            return input.col;
        }
    )";

    ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;
    hr = D3DCompile(shaderSource, strlen(shaderSource), nullptr, nullptr, nullptr, "vs_main", "vs_4_0", 0, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        m_statusString = "VS compile failed";
        return false;
    }

    hr = D3DCompile(shaderSource, strlen(shaderSource), nullptr, nullptr, nullptr, "ps_main", "ps_4_0", 0, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        m_statusString = "PS compile failed";
        return false;
    }

    hr = m_d3dDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_vs.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = m_d3dDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_ps.GetAddressOf());
    if (FAILED(hr)) return false;

    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = m_d3dDevice->CreateInputLayout(layoutDesc, ARRAYSIZE(layoutDesc), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), m_inputLayout.GetAddressOf());
    if (FAILED(hr)) return false;

    // scratch vertex buffer
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = sizeof(OverlayVertex) * 16384;
    vbDesc.Usage = D3D11_USAGE_DYNAMIC;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_d3dDevice->CreateBuffer(&vbDesc, nullptr, m_vertexBuffer.GetAddressOf());
    if (FAILED(hr)) return false;

    // screen FX pipeline.
    if (!initScreenPipeline()) {
        m_statusString += " [screen pipeline FAILED]";
    }

    // hook desktop duplication to the same device. No capture -
    // no picture pass, overlay just stays clear.
    if (m_capturer.init(m_d3dDevice.Get())) {
        m_statusString += std::string(" [capture: ") + m_capturer.getStatusString() + "]";
    } else {
        m_statusString += std::string(" [capture OFF: ") + m_capturer.getStatusString() + "]";
    }

    return true;
}

bool OverlayWindow::initScreenPipeline() {
    const char* screenVsSrc = R"(
        struct VS_IN {
            float2 pos : POSITION;
            float2 uv : TEXCOORD0;
        };
        struct PS_IN {
            float4 pos : SV_POSITION;
            float2 uv : TEXCOORD0;
        };
        PS_IN vs_screen(VS_IN input) {
            PS_IN output;
            output.pos = float4(input.pos, 0.0f, 1.0f);
            output.uv = input.uv;
            return output;
        }
    )";
    const char* screenPsSrc = R"(
        Texture2D screenTex : register(t0);
        SamplerState screenSamp : register(s0);
        cbuffer FxParams : register(b0) {
            float2 rgbOff; float alpha; float time;
            float ghostMix; float satBoost; float driftAmp; float streakAmt;
            float sliceAmp; float blockAmp; float blockSeed; float waveAmp;
            float meltAmp; float rippleAmp; float rippleT; float zoomAmt;
            float shakeAmp; float glassAmp; float frostAmt; float prismAmt;
            float2 rippleC; float blast; float jellyAmt;
        };
        cbuffer FxHell : register(b1) {
            float invertAmt; float mirrorAmt; float hellMaster; float hpad;
        };
        cbuffer FxDup : register(b2) {
            float dupN; float dupAa; float dupAb; float dupAc;
            float dupAx; float dupAy; float dupBx; float dupBy;
            float dupCx; float dupCy; float dupMix; float dpad1;
            float dpad2; float dpad3; float dpad4; float dpad5;
        };
        struct PS_IN {
            float4 pos : SV_POSITION;
            float2 uv : TEXCOORD0;
        };
        float hash21(float2 p) {
            p = frac(p * float2(123.34, 456.21));
            p += dot(p, p + 45.32);
            return frac(p.x * p.y);
        }
        float vnoise(float2 p) {
            float2 i = floor(p);
            float2 f = frac(p);
            f = f * f * (3.0 - 2.0 * f);
            float a = hash21(i);
            float b = hash21(i + float2(1.0, 0.0));
            float c = hash21(i + float2(0.0, 1.0));
            float d = hash21(i + float2(1.0, 1.0));
            return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
        }
        float4 ps_screen(PS_IN input) : SV_TARGET {
            float2 uv = input.uv;
            // 1. transparent drift
            uv += float2(
                sin(uv.y * 6.0 + time * 0.7),
                cos(uv.x * 5.0 - time * 0.6)) * driftAmp;
            // 2. zoom punch + shake (camera kick on beats, blast adds extra)
            float zoomEff = zoomAmt + blast * 0.8;
            uv = (uv - 0.5) / (1.0 + zoomEff * 0.30) + 0.5;
            float2 sh = float2(hash21(float2(floor(time * 30.0), 3.7)),
                               hash21(float2(floor(time * 30.0), 9.1))) - 0.5;
            uv += sh * (shakeAmp + blast * 1.0) * 0.03;
            // 2b. jelly wobble [BETA]: elastic whole-screen breathing.
            float jang = jellyAmt * 0.06;
            float jcj = cos(jang);
            float jsj = sin(jang);
            float2 jc = uv - 0.5;
            uv = 0.5 + float2(jc.x * jcj - jc.y * jsj, jc.x * jsj + jc.y * jcj)
                 * (1.0 + jellyAmt * 0.08);
            // 3. melt: drip downward, per-column noise
            float colN = vnoise(float2(uv.x * 24.0, 1.5));
            uv.y = uv.y + meltAmp * colN * (1.0 - uv.y) * 0.35;
            // 4. warp waves
            uv.x += sin(uv.y * 30.0 + time * 3.0) * waveAmp * 0.012;
            uv.y += cos(uv.x * 24.0 - time * 2.2) * waveAmp * 0.009;
            // 5. ripple shockwave ring
            float2 rc = uv - rippleC;
            float rd = max(length(rc), 1e-4);
            float ring = sin(rd * 90.0 - rippleT * 18.0)
                       * exp(-rd * 5.0) * exp(-rippleT * 2.2) * rippleAmp;
            uv += (rc / rd) * ring * 0.03;
            // 6. thin row slices
            float row = floor(uv.y * 180.0);
            float h1 = hash21(float2(row, blockSeed));
            float sMask = step(h1, sliceAmp * 0.30);
            uv.x += (hash21(float2(row, blockSeed + 7.0)) - 0.5) * sliceAmp * 0.05 * sMask;
            // 7. chunky glitch blocks
            float2 cell = floor(uv * float2(24.0, 14.0));
            float hb = hash21(cell + blockSeed);
            float bMask = step(hb, blockAmp * 0.12);
            uv += (float2(hash21(cell + blockSeed + 3.0),
                          hash21(cell + blockSeed + 11.0)) - 0.5)
                  * blockAmp * 0.12 * bMask;
            // 8. glass rain: droplets sliding down the glass refract UV.
            // Each drop pulls nearby pixels toward its center (lens) and
            // leaves a bright rim highlight.
            float rim = 0.0;
            for (int k = 0; k < 6; k++) {
                float fk = float(k);
                float lane = hash21(float2(fk * 3.1, 1.7));
                float fall = 0.010 + 0.030 * hash21(float2(fk * 7.7, 4.2));
                float yy = frac(hash21(float2(fk * 5.3, 9.1)) - time * fall);
                float xx = lane + sin(time * 0.3 + fk * 2.1) * 0.02;
                float2 dv = uv - float2(xx, yy);
                float dd = max(length(dv * float2(1.6, 1.0)), 1e-4);
                float rad = 0.06 + 0.05 * hash21(float2(fk, 3.3));
                float prof = exp(-dd * dd / (rad * rad));
                uv -= (dv / dd) * prof * glassAmp * 0.05;
                float ringd = (dd - rad) * 22.0;
                rim += exp(-ringd * ringd) * glassAmp;
            }
            // 9. main taps + wide ghost echoes (+ radial prism fringes).
            // Blast stretches the split while it lasts.
            float2 off = rgbOff * (1.0 + blast * 1.5);
            float2 pv = (uv - 0.5) * prismAmt * 0.03;
            float r0 = screenTex.Sample(screenSamp, uv + off + pv).r;
            float g0 = screenTex.Sample(screenSamp, uv).g;
            float b0 = screenTex.Sample(screenSamp, uv - off - pv).b;
            float r1 = screenTex.Sample(screenSamp, uv + off * 2.7 + pv * 2.0).r;
            float g1 = screenTex.Sample(screenSamp, uv + off * 1.6).g;
            float b1 = screenTex.Sample(screenSamp, uv - off * 2.7 - pv * 2.0).b;
            float r = lerp(r0, r1, ghostMix * 0.5);
            float g = lerp(g0, g1, ghostMix * 0.35);
            float b = lerp(b0, b1, ghostMix * 0.5);
            float3 col = float3(r, g, b) + rim * 0.10;
            // 9b. duplication cascade: whole copies drift apart
            // with tinted trails (the REPEAT effect, copies not channels).
            float3 dup = float3(0.0, 0.0, 0.0);
            dup += screenTex.Sample(screenSamp, input.uv + float2(dupAx, dupAy)).rgb
                 * float3(1.0, 0.35, 0.35) * dupAa;
            dup += screenTex.Sample(screenSamp, input.uv + float2(dupBx, dupBy)).rgb
                 * float3(0.35, 1.0, 0.35) * dupAb;
            dup += screenTex.Sample(screenSamp, input.uv + float2(dupCx, dupCy)).rgb
                 * float3(0.40, 0.40, 1.0) * dupAc;
            col += dup * dupMix / (1.0 + dupN * 0.6);
            // 10. frosted glass: soft 4-tap blur over the result
            float2 fpx = float2(1.0 / 1920.0, 1.0 / 1080.0) * (1.0 + frostAmt * 8.0);
            float3 favg = (screenTex.Sample(screenSamp, uv + float2(fpx.x, 0.0)).rgb
                         + screenTex.Sample(screenSamp, uv - float2(fpx.x, 0.0)).rgb
                         + screenTex.Sample(screenSamp, uv + float2(0.0, fpx.y)).rgb
                         + screenTex.Sample(screenSamp, uv - float2(0.0, fpx.y)).rgb) * 0.25;
            col = lerp(col, favg + rim * 0.10, frostAmt * 0.8);
            // 10b. sort streaks [BETA]: bright pixels smear downward
            float sl = dot(col, float3(0.299, 0.587, 0.114));
            float2 suv = uv - float2(0.0, sl * streakAmt * 0.14);
            float3 scol = screenTex.Sample(screenSamp, suv).rgb;
            col = lerp(col, scol, clamp(streakAmt, 0.0, 1.0) * 0.9);
            // 12. HELL: repeat-synced negative flip + drop mirror band
            col = lerp(col, 1.0 - col, invertAmt);
            float mband = (1.0 - smoothstep(0.10, 0.14, abs(uv.y - 0.5))) * mirrorAmt;
            float3 mcol = screenTex.Sample(screenSamp, float2(1.0 - uv.x, uv.y)).rgb;
            col = lerp(col, mcol, mband * 0.7);
            // 11. vibrance push away from gray (+hell extra juice)
            float luma = dot(col, float3(0.299, 0.587, 0.114));
            col = lerp(float3(luma, luma, luma), col, 1.0 + satBoost + hellMaster * 0.05);
            return float4(col, alpha);
        }
    )";
    const char* textPsSrc = R"(
        Texture2D atlas : register(t0);
        SamplerState samp : register(s0);
        cbuffer TextParams : register(b0) {
            float alpha; float seed; float keep; float pad;
        };
        struct PS_IN {
            float4 pos : SV_POSITION;
            float2 uv : TEXCOORD0;
        };
        float hash21(float2 p) {
            p = frac(p * float2(234.34, 435.345));
            p += dot(p, p + 34.23);
            return frac(p.x * p.y);
        }
        float4 ps_text(PS_IN input) : SV_TARGET {
            float3 c = atlas.Sample(samp, input.uv).rgb;
            float lum = dot(c, float3(0.299, 0.587, 0.114));
            float d = hash21(input.uv * float2(911.0, 547.0) + seed * 17.0);
            float a = lum * alpha * step(d, keep);
            return float4(float3(0.75, 0.95, 1.0) * lum, a);
        }
    )";

    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    auto shaderErr = [&]() -> std::string {
        if (errBlob && errBlob->GetBufferPointer()) {
            return std::string(static_cast<const char*>(errBlob->GetBufferPointer()),
                               errBlob->GetBufferSize());
        }
        return "unknown compile error";
    };
    HRESULT hr = D3DCompile(screenVsSrc, strlen(screenVsSrc), nullptr, nullptr, nullptr,
                            "vs_screen", "vs_4_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        m_statusString += std::string(" [vs_screen FAILED: ") + shaderErr() + "]";
        return false;
    }
    hr = D3DCompile(screenPsSrc, strlen(screenPsSrc), nullptr, nullptr, nullptr,
                    "ps_screen", "ps_4_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        m_statusString += std::string(" [ps_fx FAILED: ") + shaderErr() + "]";
        return false;
    }

    hr = m_d3dDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                         nullptr, m_screenVs.GetAddressOf());
    if (FAILED(hr)) return false;
    hr = m_d3dDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                        nullptr, m_screenPs.GetAddressOf());
    if (FAILED(hr)) return false;

    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = m_d3dDevice->CreateInputLayout(layoutDesc, ARRAYSIZE(layoutDesc),
                                        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                        m_screenLayout.GetAddressOf());
    if (FAILED(hr)) return false;

    // fullscreen quad, UVs match the captured frame.
    const ScreenVertex quad[6] = {
        { -1.0f,  1.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 1.0f, 0.0f },
        { -1.0f, -1.0f, 0.0f, 1.0f },
        {  1.0f,  1.0f, 1.0f, 0.0f },
        {  1.0f, -1.0f, 1.0f, 1.0f },
        { -1.0f, -1.0f, 0.0f, 1.0f },
    };
    D3D11_BUFFER_DESC quadDesc = {};
    quadDesc.ByteWidth = sizeof(quad);
    quadDesc.Usage = D3D11_USAGE_IMMUTABLE;
    quadDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA quadData = {};
    quadData.pSysMem = quad;
    hr = m_d3dDevice->CreateBuffer(&quadDesc, &quadData, m_screenVb.GetAddressOf());
    if (FAILED(hr)) return false;

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = m_d3dDevice->CreateSamplerState(&sampDesc, m_screenSampler.GetAddressOf());
    if (FAILED(hr)) return false;

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(ScreenParams);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_d3dDevice->CreateBuffer(&cbDesc, nullptr, m_screenParamsCb.GetAddressOf());
    if (FAILED(hr)) return false;

    // HELL extras (b1): starts zeroed, filled every frame.
    D3D11_BUFFER_DESC hellDesc = {};
    hellDesc.ByteWidth = sizeof(FxHellParams);
    hellDesc.Usage = D3D11_USAGE_DYNAMIC;
    hellDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hellDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_d3dDevice->CreateBuffer(&hellDesc, nullptr, m_hellCb.GetAddressOf());
    if (FAILED(hr)) {
        m_statusString += " [hell CB FAILED]";
        return false;
    }

    // dup uniforms (b2): starts zeroed, filled every frame.
    D3D11_BUFFER_DESC dupDesc = {};
    dupDesc.ByteWidth = sizeof(FxDupParams);
    dupDesc.Usage = D3D11_USAGE_DYNAMIC;
    dupDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dupDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_d3dDevice->CreateBuffer(&dupDesc, nullptr, m_dupCb.GetAddressOf());
    if (FAILED(hr)) {
        m_statusString += " [dup CB FAILED]";
        return false;
    }

    // text shader, same vertex layout.
    ComPtr<ID3DBlob> textBlob;
    hr = D3DCompile(textPsSrc, strlen(textPsSrc), nullptr, nullptr, nullptr,
                    "ps_text", "ps_4_0", 0, 0, &textBlob, &errBlob);
    if (FAILED(hr)) {
        m_statusString += std::string(" [ps_text FAILED: ") + shaderErr() + "]";
        return false;
    }
    hr = m_d3dDevice->CreatePixelShader(textBlob->GetBufferPointer(), textBlob->GetBufferSize(),
                                        nullptr, m_textPs.GetAddressOf());
    if (FAILED(hr)) return false;

    // scratch buffer for word quads.
    D3D11_BUFFER_DESC textVbDesc = {};
    textVbDesc.ByteWidth = sizeof(ScreenVertex) * 6 * kMaxWords;
    textVbDesc.Usage = D3D11_USAGE_DYNAMIC;
    textVbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    textVbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_d3dDevice->CreateBuffer(&textVbDesc, nullptr, m_textVb.GetAddressOf());
    if (FAILED(hr)) return false;

    D3D11_BUFFER_DESC textCbDesc = {};
    textCbDesc.ByteWidth = sizeof(TextParams);
    textCbDesc.Usage = D3D11_USAGE_DYNAMIC;
    textCbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    textCbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_d3dDevice->CreateBuffer(&textCbDesc, nullptr, m_textCb.GetAddressOf());
    if (FAILED(hr)) return false;

    // if the atlas fails, words just stay off.
    if (!bakeTextAtlas()) {
        m_modules[static_cast<int>(FxModule::TEXT)] = false;
        m_statusString += " [text atlas FAILED]";
    }

    return true;
}

void OverlayWindow::setVisible(bool visible) {
    m_visible.store(visible);
    if (m_hwnd) {
        ShowWindow(m_hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (visible) {
            SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, m_width, m_height, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }
}

void OverlayWindow::setStreamerMode(bool hidden) {
    m_hiddenFromCapture = hidden;
    applyCaptureAffinity();
}

void OverlayWindow::setHellMode(bool on) {
    if (on == m_hellMode) return;
    m_hellMode = on;
    if (on) {
        m_hellSinceSec = m_timeSec;
        m_hellInv = 0.0f;
        m_invPolarity = 0;
        m_lastInvFlipSec = -1e9;
        m_mirrorT = 10.0f;
    } else {
        // smoothing pulls invert back, never gets stuck on.
        m_invPolarity = 0;
    }
}

float OverlayWindow::getHellElapsedSec() const {
    if (!m_hellMode) return 0.0f;
    return static_cast<float>(m_timeSec - m_hellSinceSec);
}

void OverlayWindow::applyCaptureAffinity() {
    if (!m_hwnd) {
        m_excludedFromCapture = m_hiddenFromCapture;
        return;
    }
    SetWindowDisplayAffinity(m_hwnd,
        m_hiddenFromCapture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
    DWORD got = 0;
    if (GetWindowDisplayAffinity(m_hwnd, &got)) {
        m_excludedFromCapture = (got == WDA_EXCLUDEFROMCAPTURE);
    } else {
        m_excludedFromCapture = m_hiddenFromCapture;
    }
}

void OverlayWindow::toggleVisible() {
    setVisible(!m_visible.load());
}

void OverlayWindow::renderFrame(const AudioAnalysisSnapshot& snapshot) {
    if (!m_hwnd || !m_visible.load() || !m_d3dContext || !m_swapChain) return;

    // stay on top of the game
    m_renderedFrames++;
    if (m_renderedFrames % 120 == 0) {
        SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // Viewport
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(m_width);
    vp.Height = static_cast<float>(m_height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_d3dContext->RSSetViewports(1, &vp);

    // Clear render target to completely transparent (alpha = 0.0)
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_d3dContext->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), nullptr);
    m_d3dContext->ClearRenderTargetView(m_renderTargetView.Get(), clearColor);

    // bent copy of the real screen (amps from band/beat/phase)
    // + ghost words on top. No drawn shapes, just the screen itself.
    renderFxChain(snapshot);

    // show it.
    m_swapChain->Present(1, 0);

    // frame log for debugging: did we draw, and if not, why. A line every ~2s.
    if (m_renderedFrames % 120 == 1) {
        std::ofstream log("nonstop_fx.log", std::ios::app);
        if (log) {
            int modmask = 0;
            for (int i = 0; i < kFxModuleCount; ++i) {
                if (m_modules[i]) modmask |= (1 << i);
            }
            log << "frame=" << m_renderedFrames
                << " phase=" << toString(m_phase.phase)
                << " master=" << m_phaseMaster
                << " drive=" << m_lastDrive
                << " intensity=" << m_lastIntensity
                << " drawn=" << (m_lastDrawn ? "yes" : "no")
                << " skip=" << m_lastSkipReason
                << " modmask=" << modmask
                << " P=" << m_fxPower
                << " amt0=" << m_amounts[0]
                << " draws=" << m_capturedFrames
                << " wordsAlive=" << m_aliveWords
                << " wordsTotal=" << m_totalSpawned
                << " lastPhrase=" << m_lastPhraseIdx
                << " jelly=" << m_lastJelly
                << " streak=" << m_lastStreak
                << " blast=" << m_blast
                << " hell=" << (m_hellMode ? 1 : 0)
                << " repHz=" << m_phase.repeatHz
                << " repStr=" << m_phase.repeatStrength
                << " dup=" << m_lastDup
                << " repCand=" << m_phase.repeatCandHz
                << "/" << m_phase.repeatCandStr
                << " onDens=" << m_phase.onsetDensity
                << " pipeline=" << (isPipelineOk() ? "ok" : "FAILED")
                << " status=[" << m_statusString << "]"
                << " capture=" << m_capturer.getStatusString()
                << "\n";
        }
    }
}

void OverlayWindow::rerollSeed() {
    std::random_device rd;
    m_seed = rd();
    m_rng.seed(m_seed);
    for (auto& a : m_accents) a = Accent{};
    for (auto& w : m_driftW) w = 1.0f;
    m_accentString = "none";
}

void OverlayWindow::triggerRipple(float cx, float cy) {
    m_rippleT = 0.0f;
    m_rippleCx = std::clamp(cx, 0.0f, 1.0f);
    m_rippleCy = std::clamp(cy, 0.0f, 1.0f);
}

float OverlayWindow::accentBoost(FxModule m) const {
    for (const auto& a : m_accents) {
        if (a.boost > 1.0f && m_timeSec < a.until && a.mod == m) {
            return a.boost;
        }
    }
    return 1.0f;
}

void OverlayWindow::updateRandom(const AudioAnalysisSnapshot& /*snapshot*/) {
    // weights slowly wander on their own.
    if (m_driftOn) {
        std::uniform_real_distribution<float> nudge(-0.24f, 0.24f);
        for (auto& w : m_driftW) {
            w = std::clamp(w + nudge(m_rng) * m_frameDt, 0.6f, 1.4f);
        }
    }
    // on a drop, 1-2 modules get loud for ~6s.
    if (m_lotteryOn && m_phase.phase == TrackPhase::DROP
        && m_prevPhase != TrackPhase::DROP) {        static const FxModule kPool[] = {
            FxModule::SLICE, FxModule::WARP, FxModule::RIPPLE,
            FxModule::GLASS, FxModule::ZOOM, FxModule::MELT,
            FxModule::JELLY, FxModule::STREAK, FxModule::DUP,
        };
        std::uniform_int_distribution<int> pick(0, 8);
        for (auto& a : m_accents) a = Accent{};
        const int count = 1 + (m_rng() % 2);
        std::string names;
        for (int i = 0; i < count; ++i) {
            FxModule mod = kPool[pick(m_rng)];
            m_accents[i].mod = mod;
            m_accents[i].boost = 1.9f;
            m_accents[i].until = static_cast<float>(m_timeSec) + 6.0f;
            if (!names.empty()) names += "+";
            names += fxModuleName(mod);
        }
        m_accentString = names + " x1.9";
    }
    // accents wear off.
    bool anyActive = false;
    for (const auto& a : m_accents) {
        if (a.boost > 1.0f && m_timeSec < a.until) {
            anyActive = true;
            break;
        }
    }
    if (!anyActive && m_accentString != "none"
        && m_phase.phase != TrackPhase::DROP) {
        m_accentString = "none";
    }
}

void OverlayWindow::renderFxChain(const AudioAnalysisSnapshot& snapshot) {
    // real frame time - presents don't vsync, so no 1/60 assumptions.
    LARGE_INTEGER qfreq, qnow;
    QueryPerformanceFrequency(&qfreq);
    QueryPerformanceCounter(&qnow);
    double dt = 1.0 / 60.0;
    if (m_qpcInit) {
        dt = static_cast<double>(qnow.QuadPart - m_lastFrameQpc.QuadPart)
            / static_cast<double>(qfreq.QuadPart);
        dt = std::clamp(dt, 0.0005, 0.1);
    }
    m_qpcInit = true;
    m_lastFrameQpc = qnow;
    const float fdt = static_cast<float>(dt);
    m_frameDt = fdt;
    m_timeSec += dt;

    // beat punch: 1 on a hit, then fades.
    if (snapshot.isOnset) {
        m_rgbFlash = 1.0f;
    } else {
        m_rgbFlash = std::max(0.0f, m_rgbFlash - m_rgbDecay * 60.0f * fdt);
    }
    // test button pins everything to full for a bit.
    m_testHold = std::max(0.0f, m_testHold - 1.2f * fdt);

    // --- Song-level follower: instant loudness vs slow ceiling. ---
    // Rate-based (tau attack 0.5s, release 20s) — frame-rate independent.
    const float level = std::clamp(
        std::max(snapshot.envelope, snapshot.rms * 2.0f), 0.0f, 1.0f);
    if (level > m_loudPeak) {
        m_loudPeak += (level - m_loudPeak) * (1.0f - std::exp(-fdt / 0.5f));
    } else {
        m_loudPeak += (level - m_loudPeak) * (1.0f - std::exp(-fdt / 20.0f));
    }
    m_loudPeak = std::clamp(m_loudPeak, 0.12f, 1.0f);
    m_songLevel = level;
    m_songGain = std::clamp(level / m_loudPeak, 0.0f, 1.0f);

    // phase just scales the ceiling. CALM keeps a floor
    // so it never fully dies.
    switch (m_phase.phase) {
    case TrackPhase::CALM:
        m_phaseMaster = 0.55f;
        break;
    case TrackPhase::BUILD:
        m_phaseMaster = 0.70f + 0.30f * std::pow(m_phase.buildProgress, 1.4f);
        break;
    case TrackPhase::DROP:
        m_phaseMaster = std::clamp(m_phase.dropPower, 0.70f, 1.0f);
        break;
    case TrackPhase::RELEASE:
        m_phaseMaster = std::max(
            std::exp(-3.0f * m_phase.timeInPhase / std::max(m_releaseTime, 0.2f)),
            0.40f);
        break;
    }

    // HELL MODE: session auto-off + no transparency limits.
    if (m_hellMode) {
        if (m_hellAutoOffMin > 0.0f
            && (m_timeSec - m_hellSinceSec) > m_hellAutoOffMin * 60.0) {
            setHellMode(false);
        } else {
            m_phaseMaster = std::clamp(m_phaseMaster * 1.2f + 0.35f, 0.0f, 1.2f);
        }
    }

    updateRandom(snapshot);

    // on a hit: ripple somewhere random, fresh glitch seed,
    // jelly kick, maybe a word blast.
    if (snapshot.isOnset) {
        std::uniform_real_distribution<float> u01(0.15f, 0.85f);
        if (isModuleEnabled(FxModule::RIPPLE)) {
            triggerRipple(u01(m_rng), u01(m_rng));
        }
        std::uniform_real_distribution<float> useed(0.0f, 100.0f);
        m_blockSeed = useed(m_rng);
        if (isModuleEnabled(FxModule::JELLY)) {
            // hard kick so you actually see it wobble.
            m_jellyV += 30.0f;
        }
        m_onsetTimes.push_back(m_timeSec);
    }
    while (!m_onsetTimes.empty() && m_timeSec - m_onsetTimes.front() > 0.35) {
        m_onsetTimes.pop_front();
    }
    // a drum fill is 4+ hits in 350ms. Sometimes (35%) it fires
    // the staircase, 5s cooldown either way.
    if (m_onsetTimes.size() >= 4 && m_timeSec - m_lastBlastSec > 5.0) {
        std::uniform_real_distribution<float> u01(0.0f, 1.0f);
        if (u01(m_rng) < 0.35f) {
            triggerBlastStaircase();
        }
        m_lastBlastSec = m_timeSec; // cooldown runs even on unlucky rolls
    }
    m_blast = std::max(0.0f, m_blast - 6.0f * fdt);
    // Repeat-synced invert (HELL only): flip polarity per hit, max 10/s.
    {
        float targetInv = 0.0f;
        if (m_hellMode && m_phase.repeatActive) {
            if (snapshot.isOnset && (m_timeSec - m_lastInvFlipSec) >= 0.1) {
                m_invPolarity ^= 1;
                m_lastInvFlipSec = m_timeSec;
            }
            if (m_invPolarity) {
                targetInv = m_phase.repeatStrength * 0.85f;
            }
        }
        const float k = (targetInv > m_hellInv) ? 0.9f : 0.35f;
        m_hellInv += (targetInv - m_hellInv) * k;
    }
    m_mirrorT += fdt;
    // HELL mirror band: brief asymmetric flash on DROP entry.
    if (m_hellMode && m_phase.phase == TrackPhase::DROP
        && m_prevPhase != TrackPhase::DROP) {
        m_mirrorT = 0.0f;
    }
    // Jelly spring integrate (~6Hz, light damping), real dt clamped.
    {
        const float sdt = std::min(fdt, 1.0f / 30.0f);
        m_jellyV += (-1420.0f * m_jellyX - 9.0f * m_jellyV) * sdt;
        m_jellyX = std::clamp(m_jellyX + m_jellyV * sdt, -1.5f, 1.5f);
    }
    m_rippleT += fdt;

    updateWords(snapshot);

    if (!m_screenVs || !m_screenPs || !m_screenVb || !m_screenParamsCb) return;
    if (m_width <= 0 || m_height <= 0) return;

    // must unbind first or the copy fails.
    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
    m_d3dContext->PSSetShaderResources(0, 1, nullSrv);

    ID3D11ShaderResourceView* srv = m_capturer.acquireFrame(m_d3dContext.Get());
    if (!srv) {
        m_lastSkipReason = "no capture frame";
        renderGhostWords();
        m_prevPhase = m_phase.phase;
        return; // no desktop frame yet (or capture off) — stay transparent
    }
    // UAC secure-desktop fix: a kept frame older than ~0.4s is a ghost
    // (e.g. a dismissed admin prompt) — never composite it over live content.
    if (!m_capturer.isCaptureFresh()) {
        m_lastSkipReason = "stale capture";
        renderGhostWords();
        m_prevPhase = m_phase.phase;
        return;
    }

    const float bassE =
        snapshot.bandsSmoothed.subBass * 0.65f + snapshot.bandsSmoothed.bass * 0.35f;
    const float midsE = snapshot.bandsSmoothed.mids;
    const float highsE = snapshot.bandsSmoothed.highs;
    const float beat = m_rgbFlash + bassE * 0.40f;

    // total push. Test button overrides everything.
    float drive = std::clamp(
        m_phaseMaster * (0.35f + 0.65f * m_songGain)
            + m_rgbFlash * 0.30f * std::max(m_phaseMaster, 0.15f),
        0.0f, 1.0f);
    drive = std::max(drive, m_testHold);
    const float intensity = std::clamp(beat * (0.15f + 0.85f * drive), 0.0f, 1.0f);
    m_lastDrive = drive;
    m_lastIntensity = intensity;
    m_lastDrawn = false;

    const auto on = [&](FxModule m) { return isModuleEnabled(m); };
    const auto dw = [&](FxModule m) {
        return m_driftOn ? m_driftW[static_cast<int>(m)] : 1.0f;
    };
    const auto amt = [&](FxModule m) { return m_amounts[static_cast<int>(m)]; };
    const auto acc = [&](FxModule m) { return accentBoost(m); };

    // melt runs on the RELEASE fade.
    float meltTarget = 0.0f;
    if (on(FxModule::MELT) && m_phase.phase == TrackPhase::RELEASE) {
        meltTarget = amt(FxModule::MELT) * dw(FxModule::MELT) * acc(FxModule::MELT)
            * std::clamp(1.0f - m_phase.timeInPhase / (m_releaseTime * 1.5f), 0.0f, 1.0f);
    }
    m_meltSmooth += (meltTarget - m_meltSmooth) * (1.0f - std::exp(-5.0f * fdt));

    ScreenParams params = {};
    const float rgbOn = (on(FxModule::RGB) ? 1.0f : 0.0f)
        * amt(FxModule::RGB) * dw(FxModule::RGB) * acc(FxModule::RGB);
    // one power knob for everything but words. HELL multiplies up to x5.
    const float P = m_fxPower * (m_hellMode ? m_hellIntensity : 1.0f);
    const float offsetPx = m_rgbStrengthPx * beat * (0.35f + 0.65f * drive) * rgbOn * P;
    params.rgbOx = offsetPx / static_cast<float>(m_width);
    params.rgbOy = (offsetPx * 0.25f) / static_cast<float>(m_height);
    // visible-to-everyone mode feeds our own capture too, so alpha
    // is capped to keep the loop calm. HELL allows full opaque.
    params.alpha = std::clamp(intensity, 0.0f, 1.0f)
        * (m_hellMode ? 1.0f : 0.95f)
        * (m_hiddenFromCapture ? 1.0f : 0.8f);
    params.time = static_cast<float>(m_timeSec);
    params.ghostMix = std::clamp(m_ghostMix * std::clamp(beat, 0.0f, 1.0f) * rgbOn * P, 0.0f, 1.0f);
    params.satBoost = m_satBoost * drive;
    params.driftAmp = m_driftAmp * (0.4f + 0.6f * drive) * P;
    params.streakAmt = std::clamp(
        (on(FxModule::STREAK) ? 1.0f : 0.0f)
            * amt(FxModule::STREAK) * dw(FxModule::STREAK) * acc(FxModule::STREAK)
            * std::clamp(highsE * 2.2f
                + ((m_phase.phase == TrackPhase::DROP) ? m_phase.dropPower * 0.4f : 0.0f),
                0.0f, 1.3f)
            * std::max(drive, 0.25f) * P,
        0.0f, 1.2f);
    params.blast = m_blast;
    params.jellyAmt = std::clamp(
        m_jellyX * (on(FxModule::JELLY) ? 1.0f : 0.0f)
            * amt(FxModule::JELLY) * dw(FxModule::JELLY) * acc(FxModule::JELLY)
            * std::max(drive, 0.30f) * P,
        -1.2f, 1.2f);
    m_lastJelly = params.jellyAmt;
    m_lastStreak = params.streakAmt;

    const float sliceOn = (on(FxModule::SLICE) ? 1.0f : 0.0f)
        * amt(FxModule::SLICE) * dw(FxModule::SLICE) * acc(FxModule::SLICE);
    params.sliceAmp = sliceOn * std::clamp(highsE * 1.3f + m_rgbFlash * 0.9f, 0.0f, 1.5f)
        * std::max(drive, 0.10f) * P;
    params.blockAmp = sliceOn * std::clamp(highsE * 1.1f, 0.0f, 1.2f)
        * std::clamp(drive + m_rgbFlash * 0.5f, 0.0f, 1.2f) * P;
    params.blockSeed = m_blockSeed;
    params.waveAmp = (on(FxModule::WARP) ? 1.0f : 0.0f)
        * amt(FxModule::WARP) * dw(FxModule::WARP) * acc(FxModule::WARP)
        * std::clamp(midsE * 1.3f, 0.0f, 1.0f) * std::clamp(drive + 0.15f, 0.0f, 1.0f) * P;

    params.meltAmp = m_meltSmooth * P;
    params.rippleAmp = (on(FxModule::RIPPLE) ? 1.0f : 0.0f)
        * amt(FxModule::RIPPLE) * dw(FxModule::RIPPLE) * acc(FxModule::RIPPLE)
        * std::clamp(drive + 0.30f, 0.0f, 1.0f) * P;
    params.rippleT = m_rippleT;

    // Glass: elegant air — mids shimmer, strongest in RELEASE afterglow
    // and CALM haze, restrained mid-fight so aim stays readable.
    const float glassOn = (on(FxModule::GLASS) ? 1.0f : 0.0f)
        * amt(FxModule::GLASS) * dw(FxModule::GLASS) * acc(FxModule::GLASS);
    float phaseAir = 0.45f;
    if (m_phase.phase == TrackPhase::RELEASE) phaseAir = 1.0f;
    else if (m_phase.phase == TrackPhase::CALM) phaseAir = 0.7f;
    const float glassBase = std::clamp(
        glassOn * std::clamp(midsE * 1.2f + 0.25f, 0.0f, 1.0f)
            * std::clamp(drive * 0.7f + 0.3f, 0.0f, 1.0f) * phaseAir * P,
        0.0f, 1.2f);
    params.glassAmp = glassBase;
    params.frostAmt = std::clamp(
        glassBase * (m_phase.phase == TrackPhase::RELEASE ? 0.9f : 0.35f), 0.0f, 1.0f);
    params.prismAmt = std::clamp(glassBase * 0.35f, 0.0f, 1.0f); // tamed rainbow
    params.zoomAmt = (on(FxModule::ZOOM) ? 1.0f : 0.0f)
        * amt(FxModule::ZOOM) * dw(FxModule::ZOOM) * acc(FxModule::ZOOM)
        * std::clamp(m_rgbFlash * 1.2f + bassE * 0.5f, 0.0f, 1.5f)
        * std::max(drive, 0.20f) * P;
    params.shakeAmp = (on(FxModule::ZOOM) ? 1.0f : 0.0f)
        * amt(FxModule::ZOOM) * dw(FxModule::ZOOM) * acc(FxModule::ZOOM)
        * std::clamp(m_rgbFlash + highsE * 0.5f, 0.0f, 1.2f)
        * std::max(drive, 0.20f) * P;
    params.rippleCx = m_rippleCx;
    params.rippleCy = m_rippleCy;
    // blast and jelly weights below.

    // repeats multiply the copies; the longer it holds,
    // the further they drift.
    FxDupParams dup = {};
    {
        const float dupTarget = m_phase.repeatActive
            ? (m_hellMode ? 3.0f : 2.0f) : 0.0f;
        const float kDup = 1.0f - std::exp(-fdt * (dupTarget > m_dupN ? 6.0f : 3.0f));
        m_dupN += (dupTarget - m_dupN) * kDup;
        if (m_phase.repeatActive) {
            m_dupGrow += fdt;
        } else {
            m_dupGrow = std::max(0.0f, m_dupGrow - fdt * 2.0f);
        }
        const float dupGate = (on(FxModule::DUP) ? 1.0f : 0.0f)
            * amt(FxModule::DUP) * dw(FxModule::DUP) * acc(FxModule::DUP);
        const float spread = (0.4f + 0.6f * m_phase.repeatStrength)
            * (1.0f + m_dupGrow * 1.5f);
        const float magPx = (m_hellMode ? 90.0f : 30.0f) * spread * dupGate;
        const float wF = static_cast<float>(m_width);
        const float hF = static_cast<float>(m_height);
        dup.dupN = m_dupN;
        dup.dupAa = 0.50f * dupGate * std::clamp(drive + 0.3f, 0.0f, 1.0f);
        dup.dupAb = 0.35f * dupGate * std::clamp(drive + 0.3f, 0.0f, 1.0f);
        dup.dupAc = 0.30f * dupGate * std::clamp(drive + 0.3f, 0.0f, 1.0f);
        dup.dupAx = magPx / wF;
        dup.dupAy = 0.0f;
        dup.dupBx = -magPx * 0.8f / wF;
        dup.dupBy = magPx * 0.3f / hF;
        dup.dupCx = magPx * 0.5f / wF;
        dup.dupCy = -magPx * 0.4f / hF;
        dup.dupMix = std::clamp(m_dupN / 3.0f, 0.0f, 1.0f);
        m_lastDup = dup.dupN * dupGate;
        D3D11_MAPPED_SUBRESOURCE dm = {};
        if (SUCCEEDED(m_d3dContext->Map(m_dupCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &dm))) {
            memcpy(dm.pData, &dup, sizeof(dup));
            m_d3dContext->Unmap(m_dupCb.Get(), 0);
        }
    }

    // skip the pass when there's nothing to show.
    bool anyFx = rgbOn > 0.0f || params.sliceAmp > 0.003f || params.blockAmp > 0.003f
        || params.waveAmp > 0.003f || params.meltAmp > 0.003f
        || params.rippleAmp * std::exp(-m_rippleT * 2.2f) > 0.003f
        || params.zoomAmt > 0.003f || params.shakeAmp > 0.003f
        || params.glassAmp > 0.003f || params.frostAmt > 0.003f
        || m_lastDup > 0.05f;
    if (intensity >= 0.02f && anyFx) {
        m_lastDrawn = true;
        m_lastSkipReason = "drawing";

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(m_d3dContext->Map(m_screenParamsCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &params, sizeof(params));
        m_d3dContext->Unmap(m_screenParamsCb.Get(), 0);
    }

    // hell uniforms.
    {
        FxHellParams hell = {};
        hell.invertAmt = std::clamp(m_hellInv, 0.0f, 1.0f);
        hell.mirrorAmt = m_hellMode
            ? std::clamp(std::exp(-m_mirrorT * 6.0f), 0.0f, 1.0f) * 0.9f
            : 0.0f;
        hell.hellMaster = m_hellMode ? m_hellIntensity : 0.0f;
        hell.hpad = 0.0f;
        D3D11_MAPPED_SUBRESOURCE hm = {};
        if (SUCCEEDED(m_d3dContext->Map(m_hellCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &hm))) {
            memcpy(hm.pData, &hell, sizeof(hell));
            m_d3dContext->Unmap(m_hellCb.Get(), 0);
        }
    }

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(m_width);
    vp.Height = static_cast<float>(m_height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_d3dContext->RSSetViewports(1, &vp);

    UINT stride = sizeof(ScreenVertex);
    UINT offset = 0;
    m_d3dContext->IASetVertexBuffers(0, 1, m_screenVb.GetAddressOf(), &stride, &offset);
    m_d3dContext->IASetInputLayout(m_screenLayout.Get());
    m_d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_d3dContext->VSSetShader(m_screenVs.Get(), nullptr, 0);
    m_d3dContext->PSSetShader(m_screenPs.Get(), nullptr, 0);
    m_d3dContext->PSSetShaderResources(0, 1, &srv);
    m_d3dContext->PSSetSamplers(0, 1, m_screenSampler.GetAddressOf());
    m_d3dContext->PSSetConstantBuffers(0, 1, m_screenParamsCb.GetAddressOf());
    m_d3dContext->PSSetConstantBuffers(1, 1, m_hellCb.GetAddressOf());
    m_d3dContext->PSSetConstantBuffers(2, 1, m_dupCb.GetAddressOf());

    const float blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_d3dContext->OMSetBlendState(m_blendState.Get(), blendFactor, 0xffffffff);

        m_d3dContext->Draw(6, 0);
        m_capturedFrames++;

        // unbind, or the next copy fails.
        m_d3dContext->PSSetShaderResources(0, 1, nullSrv);
    } else {
        m_lastSkipReason = !anyFx ? "all modules off" : "intensity < 0.02 (quiet)";
    }

    renderGhostWords();
    m_prevPhase = m_phase.phase;
}

bool OverlayWindow::bakeTextAtlas() {
    static_assert(kGhostPhraseCount == 16, "atlas rows must match phrase list");
    m_textRows = kGhostPhraseCount;
    const int W = 1024;
    const int rowH = 160;
    const int H = rowH * m_textRows;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = W;
    bmi.bmiHeader.biHeight = -H; // top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return false;
    HBITMAP hbmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmp || !bits) {
        DeleteDC(hdc);
        return false;
    }
    HGDIOBJ oldBmp = SelectObject(hdc, hbmp);
    RECT full{ 0, 0, W, H };
    FillRect(hdc, &full, (HBRUSH)GetStockObject(BLACK_BRUSH));

    HFONT font = CreateFontW(-96, 0, 0, 0, FW_LIGHT, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
    HGDIOBJ oldFont = nullptr;
    if (font) {
        oldFont = SelectObject(hdc, font);
        SetTextColor(hdc, RGB(255, 255, 255));
        SetBkMode(hdc, TRANSPARENT);
        for (int i = 0; i < m_textRows; ++i) {
            RECT r{ 0, i * rowH, W, (i + 1) * rowH };
            DrawTextW(hdc, kGhostPhrases[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            // measure each line so quads fit the text.
            SIZE sz = {};
            if (GetTextExtentPoint32W(hdc, kGhostPhrases[i],
                    static_cast<int>(wcslen(kGhostPhrases[i])), &sz)) {
                m_phraseW[i] = std::max<LONG>(sz.cx, 8);
            } else {
                m_phraseW[i] = W / 2;
            }
        }
    }

    // BGRA to RGBA shuffle.
    std::vector<uint8_t> rgba(static_cast<size_t>(W) * H * 4);
    const uint8_t* src = static_cast<const uint8_t*>(bits);
    for (int i = 0; i < W * H; ++i) {
        rgba[i * 4 + 0] = src[i * 4 + 2];
        rgba[i * 4 + 1] = src[i * 4 + 1];
        rgba[i * 4 + 2] = src[i * 4 + 0];
        rgba[i * 4 + 3] = 255;
    }

    if (oldFont) SelectObject(hdc, oldFont);
    if (font) DeleteObject(font);
    SelectObject(hdc, oldBmp);
    DeleteObject(hbmp);
    DeleteDC(hdc);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = static_cast<UINT>(W);
    td.Height = static_cast<UINT>(H);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = rgba.data();
    init.SysMemPitch = static_cast<UINT>(W * 4);
    HRESULT hr = m_d3dDevice->CreateTexture2D(&td, &init, m_textAtlas.GetAddressOf());
    if (FAILED(hr)) return false;
    hr = m_d3dDevice->CreateShaderResourceView(m_textAtlas.Get(), nullptr, m_textSrv.GetAddressOf());
    return SUCCEEDED(hr);
}

bool OverlayWindow::spawnWord(int phrase, float xPx, float yPx,
                             float scale, float life, float peak) {
    if (!m_textSrv || m_width <= 0 || m_height <= 0) return false;
    if (phrase < 0 || phrase >= kGhostPhraseCount) return false;
    for (auto& w : m_words) {
        if (!w.alive) {
            std::uniform_real_distribution<float> u01(0.0f, 1.0f);
            w.alive = true;
            w.phrase = phrase;
            w.x = xPx;
            w.y = yPx;
            w.born = static_cast<float>(m_timeSec);
            w.life = life;
            w.seed = u01(m_rng) * 100.0f;
            w.scale = scale;
            w.peak = peak;
            m_lastPhraseIdx = phrase;
            m_totalSpawned++;
            return true;
        }
    }
    return false;
}

void OverlayWindow::spawnTestWord() {
    if (!m_textSrv || m_width <= 0 || m_height <= 0) return;
    // go through phrases in order.
    const int phrase = m_testPhraseCursor % kGhostPhraseCount;
    m_testPhraseCursor++;
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    const float scale = 0.9f + u01(m_rng) * 0.5f;
    const float quadH = 110.0f * scale;
    const float quadW = static_cast<float>(m_phraseW[phrase]) * (110.0f / 160.0f) * scale;
    const float x = u01(m_rng)
        * std::max(1.0f, static_cast<float>(m_width) - quadW);
    const float band = (u01(m_rng) < 0.5f) ? 0.0f : 1.0f;
    const float y = (band < 0.5f)
        ? (0.06f + u01(m_rng) * 0.26f) * static_cast<float>(m_height)
        : (0.68f + u01(m_rng) * 0.20f) * static_cast<float>(m_height);
    if (!spawnWord(phrase, x, y, scale, 4.0f, 0.35f)) {
        // all busy: reuse slot 0.
        m_words[0].alive = false;
        spawnWord(phrase, x, y, scale, 4.0f, 0.35f);
    }
}

void OverlayWindow::triggerBlastStaircase() {
    if (!isModuleEnabled(FxModule::TEXT) || !m_textSrv) return;
    // blast: one phrase as a 3-step staircase, anywhere,
    // big and gone fast.
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    const int phrase = static_cast<int>(m_rng() % kGhostPhraseCount);
    const float scale = 1.4f + u01(m_rng) * 0.4f;
    const float quadH = 110.0f * scale;
    const float quadW = static_cast<float>(std::max(m_phraseW[phrase], 8)) * (110.0f / 160.0f) * scale;
    const float stepX = 40.0f * scale;
    const float stepY = 62.0f * scale;
    const float totalW = quadW + stepX * 2.0f;
    const float totalH = quadH + stepY * 2.0f;
    const float x0 = u01(m_rng)
        * std::max(1.0f, static_cast<float>(m_width) - totalW);
    const float y0 = u01(m_rng)
        * std::max(1.0f, static_cast<float>(m_height) - totalH);
    for (int i = 0; i < 3; ++i) {
        spawnWord(phrase, x0 + stepX * i, y0 + stepY * i, scale, 1.0f, 0.50f);
    }
    m_blast = 1.0f;
    m_lastBlastSec = m_timeSec;
}

void OverlayWindow::updateWords(const AudioAnalysisSnapshot& snapshot) {
    if (!isModuleEnabled(FxModule::TEXT) || !m_textSrv || m_width <= 0 || m_height <= 0) {
        m_aliveWords = 0;
        return;
    }
    // words follow the mids, more in builds and drops.
    // quick in, quick out.
    float spawnBoost = 0.3f;
    if (m_phase.phase == TrackPhase::BUILD) spawnBoost = 1.0f;
    else if (m_phase.phase == TrackPhase::DROP) spawnBoost = 1.5f;
    else if (m_phase.phase == TrackPhase::RELEASE) spawnBoost = 0.7f;

    int alive = 0;
    for (const auto& w : m_words) {
        if (w.alive) alive++;
    }

    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    const float p = snapshot.bandsSmoothed.mids * 0.050f * spawnBoost
        * m_amounts[static_cast<int>(FxModule::TEXT)];
    if (alive < kMaxWords && u01(m_rng) < p) {
        const int phrase = static_cast<int>(m_rng() % kGhostPhraseCount);
        const float scale = 0.7f + u01(m_rng) * 0.9f; // varied sizes
        const float quadH = 110.0f * scale;
        const float quadW = static_cast<float>(std::max(m_phraseW[phrase], 8))
            * (110.0f / 160.0f) * scale;
        const float x = u01(m_rng)
            * std::max(1.0f, static_cast<float>(m_width) - quadW);
        // ambient words stay out of the crosshair.
        float y = 0.0f;
        if (u01(m_rng) < 0.5f) {
            y = (0.06f + u01(m_rng) * 0.20f) * static_cast<float>(m_height);
        } else {
            y = (0.74f + u01(m_rng) * 0.18f) * static_cast<float>(m_height);
        }
        if (spawnWord(phrase, x, y, scale, 2.5f + u01(m_rng) * 1.5f, 0.25f)) {
            alive++;
        }
    }

    m_aliveWords = 0;
    for (auto& w : m_words) {
        if (w.alive) {
            if (static_cast<float>(m_timeSec) - w.born > w.life) {
                w.alive = false;
            } else {
                m_aliveWords++;
            }
        }
    }
}

void OverlayWindow::renderGhostWords() {
    if (!isModuleEnabled(FxModule::TEXT) || !m_textSrv || !m_textPs
        || !m_textVb || !m_textCb || m_aliveWords == 0) {
        return;
    }

    // pack one quad per live word.
    ScreenVertex verts[6 * kMaxWords] = {};
    int quad = 0;
    struct DrawCmd { int phrase; float alpha; float seed; float keep; };
    DrawCmd cmds[kMaxWords] = {};

    const float W = static_cast<float>(m_width);
    const float H = static_cast<float>(m_height);
    for (const auto& w : m_words) {
        if (!w.alive || quad >= kMaxWords) continue;
        const float age = static_cast<float>(m_timeSec) - w.born;
        const float fadeIn = std::clamp(age / 0.4f, 0.0f, 1.0f);
        const float fadeOut = std::clamp((w.life - age) / 1.0f, 0.0f, 1.0f);
        const float env = std::min(fadeIn, fadeOut);
        if (env <= 0.0f) continue;

        // quad fits the text.
        const float glyphH = 160.0f; // atlas row height
        const float quadH = 110.0f * w.scale;
        const float quadW = static_cast<float>(std::max(m_phraseW[w.phrase], 8))
            * (quadH / glyphH);
        const float x0 = std::min(w.x, W - 1.0f);
        const float y0 = std::min(w.y, H - 1.0f);
        const float x1 = std::min(x0 + quadW, W);
        const float y1 = std::min(y0 + quadH, H);
        // Atlas: text is centered in the 1024-wide row — crop to it.
        const float textU0 = (1024.0f - static_cast<float>(std::max(m_phraseW[w.phrase], 8))) * 0.5f / 1024.0f;
        const float textU1 = 1.0f - textU0;
        const float ndcX0 = (x0 / W) * 2.0f - 1.0f;
        const float ndcX1 = (x1 / W) * 2.0f - 1.0f;
        const float ndcTop = 1.0f - (y0 / H) * 2.0f;
        const float ndcBot = 1.0f - (y1 / H) * 2.0f;
        const float v0 = static_cast<float>(w.phrase) / static_cast<float>(m_textRows);
        const float v1 = static_cast<float>(w.phrase + 1) / static_cast<float>(m_textRows);

        ScreenVertex* q = &verts[quad * 6];
        q[0] = { ndcX0, ndcTop, textU0, v0 };
        q[1] = { ndcX1, ndcTop, textU1, v0 };
        q[2] = { ndcX0, ndcBot, textU0, v1 };
        q[3] = { ndcX1, ndcTop, textU1, v0 };
        q[4] = { ndcX1, ndcBot, textU1, v1 };
        q[5] = { ndcX0, ndcBot, textU0, v1 };

        cmds[quad].phrase = w.phrase;
        cmds[quad].alpha = env * w.peak * m_amounts[static_cast<int>(FxModule::TEXT)];
        cmds[quad].seed = w.seed;
        cmds[quad].keep = std::clamp(env * 1.2f, 0.0f, 1.0f);
        quad++;
    }
    if (quad == 0) return;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_d3dContext->Map(m_textVb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    memcpy(mapped.pData, verts, sizeof(ScreenVertex) * 6 * quad);
    m_d3dContext->Unmap(m_textVb.Get(), 0);

    UINT stride = sizeof(ScreenVertex);
    UINT offset = 0;
    m_d3dContext->IASetVertexBuffers(0, 1, m_textVb.GetAddressOf(), &stride, &offset);
    m_d3dContext->IASetInputLayout(m_screenLayout.Get());
    m_d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_d3dContext->VSSetShader(m_screenVs.Get(), nullptr, 0);
    m_d3dContext->PSSetShader(m_textPs.Get(), nullptr, 0);
    m_d3dContext->PSSetSamplers(0, 1, m_screenSampler.GetAddressOf());

    const float blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_d3dContext->OMSetBlendState(m_blendState.Get(), blendFactor, 0xffffffff);

    for (int i = 0; i < quad; ++i) {
        TextParams tp = {};
        tp.alpha = cmds[i].alpha;
        tp.seed = cmds[i].seed;
        tp.keep = cmds[i].keep;
        tp.pad = 0.0f;
        D3D11_MAPPED_SUBRESOURCE cm = {};
        if (SUCCEEDED(m_d3dContext->Map(m_textCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
            memcpy(cm.pData, &tp, sizeof(tp));
            m_d3dContext->Unmap(m_textCb.Get(), 0);
        }
        ID3D11ShaderResourceView* srv = m_textSrv.Get();
        m_d3dContext->PSSetShaderResources(0, 1, &srv);
        m_d3dContext->PSSetConstantBuffers(0, 1, m_textCb.GetAddressOf());
        m_d3dContext->Draw(6, static_cast<UINT>(i * 6));
    }
    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
    m_d3dContext->PSSetShaderResources(0, 1, nullSrv);
}

// old quad pass, gutted. Left as a stub so nothing else has to change.
void OverlayWindow::renderOverlayGraphics(const AudioAnalysisSnapshot& /*snapshot*/) {
    return;
}

void OverlayWindow::shutdown() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    m_capturer.shutdown();
    m_textCb.Reset();
    m_textVb.Reset();
    m_textPs.Reset();
    m_textSrv.Reset();
    m_textAtlas.Reset();
    m_screenParamsCb.Reset();
    m_hellCb.Reset();
    m_dupCb.Reset();
    m_screenSampler.Reset();
    m_screenLayout.Reset();
    m_screenVb.Reset();
    m_screenPs.Reset();
    m_screenVs.Reset();
    m_renderTargetView.Reset();
    m_swapChain.Reset();
    m_blendState.Reset();
    m_vs.Reset();
    m_ps.Reset();
    m_vertexBuffer.Reset();
    m_inputLayout.Reset();
    m_d3dContext.Reset();
    m_d3dDevice.Reset();
    m_statusString = "Shutdown";
}

} // namespace nonstop
