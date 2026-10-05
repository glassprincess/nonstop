#pragma once

#include "core/types.h"
#include <vector>
#include <memory>
#include <chrono>

struct PFFFT_Setup;

namespace nonstop {

class FftAnalyzer {
public:
    static constexpr size_t FFT_SIZE = 2048;
    static constexpr size_t HOP_SIZE = 512;

    explicit FftAnalyzer(uint32_t sampleRate = 48000);
    ~FftAnalyzer();

    void setSampleRate(uint32_t sampleRate);
    uint32_t getSampleRate() const { return m_sampleRate; }

    // Feed new mono samples into analyzer
    void processSamples(const float* samples, size_t count);

    // Get latest computed snapshot
    AudioAnalysisSnapshot getSnapshot() const;

    // Parameter tuning
    void setSensitivity(float s) { m_sensitivity = s; }
    float getSensitivity() const { return m_sensitivity; }

    void setOnsetThreshold(float t) { m_onsetMultiplier = t; }
    float getOnsetThreshold() const { return m_onsetMultiplier; }

private:
    void performFft();
    void computeBands(const float* magnitudes, size_t numBins);
    void computeLogSpectrum(const float* magnitudes, size_t numBins);
    void detectOnset(const float* magnitudes, size_t numBins);

    uint32_t m_sampleRate = 48000;
    float m_sensitivity = 1.0f;
    float m_onsetMultiplier = 1.5f;

    PFFFT_Setup* m_pffft = nullptr;

    // Sliding window buffer
    std::vector<float> m_inputBuffer;
    size_t m_samplesSinceLastFft = 0;

    // Window and work arrays
    std::vector<float> m_hannWindow;
    std::vector<float> m_windowedInput;
    std::vector<float> m_fftOutput;
    std::vector<float> m_magnitudes;
    std::vector<float> m_prevMagnitudes;

    // Spectral flux history for adaptive threshold
    std::vector<float> m_fluxHistory;
    size_t m_fluxHistoryIndex = 0;

    // Refractory period: one transient must not fire several onsets in a
    // row (FFT hops are ~10ms apart). Counted in performFft() ticks.
    int m_onsetCooldown = 0;

    // Monotonic consumed-sample counter -> snapshot.audioTimeSec.
    uint64_t m_totalFrames = 0;

    // Result snapshot
    AudioAnalysisSnapshot m_snapshot;

    // Log bar frequency bin mappings
    struct BarRange {
        size_t startBin;
        size_t endBin;
    };
    std::vector<BarRange> m_barRanges;
};

} // namespace nonstop
