#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace nonstop {

// which hertz go where
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

// everything the analyzer figured out, packed for the UI and the FX
struct AudioAnalysisSnapshot {
    // loudness
    float rms = 0.0f;
    float peak = 0.0f;
    float envelope = 0.0f;

    // band energy, raw and smoothed
    FrequencyBands bandsRaw;
    FrequencyBands bandsSmoothed;

    // log bars for the visualizer
    static constexpr size_t NUM_SPECTRUM_BARS = 64;
    float spectrumBars[NUM_SPECTRUM_BARS] = {0.0f};
    float spectrumPeaks[NUM_SPECTRUM_BARS] = {0.0f};

    // did something just hit?
    float spectralFlux = 0.0f;
    float fluxThreshold = 0.0f;
    bool isOnset = false;

    // a bit of waveform to draw
    static constexpr size_t WAVEFORM_SAMPLES = 256;
    float waveform[WAVEFORM_SAMPLES] = {0.0f};

    // timing trivia
    float captureLatencyMs = 0.0f;
    float dspProcessingMs = 0.0f;
    uint64_t frameCounter = 0;
    // audio clock in seconds. The detector stamps hits with this:
    // wall clock lies when frames gulp audio in bursts (gaps shrink
    // to ~ms and the repeat math falls apart).
    double audioTimeSec = 0.0;
};

} // namespace nonstop
