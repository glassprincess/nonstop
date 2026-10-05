#include "audio/fft_analyzer.h"

#include <pffft.h>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <windows.h>

namespace nonstop {

static constexpr float PI = 3.14159265358979323846f;

FftAnalyzer::FftAnalyzer(uint32_t sampleRate)
    : m_sampleRate(sampleRate)
    , m_inputBuffer(FFT_SIZE, 0.0f)
    , m_hannWindow(FFT_SIZE)
    , m_windowedInput(FFT_SIZE)
    , m_fftOutput(FFT_SIZE)
    , m_magnitudes(FFT_SIZE / 2 + 1, 0.0f)
    , m_prevMagnitudes(FFT_SIZE / 2 + 1, 0.0f)
    , m_fluxHistory(32, 0.0f)
{
    m_pffft = pffft_new_setup(static_cast<int>(FFT_SIZE), PFFFT_REAL);

    // Precompute Hann window
    for (size_t i = 0; i < FFT_SIZE; ++i) {
        m_hannWindow[i] = 0.5f * (1.0f - std::cos(2.0f * PI * static_cast<float>(i) / static_cast<float>(FFT_SIZE - 1)));
    }

    setSampleRate(sampleRate);
}

FftAnalyzer::~FftAnalyzer() {
    if (m_pffft) {
        pffft_destroy_setup(m_pffft);
        m_pffft = nullptr;
    }
}

void FftAnalyzer::setSampleRate(uint32_t sampleRate) {
    m_sampleRate = (sampleRate > 0) ? sampleRate : 48000;

    // Precompute log bar frequency ranges (20 Hz - 20000 Hz)
    m_barRanges.resize(AudioAnalysisSnapshot::NUM_SPECTRUM_BARS);
    const float minFreq = 20.0f;
    const float maxFreq = std::min(20000.0f, static_cast<float>(m_sampleRate) * 0.49f);
    const float numBars = static_cast<float>(AudioAnalysisSnapshot::NUM_SPECTRUM_BARS);
    const float binWidth = static_cast<float>(m_sampleRate) / static_cast<float>(FFT_SIZE);

    for (size_t i = 0; i < AudioAnalysisSnapshot::NUM_SPECTRUM_BARS; ++i) {
        float fStart = minFreq * std::pow(maxFreq / minFreq, static_cast<float>(i) / numBars);
        float fEnd = minFreq * std::pow(maxFreq / minFreq, static_cast<float>(i + 1) / numBars);

        size_t bStart = static_cast<size_t>(std::floor(fStart / binWidth));
        size_t bEnd = static_cast<size_t>(std::ceil(fEnd / binWidth));

        if (bStart < 1) bStart = 1;
        if (bEnd <= bStart) bEnd = bStart + 1;
        if (bEnd > FFT_SIZE / 2) bEnd = FFT_SIZE / 2;

        m_barRanges[i] = { bStart, bEnd };
    }
}

void FftAnalyzer::processSamples(const float* samples, size_t count) {
    if (!samples || count == 0) return;

    for (size_t i = 0; i < count; ++i) {
        // Shift input buffer by 1 and append new sample
        // For performance, we can shift or keep a circular index, but with std::copy it's ultra fast for 2048 floats
        m_inputBuffer.erase(m_inputBuffer.begin());
        m_inputBuffer.push_back(samples[i]);
        m_samplesSinceLastFft++;

        if (m_samplesSinceLastFft >= HOP_SIZE) {
            performFft();
            m_samplesSinceLastFft = 0;
        }
    }

    m_totalFrames += count;
    m_snapshot.audioTimeSec = static_cast<double>(m_totalFrames)
        / static_cast<double>(m_sampleRate > 0 ? m_sampleRate : 48000);
}

void FftAnalyzer::performFft() {
    if (!m_pffft) return;

    LARGE_INTEGER startPerf, endPerf, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&startPerf);

    // 1. Compute RMS, peak, and waveform snapshot from input buffer
    float sumSq = 0.0f;
    float peak = 0.0f;
    const size_t waveformStep = FFT_SIZE / AudioAnalysisSnapshot::WAVEFORM_SAMPLES;

