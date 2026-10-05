#include "audio/phase_detector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nonstop {

PhaseDetector::PhaseDetector() = default;

void PhaseDetector::reset() {
    m_state = PhaseState{};
    m_history.clear();
    m_onsets.clear();
    m_lastHistPush = -1.0;
    m_entryBass = 0.0f;
    m_dropPeak = 0.0f;
    m_log.clear();
    m_clockInit = false;
}

double PhaseDetector::nowSeconds() {
    auto now = std::chrono::steady_clock::now();
    if (!m_clockInit) {
        m_clockBase = now;
        m_clockInit = true;
        return 0.0;
    }
    return std::chrono::duration<double>(now - m_clockBase).count();
}

float PhaseDetector::bassAt(double tAgo) const {
    if (m_history.empty()) return 0.0f;
    const double target = m_history.back().t - tAgo;
    float v = m_history.front().bass;
    for (const auto& h : m_history) {
        if (h.t <= target) v = h.bass;
        else break;
    }
    return v;
}

float PhaseDetector::avgHighs(double window) const {
    if (m_history.empty()) return 0.0f;
    const double since = m_history.back().t - window;
    float sum = 0.0f;
    int n = 0;
    for (const auto& h : m_history) {
        if (h.t >= since) {
            sum += h.highs;
            n++;
        }
    }
    return (n > 0) ? sum / static_cast<float>(n) : 0.0f;
}

void PhaseDetector::enterPhase(TrackPhase next, const char* reason) {
    if (next == m_state.phase) return;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "t=%.1f %s->%s (%s)",
        m_lastUpdateT, toString(m_state.phase), toString(next), reason);
    m_log.emplace_back(buf);
    if (m_log.size() > 12) m_log.erase(m_log.begin());

    m_state.phase = next;
    m_state.timeInPhase = 0.0f;
    m_state.buildProgress = 0.0f;
    if (next == TrackPhase::DROP) {
        m_state.dropPower = std::clamp(m_state.dropPower, 0.0f, 1.0f);
    } else if (next != TrackPhase::RELEASE) {
        m_state.dropPower = 0.0f;
    }
}

