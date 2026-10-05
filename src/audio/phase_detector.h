#pragma once

#include "core/types.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace nonstop {

enum class TrackPhase {
    CALM = 0,
    BUILD = 1,
    DROP = 2,
    RELEASE = 3,
};

inline const char* toString(TrackPhase p) {
    switch (p) {
    case TrackPhase::CALM: return "CALM";
    case TrackPhase::BUILD: return "BUILD";
    case TrackPhase::DROP: return "DROP";
    case TrackPhase::RELEASE: return "RELEASE";
    }
    return "?";
}

// All thresholds are explicitly "tune on test tracks" (TZ 0.2-0.3:
// no invented constants treated as truth). Defaults are starting points.
struct PhaseParams {
    float buildSlopeWindow = 3.0f;   // seconds of bass history for slope
    float buildSlopeThresh = 0.030f; // bass energy rise per second -> BUILD
    float highsLiftThresh = 0.05f;   // highs above recent avg -> BUILD confirm
    float onsetDensityThresh = 2.5f; // onsets per second -> BUILD confirm
    float buildMinTime = 1.5f;       // min seconds before BUILD can drop
    float maxBuildTime = 30.0f;      // BUILD fizzle -> RELEASE (avoid stuck)
    float dropJump = 0.22f;          // bass rise inside dropWindow -> DROP
    float dropWindow = 0.35f;        // seconds to measure the jump
    float dropPowerScale = 0.55f;    // jump range mapped to dropPower 0..1
    float maxDropTime = 20.0f;       // DROP -> RELEASE fallback
    float releaseRatio = 0.55f;      // bass < dropPeak*ratio -> RELEASE
    float calmFloor = 0.12f;         // bass below -> CALM candidate
    float releaseTime = 2.5f;        // overlay RELEASE decay duration (acceptance #4)
    float minDwell = 0.8f;           // min seconds in any phase (anti-chatter)
    float maxReleaseTime = 14.0f;    // RELEASE -> CALM fallback
    // Repeat detector (HELL MODE invert sync): periodic onset bursts.
    float repMinHz = 3.0f;           // repeat rate band low
    float repMaxHz = 18.0f;          // repeat rate band high
    float repOnThresh = 0.50f;       // regularity to arm repeat
    float repOffThresh = 0.35f;      // regularity to release (hysteresis)
    float repSens = 1.0f;            // UI single knob 0.5..1.5 scales repOnThresh
};

struct PhaseState {
    TrackPhase phase = TrackPhase::CALM;
    float buildProgress = 0.0f; // 0..1 since BUILD entry
    float dropPower = 0.0f;     // 0..1 how hard the drop hit
    float timeInPhase = 0.0f;   // seconds
    float onsetDensity = 0.0f;  // onsets/sec (diagnostics)
    float bassSlope = 0.0f;     // per-second bass rise (diagnostics)
    // Repeat tracker (HELL MODE): fast periodic hits (rolls, repeat-kicks).
    bool repeatActive = false;
    float repeatHz = 0.0f;      // estimated hits/sec while active
    float repeatStrength = 0.0f;// 0..1 regularity while active
    float repeatCandHz = 0.0f;  // last candidate estimate (debug, always live)
    float repeatCandStr = 0.0f; // last candidate strength (debug, always live)
};

// Stage 4: CALM / BUILD / DROP / RELEASE detector (TZ 3.5, 12.4).
// Single-threaded: update() on the main/render thread once per frame.
class PhaseDetector {
public:
    PhaseDetector();

    void update(const AudioAnalysisSnapshot& snapshot);
    void reset();

    PhaseState getState() const { return m_state; }
    PhaseParams& params() { return m_params; }
    const PhaseParams& params() const { return m_params; }

    // Last transitions, newest last (for diagnostics log).
    std::vector<std::string> getLog() const { return m_log; }

private:
    struct HistPoint {
        double t = 0.0;
        float bass = 0.0f;
        float highs = 0.0f;
    };

    double nowSeconds();
    float bassAt(double tAgo) const;
    float avgHighs(double window) const;
    void enterPhase(TrackPhase next, const char* reason);

    PhaseParams m_params;
    PhaseState m_state;

    std::deque<HistPoint> m_history; // ~20Hz samples, 8s deep
    std::deque<double> m_onsets;     // onset timestamps, 2s deep
    double m_lastHistPush = -1.0;

    float m_entryBass = 0.0f;  // bass at BUILD entry (progress reference)
    float m_dropPeak = 0.0f;   // bass at DROP entry (release reference)

    std::chrono::steady_clock::time_point m_clockBase;
    bool m_clockInit = false;
    double m_lastUpdateT = 0.0;

    std::vector<std::string> m_log;
};

} // namespace nonstop
