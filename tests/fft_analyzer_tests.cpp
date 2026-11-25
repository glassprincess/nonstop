#include "audio/fft_analyzer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

int main() {
    nonstop::FftAnalyzer analyzer;
    std::vector<float> input(8192);
    for (size_t i = 0; i < input.size(); ++i) input[i] = static_cast<float>(i) / 8192.0f;
    size_t consumed = 0;
    for (size_t chunk : {size_t(1), size_t(510), size_t(1), size_t(777), size_t(3), size_t(6900)}) {
        analyzer.processSamples(input.data() + consumed, chunk);
        consumed += chunk;
        const auto snap = analyzer.getSnapshot();
        check(snap.frameCounter == consumed / 512, "FFT cadence must survive partial hops");
        if (snap.frameCounter == 0) continue;
        const size_t end = consumed / 512 * 512;
        float sumSq = 0.0f;
        for (size_t i = 0; i < 2048; ++i) {
            const auto index = static_cast<long long>(end) - 2048 + static_cast<long long>(i);
            const float value = index < 0 ? 0.0f : input[static_cast<size_t>(index)];
            sumSq += value * value;
            if (i % 8 == 0) check(snap.waveform[i / 8] == value, "waveform must keep chronological samples and startup zero padding");
        }
        check(std::abs(snap.rms - std::sqrt(sumSq / 2048)) < 1e-6f, "RMS must match the completed FFT window");
    }
    analyzer.processSamples(nullptr, 10);
    analyzer.processSamples(input.data(), 0);
    check(analyzer.getSnapshot().frameCounter == 16, "empty input must not run FFT");
    std::puts("PASS: FFT windows and chunk boundaries");
}
