#include "audio/phase_detector.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

int main(int argc, char** argv) {
    nonstop::PhaseDetector detector;
    nonstop::AudioAnalysisSnapshot snapshot;
    snapshot.isOnset = true;
    detector.update(snapshot);
    check(detector.getState().onsetDensity == 0.5f, "the first timestamp may be zero");
    detector.reset();
    snapshot.audioTimeSec = 1.0;
    snapshot.isOnset = true;
    detector.update(snapshot);
    if (argc > 1) {
        const auto before = detector.getState();
        for (int i = 0; i < 60; ++i) detector.update(snapshot);
        const auto after = detector.getState();
        check(after.onsetDensity == before.onsetDensity, "one audio snapshot must count only once");
        check(after.timeInPhase == before.timeInPhase, "render frames must not advance the audio clock");
        detector.reset();
        detector.update(snapshot);
        check(detector.getState().onsetDensity == 0.5f, "reset must accept the first snapshot again");
        snapshot.audioTimeSec = 0.5;
        snapshot.isOnset = false;
        const float beforeClockChange = detector.getState().timeInPhase;
        detector.update(snapshot);
        check(detector.getState().timeInPhase > beforeClockChange,
              "an audio clock discontinuity must not freeze updates");
    } else {
        for (int i = 1; i < 6; ++i) {
            snapshot.audioTimeSec = 1.0 + i * 0.1;
            detector.update(snapshot);
        }
        check(detector.getState().repeatActive, "six regular 10 Hz hits must arm repeat");
        snapshot.isOnset = false;
        snapshot.audioTimeSec = 1.9;
        detector.update(snapshot);
        check(detector.getState().repeatActive, "a short gap must preserve repeat");
        snapshot.audioTimeSec = 2.01;
        detector.update(snapshot);
        const auto expired = detector.getState();
        check(!expired.repeatActive, "repeat must expire after 0.5s without a hit, even with six hits in history");
        check(expired.repeatHz == 0.0f && expired.repeatStrength == 0.0f,
              "expired repeat must not drive effects");
        check(expired.repeatCandHz == 0.0f && expired.repeatCandStr == 0.0f,
              "expired candidate diagnostics must clear");
        for (int i = 0; i < 6; ++i) {
            snapshot.audioTimeSec = 4.0 + i * 0.1;
            snapshot.isOnset = true;
            detector.update(snapshot);
        }
        check(detector.getState().repeatActive, "a new burst must rearm repeat");
    }
    std::puts("PASS: phase detector");
}
