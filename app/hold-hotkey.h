#pragma once
#include <cstdint>

namespace yanflow {
// A small state machine shared by the Win32 hook and the focused smoke.
class HoldHotkey {
public:
    enum class Action { None, Press, Release };
    Action key(uint32_t vk, bool down, bool injected = false)
    {
        if (injected) return Action::None;
        uint32_t bit = 0;
        switch (vk) {
        case 0xa2: bit = 1; break; // left Ctrl
        case 0xa3: bit = 2; break;
        case 0x5b: bit = 4; break; // left Win
        case 0x5c: bit = 8; break;
        case 0xa0: bit = 16; break;
        case 0xa1: bit = 32; break;
        case 0xa4: bit = 64; break;
        case 0xa5: bit = 128; break;
        default:
            if (down && chord()) blocked_ = true;
            break;
        }
        if (bit) {
            if (down) keys_ |= bit;
            else keys_ &= ~bit;
        }
        if ((keys_ & 15) == 0) blocked_ = false;
        const bool next = chord() && (keys_ & 240) == 0 && !blocked_;
        if (active_ == next) return Action::None;
        active_ = next;
        if (!next) blocked_ = true; // no repeats/retrigger until modifiers released
        return next ? Action::Press : Action::Release;
    }
    bool active() const { return active_; }
    bool chord() const { return (keys_ & 3) && (keys_ & 12); }
    void reset() { keys_ = 0; active_ = false; blocked_ = false; }
private:
    uint32_t keys_ = 0;
    bool active_ = false, blocked_ = false;
};
} // namespace yanflow
