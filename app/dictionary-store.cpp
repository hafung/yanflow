#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "dictionary-store.h"
#include "text-pipeline.h"
#include <algorithm>
#include <cstring>
#include <sstream>

namespace yanflow {
namespace {
std::wstring fold(std::wstring value)
{
    for (auto& c : value) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    return value;
}
std::wstring ruleLine(const DictionaryRule& rule)
{
    return rule.kind + L"\t" + rule.source + L"\t" + rule.target + (rule.context.empty() ? L"" : L"\t" + rule.context);
}
}
std::wstring applicationName(const std::wstring& path)
{
    const auto slash = path.find_last_of(L"\\/");
    auto name = fold(path.substr(slash == std::wstring::npos ? 0 : slash + 1));
    if (name.size() < 5 || name.size() > 120 || name.substr(name.size() - 4) != L".exe" ||
        name.find_first_of(L"<>:\"/\\|?*=;[]\t\r\n") != std::wstring::npos ||
        std::any_of(name.begin(), name.end(), [](wchar_t c) { return c < 0x20; })) return L"";
    return name;
}
bool developmentApplication(const std::wstring& app)
{
    const auto name = applicationName(app);
    for (const auto* known : {L"code.exe", L"code - insiders.exe", L"vscodium.exe", L"devenv.exe", L"windowsterminal.exe",
        L"powershell.exe", L"pwsh.exe", L"idea64.exe", L"rider64.exe", L"pycharm64.exe", L"webstorm64.exe", L"clion64.exe"})
        if (name == known) return true;
    return false;
}
std::vector<std::wstring> dictionaryOverlays(const std::wstring& user, const std::wstring& package,
    const std::wstring& app, DictionaryProfile profile)
{
    std::vector<std::wstring> paths;
    const auto name = applicationName(app);
    if (profile == DictionaryProfile::Development || (profile == DictionaryProfile::Automatic && developmentApplication(name))) {
        const auto personal = user + L"\\dictionaries\\development.tsv";
        paths.push_back(GetFileAttributesW(personal.c_str()) != INVALID_FILE_ATTRIBUTES
            ? personal : package + L"\\dictionary-development.tsv");
    }
    if (!name.empty()) paths.push_back(user + L"\\dictionaries\\apps\\" + name + L".tsv");
    return paths;
}
bool rememberDictionaryRule(const std::wstring& path, const DictionaryRule& rule, std::wstring& error)
{
    error.clear();
    if (rule.source == rule.target || rule.source.find_first_of(L"\t\r\n") != std::wstring::npos ||
        rule.target.find_first_of(L"\t\r\n") != std::wstring::npos || rule.context.find_first_of(L"\t\r\n") != std::wstring::npos ||
        (rule.source.size() == 1 && isHan(rule.source[0]) && rule.context.empty())) {
        error = L"请填写不同的原词和正确词；中文单字替换必须填写上下文。"; return false;
    }
    TextDictionary validation;
    if (validation.load(ruleLine(rule)) != 1) { error = L"词典规则无效，请检查长度和上下文。"; return false; }
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) { error = L"词典路径无效。"; return false; }
    const auto directory = path.substr(0, slash);
    // The caller chooses paths inside the existing user directory; create only
    // the known dictionary directories, never follow a user-entered filename.
    const auto parent = directory.find_last_of(L"\\/");
    if (parent != std::wstring::npos) CreateDirectoryW(directory.substr(0, parent).c_str(), nullptr);
    CreateDirectoryW(directory.c_str(), nullptr);
    HANDLE lock = CreateFileW((path + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (lock == INVALID_HANDLE_VALUE) { error = L"词典正在被其他进程编辑，或目录不可写，请稍后重试。"; return false; }
    std::wstring contents;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && !readUtf8FileChecked(path, contents)) {
        CloseHandle(lock); error = L"词典不是有效 UTF-8，或超过大小上限；已保留原文件。"; return false;
    }
    std::wistringstream stream(contents);
    std::wstring line, updated;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        const auto first = line.find(L'\t');
        const auto second = first == std::wstring::npos ? first : line.find(L'\t', first + 1);
        if (!line.empty() && line.front() != L'#' && first != std::wstring::npos && second != std::wstring::npos &&
            fold(line.substr(first + 1, second - first - 1)) == fold(rule.source)) continue;
        updated += line + L"\n";
    }
    if (updated.empty()) updated = L"# YanFlow user-confirmed corrections; UTF-8 TAB-separated\n";
    updated += ruleLine(rule) + L"\n";
    size_t entries = 0;
    std::wistringstream countStream(updated);
    while (std::getline(countStream, line)) if (!line.empty() && line.front() != L'#') ++entries;
    if (entries > 2048) {
        CloseHandle(lock); error = L"词典已达到 2048 条上限，请先清理旧规则。"; return false;
    }
    const int utf8Bytes = updated.size() <= 1024 * 1024 ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        updated.data(), static_cast<int>(updated.size()), nullptr, 0, nullptr, nullptr) : 0;
    if (utf8Bytes <= 0 || utf8Bytes > 1024 * 1024) {
        CloseHandle(lock); error = L"词典过大，已保留原文件。"; return false;
    }
    const auto temporary = path + L".partial";
    const bool saved = writeUtf8File(temporary, updated) && MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!saved) { DeleteFileW(temporary.c_str()); error = L"保存词典失败，已保留原文件，请检查写入权限。"; }
    CloseHandle(lock);
    return saved;
}
int runDictionaryStoreSmoke()
{
    if (applicationName(L"C:\\Apps\\Code.EXE") != L"code.exe" || !applicationName(L"C:\\bad=app.exe").empty() ||
        !developmentApplication(L"CODE.EXE") || developmentApplication(L"notepad.exe")) return 120;
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temporary)) return 121;
    const auto root = std::wstring(temporary) + L"YanFlow-dictionary-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(root.c_str(), nullptr);
    const auto path = root + L"\\dictionary.tsv";
    std::wstring error;
    const DictionaryRule first{L"term", L"热此", L"热词", L"语音识别"};
    const auto cleanup = [&] { DeleteFileW(path.c_str()); DeleteFileW((path + L".partial").c_str()); RemoveDirectoryW(root.c_str()); };
    if (!rememberDictionaryRule(path, first, error)) { cleanup(); return 122; }
    TextDictionary dictionary;
    dictionary.load(readUtf8File(path));
    if (dictionary.apply(L"语音识别支持热此").text != L"语音识别支持热词") { cleanup(); return 123; }
    if (!rememberDictionaryRule(path, {L"term", L"热此", L"专有热词", L"语音识别"}, error)) { cleanup(); return 124; }
    dictionary.load(readUtf8File(path));
    if (dictionary.apply(L"语音识别支持热此").text != L"语音识别支持专有热词" ||
        rememberDictionaryRule(path, {L"term", L"的", L"地", L""}, error)) { cleanup(); return 125; }
    dictionary.load(L"term\t热此\t通用热词\n");
    dictionary.overlay(readUtf8File(path));
    if (dictionary.apply(L"语音识别支持热此").text != L"语音识别支持专有热词") { cleanup(); return 126; }
    const auto general = dictionaryOverlays(root, root, L"notepad.exe", DictionaryProfile::Automatic);
    const auto developer = dictionaryOverlays(root, root, L"code.exe", DictionaryProfile::Automatic);
    if (general.size() != 1 || developer.size() != 2 || general[0] == developer[0]) { cleanup(); return 127; }
    if (!writeUtf8File(path, L"old\n") ||
        rememberDictionaryRule(path, {L"term", L"wrong\tcolumn", L"correct", L""}, error) || readUtf8File(path) != L"old\n") { cleanup(); return 128; }
    HANDLE corrupted = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (corrupted == INVALID_HANDLE_VALUE) { cleanup(); return 129; }
    const unsigned char bad[]{0xff, 0xfe, 0xff}; DWORD written = 0;
    const bool corruptedWritten = WriteFile(corrupted, bad, sizeof(bad), &written, nullptr) && written == sizeof(bad);
    CloseHandle(corrupted);
    std::wstring invalid;
    if (!corruptedWritten || rememberDictionaryRule(path, first, error) || readUtf8FileChecked(path, invalid)) { cleanup(); return 129; }
    HANDLE preserved = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    unsigned char after[3]{}; DWORD read = 0;
    const bool unchanged = preserved != INVALID_HANDLE_VALUE && ReadFile(preserved, after, sizeof(after), &read, nullptr) &&
        read == sizeof(after) && memcmp(after, bad, sizeof(after)) == 0;
    if (preserved != INVALID_HANDLE_VALUE) CloseHandle(preserved);
    if (!unchanged) { cleanup(); return 129; }
    cleanup(); return 0;
}
}