    for (size_t i = 0; i < FFT_SIZE; ++i) {
        float val = m_inputBuffer[i];
        sumSq += val * val;
        float absVal = std::abs(val);
        if (absVal > peak) peak = absVal;

        if (i % waveformStep == 0 && (i / waveformStep) < AudioAnalysisSnapshot::WAVEFORM_SAMPLES) {
            m_snapshot.waveform[i / waveformStep] = val;
        }

        // Apply Hann window
        m_windowedInput[i] = val * m_hannWindow[i];
    }

    float rms = std::sqrt(sumSq / static_cast<float>(FFT_SIZE));
    m_snapshot.rms = rms;
    m_snapshot.peak = peak;

    // Smooth envelope tracker (fast attack, smooth release)
    const float attack = 0.4f;
    const float release = 0.08f;
    if (peak > m_snapshot.envelope) {
        m_snapshot.envelope += attack * (peak - m_snapshot.envelope);
    } else {
        m_snapshot.envelope += release * (peak - m_snapshot.envelope);
    }

    // 2. Perform forward FFT
    pffft_transform_ordered(m_pffft, m_windowedInput.data(), m_fftOutput.data(), nullptr, PFFFT_FORWARD);

    // 3. Compute magnitude spectrum
    const float norm = 2.0f / static_cast<float>(FFT_SIZE);
    m_magnitudes[0] = std::abs(m_fftOutput[0]) / static_cast<float>(FFT_SIZE);
    m_magnitudes[FFT_SIZE / 2] = std::abs(m_fftOutput[1]) / static_cast<float>(FFT_SIZE);

    for (size_t k = 1; k < FFT_SIZE / 2; ++k) {
        float r = m_fftOutput[2 * k];
        float im = m_fftOutput[2 * k + 1];
        m_magnitudes[k] = std::sqrt(r * r + im * im) * norm * m_sensitivity;
    }

    // 4. Compute 4 frequency bands
    computeBands(m_magnitudes.data(), FFT_SIZE / 2);

    // 5. Compute logarithmic spectrum bars for UI
    computeLogSpectrum(m_magnitudes.data(), FFT_SIZE / 2);

    // 6. Spectral Flux and Onset detection
    detectOnset(m_magnitudes.data(), FFT_SIZE / 2);

    std::copy(m_magnitudes.begin(), m_magnitudes.end(), m_prevMagnitudes.begin());

    QueryPerformanceCounter(&endPerf);
    m_snapshot.dspProcessingMs = static_cast<float>(endPerf.QuadPart - startPerf.QuadPart) * 1000.0f / static_cast<float>(freq.QuadPart);
    m_snapshot.frameCounter++;
}

void FftAnalyzer::computeBands(const float* magnitudes, size_t numBins) {
    const float binWidth = static_cast<float>(m_sampleRate) / static_cast<float>(FFT_SIZE);

    // Section 3 definitions:
    // Sub-bass: 20 - 60 Hz
    // Bass: 60 - 250 Hz
    // Mids: 250 - 4000 Hz
    // Highs: 4000 - 20000 Hz
    auto getBandEnergy = [&](float fMin, float fMax) -> float {
        size_t bMin = static_cast<size_t>(fMin / binWidth);
        size_t bMax = static_cast<size_t>(fMax / binWidth);
        if (bMin < 1) bMin = 1;
        if (bMax > numBins) bMax = numBins;
        if (bMax <= bMin) return magnitudes[bMin];

        float sum = 0.0f;
        for (size_t b = bMin; b < bMax; ++b) {
            sum += magnitudes[b] * magnitudes[b];
        }
        return std::sqrt(sum / static_cast<float>(bMax - bMin)) * 4.0f;
    };

    m_snapshot.bandsRaw.subBass = std::clamp(getBandEnergy(20.0f, 60.0f), 0.0f, 1.0f);
    m_snapshot.bandsRaw.bass    = std::clamp(getBandEnergy(60.0f, 250.0f), 0.0f, 1.0f);
    m_snapshot.bandsRaw.mids    = std::clamp(getBandEnergy(250.0f, 4000.0f), 0.0f, 1.0f);
    m_snapshot.bandsRaw.highs   = std::clamp(getBandEnergy(4000.0f, 20000.0f), 0.0f, 1.0f);

    // Smooth bands
    auto smoothVal = [](float current, float target, float attack, float decay) {
        if (target > current) {
            return current + attack * (target - current);
        } else {
            return current + decay * (target - current);
        }
    };

    m_snapshot.bandsSmoothed.subBass = smoothVal(m_snapshot.bandsSmoothed.subBass, m_snapshot.bandsRaw.subBass, 0.6f, 0.15f);
    m_snapshot.bandsSmoothed.bass    = smoothVal(m_snapshot.bandsSmoothed.bass,    m_snapshot.bandsRaw.bass,    0.6f, 0.15f);
    m_snapshot.bandsSmoothed.mids    = smoothVal(m_snapshot.bandsSmoothed.mids,    m_snapshot.bandsRaw.mids,    0.5f, 0.18f);
    m_snapshot.bandsSmoothed.highs   = smoothVal(m_snapshot.bandsSmoothed.highs,   m_snapshot.bandsRaw.highs,   0.5f, 0.20f);
}

