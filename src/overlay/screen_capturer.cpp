#include "overlay/screen_capturer.h"
#include <sstream>
#include <iomanip>

namespace nonstop {

ScreenCapturer::ScreenCapturer() = default;

ScreenCapturer::~ScreenCapturer() {
    shutdown();
}

bool ScreenCapturer::init(ID3D11Device* device) {
    shutdown();
    if (!device) {
        m_statusString = "init failed: null D3D device";
        return false;
    }
    m_device = device;
    if (!createDuplication()) {
        return false;
    }
    m_active = true;
    return true;
}

void ScreenCapturer::shutdown() {
    m_duplication.Reset();
    m_captureSrv.Reset();
    m_captureTex.Reset();
    m_device.Reset();
    m_width = 0;
    m_height = 0;
    m_active = false;
}

bool ScreenCapturer::createDuplication() {
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = m_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) {
        m_statusString = "No IDXGIDevice on overlay device";
        return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr) || !adapter) {
        m_statusString = "GetAdapter failed";
        return false;
    }

    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(0, &output); // primary output
    if (FAILED(hr) || !output) {
        std::stringstream ss;
        ss << "EnumOutputs(0) failed (0x" << std::hex << hr << ")";
        m_statusString = ss.str();
        return false;
    }

    ComPtr<IDXGIOutput1> output1;
    hr = output->QueryInterface(IID_PPV_ARGS(&output1));
    if (FAILED(hr) || !output1) {
        std::stringstream ss;
        ss << "IDXGIOutput1 unavailable (0x" << std::hex << hr << ") — OS/Driver too old?";
        m_statusString = ss.str();
        return false;
    }

    hr = output1->DuplicateOutput(m_device.Get(), &m_duplication);
    if (FAILED(hr) || !m_duplication) {
        std::stringstream ss;
        ss << "DuplicateOutput failed (0x" << std::hex << hr << ")";
        if (hr == E_ACCESSDENIED) {
            ss << " — protected content or secure desktop";
        } else if (hr == DXGI_ERROR_UNSUPPORTED) {
            ss << " — adapter does not support duplication (remote session?)";
        }
        m_statusString = ss.str();
        m_errors++;
        return false;
    }

    DXGI_OUTDUPL_DESC desc = {};
    m_duplication->GetDesc(&desc);
    m_width = desc.ModeDesc.Width;
    m_height = desc.ModeDesc.Height;

    std::stringstream ss;
    ss << "Active " << m_width << "x" << m_height
       << (desc.DesktopImageInSystemMemory ? " (sysmem)" : " (gpu)");
    m_statusString = ss.str();
    return true;
}

bool ScreenCapturer::ensureCaptureTexture(ID3D11Texture2D* src) {
    if (!src) return false;
    D3D11_TEXTURE2D_DESC srcDesc = {};
    src->GetDesc(&srcDesc);

    if (m_captureTex) {
        D3D11_TEXTURE2D_DESC cur = {};
        m_captureTex->GetDesc(&cur);
        if (cur.Width == srcDesc.Width && cur.Height == srcDesc.Height &&
            cur.Format == srcDesc.Format) {
            return true;
        }
        m_captureSrv.Reset();
        m_captureTex.Reset();
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = srcDesc.Width;
    texDesc.Height = srcDesc.Height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = srcDesc.Format;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;

    HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_captureTex);
    if (FAILED(hr) || !m_captureTex) {
        std::stringstream ss;
        ss << "CreateTexture2D for capture failed (0x" << std::hex << hr << ")";
        m_statusString = ss.str();
        return false;
    }

    hr = m_device->CreateShaderResourceView(m_captureTex.Get(), nullptr, &m_captureSrv);
    if (FAILED(hr) || !m_captureSrv) {
        m_captureTex.Reset();
        m_statusString = "CreateShaderResourceView for capture failed";
        return false;
    }

    m_width = static_cast<int>(texDesc.Width);
    m_height = static_cast<int>(texDesc.Height);
    return true;
}

double ScreenCapturer::nowSeconds() {
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart);
}

double ScreenCapturer::secondsSinceFresh() const {
    return nowSeconds() - m_lastFreshSec;
}

ID3D11ShaderResourceView* ScreenCapturer::acquireFrame(ID3D11DeviceContext* context) {
    if (!m_active || !m_duplication || !context) {
        return m_captureSrv.Get();
    }

    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
    ComPtr<IDXGIResource> desktopRes;
    HRESULT hr = m_duplication->AcquireNextFrame(0, &frameInfo, &desktopRes);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        m_timeouts++;
        return m_captureSrv.Get(); // reuse last good frame (freshness checked by caller)
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        // screen mode changed (or that admin prompt screen): the grabber
        // is dead, and whatever frame we kept is stale on the spot.
        m_errors++;
        m_lastFreshSec = -1e9; // invalidate
        m_duplication.Reset();
        if (createDuplication()) {
            m_statusString += " [recovered after ACCESS_LOST]";
        } else {
            m_statusString += " [ACCESS_LOST, reinit failed]";
        }
        return m_captureSrv.Get();
    }
    if (FAILED(hr) || !desktopRes) {
        m_errors++;
        return m_captureSrv.Get();
    }

    // Freshness: only frames with new pixels reset the clock. Empty polls
    // keep the old timestamp, so a frozen screen ages out instead of
    // sticking around forever.
    const long long present = frameInfo.LastPresentTime.QuadPart;
    const bool hasNewPixels = (frameInfo.AccumulatedFrames > 0)
        || (present != 0 && present != m_prevPresent);
    if (hasNewPixels) {
        m_prevPresent = present;
        m_lastFreshSec = nowSeconds();
    }

    ComPtr<ID3D11Texture2D> srcTex;
    hr = desktopRes->QueryInterface(IID_PPV_ARGS(&srcTex));
    if (SUCCEEDED(hr) && srcTex && ensureCaptureTexture(srcTex.Get())) {
        context->CopyResource(m_captureTex.Get(), srcTex.Get());
        m_capturedFrames++;
    } else {
        m_errors++;
    }

    m_duplication->ReleaseFrame();
    return m_captureSrv.Get();
}

} // namespace nonstop
