#pragma once

#include <windows.h>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace nonstop {

class HotkeyManager {
public:
    using HotkeyCallback = std::function<void()>;

    HotkeyManager();
    ~HotkeyManager();

    // Start background hotkey thread listening for Ctrl+Alt+X (and Ctrl+Shift+F12).
    // Uses a hidden message-only window in the worker thread (reliable delivery),
    // instead of RegisterHotKey(NULL,...) which depends on thread-queue quirks.
    bool start();

    // Stop listening
    void stop();

    // Legacy callback (invoked on the hotkey worker thread — must be
    // thread-safe and fast; do NOT touch ImGui/D3D directly).
    // Preferred path for UI toggles: poll consumePanicPress() on the main
    // thread each frame (see main.cpp) and execute ShowWindow there.
    void setPanicCallback(HotkeyCallback callback) {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_panicCallback = std::move(callback);
    }

    // Main-thread polling API (thread-safe, lock-free).
    // Returns true exactly once per physical hotkey press.
    bool consumePanicPress();

    int getPressCount() const { return m_pressCount.load(std::memory_order_acquire); }
    ULONGLONG getLastPressTickMs() const { return m_lastPressTickMs.load(std::memory_order_acquire); }

    std::string getStatusString() const;

private:
    void onHotkeyFromWorker(int hotkeyId);

    std::atomic<bool> m_running{false};
    std::unique_ptr<std::thread> m_thread;
    DWORD m_threadId = 0;
    HWND m_hotkeyHwnd = nullptr;

    mutable std::mutex m_callbackMutex;
    HotkeyCallback m_panicCallback;

    // Lock-free press queue: worker increments, main thread consumes.
    std::atomic<int> m_pendingPresses{0};
    std::atomic<int> m_pressCount{0};
    std::atomic<ULONGLONG> m_lastPressTickMs{0};

    mutable std::mutex m_statusMutex;
    std::string m_statusString = "Not started";
};

} // namespace nonstop
