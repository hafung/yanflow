#pragma once
#include <windows.h>
#include "hold-hotkey.h"
#include <atomic>
#include <thread>

namespace yanflow {
// The hook and key state belong to this thread, never to the UI thread.
class KeyboardListener {
public:
    ~KeyboardListener();
    bool start(HWND target, UINT holdMessage, UINT toggleMessage, bool holdEnabled, bool testMode = false);
    void stop();
    void configure(bool holdEnabled);
    void refresh();
    bool available() const { return available_.load(); }
    bool holdActive() const { return holdActive_.load(); }
    // Synthetic traces delivered through the same thread/dispatch as the hook.
    bool testKey(uint32_t vk, bool down, bool injected = false);
    bool testReleasedSnapshot(uint32_t modifiers);
private:
    static LRESULT CALLBACK hook(int code, WPARAM message, LPARAM parameter);
    void run();
    void key(uint32_t vk, bool down, bool injected);
    void reconcileReleases(uint32_t modifiers);
    void install(bool reset);
    bool testMessage(UINT message, WPARAM parameter);
    HWND target_ = nullptr;
    UINT holdMessage_ = 0, toggleMessage_ = 0;
    HHOOK hook_ = nullptr;
    HANDLE ready_ = nullptr, testAck_ = nullptr;
    std::thread thread_;
    std::atomic<DWORD> threadId_{0};
    std::atomic<bool> available_{false}, holdActive_{false}, holdEnabled_{true}, stopping_{true};
    bool testMode_ = false;
    HoldHotkey hold_;
    ListeningToggleHotkey toggle_;
    uint64_t lastKeyTick_ = 0, lastInstallTick_ = 0;
    inline static thread_local KeyboardListener* owner_ = nullptr;
};
}
