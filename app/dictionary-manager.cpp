#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "dictionary-manager.h"
#include "dictionary-store.h"
#include "text-pipeline.h"
#include <commctrl.h>
#include <algorithm>
#include <map>
#include <vector>

namespace yanflow {
namespace {
enum Control { Scope = 400, Reload, Search, Rules, Source, Target, Context, New, Save, Delete, Test, Preview, Status, Close, ScopeHelp };
std::wstring fold(std::wstring text)
{
    for (auto& c : text) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    return text;
}
std::wstring controlText(HWND window, int id)
{
    HWND control = GetDlgItem(window, id);
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(length)); return text;
}
std::wstring serialize(const DictionaryRule& rule)
{
    return rule.kind + L"\t" + rule.source + L"\t" + rule.target + (rule.context.empty() ? L"" : L"\t" + rule.context);
}
struct Entry {
    DictionaryRule rule;
    size_t start = 0, end = 0, line = 0;
    bool valid = false, conflict = false;
};
std::vector<Entry> entries(const std::wstring& text)
{
    std::vector<Entry> result;
    size_t lineNumber = 0;
    for (size_t start = 0; start < text.size();) {
        const auto newline = text.find(L'\n', start);
        const size_t end = newline == std::wstring::npos ? text.size() : newline + 1;
        auto line = text.substr(start, (newline == std::wstring::npos ? end : newline) - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        ++lineNumber;
        if (!line.empty() && line.front() != L'#') {
            Entry entry; entry.start = start; entry.end = end; entry.line = lineNumber;
            std::vector<std::wstring> fields;
            size_t begin = 0;
            for (;;) {
                const auto tab = line.find(L'\t', begin);
                fields.push_back(line.substr(begin, tab == std::wstring::npos ? tab : tab - begin));
                if (tab == std::wstring::npos) break;
                begin = tab + 1;
            }
            TextDictionary validation;
            entry.valid = validation.load(line) == 1;
            if (entry.valid) entry.rule = {fields[0], fields[1], fields[2], fields.size() == 4 ? fields[3] : L""};
            else entry.rule = {L"term", L"无效规则（第 " + std::to_wstring(lineNumber) + L" 行）", L"", L""};
            result.push_back(std::move(entry));
            if (result.size() > 2048) break;
        }
        start = end;
    }
    std::map<std::wstring, std::pair<std::wstring, bool>> targets;
    for (const auto& entry : result) if (entry.valid) {
        auto inserted = targets.emplace(fold(entry.rule.source), std::make_pair(entry.rule.target, false));
        if (!inserted.second && inserted.first->second.first != entry.rule.target) inserted.first->second.second = true;
    }
    for (auto& entry : result) if (entry.valid) entry.conflict = targets[fold(entry.rule.source)].second;
    return result;
}
struct Layer { std::wstring label, path, templatePath, help; };

class Manager {
public:
    Manager(HWND owner, HWND& active, const std::wstring& user, const std::wstring& package,
        const std::wstring& application, DictionaryScope initial) : owner_(owner), active_(active)
    {
        layers_.push_back({L"通用 · 所有应用", user + L"\\dictionary.tsv", package + L"\\dictionary.tsv",
            L"用于所有应用；同一误词如有应用专用规则，将优先使用应用专用规则。"});
        layers_.push_back({L"开发 · 开发词库启用时", user + L"\\dictionaries\\development.tsv", package + L"\\dictionary-development.tsv",
            L"用于自动识别的开发工具，或手动选择“通用＋开发”的应用；应用专用规则优先。"});
        std::vector<std::wstring> applications;
        const auto app = applicationName(application);
        if (!app.empty()) applications.push_back(app);
        WIN32_FIND_DATAW found{};
        HANDLE scan = FindFirstFileW((user + L"\\dictionaries\\apps\\*.tsv").c_str(), &found);
        if (scan != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring filename = found.cFileName;
                const auto name = filename.size() > 4 ? applicationName(filename.substr(0, filename.size() - 4)) : L"";
                if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !name.empty()) applications.push_back(name);
            } while (applications.size() < 2048 && FindNextFileW(scan, &found));
            FindClose(scan);
        }
        std::sort(applications.begin(), applications.end());
        applications.erase(std::unique(applications.begin(), applications.end()), applications.end());
        for (const auto& name : applications) {
            if (initial == DictionaryScope::Application && name == app) layer_ = static_cast<int>(layers_.size());
            layers_.push_back({L"仅此应用 · " + name, user + L"\\dictionaries\\apps\\" + name + L".tsv", L"",
                L"仅用于 " + name + L"；同源规则覆盖通用与开发词库。可在此管理以前记住的应用规则。"});
        }
        if (initial == DictionaryScope::Development) layer_ = 1;
    }
    ~Manager()
    {
        if (window_) DestroyWindow(window_);
        if (font_) DeleteObject(font_);
    }
    bool create(bool visible)
    {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&controls);
        WNDCLASSEXW type{sizeof(type)};
        type.lpfnWndProc = procedure; type.hInstance = instance;
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1)); type.lpszClassName = L"YanFlowDictionaryManager";
        RegisterClassExW(&type);
        RECT bounds{0, 0, 744, 650};
        const DWORD style = WS_CAPTION | WS_SYSMENU;
        AdjustWindowRectEx(&bounds, style, FALSE, WS_EX_DLGMODALFRAME);
        window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, type.lpszClassName, L"词库管理", style,
            CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top, owner_, nullptr, instance, this);
        if (!window_) return false;
        active_ = window_;
        font_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        label(L"纠正并记住：从刚才的识别错误添加规则，并复制本次正确文本。", 20, 16, 704, 24);
        label(L"词库管理：提前添加、查找、修改或删除同一套规则。保存后，下次识别生效。", 20, 42, 704, 24);
        label(L"适用范围", 20, 80, 88, 24);
        control(L"COMBOBOX", L"", Scope, 108, 76, 496, 280, CBS_DROPDOWNLIST);
        control(L"BUTTON", L"重新加载", Reload, 620, 76, 104, 30);
        label(L"", 20, 116, 704, 42, ScopeHelp);
        label(L"查找规则", 20, 168, 88, 24);
        control(L"EDIT", L"", Search, 108, 164, 616, 28, ES_AUTOHSCROLL, WS_EX_CLIENTEDGE);
        const HWND list = control(WC_LISTVIEWW, L"", Rules, 20, 204, 704, 186, LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t* titles[]{L"误识别词", L"正确词", L"上下文（同句）", L"匹配 / 状态"};
        const int widths[]{174, 174, 214, 120};
        for (int i = 0; i < 4; ++i) {
            LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH; column.pszText = const_cast<wchar_t*>(titles[i]); column.cx = widths[i];
            ListView_InsertColumn(list, i, &column);
        }
        label(L"误识别词（只填误词，如 note ZS）", 20, 402, 342, 24);
        label(L"正确词（例如 Node.js）", 382, 402, 342, 24);
        control(L"EDIT", L"", Source, 20, 430, 342, 28, ES_AUTOHSCROLL, WS_EX_CLIENTEDGE);
        control(L"EDIT", L"", Target, 382, 430, 342, 28, ES_AUTOHSCROLL, WS_EX_CLIENTEDGE);
        label(L"上下文", 20, 472, 88, 24);
        control(L"EDIT", L"", Context, 108, 468, 254, 28, ES_AUTOHSCROLL, WS_EX_CLIENTEDGE);
        label(L"可留空；中文单字必填。同句含此文字才替换。", 382, 468, 342, 36);
        control(L"BUTTON", L"新增", New, 20, 508, 88, 30);
        control(L"BUTTON", L"添加规则", Save, 120, 508, 116, 30, BS_DEFPUSHBUTTON);
        control(L"BUTTON", L"删除选中规则", Delete, 248, 508, 136, 30);
        label(L"试一句", 20, 552, 88, 24);
        control(L"EDIT", L"note ZS的最新版本是什么？", Test, 108, 548, 616, 28, ES_AUTOHSCROLL, WS_EX_CLIENTEDGE);
        label(L"", 20, 584, 704, 24, Preview);
        label(L"", 20, 618, 600, 28, Status);
        control(L"BUTTON", L"关闭", Close, 636, 614, 88, 30);
        for (int id : {Source, Target, Context}) SendDlgItemMessageW(window_, id, EM_SETLIMITTEXT, 128, 0);
        SendDlgItemMessageW(window_, Search, EM_SETLIMITTEXT, 128, 0);
        SendDlgItemMessageW(window_, Test, EM_SETLIMITTEXT, 4096, 0);
        SendDlgItemMessageW(window_, Source, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"例如：note ZS"));
        SendDlgItemMessageW(window_, Target, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"例如：Node.js"));
        SendDlgItemMessageW(window_, Context, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"例如：最新"));
        for (const auto& item : layers_) SendDlgItemMessageW(window_, Scope, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.label.c_str()));
        SendDlgItemMessageW(window_, Scope, CB_SETCURSEL, layer_, 0);
        ready_ = true; load();
        if (visible) { ShowWindow(window_, SW_SHOW); SetForegroundWindow(window_); SetFocus(GetDlgItem(window_, Source)); }
        return true;
    }
    void run()
    {
        if (!create(true)) { MessageBoxW(owner_, L"无法打开词库管理窗口。", L"词库管理", MB_OK | MB_ICONWARNING); return; }
        if (owner_) EnableWindow(owner_, FALSE);
        MSG message{};
        int received = 0;
        while (window_ && (received = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (!IsDialogMessageW(window_, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        if (owner_) { EnableWindow(owner_, TRUE); SetForegroundWindow(owner_); }
        if (received == 0) PostQuitMessage(static_cast<int>(message.wParam));
    }
    bool smoke();
private:
    HWND control(const wchar_t* type, const wchar_t* text, int id, int x, int y, int width, int height,
        DWORD style = 0, DWORD extended = 0)
    {
        HWND child = CreateWindowExW(extended, type, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, x, y, width, height,
            window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE); return child;
    }
    void label(const wchar_t* text, int x, int y, int width, int height, int id = 0)
    {
        HWND child = control(L"STATIC", text, id, x, y, width, height);
        SetWindowLongPtrW(child, GWL_STYLE, GetWindowLongPtrW(child, GWL_STYLE) & ~WS_TABSTOP);
    }
    void set(int id, const std::wstring& text) { SetDlgItemTextW(window_, id, text.c_str()); }
    void status(const std::wstring& text) { set(Status, text); }
    void load()
    {
        const auto& layer = layers_[layer_];
        set(ScopeHelp, layer.help);
        exists_ = GetFileAttributesW(layer.path.c_str()) != INVALID_FILE_ATTRIBUTES;
        original_.clear(); contents_.clear(); rows_.clear(); writable_ = false;
        const auto input = exists_ ? layer.path : layer.templatePath;
        if (!input.empty() && !readUtf8FileChecked(input, contents_)) {
            rebuild(); clear(); EnableWindow(GetDlgItem(window_, Save), FALSE);
            status(L"无法读取词库（编码、大小或权限错误）。已保留原文件；修复后可重新加载。"); return;
        }
        if (exists_) original_ = contents_;
        rows_ = entries(contents_);
        if (rows_.size() > 2048) {
            rebuild(); clear(); status(L"词库超过 2048 条上限，已保留原文件；请先清理超限内容后重新加载。"); return;
        }
        writable_ = true;
        rebuild(); clear();
        const auto invalid = std::count_if(rows_.begin(), rows_.end(), [](const Entry& entry) { return !entry.valid || entry.conflict; });
        status(std::to_wstring(rows_.size()) + L" 条规则" + (invalid ? L"；存在无效或冲突规则，选中后可修改或删除。" : L"；保存后下次识别生效。"));
    }
    void rebuild()
    {
        rebuilding_ = true;
        HWND list = GetDlgItem(window_, Rules); ListView_DeleteAllItems(list);
        const auto query = fold(controlText(window_, Search));
        for (size_t index = 0; index < rows_.size(); ++index) {
            auto& row = rows_[index];
            if (!query.empty() && fold(row.rule.source + L" " + row.rule.target + L" " + row.rule.context).find(query) == std::wstring::npos) continue;
            LVITEMW item{}; item.mask = LVIF_TEXT | LVIF_PARAM; item.iItem = ListView_GetItemCount(list);
            item.pszText = row.rule.source.data(); item.lParam = static_cast<LPARAM>(index);
            const int at = ListView_InsertItem(list, &item);
            ListView_SetItemText(list, at, 1, row.rule.target.data());
            ListView_SetItemText(list, at, 2, row.rule.context.data());
            std::wstring state = !row.valid ? L"无效 / 已停用" : row.conflict ? L"冲突 / 已停用" :
                row.rule.kind == L"case" ? L"规范大小写" : row.rule.kind == L"phonetic" ? L"中文同音词" : L"精确替换";
            ListView_SetItemText(list, at, 3, state.data());
        }
        rebuilding_ = false;
    }
    void clear()
    {
        selected_ = -1; set(Source, L""); set(Target, L""); set(Context, L"");
        ListView_SetItemState(GetDlgItem(window_, Rules), -1, 0, LVIS_SELECTED);
        set(Save, L"添加规则"); EnableWindow(GetDlgItem(window_, Save), writable_);
        EnableWindow(GetDlgItem(window_, Delete), FALSE); preview();
    }
    void select(int item)
    {
        LVITEMW data{}; data.iItem = item; data.mask = LVIF_PARAM;
        if (!ListView_GetItem(GetDlgItem(window_, Rules), &data) || data.lParam < 0 || static_cast<size_t>(data.lParam) >= rows_.size()) return;
        selected_ = static_cast<int>(data.lParam);
        const auto& row = rows_[selected_];
        set(Source, row.valid ? row.rule.source : L""); set(Target, row.rule.target); set(Context, row.rule.context);
        set(Save, L"保存修改"); EnableWindow(GetDlgItem(window_, Delete), writable_);
        if (!row.valid) status(L"此行不会生效。可填写新规则替换此行，或删除；其余内容保持原样。");
        preview();
    }
    DictionaryRule formRule() const
    {
        DictionaryRule rule{L"term", controlText(window_, Source), controlText(window_, Target), controlText(window_, Context)};
        if (fold(rule.source) == fold(rule.target)) rule.kind = L"case";
        else if (selected_ >= 0 && rows_[selected_].valid && rows_[selected_].rule.kind == L"phonetic") rule.kind = L"phonetic";
        return rule;
    }
    void preview()
    {
        if (!ready_) return;
        const auto rule = formRule(); std::wstring error;
        if (rule.source.empty() || rule.target.empty()) { set(Preview, L"本条预览：填写误识别词和正确词，再用一句话试试。仅预览替换规则。"); return; }
        if (!validateDictionaryRule(rule, error)) { set(Preview, L"本条预览：" + error); return; }
        TextDictionary dictionary; dictionary.load(serialize(rule));
        const auto test = controlText(window_, Test), result = dictionary.apply(test).text;
        set(Preview, result == test ? L"本条预览：未替换，请检查误词、同句上下文或代码保护。" : L"本条预览：" + result);
    }
    bool save(bool deleting = false)
    {
        if (!writable_ || (deleting && selected_ < 0)) return false;
        const auto rule = formRule(); std::wstring error;
        if (!deleting) {
            if (!validateDictionaryRule(rule, error)) { status(error); return false; }
            for (size_t index = 0; index < rows_.size(); ++index) {
                const auto& row = rows_[index];
                if (static_cast<int>(index) == selected_ || !row.valid || fold(row.rule.source) != fold(rule.source)) continue;
                const bool sameMatch = row.rule.source == rule.source || row.rule.kind == L"case" || rule.kind == L"case";
                if (row.rule.target != rule.target || (sameMatch && row.rule.context == rule.context)) {
                    status(L"此误词已有重复或不同目标的规则，请查找并修改已有规则，避免冲突。"); return false;
                }
            }
        }
        auto updated = contents_;
        const auto replacement = deleting ? L"" : serialize(rule) + L"\n";
        if (selected_ >= 0) {
            const auto& row = rows_[selected_]; updated.replace(row.start, row.end - row.start, replacement);
        } else {
            if (!updated.empty() && updated.back() != L'\n') updated += L"\n";
            updated += replacement;
        }
        if (!saveDictionaryDocument(layers_[layer_].path, exists_, original_, updated, error)) { status(error); return false; }
        load(); status(deleting ? L"已删除；下次识别生效。" : L"已保存；下次识别生效。可点“新增”继续添加其他误词。"); return true;
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<Manager*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = reinterpret_cast<Manager*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wParam, lParam);
        if (message == DM_GETDEFID) return MAKELONG(Save, DC_HASDEFID);
        if (message == WM_COMMAND && self->ready_) {
            const int id = LOWORD(wParam), event = HIWORD(wParam);
            if (id == Close || id == IDCANCEL) DestroyWindow(window);
            else if (id == Scope && event == CBN_SELCHANGE) {
                self->layer_ = static_cast<int>(SendDlgItemMessageW(window, Scope, CB_GETCURSEL, 0, 0)); self->load();
            } else if (id == Reload) self->load();
            else if (id == Search && event == EN_CHANGE) { self->rebuild(); self->clear(); }
            else if ((id == Source || id == Target || id == Context || id == Test) && event == EN_CHANGE) self->preview();
            else if (id == New) { self->clear(); SetFocus(GetDlgItem(window, Source)); }
            else if (id == Save) self->save();
            else if (id == Delete) self->save(true);
            return 0;
        }
        if (message == WM_NOTIFY && self->ready_ && !self->rebuilding_) {
            const auto* change = reinterpret_cast<NMLISTVIEW*>(lParam);
            if (change->hdr.idFrom == Rules && change->hdr.code == LVN_ITEMCHANGED && (change->uChanged & LVIF_STATE) &&
                (change->uNewState & LVIS_SELECTED) && !(change->uOldState & LVIS_SELECTED)) self->select(change->iItem);
        }
        if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
        if (message == WM_DESTROY) { self->window_ = nullptr; self->active_ = nullptr; return 0; }
        return DefWindowProcW(window, message, wParam, lParam);
    }
    HWND owner_ = nullptr, window_ = nullptr;
    HWND& active_;
    HFONT font_ = nullptr;
    std::vector<Layer> layers_;
    std::vector<Entry> rows_;
    int layer_ = 0, selected_ = -1;
    bool ready_ = false, rebuilding_ = false, exists_ = false, writable_ = false;
    std::wstring original_, contents_;
};

bool Manager::smoke()
{
    if (!create(false)) return false;
    const auto generalPath = layers_[0].path;
    // Start from the packaged template, preserving comments and even unsupported lines.
    if (rows_.size() != 2 || rows_[1].valid || exists_) return false;
    set(Source, L"note ZS"); set(Target, L"Node.js"); set(Context, L"最新");
    if (controlText(window_, Preview) != L"本条预览：Node.js的最新版本是什么？") return false;
    set(Test, L"`note ZS`最新");
    if (controlText(window_, Preview).find(L"未替换") == std::wstring::npos) return false;
    set(Test, L"note ZS的最新版本是什么？");
    if (!save()) return false;
    TextDictionary dictionary; dictionary.load(readUtf8File(generalPath));
    if (dictionary.apply(L"note ZS的最新版本是什么？").text != L"Node.js的最新版本是什么？" ||
        dictionary.apply(L"note ZS是什么？").text != L"note ZS是什么？") return false;
    set(Search, L"note");
    if (ListView_GetItemCount(GetDlgItem(window_, Rules)) != 1) return false;
    ListView_SetItemState(GetDlgItem(window_, Rules), 0, LVIS_SELECTED, LVIS_SELECTED);
    if (selected_ != 2 || controlText(window_, Target) != L"Node.js") return false;
    set(Source, L"noZS");
    if (!save() || readUtf8File(generalPath).find(L"term\tnoZS\tNode.js\t最新\n") == std::wstring::npos) return false;
    set(Search, L"");
    const auto before = readUtf8File(generalPath);
    set(Source, L"noZS"); set(Target, L"wrong"); set(Context, L"最新");
    if (save() || readUtf8File(generalPath) != before) return false;
    clear(); set(Source, L"的"); set(Target, L"地");
    if (save() || readUtf8File(generalPath) != before) return false;
    clear(); set(Source, L"extra"); set(Target, L"Extra word");
    if (!writeUtf8File(generalPath, before + L"# external update\n") || save() ||
        readUtf8File(generalPath) != before + L"# external update\n") return false;
    load();
    ListView_SetItemState(GetDlgItem(window_, Rules), 2, LVIS_SELECTED, LVIS_SELECTED);
    if (!save(true)) return false;
    const auto after = readUtf8File(generalPath);
    if (after.find(L"# retained comment\r\n") == std::wstring::npos || after.find(L"unsupported line\r\n") == std::wstring::npos ||
        after.find(L"# external update\n") == std::wstring::npos || after.find(L"noZS") != std::wstring::npos) return false;
    layer_ = 2; SendDlgItemMessageW(window_, Scope, CB_SETCURSEL, layer_, 0); load();
    set(Source, L"note ZS"); set(Target, L"Node.js"); set(Context, L"最新");
    if (!save() || readUtf8File(generalPath) != after || !readUtf8File(layers_[2].path).size()) return false;
    HANDLE lock = CreateFileW((layers_[2].path + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (lock == INVALID_HANDLE_VALUE) return false;
    set(Source, L"noZS"); set(Target, L"Node.js"); set(Context, L"最新");
    const auto appBefore = readUtf8File(layers_[2].path);
    const bool lockSafe = !save() && readUtf8File(layers_[2].path) == appBefore;
    CloseHandle(lock);
    if (!lockSafe || !save()) return false;
    set(Source, L"note zs"); set(Target, L"Node.js"); set(Context, L"最新");
    if (!save()) return false;
    dictionary.load(readUtf8File(layers_[2].path));
    if (dictionary.apply(L"note ZS的最新版本是什么？").text != L"Node.js的最新版本是什么？" ||
        dictionary.apply(L"note zs的最新版本是什么？").text != L"Node.js的最新版本是什么？") return false;
    while (!rows_.empty()) {
        ListView_SetItemState(GetDlgItem(window_, Rules), 0, LVIS_SELECTED, LVIS_SELECTED);
        if (!save(true)) return false;
    }
    if (!exists_ || !readUtf8File(layers_[2].path).empty()) return false;
    HWND otherActive = nullptr;
    Manager reopened(nullptr, otherActive, generalPath.substr(0, generalPath.find_last_of(L'\\')),
        layers_[0].templatePath.substr(0, layers_[0].templatePath.find_last_of(L'\\')), L"", DictionaryScope::General);
    if (!reopened.create(false) || reopened.layers_.size() != 3 || reopened.layers_[2].path != layers_[2].path) return false;
    layer_ = 1; load(); // Development edits must not modify the packaged template.
    set(Source, L"西加加"); set(Target, L"C++");
    if (!save() || readUtf8File(layers_[1].templatePath) != L"case\tcmake\tCMake\n") return false;
    std::wstring oversized;
    for (int i = 0; i < 2049; ++i) oversized += L"term\talias" + std::to_wstring(i) + L"\tcanonical\n";
    if (!writeUtf8File(generalPath, oversized)) return false;
    layer_ = 0; load();
    if (writable_ || save() || readUtf8File(generalPath) != oversized) return false;
    // Invalid UTF-8 is never represented as an empty writable dictionary.
    HANDLE corrupt = CreateFileW(generalPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (corrupt == INVALID_HANDLE_VALUE) return false;
    const BYTE bad[]{0xff, 0xfe, 0xff}; DWORD written = 0;
    const bool corrupted = WriteFile(corrupt, bad, sizeof(bad), &written, nullptr) && written == sizeof(bad); CloseHandle(corrupt);
    layer_ = 0; load();
    if (!corrupted || writable_ || IsWindowEnabled(GetDlgItem(window_, Save)) || save()) return false;
    HANDLE preserved = CreateFileW(generalPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    BYTE afterBytes[3]{}; DWORD read = 0;
    const bool unchanged = preserved != INVALID_HANDLE_VALUE && ReadFile(preserved, afterBytes, sizeof(afterBytes), &read, nullptr) &&
        read == sizeof(afterBytes) && std::equal(std::begin(bad), std::end(bad), std::begin(afterBytes));
    if (preserved != INVALID_HANDLE_VALUE) CloseHandle(preserved);
    if (!unchanged) return false;
    return true;
}
}

void showDictionaryManager(HWND owner, HWND& active, const std::wstring& user, const std::wstring& package,
    const std::wstring& application, DictionaryScope scope)
{
    if (active) { SetForegroundWindow(active); return; }
    Manager manager(owner, active, user, package, application, scope); manager.run();
}
int runDictionaryManagerSmoke()
{
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temporary)) return 160;
    const auto root = std::wstring(temporary) + L"YanFlow-manager-" + std::to_wstring(GetCurrentProcessId());
    const auto user = root + L"\\user", package = root + L"\\package";
    CreateDirectoryW(root.c_str(), nullptr); CreateDirectoryW(user.c_str(), nullptr); CreateDirectoryW(package.c_str(), nullptr);
    bool passed = writeUtf8File(package + L"\\dictionary.tsv", L"# retained comment\r\ncase\topenai\tOpenAI\r\nunsupported line\r\n") &&
        writeUtf8File(package + L"\\dictionary-development.tsv", L"case\tcmake\tCMake\n");
    HWND active = nullptr;
    { Manager manager(nullptr, active, user, package, L"notepad.exe", DictionaryScope::General); passed = passed && manager.smoke(); }
    const std::wstring files[]{user + L"\\dictionary.tsv", user + L"\\dictionaries\\development.tsv", user + L"\\dictionaries\\apps\\notepad.exe.tsv",
        package + L"\\dictionary.tsv", package + L"\\dictionary-development.tsv"};
    for (const auto& file : files) { DeleteFileW(file.c_str()); DeleteFileW((file + L".partial").c_str()); }
    RemoveDirectoryW((user + L"\\dictionaries\\apps").c_str()); RemoveDirectoryW((user + L"\\dictionaries").c_str());
    RemoveDirectoryW(user.c_str()); RemoveDirectoryW(package.c_str()); RemoveDirectoryW(root.c_str());
    return passed && !active ? 0 : 161;
}
}
