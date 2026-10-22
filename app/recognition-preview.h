#pragma once
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace yanflow {
// Owns a non-activating, click-through window. Accessibility queries run on a
// separate MTA thread, never on the capture or UI thread.
class RecognitionPreview {
public:
    bool initialize(HINSTANCE instance);
    ~RecognitionPreview();
    void begin(HWND target);
    void setText(const std::wstring& text);
    void refresh();
    void finish();
    bool visible() const { return window_ && IsWindowVisible(window_); }
    bool runSmoke();
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    void locate();
    void paint();
    SIZE measure(UINT dpi) const;
    HWND window_ = nullptr;
    std::wstring text_;
    std::thread locator_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool running_ = true, active_ = false;
    HWND target_ = nullptr, locatedTarget_ = nullptr, locatedFocus_ = nullptr;
    RECT caret_{};
    uint64_t locatedTick_ = 0;
};
}
