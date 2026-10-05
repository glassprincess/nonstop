#include "core/hotkey_manager.h"
#include <sstream>

namespace nonstop {

constexpr int HOTKEY_PANIC_CTRL_ALT_X = 1001;
constexpr int HOTKEY_PANIC_CTRL_SHIFT_F12 = 1002;

static const wchar_t* kHotkeyWndClass = L"NonstopHotkeyMsgClass";

HotkeyManager::HotkeyManager() = default;

HotkeyManager::~HotkeyManager() {
    stop();
}

bool HotkeyManager::start() {
    stop();
    m_running.store(true, std::memory_order_release);
    m_pendingPresses.store(0, std::memory_order_release);

    HANDLE hReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    m_thread = std::make_unique<std::thread>([this, hReadyEvent]() {
        m_threadId = GetCurrentThreadId();
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

        // make sure this thread has a message queue first, or the
        // hotkeys silently never arrive.
        MSG dummy;
        PeekMessageW(&dummy, nullptr, 0, 0, PM_NOREMOVE);

        // invisible window just to catch the hotkeys, works best.
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kHotkeyWndClass;
        RegisterClassExW(&wc);

        HWND hwnd = CreateWindowExW(
            0, kHotkeyWndClass, L"NonstopHotkeys",
            0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        m_hotkeyHwnd = hwnd;

        BOOL hk1 = FALSE, hk2 = FALSE;
        DWORD err1 = 0, err2 = 0;
        if (hwnd) {
            hk1 = RegisterHotKey(hwnd, HOTKEY_PANIC_CTRL_ALT_X,
                                 MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'X');
            if (!hk1) {
                err1 = GetLastError();
                hk1 = RegisterHotKey(hwnd, HOTKEY_PANIC_CTRL_ALT_X,
                                     MOD_CONTROL | MOD_ALT, 'X');
                if (!hk1) err1 = GetLastError();
            }
            hk2 = RegisterHotKey(hwnd, HOTKEY_PANIC_CTRL_SHIFT_F12,
                                 MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F12);
            if (!hk2) {
                err2 = GetLastError();
                hk2 = RegisterHotKey(hwnd, HOTKEY_PANIC_CTRL_SHIFT_F12,
                                     MOD_CONTROL | MOD_SHIFT, VK_F12);
                if (!hk2) err2 = GetLastError();
            }
        } else {
            err1 = GetLastError();
            err2 = err1;
        }

        {
            std::stringstream ss;
            ss << "Listening: [Ctrl+Alt+X: " << (hk1 ? "OK" : "CONFLICT")
               << (hk1 ? "" : (" err=" + std::to_string(err1)))
               << "] [Ctrl+Shift+F12: " << (hk2 ? "OK" : "CONFLICT")
               << (hk2 ? "" : (" err=" + std::to_string(err2)))
               << "] presses=" << m_pressCount.load();
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_statusString = ss.str();
        }

        SetEvent(hReadyEvent);

        MSG msg;
        while (m_running.load(std::memory_order_acquire) &&
               GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_HOTKEY) {
                if (msg.wParam == HOTKEY_PANIC_CTRL_ALT_X ||
                    msg.wParam == HOTKEY_PANIC_CTRL_SHIFT_F12) {
                    onHotkeyFromWorker(static_cast<int>(msg.wParam));
                }
                continue;
            }
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (hwnd) {
            UnregisterHotKey(hwnd, HOTKEY_PANIC_CTRL_ALT_X);
            UnregisterHotKey(hwnd, HOTKEY_PANIC_CTRL_SHIFT_F12);
            DestroyWindow(hwnd);
        }
        UnregisterClassW(kHotkeyWndClass, GetModuleHandleW(nullptr));
        m_hotkeyHwnd = nullptr;
    });

    WaitForSingleObject(hReadyEvent, 2000);
    CloseHandle(hReadyEvent);
    return true;
}

void HotkeyManager::onHotkeyFromWorker(int /*hotkeyId*/) {
    m_pressCount.fetch_add(1, std::memory_order_acq_rel);
    m_pendingPresses.fetch_add(1, std::memory_order_acq_rel);
    m_lastPressTickMs.store(GetTickCount64(), std::memory_order_release);

    {
        std::stringstream ss;
        ss << "Listening: presses=" << m_pressCount.load(std::memory_order_acquire)
           << " last=" << m_lastPressTickMs.load(std::memory_order_acquire) << "ms";
        std::lock_guard<std::mutex> lock(m_statusMutex);
        // keep the OK/CONFLICT bit, tack the press count on.
        if (m_statusString.find("Listening") == std::string::npos) {
            m_statusString = ss.str();
        } else {
            // same text as before, plus presses.
            auto pos = m_statusString.find(" presses=");
            std::string base = (pos == std::string::npos)
                ? m_statusString : m_statusString.substr(0, pos);
            m_statusString = base + " presses=" +
                std::to_string(m_pressCount.load()) +
                " last=" + std::to_string(m_lastPressTickMs.load()) + "ms";
        }
    }

    // old-school callback, runs on the worker thread: keep it tiny.
    HotkeyCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        cb = m_panicCallback;
    }
    if (cb) {
        cb();
    }
}

bool HotkeyManager::consumePanicPress() {
    int pending = m_pendingPresses.load(std::memory_order_acquire);
    while (pending > 0) {
        if (m_pendingPresses.compare_exchange_weak(
                pending, pending - 1,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

std::string HotkeyManager::getStatusString() const {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    return m_statusString;
}

void HotkeyManager::stop() {
    if (m_running.load()) {
        m_running.store(false);
        if (m_threadId != 0) {
            PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
        }
        if (m_thread && m_thread->joinable()) {
            m_thread->join();
        }
        m_thread.reset();
        m_threadId = 0;
        m_hotkeyHwnd = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_statusString = "Stopped";
        }
    }
}

} // namespace nonstop
