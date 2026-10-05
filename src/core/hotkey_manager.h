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

    // Background thread listening for Ctrl+Alt+X (and Ctrl+Shift+F12).
    // Hotkeys hang off a hidden window in that thread - plain
    // RegisterHotKey(NULL,...) misses depending on queue mood.
    bool start();

    // Stop listening
    void stop();

    // Old-school callback (worker thread - keep it tiny, no UI calls).
    // Better: poll consumePanicPress() on the main thread each frame.
    void setPanicCallback(HotkeyCallback callback) {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_panicCallback = std::move(callback);
    }

    // Ask once per press, main thread side.
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

    // press counter: worker bumps it, main thread eats it.
    std::atomic<int> m_pendingPresses{0};
    std::atomic<int> m_pressCount{0};
    std::atomic<ULONGLONG> m_lastPressTickMs{0};

    mutable std::mutex m_statusMutex;
    std::string m_statusString = "Not started";
};

} // namespace nonstop