void PhaseDetector::update(const AudioAnalysisSnapshot& snapshot) {
    // Audio clock, NOT wall clock: frames drain audio in catch-up bursts,
    // so wall-stamped onset intervals collapse to ~ms and repeat math dies.
    // (Silence still advances: loopback always streams while capturing.)
    const double t = snapshot.audioTimeSec;
    float dt = (m_lastUpdateT > 0.0) ? static_cast<float>(t - m_lastUpdateT) : (1.0f / 60.0f);
    dt = std::clamp(dt, 1.0f / 240.0f, 0.25f);
    m_lastUpdateT = t;
    m_state.timeInPhase += dt;

    const float bass = snapshot.bandsSmoothed.subBass * 0.6f
                     + snapshot.bandsSmoothed.bass * 0.4f;
    const float highs = snapshot.bandsSmoothed.highs;

    // ~20Hz history sampling, 8s deep.
    if (m_lastHistPush < 0.0 || (t - m_lastHistPush) >= 0.05) {
        m_history.push_back({ t, bass, highs });
        m_lastHistPush = t;
        while (!m_history.empty() && (t - m_history.front().t) > 8.0) {
            m_history.pop_front();
        }
    }

    if (snapshot.isOnset) {
        m_onsets.push_back(t);
    }
    while (!m_onsets.empty() && (t - m_onsets.front()) > 2.0) {
        m_onsets.pop_front();
    }
    m_state.onsetDensity = static_cast<float>(m_onsets.size()) / 2.0f;

    // --- Repeat tracker (HELL MODE invert sync) ---
    // Last 6 onsets -> 5 intervals; regular fast hits = repeat.
    // NOTE: needs size>=6 (size-6 index); exactly-5 previously underflowed.
    {
        if (m_onsets.size() >= 6) {
            const size_t n = m_onsets.size();
            double iv[5] = {};
            for (size_t i = 0; i < 5; ++i) {
                iv[i] = m_onsets[n - 5 + i] - m_onsets[n - 6 + i];
            }
            double mean = 0.0;
            for (int i = 0; i < 5; ++i) mean += iv[i];
            mean /= 5.0;
            // Chatter guard: sub-30ms intervals are FFT residue, never music.
            // (Must sit BELOW the analyzer refractory floor of ~43ms, or
            // dense passages veto themselves forever — the silent-repeat bug.)
            bool chatter = false;
            for (int i = 0; i < 5; ++i) {
                if (iv[i] < 0.030) {
                    chatter = true;
                    break;
                }
            }
            double var = 0.0;
            for (int i = 0; i < 5; ++i) var += (iv[i] - mean) * (iv[i] - mean);
            const double cv = (mean > 1e-6) ? std::sqrt(var / 5.0) / mean : 1e9;
            // Humanized rolls live at CV 0.3-0.5; map generously so real
            // repeats (not just metronomes) arm the detector.
            float strength = std::clamp((1.0f - static_cast<float>(cv)) * 1.4f, 0.0f, 1.0f);
            if (chatter) strength = 0.0f;
            const float hz = (mean > 1e-6 && !chatter) ? static_cast<float>(1.0 / mean) : 0.0f;
            m_state.repeatCandHz = hz;
            m_state.repeatCandStr = strength;
            const bool inBand = hz >= m_params.repMinHz && hz <= m_params.repMaxHz;
            const float onTh = m_params.repOnThresh * m_params.repSens;
            const float offTh = m_params.repOffThresh * m_params.repSens;
            if (!m_state.repeatActive && inBand && strength > onTh) {
                m_state.repeatActive = true;
            } else if (m_state.repeatActive && (!inBand || strength < offTh)) {
                m_state.repeatActive = false;
            }
            if (m_state.repeatActive) {
                m_state.repeatHz = hz;
                m_state.repeatStrength = strength;
            } else {
                m_state.repeatHz = 0.0f;
                m_state.repeatStrength = 0.0f;
            }
        } else if (m_state.repeatActive) {
            // Starved of onsets: release quickly (freshness over memory).
            if (m_onsets.empty() || (t - m_onsets.back()) > 0.5) {
                m_state.repeatActive = false;
                m_state.repeatHz = 0.0f;
                m_state.repeatStrength = 0.0f;
            }
        }
    }

    const float slopeWin = m_params.buildSlopeWindow;
    m_state.bassSlope = (bass - bassAt(slopeWin)) / slopeWin;

    const bool dwelled = m_state.timeInPhase >= m_params.minDwell;

    switch (m_state.phase) {
    case TrackPhase::CALM: {
        if (dwelled && m_history.size() > 10) {
            const bool rising = m_state.bassSlope > m_params.buildSlopeThresh;
            const bool dense = m_state.onsetDensity > m_params.onsetDensityThresh;
            const bool lifted = highs > avgHighs(slopeWin) + m_params.highsLiftThresh;
            if (rising && (dense || lifted)) {
                m_entryBass = bass;
                enterPhase(TrackPhase::BUILD, "rise");
            }
        }
        break;
    }
    case TrackPhase::BUILD: {
        // Progress: energy climb since entry, normalized to ~3x drop jump.
        const float climb = bass - m_entryBass;
        const float byEnergy = climb / (m_params.dropJump * 3.0f);
        const float byTime = m_state.timeInPhase / 10.0f;
        m_state.buildProgress = std::clamp(std::max(byEnergy, byTime), 0.0f, 1.0f);

        // Drop = sharp bass jump inside a short window.
        const float windowMin = bassAt(m_params.dropWindow);
        const float jump = bass - std::min(windowMin, m_entryBass);
        if (dwelled && m_state.timeInPhase >= m_params.buildMinTime
            && jump > m_params.dropJump) {
            m_state.dropPower = std::clamp(jump / m_params.dropPowerScale, 0.0f, 1.0f);
            m_dropPeak = bass;
            enterPhase(TrackPhase::DROP, "jump");
        } else if (m_state.timeInPhase > m_params.maxBuildTime) {
            enterPhase(TrackPhase::RELEASE, "fizzle");
        }
        break;
    }
    case TrackPhase::DROP: {
        m_dropPeak = std::max(m_dropPeak, bass);
        if (dwelled && bass < m_dropPeak * m_params.releaseRatio) {
            enterPhase(TrackPhase::RELEASE, "decay");
        } else if (m_state.timeInPhase > m_params.maxDropTime) {
            enterPhase(TrackPhase::RELEASE, "timeout");
        }
        break;
    }
    case TrackPhase::RELEASE: {
        const bool quiet = bass < m_params.calmFloor
            && std::abs(m_state.bassSlope) < m_params.buildSlopeThresh * 0.5f;
        if ((dwelled && quiet) || m_state.timeInPhase > m_params.maxReleaseTime) {
            m_entryBass = bass;
            enterPhase(TrackPhase::CALM, "settled");
        } else if (dwelled && m_state.bassSlope > m_params.buildSlopeThresh
                   && m_state.onsetDensity > m_params.onsetDensityThresh) {
            // New buildup straight out of release (double-drop tracks).
            m_entryBass = bass;
            enterPhase(TrackPhase::BUILD, "rebuild");
        }
        break;
    }
    }
}

} // namespace nonstop
