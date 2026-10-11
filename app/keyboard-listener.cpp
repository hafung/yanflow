#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "keyboard-listener.h"
#include <array>

namespace yanflow {
namespace {
constexpr UINT Configure = WM_APP + 1, Refresh = WM_APP + 2, MaskStart = WM_APP + 3;
constexpr UINT TestKey = WM_APP + 4, TestSnapshot = WM_APP + 5;
constexpr std::array<uint32_t, 8> Modifiers{VK_LCONTROL, VK_RCONTROL, VK_LWIN, VK_RWIN, VK_LSHIFT, VK_RSHIFT, VK_LMENU, VK_RMENU};
uint32_t physicalModifiers()
{
    uint32_t mask = 0;
    for (size_t i = 0; i < Modifiers.size(); ++i) if (GetAsyncKeyState(static_cast<int>(Modifiers[i])) & 0x8000) mask |= 1u << i;
    return mask;
}
uint32_t normalize(uint32_t vk, uint32_t scan = 0, uint32_t flags = 0)
{
    if (vk == VK_CONTROL) return flags & LLKHF_EXTENDED ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU) return flags & LLKHF_EXTENDED ? VK_RMENU : VK_LMENU;
    if (vk == VK_SHIFT) return scan == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    return vk;
}
}
KeyboardListener::~KeyboardListener() { stop(); }
bool KeyboardListener::start(HWND target, UINT holdMessage, UINT toggleMessage, bool enabled, bool testMode)
{
    stop(); target_ = target; holdMessage_ = holdMessage; toggleMessage_ = toggleMessage;
    holdEnabled_.store(enabled); testMode_ = testMode;
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (testMode_) testAck_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ready_ || (testMode_ && !testAck_)) { stop(); return false; }
    stopping_.store(false);
    thread_ = std::thread(&KeyboardListener::run, this);
    // Do not expose a ready application before its keyboard queue/hook exists.
    if (WaitForSingleObject(ready_, 5000) != WAIT_OBJECT_0) { stop(); return false; }
    return available();
}
void KeyboardListener::stop()
{
    stopping_.store(true);
    if (thread_.joinable()) {
        const DWORD id = threadId_.load();
        if (id) PostThreadMessageW(id, WM_QUIT, 0, 0);
        thread_.join();
    }
    if (ready_) CloseHandle(ready_);
    if (testAck_) CloseHandle(testAck_);
    ready_ = testAck_ = nullptr; available_.store(false); holdActive_.store(false);
}
void KeyboardListener::configure(bool enabled)
{
    holdEnabled_.store(enabled);
    if (const DWORD id = threadId_.load()) PostThreadMessageW(id, Configure, 0, 0);
}
void KeyboardListener::refresh()
{
    if (const DWORD id = threadId_.load()) PostThreadMessageW(id, Refresh, 0, 0);
}
void KeyboardListener::install(bool reset)
{
    // Install before removing the old handle: no deliberate gap between hooks.
    const HHOOK next = SetWindowsHookExW(WH_KEYBOARD_LL, hook, GetModuleHandleW(nullptr), 0);
    if (next) {
        if (hook_) UnhookWindowsHookEx(hook_);
        hook_ = next;
    }
    available_.store(hook_ != nullptr); lastInstallTick_ = GetTickCount64();
    if (reset) {
        hold_.reset(); toggle_.reset();
        const auto current = testMode_ ? 0 : physicalModifiers();
        // Remember a modifier held while launching, without starting a session.
        for (size_t i = 0; i < Modifiers.size(); ++i) if (current & (1u << i)) {
            hold_.key(Modifiers[i], true); toggle_.key(Modifiers[i], true);
        }
        holdActive_.store(holdEnabled_.load() && hold_.active());
        lastKeyTick_ = GetTickCount64();
    }
}
void KeyboardListener::key(uint32_t vk, bool down, bool injected)
{
    if (injected) return;
    lastKeyTick_ = GetTickCount64();
    const bool toggled = toggle_.key(vk, down);
    const auto action = hold_.key(vk, down);
    holdActive_.store(holdEnabled_.load() && hold_.active());
    if (toggled || (holdEnabled_.load() && action == HoldHotkey::Action::Press)) {
        if (!testMode_) PostThreadMessageW(threadId_.load(), MaskStart, 0, 0);
    }
    if (toggled) PostMessageW(target_, toggleMessage_, 0, 0);
    else if (holdEnabled_.load() && action != HoldHotkey::Action::None)
        PostMessageW(target_, holdMessage_, action == HoldHotkey::Action::Press ? 1 : 0, 0);
}
void KeyboardListener::reconcileReleases(uint32_t current)
{
    // Only repair missing releases. Polling must never activate injected chords.
    const uint32_t missing = hold_.modifierKeys() & ~current;
    for (size_t i = 0; i < Modifiers.size(); ++i) if (missing & (1u << i)) key(Modifiers[i], false, false);
}
LRESULT CALLBACK KeyboardListener::hook(int code, WPARAM message, LPARAM parameter)
{
    if (code == HC_ACTION && owner_ && !owner_->testMode_) {
        const auto* event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(parameter);
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        if (down || message == WM_KEYUP || message == WM_SYSKEYUP)
            owner_->key(normalize(event->vkCode, event->scanCode, event->flags), down, (event->flags & LLKHF_INJECTED) != 0);
    }
    const auto result = CallNextHookEx(nullptr, code, message, parameter);
    if (code == HC_ACTION && owner_ && !owner_->testMode_ &&
        !(reinterpret_cast<const KBDLLHOOKSTRUCT*>(parameter)->flags & LLKHF_INJECTED)) owner_->lastKeyTick_ = GetTickCount64();
    return result;
}
void KeyboardListener::run()
{
    owner_ = this;
    MSG message{}; PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    threadId_.store(GetCurrentThreadId());
    install(true);
    const UINT_PTR timer = SetTimer(nullptr, 0, 50, nullptr);
    SetEvent(ready_);
    while (!stopping_.load() && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == Configure) install(true);
        else if (message.message == Refresh) install(true);
        else if (message.message == MaskStart) {
            // After the hook has returned, mask Start without swallowing modifiers.
            INPUT mask[2]{}; mask[0].type = mask[1].type = INPUT_KEYBOARD;
            mask[0].ki.wVk = mask[1].ki.wVk = 0xe8; mask[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, mask, sizeof(INPUT));
        } else if (testMode_ && message.message == TestKey) {
            key(normalize(static_cast<uint32_t>(message.wParam & 255)), (message.wParam & 256) != 0, (message.wParam & 512) != 0);
            SetEvent(testAck_);
        } else if (testMode_ && message.message == TestSnapshot) {
            reconcileReleases(static_cast<uint32_t>(message.wParam)); SetEvent(testAck_);
        } else if (message.message == WM_TIMER && message.wParam == timer && !testMode_) {
            const auto now = GetTickCount64();
            const auto current = physicalModifiers();
            // Hook callbacks precede async key updates; never reconcile immediately.
            if (now - lastKeyTick_ >= 100) reconcileReleases(current);
            // Windows cannot report a silently removed hook. Refresh only at rest.
            if (!current && !hold_.modifierKeys() && now - lastKeyTick_ >= 15000 && now - lastInstallTick_ >= 15000)
                install(false);
        } else { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (timer) KillTimer(nullptr, timer);
    if (hook_) UnhookWindowsHookEx(hook_);
    hook_ = nullptr; owner_ = nullptr; threadId_.store(0); available_.store(false); holdActive_.store(false);
}
bool KeyboardListener::testMessage(UINT message, WPARAM parameter)
{
    if (!testMode_ || !testAck_ || !available()) return false;
    ResetEvent(testAck_);
    return PostThreadMessageW(threadId_.load(), message, parameter, 0) && WaitForSingleObject(testAck_, 2000) == WAIT_OBJECT_0;
}
bool KeyboardListener::testKey(uint32_t vk, bool down, bool injected)
{
    return testMessage(TestKey, (vk & 255) | (down ? 256 : 0) | (injected ? 512 : 0));
}
bool KeyboardListener::testReleasedSnapshot(uint32_t modifiers) { return testMessage(TestSnapshot, modifiers); }
}
