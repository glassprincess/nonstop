#pragma once

#include "core/types.h"
#include "audio/phase_detector.h"
#include "overlay/screen_capturer.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <random>
#include <string>
#include <atomic>
#include <deque>
#include <algorithm>
#include <vector>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif

namespace nonstop {

using Microsoft::WRL::ComPtr;

// Wave-1 effect modules (each independently toggleable in the EFFECTS menu).
enum class FxModule : int {
    RGB = 0,    // chromatic split + echoes (bass)
    SLICE = 1,  // row shifts + glitch blocks (highs/onsets)
    WARP = 2,   // waves (mids)
    MELT = 3,   // screen melting (release)
    RIPPLE = 4, // onset shockwave rings
    GLASS = 5,  // rain drops + frost + prism (mids/air)
    ZOOM = 6,   // zoom punch + shake (beats/bass, TZ 4.5)
    TEXT = 7,   // ghost words
    JELLY = 8,  // [BETA] elastic whole-screen wobble (onsets)
    STREAK = 9, // [BETA] bright-pixel vertical smear (highs/drop)
    DUP = 10,   // duplication cascade: copies drift apart (repeats)
    COUNT = 11,
};
constexpr int kFxModuleCount = 11;
const char* fxModuleName(FxModule m);

class OverlayWindow {
public:
    OverlayWindow();
    ~OverlayWindow();

    // Create fullscreen transparent click-through overlay
    bool create(int width = 0, int height = 0);

    // Show or hide overlay
    void setVisible(bool visible);
    bool isVisible() const { return m_visible.load(); }
    void toggleVisible();

    // Per-frame phase input (Stage 4). Call before renderFrame().
    void setPhaseState(const PhaseState& st) { m_phase = st; }
    const PhaseState& getPhaseState() const { return m_phase; }
    void setReleaseTime(float t) { m_releaseTime = t; }

    // Render one audio-reactive frame onto the transparent surface
    void renderFrame(const AudioAnalysisSnapshot& snapshot);

    // Destroy window and D3D resources
    void shutdown();

    // Diagnostics & Status
    bool isExcludedFromCapture() const { return m_excludedFromCapture; }
    HWND getHwnd() const { return m_hwnd; }
    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }
    std::string getStatusString() const { return m_statusString; }

    // ---- Wave-1 modules (EFFECTS menu). Main thread only. ----
    bool isModuleEnabled(FxModule m) const { return m_modules[static_cast<int>(m)]; }
    void setModuleEnabled(FxModule m, bool on) { m_modules[static_cast<int>(m)] = on; }

    void setRgbStrengthPx(float px) { m_rgbStrengthPx = px; }
    float getRgbStrengthPx() const { return m_rgbStrengthPx; }
    void setRgbDecay(float decayPerFrame) { m_rgbDecay = decayPerFrame; }
    float getRgbDecay() const { return m_rgbDecay; }
    void triggerRgbTestFlash() { m_rgbFlash = 1.0f; m_testHold = 1.0f; triggerRipple(0.5f, 0.5f); }
    float getRgbFlash() const { return m_rgbFlash; }
    void setDriftAmp(float a) { m_driftAmp = a; }
    float getDriftAmp() const { return m_driftAmp; }
    void setGhostMix(float g) { m_ghostMix = g; }
    float getGhostMix() const { return m_ghostMix; }
    void setSatBoost(float s) { m_satBoost = s; }
    float getSatBoost() const { return m_satBoost; }
    // Per-module amounts (EFFECTS sliders, 0..1).
    void setModuleAmount(FxModule m, float v) { m_amounts[static_cast<int>(m)] = v; }
    float getModuleAmount(FxModule m) const { return m_amounts[static_cast<int>(m)]; }
    float getSongLevel() const { return m_songLevel; }
    float getSongGain() const { return m_songGain; }
    float getPhaseMaster() const { return m_phaseMaster; }
    void setFxPower(float p) { m_fxPower = p; }
    float getFxPower() const { return m_fxPower; }
    // HELL MODE: 500% overdrive, armed ONLY via the epilepsy warning dialog.
    void setHellMode(bool on);
    bool isHellMode() const { return m_hellMode; }
    void setHellIntensity(float v) { m_hellIntensity = std::clamp(v, 1.0f, 5.0f); }
    float getHellIntensity() const { return m_hellIntensity; }
    void setHellAutoOffMin(float m) { m_hellAutoOffMin = m; }
    float getHellAutoOffMin() const { return m_hellAutoOffMin; }
    float getHellElapsedSec() const;
    float getRepeatHz() const { return m_phase.repeatHz; }
    float getRepeatStrength() const { return m_phase.repeatStrength; }
    bool isRepeatActive() const { return m_phase.repeatActive; }
    // Streamer mode: hidden=true excludes the overlay from ALL captures
    // (stream doesn't see the rave); hidden=false (default) makes it
    // visible to Discord screen share and friends.
    void setStreamerMode(bool hidden);
    bool isStreamerMode() const { return m_hiddenFromCapture; }
    float getLastDrive() const { return m_lastDrive; }
    float getLastIntensity() const { return m_lastIntensity; }
    bool wasLastFrameDrawn() const { return m_lastDrawn; }
    std::string getLastSkipReason() const { return m_lastSkipReason; }
    bool isPipelineOk() const {
        return m_screenVs && m_screenPs && m_screenVb && m_screenParamsCb;
    }

