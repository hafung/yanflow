#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "text-pipeline.h"
#include "text-correction.h"
#include "csc-protocol.h"
#include <algorithm>
#include <cstdio>
#include <vector>

namespace yanflow {
bool readUtf8FileChecked(const std::wstring& path, std::wstring& text)
{
    text.clear();
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") || !file) return false;
    _fseeki64(file, 0, SEEK_END);
    const auto length = _ftelli64(file);
    _fseeki64(file, 0, SEEK_SET);
    if (length < 0 || length > 1024 * 1024) { fclose(file); return false; }
    if (length == 0) { fclose(file); return true; }
    std::string bytes(static_cast<size_t>(length), '\0');
    const bool ok = fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    fclose(file);
    if (!ok) return false;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (count <= 0) return false;
    text.assign(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), text.data(), count);
    if (!text.empty() && text.front() == 0xfeff) text.erase(text.begin());
    return true;
}
std::wstring readUtf8File(const std::wstring& path) { std::wstring text; readUtf8FileChecked(path, text); return text; }
bool writeUtf8File(const std::wstring& path, const std::wstring& text)
{
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!text.empty() && count <= 0) return false;
    std::string bytes(count, '\0');
    if (count) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), bytes.data(), count, nullptr, nullptr);
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") || !file) return false;
    const bool ok = bytes.empty() || fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return fclose(file) == 0 && ok;
}

