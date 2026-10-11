#pragma once
#include <windows.h>
#include <string>

namespace yanflow {
struct TextDelivery {
    // Submission and exact read-back verification are separate outcomes.
    bool submitted = false;
    std::wstring status = L"bubble", method = L"none", observed;
};
TextDelivery deliverText(HWND foreground, HWND clipboardOwner, const std::wstring& text);
int runTextDeliverySmoke();
}
