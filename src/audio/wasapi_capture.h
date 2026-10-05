#pragma once

#include "core/types.h"
#include "audio/ring_buffer.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>

namespace nonstop {

using Microsoft::WRL::ComPtr;

class WasapiCapture {
public:
    WasapiCapture();
    ~WasapiCapture();

    // Enumerate active audio render endpoints
    std::vector<AudioDeviceInfo> enumerateDevices();

    // Start capturing from specified device (empty string = default system audio)
    bool start(const std::wstring& deviceId = L"");

    // Stop capturing
    void stop();

    // Check if capture is running
    bool isRunning() const { return m_running.load(); }

    // Read available mono float samples from ring buffer
    size_t readSamples(float* outBuffer, size_t count);

    // Get current device & format information
    AudioFormatInfo getFormatInfo() const { return m_formatInfo; }
    std::wstring getCurrentDeviceId() const { return m_currentDeviceId; }
    std::wstring getCurrentDeviceName() const { return m_currentDeviceName; }

    // Error or status message
    std::string getStatusMessage() const { return m_statusMessage; }

private:
    void captureLoop();
    bool initializeAudioClient(IMMDevice* pDevice);
    void cleanupAudioClient();

    std::wstring m_currentDeviceId;
    std::wstring m_currentDeviceName;
    AudioFormatInfo m_formatInfo;
    std::string m_statusMessage;

    ComPtr<IMMDeviceEnumerator> m_deviceEnumerator;
    ComPtr<IAudioClient> m_audioClient;
    ComPtr<IAudioCaptureClient> m_captureClient;
    WAVEFORMATEX* m_mixFormat = nullptr;

    std::atomic<bool> m_running{false};
    std::unique_ptr<std::thread> m_captureThread;
    RingBuffer<float> m_ringBuffer{32768};

    // Intermediate mono conversion buffer
    std::vector<float> m_monoConversionBuffer;
};

} // namespace nonstop