void FftAnalyzer::computeLogSpectrum(const float* magnitudes, size_t numBins) {
    for (size_t i = 0; i < AudioAnalysisSnapshot::NUM_SPECTRUM_BARS; ++i) {
        const auto& range = m_barRanges[i];
        float maxVal = 0.0f;
        for (size_t b = range.startBin; b < range.endBin && b < numBins; ++b) {
            if (magnitudes[b] > maxVal) {
                maxVal = magnitudes[b];
            }
        }

        // Convert to dB scale with floor around -60dB
        float valNorm = 0.0f;
        if (maxVal > 0.0001f) {
            float db = 20.0f * std::log10(maxVal);
            valNorm = (db + 60.0f) / 60.0f;
            if (valNorm < 0.0f) valNorm = 0.0f;
            if (valNorm > 1.0f) valNorm = 1.0f;
        }

        // Fast attack, smooth decay
        if (valNorm > m_snapshot.spectrumBars[i]) {
            m_snapshot.spectrumBars[i] = valNorm;
        } else {
            m_snapshot.spectrumBars[i] = m_snapshot.spectrumBars[i] * 0.88f;
        }

        // Peak hold decay
        if (valNorm > m_snapshot.spectrumPeaks[i]) {
            m_snapshot.spectrumPeaks[i] = valNorm;
        } else {
            m_snapshot.spectrumPeaks[i] = std::max(0.0f, m_snapshot.spectrumPeaks[i] - 0.015f);
        }
    }
}

void FftAnalyzer::detectOnset(const float* magnitudes, size_t numBins) {
    // Spectral Flux = sum of positive energy differences
    float flux = 0.0f;
    for (size_t k = 1; k < numBins; ++k) {
        float diff = magnitudes[k] - m_prevMagnitudes[k];
        if (diff > 0.0f) {
            flux += diff;
        }
    }

    m_snapshot.spectralFlux = flux;

    // Moving average of flux for dynamic thresholding
    m_fluxHistory[m_fluxHistoryIndex] = flux;
    m_fluxHistoryIndex = (m_fluxHistoryIndex + 1) % m_fluxHistory.size();

    float avgFlux = std::accumulate(m_fluxHistory.begin(), m_fluxHistory.end(), 0.0f) / static_cast<float>(m_fluxHistory.size());
    float threshold = avgFlux * m_onsetMultiplier + 0.02f;
    m_snapshot.fluxThreshold = threshold;

    bool hit = (flux > threshold) && (m_snapshot.peak > 0.05f);
    // Refractory: suppress chatter from consecutive FFT hops of one hit.
    // 4 hops ~= 43ms at 48kHz — still allows real 20Hz rolls through.
    if (m_onsetCooldown > 0) {
        m_onsetCooldown--;
        hit = false;
    } else if (hit) {
        m_onsetCooldown = 4;
    }
    m_snapshot.isOnset = hit;
}

AudioAnalysisSnapshot FftAnalyzer::getSnapshot() const {
    return m_snapshot;
}

} // namespace nonstop
