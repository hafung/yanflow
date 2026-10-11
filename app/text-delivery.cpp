#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "text-delivery.h"
#include <cwchar>
#include <cstring>
#include <future>
#include <thread>
#include <vector>

namespace yanflow {
namespace {
bool nativeEditor(HWND editor) {
    wchar_t name[256]{};
    if (!GetClassNameW(editor, name, static_cast<int>(std::size(name)))) return false;
    return _wcsicmp(name, L"Edit") == 0 || _wcsnicmp(name, L"RichEdit", 8) == 0 ||
        std::wcsncmp(name, L"WindowsForms10.EDIT.", 20) == 0;
}
bool message(HWND editor, UINT type, WPARAM first, LPARAM second, DWORD_PTR& value) {
    return SendMessageTimeoutW(editor, type, first, second, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &value) != 0;
}
bool readText(HWND editor, std::wstring& text) {
    DWORD_PTR length = 0, received = 0;
    if (!message(editor, WM_GETTEXTLENGTH, 0, 0, length) || length > 65534) return false;
    std::vector<wchar_t> buffer(length + 1);
    if (!message(editor, WM_GETTEXT, buffer.size(), reinterpret_cast<LPARAM>(buffer.data()), received) || received > length) return false;
    text.assign(buffer.data(), received); return true;
}
TextDelivery deliverNative(HWND editor, const std::wstring& text, HWND foreground = nullptr) {
    TextDelivery delivery;
    delivery.method = L"unicode_edit";
    // ANSI controls cannot represent arbitrary dictation. Preserve the result
    // in the bubble instead of converting unrepresentable characters to '?'.
    if (!IsWindowUnicode(editor) || !IsWindowEnabled(editor) || (GetWindowLongPtrW(editor, GWL_STYLE) & ES_READONLY)) return delivery;
    std::wstring before;
    DWORD_PTR selection = 0, ignored = 0;
    if (!readText(editor, before) || !message(editor, EM_GETSEL, 0, 0, selection)) return delivery;
    const size_t start = LOWORD(selection), end = HIWORD(selection);
    if (start > end || end > before.size() || before.size() + text.size() > 65534) return delivery;
    auto expected = before; expected.replace(start, end - start, text);
    if (foreground) {
        GUITHREADINFO info{sizeof(info)};
        if (GetForegroundWindow() != foreground || !GetGUIThreadInfo(GetWindowThreadProcessId(foreground, nullptr), &info) ||
            info.hwndFocus != editor) return delivery;
    }
    // This wide message bypasses TranslateMessage/ANSI message loops and IMEs.
    if (!message(editor, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(text.c_str()), ignored)) {
        delivery.status = L"unconfirmed"; return delivery;
    }
    std::wstring after;
    if (!readText(editor, after)) { delivery.status = L"unconfirmed"; return delivery; }
    delivery.submitted = after == expected;
    const auto suffix = before.substr(end);
    if (after.size() >= start + suffix.size() && after.compare(0, start, before, 0, start) == 0 &&
        after.compare(after.size() - suffix.size(), suffix.size(), suffix) == 0)
        delivery.observed = after.substr(start, after.size() - start - suffix.size());
    delivery.status = delivery.submitted ? L"verified" : L"mismatch";
    return delivery;
}
bool copyUnicode(HWND owner, const std::wstring& text) {
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return false;
    auto* buffer = GlobalLock(memory);
    if (!buffer) { GlobalFree(memory); return false; }
    memcpy(buffer, text.c_str(), bytes); GlobalUnlock(memory);
    if (!OpenClipboard(owner)) { GlobalFree(memory); return false; }
    const bool copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
    if (!copied) GlobalFree(memory);
    return copied;
}
}
TextDelivery deliverText(HWND foreground, HWND clipboardOwner, const std::wstring& text) {
    TextDelivery delivery;
    if (text.empty() || !foreground || GetForegroundWindow() != foreground) return delivery;
    GUITHREADINFO info{sizeof(info)};
    if (!GetGUIThreadInfo(GetWindowThreadProcessId(foreground, nullptr), &info) || !info.hwndFocus ||
        (info.hwndFocus != foreground && !IsChild(foreground, info.hwndFocus))) return delivery;
    if (nativeEditor(info.hwndFocus)) return deliverNative(info.hwndFocus, text, foreground);
    // Browser/modern editor text arrives through the Unicode clipboard. Do not
    // send Ctrl+V while the user is holding modifiers from a recording hotkey.
    for (int key : {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN})
        if (GetAsyncKeyState(key) & 0x8000) return delivery;
    if (!copyUnicode(clipboardOwner, text) || GetForegroundWindow() != foreground) return delivery;
    GUITHREADINFO current{sizeof(current)};
    if (!GetGUIThreadInfo(GetWindowThreadProcessId(foreground, nullptr), &current) || current.hwndFocus != info.hwndFocus) return delivery;
    INPUT inputs[4]{};
    for (auto& input : inputs) input.type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = inputs[3].ki.wVk = VK_CONTROL;
    inputs[1].ki.wVk = inputs[2].ki.wVk = 'V';
    inputs[2].ki.dwFlags = inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    delivery.submitted = SendInput(4, inputs, sizeof(INPUT)) == 4;
    delivery.method = L"unicode_clipboard";
    delivery.status = delivery.submitted ? L"submitted_unverified" : L"bubble";
    return delivery;
}
int runTextDeliverySmoke() {
    // Exercise a Unicode edit on another GUI thread with an ANSI message
    // loop: direct wide insertion must survive this legacy-host combination.
    std::promise<HWND> ready;
    auto handle = ready.get_future();
    std::thread host([&ready] {
        HWND hosted = CreateWindowExW(0, L"EDIT", L"prefix:OLD:suffix", WS_POPUP | ES_MULTILINE,
            0, 0, 300, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ready.set_value(hosted);
        if (!hosted) return;
        MSG event{};
        while (GetMessageA(&event, nullptr, 0, 0) > 0) { TranslateMessage(&event); DispatchMessageA(&event); }
        DestroyWindow(hosted);
    });
    HWND hosted = handle.get();
    bool legacyExact = false;
    if (hosted) {
        SendMessageW(hosted, EM_SETSEL, 7, 10);
        const auto legacy = deliverNative(hosted, L"note ZS的最新版板是什？ Node.js 中文 😀");
        legacyExact = legacy.submitted && legacy.observed == L"note ZS的最新版板是什？ Node.js 中文 😀";
        PostThreadMessageW(GetWindowThreadProcessId(hosted, nullptr), WM_QUIT, 0, 0);
    }
    host.join();
    HWND editor = CreateWindowExW(0, L"EDIT", L"prefix:OLD:suffix", WS_POPUP | ES_MULTILINE, 0, 0, 300, 100,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!editor) return 150;
    SendMessageW(editor, EM_SETSEL, 7, 10);
    const auto text = L"note ZS的最新版板是什？ Node.js 中文 😀";
    const auto result = deliverNative(editor, text);
    const bool exact = result.submitted && result.status == L"verified" && result.observed == text;
    SendMessageW(editor, EM_SETREADONLY, TRUE, 0);
    const auto readonly = deliverNative(editor, L"不可写");
    std::wstring after; readText(editor, after);
    DestroyWindow(editor);
    HWND ansi = CreateWindowExA(0, "EDIT", "unchanged", WS_POPUP, 0, 0, 300, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const auto unsupported = deliverNative(ansi, text);
    std::wstring ansiAfter; readText(ansi, ansiAfter); DestroyWindow(ansi);
    return legacyExact && exact && !readonly.submitted && after == std::wstring(L"prefix:") + text + L":suffix" &&
        !unsupported.submitted && ansiAfter == L"unchanged" ? 0 : 151;
}
}
