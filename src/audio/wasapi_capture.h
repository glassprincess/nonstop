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

    // list what Windows can play to right now
    std::vector<AudioDeviceInfo> enumerateDevices();

    // start listening to a device (empty = whatever's default)
    bool start(const std::wstring& deviceId = L"");

    // stop it
    void stop();

    // still running?
    bool isRunning() const { return m_running.load(); }

    // pull mono samples out of the ring
    size_t readSamples(float* outBuffer, size_t count);

    // what's the format, which device
    AudioFormatInfo getFormatInfo() const { return m_formatInfo; }
    std::wstring getCurrentDeviceId() const { return m_currentDeviceId; }
    std::wstring getCurrentDeviceName() const { return m_currentDeviceName; }

    // last error in plain words
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

    // stereo-to-mono scratch
    std::vector<float> m_monoConversionBuffer;
};

} // namespace nonstop
