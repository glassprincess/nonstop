#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace nonstop {

// Core frequency bands defined by section 3 of specification
struct FrequencyBands {
    float subBass = 0.0f; // 20 - 60 Hz
    float bass = 0.0f;    // 60 - 250 Hz
    float mids = 0.0f;    // 250 - 4000 Hz
    float highs = 0.0f;   // 4000 - 20000 Hz
};

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

struct AudioFormatInfo {
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint32_t bitsPerSample = 0;
    uint32_t bufferFrameCount = 0;
    float bufferLatencyMs = 0.0f;
    bool isFloat = true;
};

// Live audio analysis snapshot for UI and visual effects
struct AudioAnalysisSnapshot {
    // Overall volume
    float rms = 0.0f;
    float peak = 0.0f;
    float envelope = 0.0f;

    // Energy bands (raw & smoothed [0.0 - 1.0])
    FrequencyBands bandsRaw;
    FrequencyBands bandsSmoothed;

    // Visualizer logarithmic spectrum bars (e.g. 64 bars)
    static constexpr size_t NUM_SPECTRUM_BARS = 64;
    float spectrumBars[NUM_SPECTRUM_BARS] = {0.0f};
    float spectrumPeaks[NUM_SPECTRUM_BARS] = {0.0f};

    // Onsets / beat attacks
    float spectralFlux = 0.0f;
    float fluxThreshold = 0.0f;
    bool isOnset = false;

    // Waveform mini-buffer for oscilloscope (e.g. 256 samples)
    static constexpr size_t WAVEFORM_SAMPLES = 256;
    float waveform[WAVEFORM_SAMPLES] = {0.0f};

    // Latency & performance diagnostics
    float captureLatencyMs = 0.0f;
    float dspProcessingMs = 0.0f;
    uint64_t frameCounter = 0;
    // Monotonic audio clock (seconds of consumed samples). Detector uses
    // this for onset timestamps: wall-clock lies when frames drain audio
    // in catch-up bursts (intervals collapse to ~ms and kill repeat math).
    double audioTimeSec = 0.0;
};

} // namespace nonstop