struct TextPipeline::State {
    HANDLE process = nullptr, input = nullptr, output = nullptr, diagnostic = nullptr;
    bool failed = false;
    std::wstring error;
    ~State() { stop(); }
    void stop()
    {
        if (input) CloseHandle(input);
        if (output) CloseHandle(output);
        input = output = nullptr;
        if (process) {
            if (WaitForSingleObject(process, 300) == WAIT_TIMEOUT) {
                TerminateProcess(process, 1); WaitForSingleObject(process, 1000);
            }
            CloseHandle(process); process = nullptr;
        }
        if (diagnostic) {
            if (!error.empty()) {
                SetFilePointer(diagnostic, 0, nullptr, FILE_BEGIN);
                char bytes[2048]{}; DWORD received = 0;
                if (ReadFile(diagnostic, bytes, sizeof(bytes), &received, nullptr) && received) {
                    const int size = MultiByteToWideChar(CP_UTF8, 0, bytes, received, nullptr, 0);
                    std::wstring details(size, L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, bytes, received, details.data(), size);
                    error += L"\n" + details;
                }
            }
            CloseHandle(diagnostic); diagnostic = nullptr;
        }
    }
    bool read(void* data, size_t bytes, uint64_t deadline)
    {
        auto* cursor = static_cast<unsigned char*>(data);
        while (bytes) {
            DWORD available = 0, received = 0;
            if (!PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr)) return false;
            if (!available) {
                if (GetTickCount64() >= deadline || WaitForSingleObject(process, 0) != WAIT_TIMEOUT) return false;
                Sleep(2); continue;
            }
            if (!ReadFile(output, cursor, static_cast<DWORD>(std::min<size_t>(bytes, available)), &received, nullptr) || !received) return false;
            cursor += received; bytes -= received;
        }
        return true;
    }
    bool start(const std::wstring& root)
    {
        const auto directory = root + L"\\csc";
        const auto executable = directory + L"\\yanflow-csc-worker.exe";
        for (const auto* name : {L"yanflow-csc-worker.exe", L"yanflow-onnxruntime.dll", L"model.onnx", L"vocab.txt"}) {
            if (GetFileAttributesW((directory + L"\\" + name).c_str()) == INVALID_FILE_ATTRIBUTES) {
                error = L"安装包缺少 MacBERT 文件：" + directory + L"\\" + name;
                return false;
            }
        }
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        wchar_t temporary[MAX_PATH]{}, log[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, temporary) && GetTempFileNameW(temporary, L"yfc", 0, log)) {
            diagnostic = CreateFileW(log, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                &security, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (diagnostic == INVALID_HANDLE_VALUE) { diagnostic = nullptr; DeleteFileW(log); }
        }
        if (!diagnostic) {
            error = L"无法创建 MacBERT 诊断文件（Windows 错误 " + std::to_wstring(GetLastError()) + L"）。";
            return false;
        }
        HANDLE childInput = nullptr, childOutput = nullptr;
        if (!CreatePipe(&childInput, &input, &security, 16384) || !CreatePipe(&output, &childOutput, &security, 16384)) {
            if (childInput) CloseHandle(childInput);
            if (childOutput) CloseHandle(childOutput);
            error = L"无法创建 MacBERT 通信管道。"; stop(); return false;
        }
        SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = childInput; startup.hStdOutput = childOutput; startup.hStdError = diagnostic;
        PROCESS_INFORMATION info{};
        std::wstring command = L"\"" + executable + L"\"";
        const BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &info);
        const DWORD launchError = created ? 0 : GetLastError();
        CloseHandle(childInput); CloseHandle(childOutput);
        if (!created) { error = L"MacBERT 启动失败（Windows 错误 " + std::to_wstring(launchError) + L"）。"; stop(); return false; }
        CloseHandle(info.hThread); process = info.hProcess;
        uint32_t ready[2]{};
        if (!read(ready, sizeof(ready), GetTickCount64() + 30000) || ready[0] != kCscReady || ready[1] != kCscVersion) {
            DWORD exit = STILL_ACTIVE; GetExitCodeProcess(process, &exit);
            error = exit == STILL_ACTIVE ? L"MacBERT 初始化超时或协议不匹配。"
                : L"MacBERT 初始化失败（退出码 " + std::to_wstring(exit) + L"）。";
            stop(); return false;
        }
        return true;
    }
    bool propose(const std::wstring& text, const std::wstring& root, std::vector<CharacterProposal>& proposals)
    {
        if (!process && !start(root)) return false;
        const uint32_t length = static_cast<uint32_t>(text.size());
        std::vector<unsigned char> request(sizeof(length) + length * sizeof(wchar_t));
        memcpy(request.data(), &length, sizeof(length));
        memcpy(request.data() + sizeof(length), text.data(), length * sizeof(wchar_t));
        DWORD written = 0;
        // One bounded request fits in the empty pipe; no unbounded blocking write.
        if (!WriteFile(input, request.data(), static_cast<DWORD>(request.size()), &written, nullptr) || written != request.size()) return false;
        const auto deadline = GetTickCount64() + 10000;
        uint32_t count = 0;
        if (!read(&count, sizeof(count), deadline) || count > kCscMaximumCharacters) return false;
        std::vector<CscProposal> wire(count);
        if (!read(wire.data(), wire.size() * sizeof(CscProposal), deadline)) return false;
        for (const auto& item : wire) {
            if (item.original > 0xffff || item.replacement > 0xffff || item.offset >= text.size()) return false;
            proposals.push_back({item.offset, static_cast<wchar_t>(item.original), static_cast<wchar_t>(item.replacement),
                item.confidence, item.originalConfidence, item.runnerUpConfidence});
        }
        return true;
    }
};
TextPipeline::TextPipeline() : state_(std::make_unique<State>()) {}
TextPipeline::~TextPipeline() = default;
bool TextPipeline::cscFailed() const { return state_->failed; }
const std::wstring& TextPipeline::cscError() const { return state_->error; }
void TextPipeline::resetCsc() { state_->stop(); state_->failed = false; state_->error.clear(); }
bool TextPipeline::warmupCsc(const std::wstring& root) {
    if (state_->failed) return false;
    if (state_->process || state_->start(root)) return true;
    state_->failed = true; return false;
}
std::wstring TextPipeline::correct(const std::wstring& raw, const std::wstring& root,
    const std::wstring& dictionaryPath, bool enableCsc, const std::vector<std::wstring>& overlays)
{
    TextDictionary dictionary;
    dictionary.load(readUtf8File(dictionaryPath));
    for (const auto& path : overlays) dictionary.overlay(readUtf8File(path));
    const auto first = dictionary.apply(raw);
    if (!enableCsc) { resetCsc(); return first.text; }
    if (state_->failed || first.text.empty() || first.text.size() > kCscMaximumCharacters) return first.text;
    std::vector<CharacterProposal> proposals;
    if (!state_->propose(first.text, root, proposals)) {
        if (state_->error.empty()) state_->error = L"MacBERT 纠错超时、进程退出或响应无效。";
        state_->failed = true; state_->stop(); return first.text;
    }
    return applyChineseCorrections(first, proposals);
}
int correctTextFile(const std::wstring& input, const std::wstring& output, bool enableCsc)
{
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) return 2;
    std::wstring root(path, length); root.resize(root.find_last_of(L"\\/"));
    const auto raw = readUtf8File(input);
    if (raw.empty()) return 30;
    TextPipeline pipeline;
    const auto final = pipeline.correct(raw, root, root + L"\\dictionary.tsv", enableCsc);
    if (!writeUtf8File(output, final)) return 31;
    if (pipeline.cscFailed()) writeUtf8File(output + L".diagnostic.txt", pipeline.cscError());
    else DeleteFileW((output + L".diagnostic.txt").c_str());
    return pipeline.cscFailed() ? 32 : 0;
}
int runCscPipelineSmoke()
{
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) return 152;
    std::wstring root(path, length); root.resize(root.find_last_of(L"\\/"));
    TextPipeline pipeline;
    if (!pipeline.warmupCsc(root)) return 153;
    const auto input = L"OpenAI ASR 123 `新情` note ZS";
    for (int i = 0; i < 2; ++i) {
        if (pipeline.correct(input, root, root + L"\\dictionary.tsv", true) != input ||
            pipeline.cscFailed() || !pipeline.cscError().empty()) return 154;
    }
    pipeline.resetCsc();
    // A nonexistent nested package tests error reporting and sticky fallback.
    const auto missing = root + L"\\missing-csc-smoke";
    if (pipeline.warmupCsc(missing) || !pipeline.cscFailed() || pipeline.cscError().find(L"yanflow-csc-worker.exe") == std::wstring::npos) return 155;
    if (pipeline.correct(input, missing, root + L"\\dictionary.tsv", true) != input) return 156;
    pipeline.resetCsc();
    return pipeline.warmupCsc(root) && !pipeline.cscFailed() && pipeline.cscError().empty() ? 0 : 157;
}
} // namespace yanflow
