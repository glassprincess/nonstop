#include "audio/wasapi_capture.h"

#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <mmreg.h>
#include <cmath>
#include <iostream>

namespace nonstop {

WasapiCapture::WasapiCapture() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(m_deviceEnumerator.GetAddressOf())
    );

    if (FAILED(hr)) {
        m_statusMessage = "Failed to create MMDeviceEnumerator";
    }
}

WasapiCapture::~WasapiCapture() {
    stop();
    cleanupAudioClient();
    CoUninitialize();
}

std::vector<AudioDeviceInfo> WasapiCapture::enumerateDevices() {
    std::vector<AudioDeviceInfo> devices;
    if (!m_deviceEnumerator) return devices;

    ComPtr<IMMDevice> defaultDevice;
    std::wstring defaultId;
    if (SUCCEEDED(m_deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice))) {
        LPWSTR pId = nullptr;
        if (SUCCEEDED(defaultDevice->GetId(&pId))) {
            defaultId = pId;
            CoTaskMemFree(pId);
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    HRESULT hr = m_deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) return devices;

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;

        LPWSTR pId = nullptr;
        if (FAILED(device->GetId(&pId))) continue;
        std::wstring id = pId;
        CoTaskMemFree(pId);

        ComPtr<IPropertyStore> props;
        std::wstring friendlyName = L"Audio Device";
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName))) {
                if (varName.pwszVal) {
                    friendlyName = varName.pwszVal;
                }
                PropVariantClear(&varName);
            }
        }

        AudioDeviceInfo info;
        info.id = id;
        info.name = friendlyName;
        info.isDefault = (id == defaultId);
        devices.push_back(info);
    }

    return devices;
}

bool WasapiCapture::initializeAudioClient(IMMDevice* pDevice) {
    cleanupAudioClient();

    if (!pDevice) return false;

    HRESULT hr = pDevice->Activate(
        __uuidof(IAudioClient),
        CLSCTX_ALL,
        nullptr,
        reinterpret_cast<void**>(m_audioClient.GetAddressOf())
    );
    if (FAILED(hr)) {
        m_statusMessage = "Failed to activate IAudioClient";
        return false;
    }

    hr = m_audioClient->GetMixFormat(&m_mixFormat);
    if (FAILED(hr) || !m_mixFormat) {
        m_statusMessage = "Failed to get MixFormat";
        return false;
    }

    m_formatInfo.sampleRate = m_mixFormat->nSamplesPerSec;
    m_formatInfo.channels = m_mixFormat->nChannels;
    m_formatInfo.bitsPerSample = m_mixFormat->wBitsPerSample;

    bool isFloat = false;
    if (m_mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        isFloat = true;
    } else if (m_mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* pExt = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_mixFormat);
        if (IsEqualGUID(pExt->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            isFloat = true;
        }
    }
    m_formatInfo.isFloat = isFloat;

    // Request 20ms buffer (200,000 in 100ns units) for ultra low latency loopback
    REFERENCE_TIME hnsRequestedDuration = 200000;
    hr = m_audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        hnsRequestedDuration,
        0,
        m_mixFormat,
        nullptr
    );

    if (FAILED(hr)) {
        m_statusMessage = "Failed to initialize IAudioClient with LOOPBACK flags";
        return false;
    }

    UINT32 bufferSize = 0;
    if (SUCCEEDED(m_audioClient->GetBufferSize(&bufferSize))) {
        m_formatInfo.bufferFrameCount = bufferSize;
        m_formatInfo.bufferLatencyMs = (float)bufferSize * 1000.0f / (float)m_formatInfo.sampleRate;
    }

    REFERENCE_TIME streamLatency = 0;
    if (SUCCEEDED(m_audioClient->GetStreamLatency(&streamLatency))) {
        float latencyFromStream = (float)streamLatency / 10000.0f; // 100ns to ms
        if (latencyFromStream > 0.0f) {
            m_formatInfo.bufferLatencyMs = latencyFromStream;
        }
    }

    hr = m_audioClient->GetService(
        __uuidof(IAudioCaptureClient),
        reinterpret_cast<void**>(m_captureClient.GetAddressOf())
    );
    if (FAILED(hr)) {
        m_statusMessage = "Failed to get IAudioCaptureClient";
        return false;
    }

    return true;
}

void WasapiCapture::cleanupAudioClient() {
    if (m_captureClient) {
        m_captureClient.Reset();
    }
    if (m_audioClient) {
        m_audioClient->Stop();
        m_audioClient.Reset();
    }
    if (m_mixFormat) {
        CoTaskMemFree(m_mixFormat);
        m_mixFormat = nullptr;
    }
}

