#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "recognition-preview.h"
#include <ole2.h>
#include <uiautomation.h>
#include <oleacc.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwchar>

namespace yanflow {
namespace {
struct DpiScope {
    DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ~DpiScope() { if (previous) SetThreadDpiAwarenessContext(previous); }
};
int scaled(int value, UINT dpi) { return MulDiv(value, dpi, 96); }
HFONT previewFont(UINT dpi) {
    return CreateFontW(-scaled(15, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Microsoft YaHei UI");
}
RECT placeBubble(RECT caret, SIZE size, RECT work, int gap) {
    const LONG width = std::min(size.cx, work.right - work.left);
    const LONG height = std::min(size.cy, work.bottom - work.top);
    const LONG left = std::clamp(caret.left - gap, work.left, work.right - width);
    LONG top = caret.top - gap - height;
    if (top < work.top) top = caret.bottom + gap;
    top = std::clamp(top, work.top, work.bottom - height);
    return {left, top, left + width, top + height};
}
bool rangeRectangle(IUIAutomationTextRange* range, RECT& rect) {
    const auto read = [&](RECT& output) {
        SAFEARRAY* bounds = nullptr;
        if (FAILED(range->GetBoundingRectangles(&bounds)) || !bounds) return false;
        LONG begin = 0, end = -1;
        SafeArrayGetLBound(bounds, 1, &begin); SafeArrayGetUBound(bounds, 1, &end);
        double* values = nullptr;
        bool valid = false;
        if (end - begin >= 3 && SUCCEEDED(SafeArrayAccessData(bounds, reinterpret_cast<void**>(&values)))) {
            if (std::isfinite(values[0]) && std::isfinite(values[1]) && std::isfinite(values[2]) &&
                std::isfinite(values[3]) && std::abs(values[0]) < 1000000 && std::abs(values[1]) < 1000000 &&
                values[2] >= 0 && values[2] < 1000000 && values[3] > 0 && values[3] < 1000000) {
                output = {static_cast<LONG>(values[0]), static_cast<LONG>(values[1]),
                    static_cast<LONG>(values[0] + std::max(1.0, values[2])), static_cast<LONG>(values[1] + values[3])};
                valid = true;
            }
            SafeArrayUnaccessData(bounds);
        }
        SafeArrayDestroy(bounds); return valid;
    };
    if (read(rect)) return true;
    // Some providers omit bounds for a collapsed range. Measure the adjacent
    // character without changing the selection in the application.
    int moved = 0;
    if (SUCCEEDED(range->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Character, 1, &moved)) && moved && read(rect)) {
        rect.right = rect.left + 1; return true;
    }
    if (SUCCEEDED(range->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character, -1, &moved)) && moved && read(rect)) {
        rect.left = rect.right - 1; return true;
    }
    return false;
}
bool locateCaret(HWND target, IUIAutomation* automation, RECT& rect, HWND& focus) {
    if (!target || GetForegroundWindow() != target) return false;
    GUITHREADINFO info{sizeof(info)};
    if (!GetGUIThreadInfo(GetWindowThreadProcessId(target, nullptr), &info) || !info.hwndFocus) return false;
    focus = info.hwndFocus;
    if (focus != target && !IsChild(target, focus)) return false;
    wchar_t name[256]{}; GetClassNameW(focus, name, 256);
    const bool native = _wcsicmp(name, L"Edit") == 0 || _wcsnicmp(name, L"RichEdit", 8) == 0 ||
        std::wcsncmp(name, L"WindowsForms10.EDIT.", 20) == 0;
    if (native) {
        if (!IsWindowEnabled(focus) || (GetWindowLongPtrW(focus, GWL_STYLE) & (ES_READONLY | ES_PASSWORD))) return false;
        if (info.hwndCaret && info.rcCaret.bottom > info.rcCaret.top) {
            rect = info.rcCaret;
            MapWindowPoints(info.hwndCaret, nullptr, reinterpret_cast<POINT*>(&rect), 2);
            return true;
        }
    }
    if (!automation) return false;
    IUIAutomationElement* element = nullptr;
    if (FAILED(automation->GetFocusedElement(&element)) || !element) return false;
    CONTROLTYPEID type = 0; BOOL enabled = FALSE, password = TRUE, offscreen = TRUE;
    element->get_CurrentControlType(&type); element->get_CurrentIsEnabled(&enabled);
    element->get_CurrentIsPassword(&password); element->get_CurrentIsOffscreen(&offscreen);
    bool editable = enabled && !password && !offscreen && (type == UIA_EditControlTypeId || type == UIA_DocumentControlTypeId);
    IUIAutomationValuePattern* value = nullptr;
    if (SUCCEEDED(element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value))) && value) {
        BOOL readonly = TRUE;
        if (SUCCEEDED(value->get_CurrentIsReadOnly(&readonly)) && readonly) editable = false;
        value->Release();
    }
    IUIAutomationTextRange* range = nullptr;
    IUIAutomationTextPattern2* text2 = nullptr;
    if (editable && SUCCEEDED(element->GetCurrentPatternAs(UIA_TextPattern2Id, IID_PPV_ARGS(&text2))) && text2) {
        BOOL active = FALSE;
        text2->GetCaretRange(&active, &range);
        if (!active && range) { range->Release(); range = nullptr; }
        text2->Release();
    }
    if (editable && !range) {
        IUIAutomationTextPattern* text = nullptr;
        if (SUCCEEDED(element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text))) && text) {
            IUIAutomationTextRangeArray* selection = nullptr;
            if (SUCCEEDED(text->GetSelection(&selection)) && selection) {
                int count = 0; selection->get_Length(&count);
                if (count == 1) selection->GetElement(0, &range);
                selection->Release();
            }
            text->Release();
        }
    }
    bool found = false;
    if (range) {
        VARIANT readonly{};
        if (SUCCEEDED(range->GetAttributeValue(UIA_IsReadOnlyAttributeId, &readonly)) && readonly.vt == VT_BOOL && readonly.boolVal) editable = false;
        VariantClear(&readonly);
        if (editable) found = rangeRectangle(range, rect);
        range->Release();
    }
    element->Release();
    // Electron apps may expose their caret through MSAA instead of TextPattern.
    if (editable && !found) {
        if (info.hwndCaret && info.rcCaret.bottom > info.rcCaret.top) {
            rect = info.rcCaret; MapWindowPoints(info.hwndCaret, nullptr, reinterpret_cast<POINT*>(&rect), 2); found = true;
        } else {
            IAccessible* caret = nullptr;
            if (SUCCEEDED(AccessibleObjectFromWindow(focus, static_cast<DWORD>(OBJID_CARET), IID_IAccessible, reinterpret_cast<void**>(&caret))) && caret) {
                long x = 0, y = 0, width = 0, height = 0;
                VARIANT child{}; child.vt = VT_I4; child.lVal = CHILDID_SELF;
                if (SUCCEEDED(caret->accLocation(&x, &y, &width, &height, child)) && height > 0) {
                    rect = {x, y, x + std::max(1L, width), y + height}; found = true;
                }
                caret->Release();
            }
        }
    }
    RECT client{}; GetWindowRect(target, &client);
    return editable && found && rect.left >= client.left && rect.left < client.right &&
        rect.top >= client.top && rect.bottom <= client.bottom && GetForegroundWindow() == target;
}
}