    // ---- Randomization (drop lottery + binding drift). ----
    bool isLotteryOn() const { return m_lotteryOn; }
    void setLotteryOn(bool on) { m_lotteryOn = on; }
    bool isDriftOn() const { return m_driftOn; }
    void setDriftOn(bool on) { m_driftOn = on; }
    uint32_t getSeed() const { return m_seed; }
    void rerollSeed(); // random reseed + clear accents
    std::string getAccentString() const { return m_accentString; }

    bool isCaptureActive() const { return m_capturer.isActive(); }
    std::string getCaptureStatusString() const { return m_capturer.getStatusString(); }
    int getCaptureWidth() const { return m_capturer.getWidth(); }
    int getCaptureHeight() const { return m_capturer.getHeight(); }
    uint64_t getCapturedFrames() const { return m_capturedFrames; }
    int getAliveWords() const { return m_aliveWords; }
    uint64_t getTotalWordsSpawned() const { return m_totalSpawned; }
    int getLastPhraseIdx() const { return m_lastPhraseIdx; }
    void spawnTestWord(); // force-spawn next phrase in order (vetting helper)

private:
    bool initD3D();
    bool initScreenPipeline();
    bool bakeTextAtlas();
    void renderFxChain(const AudioAnalysisSnapshot& snapshot);
    void renderOverlayGraphics(const AudioAnalysisSnapshot& snapshot);
    void renderGhostWords();
    void updateWords(const AudioAnalysisSnapshot& snapshot);
    void updateRandom(const AudioAnalysisSnapshot& snapshot);
    void applyCaptureAffinity();
    void triggerRipple(float cx, float cy);
    float accentBoost(FxModule m) const;
    // Spawn one word quad; returns false if no free slot.
    bool spawnWord(int phrase, float xPx, float yPx, float scale, float life, float peak);
    void triggerBlastStaircase();

    HWND m_hwnd = nullptr;
    int m_width = 0;
    int m_height = 0;
    std::atomic<bool> m_visible{true};
    bool m_excludedFromCapture = false;
    std::string m_statusString = "Not initialized";

    // D3D11 resources
    ComPtr<ID3D11Device> m_d3dDevice;
    ComPtr<ID3D11DeviceContext> m_d3dContext;
    ComPtr<IDXGISwapChain> m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_renderTargetView;
    ComPtr<ID3D11BlendState> m_blendState;

    // Primitives rendering (legacy color quads; kept for text-free fallback)
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_ps;
    ComPtr<ID3D11Buffer> m_vertexBuffer;
    ComPtr<ID3D11InputLayout> m_inputLayout;

    // Screen-space FX chain (wave-1 uber-shader)
    ComPtr<ID3D11VertexShader> m_screenVs;
    ComPtr<ID3D11PixelShader> m_screenPs;
    ComPtr<ID3D11Buffer> m_screenVb;
    ComPtr<ID3D11InputLayout> m_screenLayout;
    ComPtr<ID3D11SamplerState> m_screenSampler;
    ComPtr<ID3D11Buffer> m_screenParamsCb;
    ComPtr<ID3D11Buffer> m_hellCb; // FxHell b1: invert/mirror (HELL MODE)
    ComPtr<ID3D11Buffer> m_dupCb; // FxDup b2: duplication cascade (repeats)
    ScreenCapturer m_capturer;

    // Ghost-words text atlas (baked at init) + dynamic quad buffer + shader
    ComPtr<ID3D11Texture2D> m_textAtlas;
    ComPtr<ID3D11ShaderResourceView> m_textSrv;
    ComPtr<ID3D11PixelShader> m_textPs;
    ComPtr<ID3D11Buffer> m_textVb;
    ComPtr<ID3D11Buffer> m_textCb;
    int m_textRows = 0;