bool WasapiCapture::start(const std::wstring& deviceId) {
    stop();

    if (!m_deviceEnumerator) {
        m_statusMessage = "No device enumerator available";
        return false;
    }

    ComPtr<IMMDevice> targetDevice;
    HRESULT hr = S_OK;

    if (deviceId.empty()) {
        hr = m_deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &targetDevice);
    } else {
        hr = m_deviceEnumerator->GetDevice(deviceId.c_str(), &targetDevice);
    }

    if (FAILED(hr) || !targetDevice) {
        m_statusMessage = "Failed to get audio endpoint device";
        return false;
    }

    LPWSTR pId = nullptr;
    if (SUCCEEDED(targetDevice->GetId(&pId))) {
        m_currentDeviceId = pId;
        CoTaskMemFree(pId);
    }

    ComPtr<IPropertyStore> props;
    m_currentDeviceName = L"Default Audio Device";
    if (SUCCEEDED(targetDevice->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName))) {
            if (varName.pwszVal) {
                m_currentDeviceName = varName.pwszVal;
            }
            PropVariantClear(&varName);
        }
    }

    if (!initializeAudioClient(targetDevice.Get())) {
        return false;
    }

    hr = m_audioClient->Start();
    if (FAILED(hr)) {
        m_statusMessage = "Failed to start IAudioClient";
        return false;
    }

    m_ringBuffer.reset();
    m_running.store(true);
    m_statusMessage = "Running";

    m_captureThread = std::make_unique<std::thread>(&WasapiCapture::captureLoop, this);
    return true;
}

void WasapiCapture::stop() {
    if (m_running.load()) {
        m_running.store(false);
        if (m_captureThread && m_captureThread->joinable()) {
            m_captureThread->join();
        }
        m_captureThread.reset();
    }
    cleanupAudioClient();
    m_statusMessage = "Stopped";
}

size_t WasapiCapture::readSamples(float* outBuffer, size_t count) {
    return m_ringBuffer.read(outBuffer, count);
}

void WasapiCapture::captureLoop() {
    // Elevate thread priority for pro audio streaming
    DWORD taskIndex = 0;
    HANDLE hAvrt = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    const uint32_t channels = m_formatInfo.channels;
    const bool isFloat = m_formatInfo.isFloat;
    const uint32_t bits = m_formatInfo.bitsPerSample;

    while (m_running.load()) {
        UINT32 packetLength = 0;
        HRESULT hr = m_captureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            // Audio device may have changed or been disconnected
            Sleep(10);
            continue;
        }

        if (packetLength == 0) {
            // No audio currently playing or buffer empty
            Sleep(4);
            continue;
        }

        while (packetLength > 0 && m_running.load()) {
            BYTE* pData = nullptr;
            UINT32 numFramesRead = 0;
            DWORD flags = 0;

            hr = m_captureClient->GetBuffer(&pData, &numFramesRead, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            if (numFramesRead > 0) {
                if (m_monoConversionBuffer.size() < numFramesRead) {
                    m_monoConversionBuffer.resize(numFramesRead);
                }

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    // Endpoint produced explicit silence
                    std::fill_n(m_monoConversionBuffer.data(), numFramesRead, 0.0f);
                } else if (pData) {
                    if (isFloat && bits == 32) {
                        const float* floatSamples = reinterpret_cast<const float*>(pData);
                        const float invChannels = 1.0f / (float)channels;

                        for (UINT32 f = 0; f < numFramesRead; ++f) {
                            float sum = 0.0f;
                            const size_t frameOffset = f * channels;
                            for (uint32_t c = 0; c < channels; ++c) {
                                sum += floatSamples[frameOffset + c];
                            }
                            m_monoConversionBuffer[f] = sum * invChannels;
                        }
                    } else if (!isFloat && bits == 16) {
                        const int16_t* intSamples = reinterpret_cast<const int16_t*>(pData);
                        const float invScale = 1.0f / (32768.0f * (float)channels);

                        for (UINT32 f = 0; f < numFramesRead; ++f) {
                            float sum = 0.0f;
                            const size_t frameOffset = f * channels;
                            for (uint32_t c = 0; c < channels; ++c) {
                                sum += (float)intSamples[frameOffset + c];
                            }
                            m_monoConversionBuffer[f] = sum * invScale;
                        }
                    } else {
                        // Fallback for unknown bit depth: fill with silence
                        std::fill_n(m_monoConversionBuffer.data(), numFramesRead, 0.0f);
                    }
                }

                m_ringBuffer.write(m_monoConversionBuffer.data(), numFramesRead);
            }

            m_captureClient->ReleaseBuffer(numFramesRead);
            hr = m_captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;
        }
    }

    if (hAvrt) {
        AvRevertMmThreadCharacteristics(hAvrt);
    }
}

} // namespace nonstop