bool RecognitionPreview::initialize(HINSTANCE instance) {
    DpiScope dpiScope;
    WNDCLASSEXW type{sizeof(type)};
    type.hInstance = instance; type.lpfnWndProc = procedure; type.lpszClassName = L"YanFlowCaretPreview";
    RegisterClassExW(&type);
    window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
        type.lpszClassName, L"言流 · 实时预览", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (!window_) return false;
    SetLayeredWindowAttributes(window_, 0, 248, LWA_ALPHA);
    locator_ = std::thread(&RecognitionPreview::locate, this);
    return true;
}
RecognitionPreview::~RecognitionPreview() {
    { std::lock_guard<std::mutex> lock(mutex_); running_ = false; }
    changed_.notify_one();
    if (locator_.joinable()) locator_.join();
    if (window_) DestroyWindow(window_);
}
void RecognitionPreview::begin(HWND target) {
    text_.clear();
    if (window_) ShowWindow(window_, SW_HIDE);
    { std::lock_guard<std::mutex> lock(mutex_); target_ = target; active_ = true; locatedTick_ = 0; }
    changed_.notify_one();
}
void RecognitionPreview::setText(const std::wstring& text) { text_ = text; refresh(); }
void RecognitionPreview::finish() {
    { std::lock_guard<std::mutex> lock(mutex_); active_ = false; locatedTick_ = 0; }
    text_.clear(); if (window_) ShowWindow(window_, SW_HIDE);
}
void RecognitionPreview::locate() {
    DpiScope dpiScope;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* automation = nullptr;
    CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
    if (!automation) CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
    IUIAutomation2* settings = nullptr;
    if (automation && SUCCEEDED(automation->QueryInterface(IID_PPV_ARGS(&settings)))) {
        settings->put_ConnectionTimeout(200); settings->put_TransactionTimeout(200); settings->Release();
    }
    std::unique_lock<std::mutex> lock(mutex_);
    while (running_) {
        changed_.wait(lock, [this] { return !running_ || active_; });
        if (!running_) break;
        const HWND target = target_;
        lock.unlock();
        RECT caret{}; HWND focus = nullptr;
        const bool found = locateCaret(target, automation, caret, focus);
        lock.lock();
        if (active_ && target == target_) {
            locatedTick_ = found ? GetTickCount64() : 0; caret_ = caret;
            locatedTarget_ = target; locatedFocus_ = focus;
        }
        changed_.wait_for(lock, std::chrono::milliseconds(160), [this, target] { return !running_ || !active_ || target_ != target; });
    }
    lock.unlock();
    if (automation) automation->Release();
    if (SUCCEEDED(com)) CoUninitialize();
}
SIZE RecognitionPreview::measure(UINT dpi) const {
    HDC dc = GetDC(window_); HFONT font = previewFont(dpi); auto old = SelectObject(dc, font);
    RECT natural{}; DrawTextW(dc, text_.c_str(), -1, &natural, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
    const LONG maximum = std::max(1L, std::min<LONG>(scaled(520, dpi), monitor.rcWork.right - monitor.rcWork.left - scaled(16, dpi)));
    const LONG width = std::min(maximum, std::max<LONG>(scaled(148, dpi), natural.right + scaled(48, dpi)));
    RECT wrapped{0, 0, std::max(1L, width - scaled(48, dpi)), 0};
    DrawTextW(dc, text_.c_str(), -1, &wrapped, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    const LONG height = std::max<LONG>(scaled(44, dpi), std::min<LONG>(scaled(124, dpi), wrapped.bottom + scaled(24, dpi)));
    SelectObject(dc, old); DeleteObject(font); ReleaseDC(window_, dc); return {width, height};
}
void RecognitionPreview::refresh() {
    if (!window_) return;
    DpiScope dpiScope;
    RECT caret{}; HWND target = nullptr, focus = nullptr;
    bool valid = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        valid = active_ && locatedTick_ && GetTickCount64() - locatedTick_ < 800;
        caret = caret_; target = locatedTarget_; focus = locatedFocus_;
    }
    GUITHREADINFO info{sizeof(info)};
    if (text_.empty() || !valid || GetForegroundWindow() != target || !GetGUIThreadInfo(GetWindowThreadProcessId(target, nullptr), &info) || info.hwndFocus != focus) {
        ShowWindow(window_, SW_HIDE); return;
    }
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromRect(&caret, MONITOR_DEFAULTTONEAREST), &monitor);
    if (MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST) != MonitorFromRect(&caret, MONITOR_DEFAULTTONEAREST))
        SetWindowPos(window_, nullptr, caret.left, caret.top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
    const UINT dpi = GetDpiForWindow(window_);
    const RECT rect = placeBubble(caret, measure(dpi), monitor.rcWork, scaled(10, dpi));
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    SetWindowRgn(window_, CreateRoundRectRgn(0, 0, width + 1, height + 1, scaled(20, dpi), scaled(20, dpi)), FALSE);
    SetWindowPos(window_, HWND_TOPMOST, rect.left, rect.top, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(window_, nullptr, FALSE);
}
LRESULT CALLBACK RecognitionPreview::procedure(HWND window, UINT message, WPARAM first, LPARAM second) {
    auto* self = reinterpret_cast<RecognitionPreview*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<RecognitionPreview*>(reinterpret_cast<CREATESTRUCTW*>(second)->lpCreateParams);
        self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT && self) { self->paint(); return 0; }
    return DefWindowProcW(window, message, first, second);
}
void RecognitionPreview::paint() {
    PAINTSTRUCT paint{}; HDC dc = BeginPaint(window_, &paint);
    RECT rect{}; GetClientRect(window_, &rect);
    const UINT dpi = GetDpiForWindow(window_);
    HDC memory = CreateCompatibleDC(dc); HBITMAP bitmap = CreateCompatibleBitmap(dc, rect.right, rect.bottom);
    auto oldBitmap = SelectObject(memory, bitmap);
    HBRUSH background = CreateSolidBrush(RGB(24, 29, 42)); FillRect(memory, &rect, background); DeleteObject(background);
    HPEN border = CreatePen(PS_SOLID, 1, RGB(65, 82, 109)); auto oldPen = SelectObject(memory, border);
    auto oldBrush = SelectObject(memory, GetStockObject(NULL_BRUSH));
    RoundRect(memory, 0, 0, rect.right, rect.bottom, scaled(20, dpi), scaled(20, dpi));
    SelectObject(memory, oldPen); SelectObject(memory, oldBrush); DeleteObject(border);
    SetBkMode(memory, TRANSPARENT);
    HFONT font = previewFont(dpi); auto oldFont = SelectObject(memory, font);
    SetTextColor(memory, RGB(239, 245, 255));
    RECT body{scaled(24, dpi), scaled(12, dpi), rect.right - scaled(24, dpi), rect.bottom - scaled(12, dpi)};
    DrawTextW(memory, text_.c_str(), -1, &body, DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL | DT_END_ELLIPSIS);
    SelectObject(memory, oldFont); DeleteObject(font);
    HBRUSH accent = CreateSolidBrush(RGB(112, 211, 228));
    RECT line{scaled(11, dpi), scaled(15, dpi), scaled(13, dpi), rect.bottom - scaled(15, dpi)};
    FillRect(memory, &line, accent); DeleteObject(accent);
    BitBlt(dc, 0, 0, rect.right, rect.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, oldBitmap); DeleteObject(bitmap); DeleteDC(memory); EndPaint(window_, &paint);
}
bool RecognitionPreview::runSmoke() {
    const auto original = text_;
    text_ = L"好"; const auto shortSize = measure(96);
    text_ = L"这是一段随着说话逐步出现的实时识别文字"; const auto medium = measure(96);
    text_ = std::wstring(300, L'字'); const auto large = measure(96);
    text_ = original;
    const RECT work{-1920, -200, 0, 880};
    const RECT above = placeBubble({-200, 400, -198, 420}, {400, 80}, work, 10);
    const RECT below = placeBubble({-1910, -195, -1908, -175}, {400, 80}, work, 10);
    const RECT oversized = placeBubble({-10, 870, -8, 890}, {3000, 2000}, work, 10);
    const auto inside = [&](RECT r) { return r.left >= work.left && r.top >= work.top && r.right <= work.right && r.bottom <= work.bottom; };
    return shortSize.cx < medium.cx && medium.cx <= large.cx && large.cx <= 520 && large.cy <= 144 &&
        inside(above) && above.bottom == 390 && inside(below) && below.top == -165 && inside(oversized) &&
        (GetWindowLongPtrW(window_, GWL_EXSTYLE) & (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)) == (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT);
}
}