    struct WordInstance {
        bool alive = false;
        int phrase = 0;
        float x = 0.0f;      // px, top-left of quad
        float y = 0.0f;      // px, top-left of quad
        float born = 0.0f;   // seconds
        float life = 3.0f;   // seconds
        float seed = 0.0f;
        float scale = 1.0f;  // glyph scale (quad sized by measured width)
        float peak = 0.25f;  // peak alpha multiplier
    };
    static constexpr int kMaxWords = 4;
    WordInstance m_words[kMaxWords] = {};
    int m_aliveWords = 0;
    uint64_t m_totalSpawned = 0;
    int m_lastPhraseIdx = -1;
    int m_testPhraseCursor = 0;
    static constexpr int kGhostPhraseCountHdr = 16;
    int m_phraseW[kGhostPhraseCountHdr] = {}; // measured text widths, px (baked)

    // Module toggles + amounts (EFFECTS menu)
    bool m_modules[kFxModuleCount] = { true, true, true, true, true, true, true, true, true, true, true };
    float m_amounts[kFxModuleCount] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

    // Phase input + derived master
    PhaseState m_phase;
    TrackPhase m_prevPhase = TrackPhase::CALM;
    float m_phaseMaster = 0.0f;
    float m_meltSmooth = 0.0f;
    float m_testHold = 0.0f; // Test Flash forces full-strength demo, decays
    // Last-frame FX diagnostics (EFFECTS menu + log file)
    float m_lastDrive = 0.0f;
    float m_lastIntensity = 0.0f;
    float m_lastJelly = 0.0f;
    float m_lastStreak = 0.0f;
    float m_lastDup = 0.0f;
    bool m_lastDrawn = false;
    std::string m_lastSkipReason = "n/a";

    // Beat envelope + song follower
    float m_rgbStrengthPx = 28.0f;
    float m_rgbDecay = 0.06f;
    float m_rgbFlash = 0.0f;
    float m_loudPeak = 0.5f;
    float m_songLevel = 0.0f;
    float m_songGain = 0.0f;
    double m_timeSec = 0.0;
    float m_driftAmp = 0.0016f;
    float m_ghostMix = 0.45f;      // less rainbow: echoes tamed
    float m_satBoost = 0.15f;      // less rainbow: vibrance tamed
    float m_fxPower = 1.8f; // global FX strength multiplier (user: stronger)
    bool m_hiddenFromCapture = false; // streamer mode: hide FX from captures
    float m_rippleT = 10.0f;
    float m_rippleCx = 0.5f;
    float m_rippleCy = 0.5f;
    float m_blockSeed = 0.0f;
    // Drum-fill blast: brief full-screen distortion kick + word staircase.
    float m_blast = 0.0f;          // 1.0 on blast, fast decay
    double m_lastBlastSec = -1e9;  // cooldown
    std::deque<double> m_onsetTimes; // 350ms window for fill detection
    // Jelly spring: kicked by onsets, ~6Hz decaying wobble.
    float m_jellyX = 0.0f;
    float m_jellyV = 0.0f;
    // Duplication cascade state (repeats): copies multiply and drift.
    float m_dupN = 0.0f;    // smoothed active copy count 0..3
    float m_dupGrow = 0.0f; // grows while repeat holds (copies leave)
    // Real frame time (presents don't vsync; never assume 1/60).
    LARGE_INTEGER m_lastFrameQpc = {};
    bool m_qpcInit = false;
    float m_frameDt = 1.0f / 60.0f;
    // HELL MODE state (main thread only).
    bool m_hellMode = false;
    float m_hellIntensity = 5.0f; // 1..5 (100-500%), default full
    float m_hellAutoOffMin = 15.0f; // 0 = never auto-off
    double m_hellSinceSec = 0.0;
    // Repeat-synced invert: polarity flips per hit, max 10 flips/sec.
    int m_invPolarity = 0;
    double m_lastInvFlipSec = -1e9;
    float m_hellInv = 0.0f; // smoothed invert amount (no stuck negative)
    float m_mirrorT = 10.0f;
    float m_releaseTime = 2.5f; // synced from detector params (RELEASE curve)
    uint64_t m_capturedFrames = 0;

    // Randomization: drop-lottery accents + binding drift weights
    bool m_lotteryOn = true;
    bool m_driftOn = true;
    uint32_t m_seed = 1337;
    std::mt19937 m_rng;
    float m_driftW[kFxModuleCount] = { 1,1,1,1,1,1,1,1,1,1,1 };
    struct Accent { FxModule mod = FxModule::RGB; float boost = 1.0f; float until = 0.0f; };
    static constexpr int kMaxAccents = 2;
    Accent m_accents[kMaxAccents] = {};
    std::string m_accentString = "none";

    float m_beatFlashIntensity = 0.0f;
    uint64_t m_renderedFrames = 0;
};

} // namespace nonstop
