#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <phpx.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <ole2.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <uiautomation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cwctype>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uiautomationcore.lib")

using namespace php;

namespace {

constexpr UINT kMessageResult = WM_APP + 1;
constexpr UINT kMessageStatus = WM_APP + 2;
constexpr UINT_PTR kTimerSmoke = 1;
constexpr UINT_PTR kTimerPipelineStop = 2;
constexpr UINT_PTR kTimerAnimation = 3;
constexpr UINT_PTR kTimerCopyFeedback = 4;
constexpr int kHotkeyStart = 1;
constexpr int kHotkeyStop = 2;
constexpr int kHotkeyVisibility = 3;
constexpr int kHotkeyCopy = 4;
constexpr int kCommandToggle = 100;
constexpr int kCommandCopy = 101;
constexpr int kCommandWrite = 102;
constexpr int kCommandSettings = 103;
constexpr int kCommandExit = 104;
constexpr int kSettingsStart = 200;
constexpr int kSettingsStop = 201;
constexpr int kSettingsSave = 202;
constexpr int kSettingsCancel = 203;
constexpr int kSampleRate = 16000;
constexpr int kFramesPerBuffer = 320;
constexpr int kAudioBuffers = 6;
constexpr size_t kRingSamples = kSampleRate * 30;
constexpr size_t kMaximumUtteranceSamples = kSampleRate * 8;
constexpr int kCollapsedSize = 72;
constexpr int kBubbleWidth = 520;
constexpr int kBubbleHeight = 136;

struct HotkeyBinding {
    UINT modifiers = 0;
    UINT virtualKey = 0;
};

bool sameHotkey(const HotkeyBinding& left, const HotkeyBinding& right)
{
    return left.modifiers == right.modifiers && left.virtualKey == right.virtualKey;
}

std::wstring hotkeyLabel(const HotkeyBinding& binding)
{
    std::wstring label;
    if ((binding.modifiers & MOD_CONTROL) != 0) label += L"Ctrl+";
    if ((binding.modifiers & MOD_ALT) != 0) label += L"Alt+";
    if ((binding.modifiers & MOD_SHIFT) != 0) label += L"Shift+";
    if ((binding.modifiers & MOD_WIN) != 0) label += L"Win+";
    wchar_t name[64] = {};
    UINT scanCode = MapVirtualKeyW(binding.virtualKey, MAPVK_VK_TO_VSC) << 16;
    if (binding.virtualKey == VK_LEFT || binding.virtualKey == VK_UP || binding.virtualKey == VK_RIGHT ||
        binding.virtualKey == VK_DOWN || binding.virtualKey == VK_PRIOR || binding.virtualKey == VK_NEXT ||
        binding.virtualKey == VK_END || binding.virtualKey == VK_HOME || binding.virtualKey == VK_INSERT ||
        binding.virtualKey == VK_DELETE || binding.virtualKey == VK_DIVIDE || binding.virtualKey == VK_NUMLOCK) {
        scanCode |= 1u << 24;
    }
    if (GetKeyNameTextW(static_cast<LONG>(scanCode), name, static_cast<int>(std::size(name))) > 0) {
        label += name;
    } else {
        label += L"Key";
    }
    return label;
}

enum class ListeningState {
    Idle,
    Listening,
    Recognizing,
    Error
};

std::wstring executableDirectory()
{
    std::array<wchar_t, 32768> path = {};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    std::wstring value(path.data(), length);
    const size_t slash = value.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : value.substr(0, slash);
}

std::wstring joinPath(const std::wstring& left, const std::wstring& right)
{
    return left + L"\\" + right;
}

std::wstring utf8ToWide(const std::string& value)
{
    if (value.empty()) {
        return L"";
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return L"";
    }
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string wideToUtf8(const std::wstring& value)
{
    if (value.empty()) return "";
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring quoteArgument(const std::wstring& value)
{
    std::wstring result = L"\"";
    unsigned int slashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') {
            slashes++;
            continue;
        }
        if (character == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

bool readPcm16Wave(const std::wstring& path, std::vector<int16_t>& samples)
{
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) return false;
    std::array<uint8_t, 44> header = {};
    const bool headerOk = fread(header.data(), 1, header.size(), file) == header.size() &&
        memcmp(header.data(), "RIFF", 4) == 0 && memcmp(header.data() + 8, "WAVE", 4) == 0 &&
        memcmp(header.data() + 12, "fmt ", 4) == 0 && memcmp(header.data() + 36, "data", 4) == 0;
    uint16_t format = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bits = 0;
    uint32_t dataBytes = 0;
    if (headerOk) {
        memcpy(&format, header.data() + 20, sizeof(format));
        memcpy(&channels, header.data() + 22, sizeof(channels));
        memcpy(&sampleRate, header.data() + 24, sizeof(sampleRate));
        memcpy(&bits, header.data() + 34, sizeof(bits));
        memcpy(&dataBytes, header.data() + 40, sizeof(dataBytes));
    }
    if (!headerOk || format != 1 || channels != 1 || sampleRate != kSampleRate || bits != 16 ||
        dataBytes > kSampleRate * 120 || dataBytes % sizeof(int16_t) != 0) {
        fclose(file);
        return false;
    }
    samples.resize(dataBytes / sizeof(int16_t));
    const bool read = samples.empty() || fread(samples.data(), sizeof(int16_t), samples.size(), file) == samples.size();
    fclose(file);
    return read;
}

class AudioCapture {
public:
    AudioCapture() : ring_(kRingSamples, 0)
    {
        for (size_t index = 0; index < buffers_.size(); index++) {
            headers_[index] = {};
            headers_[index].lpData = reinterpret_cast<LPSTR>(buffers_[index].data());
            headers_[index].dwBufferLength = static_cast<DWORD>(buffers_[index].size() * sizeof(int16_t));
        }
    }

    ~AudioCapture()
    {
        stop();
    }

    bool start()
    {
        if (running_.load(std::memory_order_acquire)) {
            return true;
        }
        if (waveInGetNumDevs() == 0) {
            lastOpenResult_ = MMSYSERR_NODRIVER;
            return false;
        }
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
        captured_.store(0, std::memory_order_relaxed);
        dropped_.store(0, std::memory_order_relaxed);
        WAVEFORMATEX format = {};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = kSampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 2;
        format.nAvgBytesPerSec = kSampleRate * 2;
        lastOpenResult_ = waveInOpen(&handle_, WAVE_MAPPER, &format,
            reinterpret_cast<DWORD_PTR>(&AudioCapture::callback), reinterpret_cast<DWORD_PTR>(this),
            CALLBACK_FUNCTION);
        if (lastOpenResult_ != MMSYSERR_NOERROR) {
            handle_ = nullptr;
            return false;
        }
        for (WAVEHDR& header : headers_) {
            header.dwFlags = 0;
            header.dwBytesRecorded = 0;
            if (waveInPrepareHeader(handle_, &header, sizeof(header)) != MMSYSERR_NOERROR ||
                waveInAddBuffer(handle_, &header, sizeof(header)) != MMSYSERR_NOERROR) {
                stop();
                return false;
            }
        }
        running_.store(true, std::memory_order_release);
        if (waveInStart(handle_) != MMSYSERR_NOERROR) {
            stop();
            return false;
        }
        return true;
    }

    void stop()
    {
        running_.store(false, std::memory_order_release);
        if (handle_ == nullptr) {
            return;
        }
        waveInStop(handle_);
        waveInReset(handle_);
        for (WAVEHDR& header : headers_) {
            if ((header.dwFlags & WHDR_PREPARED) != 0) {
                waveInUnprepareHeader(handle_, &header, sizeof(header));
            }
        }
        waveInClose(handle_);
        handle_ = nullptr;
    }

    size_t read(int16_t* destination, size_t capacity)
    {
        const uint64_t readIndex = read_.load(std::memory_order_relaxed);
        const uint64_t writeIndex = write_.load(std::memory_order_acquire);
        if (writeIndex - readIndex < capacity) {
            return 0;
        }
        const size_t count = capacity;
        for (size_t index = 0; index < count; index++) {
            destination[index] = ring_[(readIndex + index) % ring_.size()];
        }
        read_.store(readIndex + count, std::memory_order_release);
        return count;
    }

    uint64_t capturedSamples() const
    {
        return captured_.load(std::memory_order_acquire);
    }

    uint64_t droppedSamples() const
    {
        return dropped_.load(std::memory_order_acquire);
    }

    uint32_t takeLevel()
    {
        return level_.exchange(0, std::memory_order_acq_rel);
    }

    bool noInputDevice() const
    {
        return lastOpenResult_ == MMSYSERR_NODRIVER;
    }

    void injectForTest(const std::vector<int16_t>& samples)
    {
        onData(samples.data(), samples.size());
    }

private:
    static void CALLBACK callback(HWAVEIN, UINT message, DWORD_PTR instance, DWORD_PTR parameter, DWORD_PTR)
    {
        if (message != WIM_DATA || instance == 0 || parameter == 0) {
            return;
        }
        AudioCapture* self = reinterpret_cast<AudioCapture*>(instance);
        WAVEHDR* header = reinterpret_cast<WAVEHDR*>(parameter);
        self->onData(reinterpret_cast<const int16_t*>(header->lpData), header->dwBytesRecorded / sizeof(int16_t));
        if (self->running_.load(std::memory_order_acquire)) {
            header->dwBytesRecorded = 0;
            waveInAddBuffer(self->handle_, header, sizeof(*header));
        }
    }

    void onData(const int16_t* samples, size_t count)
    {
        uint64_t writeIndex = write_.load(std::memory_order_relaxed);
        const uint64_t readIndex = read_.load(std::memory_order_acquire);
        uint32_t peak = 0;
        for (size_t index = 0; index < count; index++) {
            const int32_t signedSample = samples[index];
            peak = std::max(peak, static_cast<uint32_t>(std::abs(signedSample)));
            if (writeIndex - readIndex >= ring_.size()) {
                dropped_.fetch_add(count - index, std::memory_order_relaxed);
                break;
            }
            ring_[writeIndex % ring_.size()] = samples[index];
            writeIndex++;
        }
        write_.store(writeIndex, std::memory_order_release);
        captured_.fetch_add(count, std::memory_order_release);
        level_.store(std::min<uint32_t>(1000, peak * 1000 / 32768), std::memory_order_release);
    }

    HWAVEIN handle_ = nullptr;
    std::array<std::array<int16_t, kFramesPerBuffer>, kAudioBuffers> buffers_ = {};
    std::array<WAVEHDR, kAudioBuffers> headers_ = {};
    std::vector<int16_t> ring_;
    std::atomic<uint64_t> read_ = 0;
    std::atomic<uint64_t> write_ = 0;
    std::atomic<bool> running_ = false;
    std::atomic<uint64_t> captured_ = 0;
    std::atomic<uint64_t> dropped_ = 0;
    std::atomic<uint32_t> level_ = 0;
    MMRESULT lastOpenResult_ = MMSYSERR_NOERROR;
};

class PersistentAsrWorker {
public:
    ~PersistentAsrWorker()
    {
        stop();
    }

    bool transcribe(const std::wstring& root, const std::vector<int16_t>& samples, bool useVad,
        std::wstring& text, std::wstring& error, DWORD responseTimeoutOverride = 0)
    {
        for (int attempt = 0; attempt < 2; attempt++) {
            if (process_ == nullptr && !start(root, error)) {
                return false;
            }
            const ExchangeResult result = exchange(samples, useVad, text, responseTimeoutOverride);
            if (result == ExchangeResult::Success) {
                return true;
            }
            if (result == ExchangeResult::InferenceFailed) {
                error = L"离线识别失败，请缩短单次语音后重试。";
                return false;
            }
            stop();
        }
        error = L"离线识别进程失去响应，已尝试自动重启。";
        return false;
    }

    bool warmup(const std::wstring& root, std::wstring& error)
    {
        return process_ != nullptr || start(root, error);
    }

    void stop()
    {
        if (input_ != nullptr) {
            CloseHandle(input_);
            input_ = nullptr;
        }
        if (output_ != nullptr) {
            CloseHandle(output_);
            output_ = nullptr;
        }
        if (process_ != nullptr) {
            if (WaitForSingleObject(process_, 1500) == WAIT_TIMEOUT) {
                TerminateProcess(process_, 1);
                WaitForSingleObject(process_, 1000);
            }
            CloseHandle(process_);
            process_ = nullptr;
        }
    }

private:
    enum class ExchangeResult {
        Success,
        TransportFailed,
        InferenceFailed
    };

    static constexpr uint32_t kRequestMagic = 0x31514659;
    static constexpr uint32_t kResponseMagic = 0x31524659;
    static constexpr uint32_t kReadyMagic = 0x31574659;

    bool start(const std::wstring& root, std::wstring& error)
    {
        const std::wstring executable = joinPath(root, L"funasr\\yanflow-asr-worker.exe");
        const std::wstring model = joinPath(root, L"models\\sensevoice-small-q8.gguf");
        const std::wstring vad = joinPath(root, L"models\\fsmn-vad.gguf");
        if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesW(model.c_str()) == INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesW(vad.c_str()) == INVALID_FILE_ATTRIBUTES) {
            error = L"识别资源未安装。请运行 build\\windows\\build-yanflow.cmd。";
            return false;
        }

        SECURITY_ATTRIBUTES security = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE childInput = nullptr;
        HANDLE parentInput = nullptr;
        HANDLE parentOutput = nullptr;
        HANDLE childOutput = nullptr;
        if (!CreatePipe(&childInput, &parentInput, &security, 0) ||
            !CreatePipe(&parentOutput, &childOutput, &security, 0)) {
            if (childInput != nullptr) CloseHandle(childInput);
            if (parentInput != nullptr) CloseHandle(parentInput);
            if (parentOutput != nullptr) CloseHandle(parentOutput);
            if (childOutput != nullptr) CloseHandle(childOutput);
            error = L"无法创建本地识别管道。";
            return false;
        }
        SetHandleInformation(parentInput, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(parentOutput, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.hStdInput = childInput;
        startup.hStdOutput = childOutput;
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION process = {};
        std::wstring command = quoteArgument(executable) + L" -m " + quoteArgument(model) +
            L" --vad " + quoteArgument(vad) + L" --threads 8";
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        const BOOL created = CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr,
            TRUE, CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &process);
        CloseHandle(childInput);
        CloseHandle(childOutput);
        if (!created) {
            CloseHandle(parentInput);
            CloseHandle(parentOutput);
            error = L"无法启动常驻离线识别引擎。";
            return false;
        }
        CloseHandle(process.hThread);
        process_ = process.hProcess;
        input_ = parentInput;
        output_ = parentOutput;

        uint32_t magic = 0;
        uint32_t version = 0;
        uint32_t status = 1;
        if (!readValue(magic, 30000) || !readValue(version, 1000) || !readValue(status, 1000) ||
            magic != kReadyMagic || version != 1 || status != 0) {
            stop();
            error = L"离线识别引擎初始化失败。";
            return false;
        }
        return true;
    }

    ExchangeResult exchange(const std::vector<int16_t>& samples, bool useVad, std::wstring& text,
        DWORD responseTimeoutOverride)
    {
        if (samples.size() > static_cast<size_t>(kSampleRate * 60)) {
            return ExchangeResult::InferenceFailed;
        }
        const uint32_t sampleCount = static_cast<uint32_t>(samples.size());
        const uint32_t flags = useVad ? 1u : 0u;
        if (!writeValue(kRequestMagic) || !writeValue(sampleCount) || !writeValue(flags) ||
            !writeAll(samples.data(), samples.size() * sizeof(int16_t))) {
            return ExchangeResult::TransportFailed;
        }
        uint32_t magic = 0;
        uint32_t status = 1;
        uint32_t bytes = 0;
        uint64_t elapsedMicroseconds = 0;
        const uint64_t audioMilliseconds = samples.size() * 1000ull / kSampleRate;
        const DWORD responseTimeout = responseTimeoutOverride > 0 ? responseTimeoutOverride :
            static_cast<DWORD>(std::min<uint64_t>(180000,
                std::max<uint64_t>(30000, 20000 + audioMilliseconds * 8)));
        if (!readValue(magic, responseTimeout) || !readValue(status, 1000) ||
            !readValue(bytes, 1000) || !readValue(elapsedMicroseconds, 1000) ||
            magic != kResponseMagic || bytes > 1024 * 1024) {
            return ExchangeResult::TransportFailed;
        }
        std::string utf8(bytes, '\0');
        if (!readAll(utf8.data(), utf8.size(), 3000)) {
            return ExchangeResult::TransportFailed;
        }
        if (status != 0) {
            return ExchangeResult::InferenceFailed;
        }
        text = utf8ToWide(utf8);
        return ExchangeResult::Success;
    }

    template <typename Value>
    bool writeValue(const Value& value)
    {
        return writeAll(&value, sizeof(value));
    }

    template <typename Value>
    bool readValue(Value& value, DWORD timeoutMilliseconds)
    {
        return readAll(&value, sizeof(value), timeoutMilliseconds);
    }

    bool writeAll(const void* data, size_t bytes)
    {
        const uint8_t* cursor = static_cast<const uint8_t*>(data);
        while (bytes > 0) {
            DWORD written = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes, 1024 * 1024));
            if (!WriteFile(input_, cursor, chunk, &written, nullptr) || written == 0) return false;
            cursor += written;
            bytes -= written;
        }
        return true;
    }

    bool readAll(void* data, size_t bytes, DWORD timeoutMilliseconds)
    {
        uint8_t* cursor = static_cast<uint8_t*>(data);
        const uint64_t deadline = GetTickCount64() + timeoutMilliseconds;
        while (bytes > 0) {
            DWORD available = 0;
            if (!PeekNamedPipe(output_, nullptr, 0, nullptr, &available, nullptr)) return false;
            if (available == 0) {
                if (WaitForSingleObject(process_, 0) != WAIT_TIMEOUT || GetTickCount64() >= deadline) return false;
                Sleep(2);
                continue;
            }
            DWORD received = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes, available));
            if (!ReadFile(output_, cursor, chunk, &received, nullptr) || received == 0) return false;
            cursor += received;
            bytes -= received;
        }
        return true;
    }

    HANDLE process_ = nullptr;
    HANDLE input_ = nullptr;
    HANDLE output_ = nullptr;
};

class YanFlowApp {
public:
    explicit YanFlowApp(int smokeMilliseconds) : smokeMilliseconds_(smokeMilliseconds) {}

    int run()
    {
        instance_ = GetModuleHandleW(nullptr);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        InitCommonControls();
        initializeAutomation();
        if (!createWindow()) {
            releaseAutomation();
            CoUninitialize();
            return 2;
        }
        if (smokeMilliseconds_ > 0 &&
            (applicationIcon_ == nullptr || applicationSmallIcon_ == nullptr || floatingIcon_ == nullptr)) {
            exitCode_ = 70;
        }
        workerRunning_.store(true, std::memory_order_release);
        endpointThread_ = std::thread(&YanFlowApp::endpointLoop, this);
        inferenceThread_ = std::thread(&YanFlowApp::inferenceLoop, this);
        loadSettings();
        registerConfiguredHotkeys(startHotkey_, stopHotkey_, false);
        RegisterHotKey(window_, kHotkeyVisibility, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'H');
        RegisterHotKey(window_, kHotkeyCopy, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'C');
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        UpdateWindow(window_);
        SetTimer(window_, kTimerAnimation, 60, nullptr);
        if (smokeMilliseconds_ < 0) exitCode_ = 60;
        if (smokeMilliseconds_ == -3) {
            toggleListening();
            SetTimer(window_, kTimerSmoke, 1500, nullptr);
        } else if (smokeMilliseconds_ == -4) {
            std::vector<int16_t> samples;
            if (readPcm16Wave(joinPath(executableDirectory(), L"yanflow-asr-smoke.wav"), samples)) {
                for (int16_t& sample : samples) {
                    sample = static_cast<int16_t>(sample / 4);
                }
                listening_.store(true, std::memory_order_release);
                capture_.injectForTest(samples);
            }
            SetTimer(window_, kTimerPipelineStop, 500, nullptr);
            SetTimer(window_, kTimerSmoke, 10000, nullptr);
        } else if (smokeMilliseconds_ == -1 || smokeMilliseconds_ == -2 || smokeMilliseconds_ == -5) {
            if (smokeMilliseconds_ == -1) rememberTarget();
            std::vector<int16_t> samples;
            if (readPcm16Wave(joinPath(executableDirectory(), L"yanflow-asr-smoke.wav"), samples)) {
                enqueueUtterance(std::move(samples));
            }
            if (smokeMilliseconds_ == -5) exitCode_ = 0;
            SetTimer(window_, kTimerSmoke, smokeMilliseconds_ == -5 ? 20000 : 10000, nullptr);
        } else if (smokeMilliseconds_ > 0) {
            if (!runWriteSmoke() && exitCode_ == 0) exitCode_ = 71;
            SetTimer(window_, kTimerSmoke, static_cast<UINT>(smokeMilliseconds_), nullptr);
        }

        MSG message = {};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        shutdownWorkers();
        releaseAutomation();
        CoUninitialize();
        return exitCode_;
    }

private:
    static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        YanFlowApp* self = reinterpret_cast<YanFlowApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<YanFlowApp*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window_ = window;
        }
        return self == nullptr ? DefWindowProcW(window, message, wParam, lParam)
                               : self->handleMessage(message, wParam, lParam);
    }

    bool createWindow()
    {
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.hInstance = instance_;
        windowClass.lpfnWndProc = &YanFlowApp::windowProcedure;
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32649));
        applicationIcon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(1), IMAGE_ICON,
            32, 32, LR_DEFAULTCOLOR | LR_SHARED));
        applicationSmallIcon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(1), IMAGE_ICON,
            16, 16, LR_DEFAULTCOLOR | LR_SHARED));
        windowClass.hIcon = applicationIcon_;
        windowClass.hIconSm = applicationSmallIcon_;
        windowClass.lpszClassName = L"YanFlowFloatingVoiceLayer";
        windowClass.hbrBackground = nullptr;
        RegisterClassExW(&windowClass);
        POINT anchor = {GetSystemMetrics(SM_CXSCREEN) - 1, GetSystemMetrics(SM_CYSCREEN) - 1};
        HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO monitorInfo = {sizeof(MONITORINFO)};
        GetMonitorInfoW(monitor, &monitorInfo);
        const int x = monitorInfo.rcWork.right - kCollapsedSize - 20;
        const int y = monitorInfo.rcWork.bottom - kCollapsedSize - 20;
        window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
            windowClass.lpszClassName, L"言流 YanFlow", WS_POPUP,
            x, y, kCollapsedSize, kCollapsedSize, nullptr, nullptr, instance_, this);
        if (window_ == nullptr) {
            return false;
        }
        SetLayeredWindowAttributes(window_, 0, 245, LWA_ALPHA);
        SetWindowRgn(window_, CreateRoundRectRgn(0, 0, kCollapsedSize, kCollapsedSize, 24, 24), TRUE);
        floatingIcon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(2), IMAGE_ICON,
            52, 52, LR_DEFAULTCOLOR | LR_SHARED));
        return true;
    }

    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_LBUTTONDOWN:
            if (bubbleExpanded_ && GET_X_LPARAM(lParam) >=
                kBubbleWidth - (fallbackIsTranscript_ ? 120 : 60)) {
                if (!fallbackIsTranscript_ || GET_X_LPARAM(lParam) < kBubbleWidth - 60) {
                    copiedFeedback_ = copyFallback();
                    if (copiedFeedback_) SetTimer(window_, kTimerCopyFeedback, 1200, nullptr);
                } else {
                    writeFallbackToDesktop(true);
                }
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            dragOrigin_.x = GET_X_LPARAM(lParam);
            dragOrigin_.y = GET_Y_LPARAM(lParam);
            GetCursorPos(&dragCursor_);
            SetCapture(window_);
            dragged_ = false;
            return 0;
        case WM_MOUSEMOVE:
            if (GetCapture() == window_) {
                POINT cursor = {};
                GetCursorPos(&cursor);
                if (std::abs(cursor.x - dragCursor_.x) + std::abs(cursor.y - dragCursor_.y) > 3) {
                    dragged_ = true;
                    RECT rect = {};
                    GetWindowRect(window_, &rect);
                    SetWindowPos(window_, HWND_TOPMOST, rect.left + cursor.x - dragCursor_.x,
                        rect.top + cursor.y - dragCursor_.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
                    dragCursor_ = cursor;
                }
            }
            return 0;
        case WM_LBUTTONUP:
            if (GetCapture() == window_) {
                ReleaseCapture();
                if (!dragged_) {
                    toggleListening();
                }
            }
            return 0;
        case WM_RBUTTONUP:
            showContextMenu();
            return 0;
        case WM_HOTKEY:
            if (wParam == kHotkeyStart) startListening();
            if (wParam == kHotkeyStop) stopListening();
            if (wParam == kHotkeyVisibility) toggleVisibility();
            if (wParam == kHotkeyCopy) copyFallback();
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == kCommandToggle) toggleListening();
            if (LOWORD(wParam) == kCommandCopy) copyFallback();
            if (LOWORD(wParam) == kCommandWrite) writeFallbackToDesktop(true);
            if (LOWORD(wParam) == kCommandSettings) showSettings();
            if (LOWORD(wParam) == kCommandExit) DestroyWindow(window_);
            return 0;
        case kMessageResult:
            acceptResult(reinterpret_cast<std::wstring*>(lParam));
            return 0;
        case kMessageStatus:
            state_ = static_cast<ListeningState>(wParam);
            if (lParam != 0) {
                std::wstring* messageText = reinterpret_cast<std::wstring*>(lParam);
                fallbackText_ = *messageText;
                fallbackIsTranscript_ = false;
                delete messageText;
                expandBubble();
            }
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_TIMER:
            if (wParam == kTimerAnimation) {
                animationPhase_ = (animationPhase_ + 1) % 12;
                const uint32_t level = capture_.takeLevel();
                displayLevel_ = std::max(level, displayLevel_ * 3 / 4);
                if (state_ == ListeningState::Listening || state_ == ListeningState::Recognizing) {
                    InvalidateRect(window_, nullptr, FALSE);
                }
                return 0;
            }
            if (wParam == kTimerCopyFeedback) {
                KillTimer(window_, kTimerCopyFeedback);
                copiedFeedback_ = false;
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            if (wParam == kTimerPipelineStop && smokeMilliseconds_ == -4) {
                KillTimer(window_, kTimerPipelineStop);
                listening_.store(false, std::memory_order_release);
                state_ = ListeningState::Idle;
                return 0;
            }
            if (smokeMilliseconds_ == -3) {
                exitCode_ = state_ == ListeningState::Error ? 41
                    : capture_.capturedSamples() >= static_cast<uint64_t>(kSampleRate / 2) ? 0 : 40;
            }
            DestroyWindow(window_);
            return 0;
        case WM_PAINT:
            paint();
            return 0;
        case WM_DESTROY:
            UnregisterHotKey(window_, kHotkeyStart);
            UnregisterHotKey(window_, kHotkeyStop);
            UnregisterHotKey(window_, kHotkeyVisibility);
            UnregisterHotKey(window_, kHotkeyCopy);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    void initializeAutomation()
    {
        CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&automation_));
    }

    void releaseAutomation()
    {
        if (automation_ != nullptr) {
            automation_->Release();
            automation_ = nullptr;
        }
    }

    bool focusedElementAcceptsText()
    {
        if (automation_ == nullptr) {
            return false;
        }
        IUIAutomationElement* element = nullptr;
        if (FAILED(automation_->GetFocusedElement(&element)) || element == nullptr) {
            return false;
        }
        CONTROLTYPEID type = 0;
        const HRESULT result = element->get_CurrentControlType(&type);
        element->Release();
        return SUCCEEDED(result) && (type == UIA_EditControlTypeId || type == UIA_DocumentControlTypeId);
    }

    void rememberTarget()
    {
        if (smokeMilliseconds_ != -2 && smokeMilliseconds_ != -4) {
            HWND foreground = GetForegroundWindow();
            if (foreground != nullptr && foreground != window_) {
                targetWindow_ = foreground;
                targetWasEditable_ = focusedElementAcceptsText();
            }
        }
    }

    void toggleListening()
    {
        if (listening_.load(std::memory_order_acquire)) {
            stopListening();
        } else {
            startListening();
        }
    }

    void startListening()
    {
        if (listening_.load(std::memory_order_acquire)) return;
        rememberTarget();
        if (!capture_.start()) {
            state_ = ListeningState::Error;
            fallbackText_ = capture_.noInputDevice()
                ? L"未检测到音频输入设备。"
                : L"无法打开麦克风，请检查 Windows 麦克风权限或设备占用。";
            fallbackIsTranscript_ = false;
            expandBubble();
        } else {
            fallbackText_.clear();
            fallbackIsTranscript_ = false;
            copiedFeedback_ = false;
            listening_.store(true, std::memory_order_release);
            state_ = ListeningState::Listening;
            collapseBubble();
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    void stopListening()
    {
        if (!listening_.load(std::memory_order_acquire)) return;
        listening_.store(false, std::memory_order_release);
        capture_.stop();
        state_ = ListeningState::Idle;
        InvalidateRect(window_, nullptr, FALSE);
    }

    void toggleVisibility()
    {
        if (IsWindowVisible(window_)) {
            ShowWindow(window_, SW_HIDE);
        } else {
            ShowWindow(window_, SW_SHOWNOACTIVATE);
            SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }

    std::wstring settingsPath()
    {
        PWSTR roaming = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &roaming))) {
            return joinPath(executableDirectory(), L"yanflow.ini");
        }
        const std::wstring directory = joinPath(roaming, L"YanFlow");
        CoTaskMemFree(roaming);
        CreateDirectoryW(directory.c_str(), nullptr);
        return joinPath(directory, L"settings.ini");
    }

    void loadSettings()
    {
        const std::wstring path = settingsPath();
        startHotkey_.modifiers = static_cast<UINT>(GetPrivateProfileIntW(
            L"Hotkeys", L"StartModifiers", MOD_CONTROL | MOD_ALT, path.c_str()));
        startHotkey_.virtualKey = static_cast<UINT>(GetPrivateProfileIntW(
            L"Hotkeys", L"StartKey", VK_SPACE, path.c_str()));
        stopHotkey_.modifiers = static_cast<UINT>(GetPrivateProfileIntW(
            L"Hotkeys", L"StopModifiers", MOD_CONTROL | MOD_ALT, path.c_str()));
        stopHotkey_.virtualKey = static_cast<UINT>(GetPrivateProfileIntW(
            L"Hotkeys", L"StopKey", 'S', path.c_str()));
        startHotkey_.modifiers &= MOD_CONTROL | MOD_ALT | MOD_SHIFT;
        stopHotkey_.modifiers &= MOD_CONTROL | MOD_ALT | MOD_SHIFT;
        if (startHotkey_.virtualKey == 0) startHotkey_ = {MOD_CONTROL | MOD_ALT, VK_SPACE};
        if (stopHotkey_.virtualKey == 0) stopHotkey_ = {MOD_CONTROL | MOD_ALT, 'S'};
        const HotkeyBinding visibility = {MOD_CONTROL | MOD_ALT, 'H'};
        const HotkeyBinding copy = {MOD_CONTROL | MOD_ALT, 'C'};
        if (sameHotkey(startHotkey_, stopHotkey_) || sameHotkey(startHotkey_, visibility) ||
            sameHotkey(startHotkey_, copy)) {
            startHotkey_ = {MOD_CONTROL | MOD_ALT, VK_SPACE};
        }
        if (sameHotkey(stopHotkey_, startHotkey_) || sameHotkey(stopHotkey_, visibility) ||
            sameHotkey(stopHotkey_, copy)) {
            stopHotkey_ = {MOD_CONTROL | MOD_ALT, 'S'};
        }
    }

    void persistSettings()
    {
        const std::wstring path = settingsPath();
        const std::wstring startModifiers = std::to_wstring(startHotkey_.modifiers);
        const std::wstring startKey = std::to_wstring(startHotkey_.virtualKey);
        const std::wstring stopModifiers = std::to_wstring(stopHotkey_.modifiers);
        const std::wstring stopKey = std::to_wstring(stopHotkey_.virtualKey);
        WritePrivateProfileStringW(L"Hotkeys", L"StartModifiers", startModifiers.c_str(), path.c_str());
        WritePrivateProfileStringW(L"Hotkeys", L"StartKey", startKey.c_str(), path.c_str());
        WritePrivateProfileStringW(L"Hotkeys", L"StopModifiers", stopModifiers.c_str(), path.c_str());
        WritePrivateProfileStringW(L"Hotkeys", L"StopKey", stopKey.c_str(), path.c_str());
    }

    bool registerConfiguredHotkeys(const HotkeyBinding& start, const HotkeyBinding& stop, bool replaceExisting)
    {
        const HotkeyBinding oldStart = startHotkey_;
        const HotkeyBinding oldStop = stopHotkey_;
        if (replaceExisting) {
            UnregisterHotKey(window_, kHotkeyStart);
            UnregisterHotKey(window_, kHotkeyStop);
        }
        const bool startRegistered = RegisterHotKey(window_, kHotkeyStart,
            start.modifiers | MOD_NOREPEAT, start.virtualKey) != FALSE;
        const bool stopRegistered = startRegistered && RegisterHotKey(window_, kHotkeyStop,
            stop.modifiers | MOD_NOREPEAT, stop.virtualKey) != FALSE;
        if (startRegistered && stopRegistered) {
            startHotkeyRegistered_ = true;
            stopHotkeyRegistered_ = true;
            return true;
        }
        if (startRegistered) UnregisterHotKey(window_, kHotkeyStart);
        if (replaceExisting) {
            startHotkeyRegistered_ = RegisterHotKey(window_, kHotkeyStart,
                oldStart.modifiers | MOD_NOREPEAT, oldStart.virtualKey) != FALSE;
            stopHotkeyRegistered_ = RegisterHotKey(window_, kHotkeyStop,
                oldStop.modifiers | MOD_NOREPEAT, oldStop.virtualKey) != FALSE;
        } else {
            startHotkeyRegistered_ = false;
            stopHotkeyRegistered_ = false;
        }
        return false;
    }

    static HotkeyBinding bindingFromControl(HWND control)
    {
        const WORD value = static_cast<WORD>(SendMessageW(control, HKM_GETHOTKEY, 0, 0));
        HotkeyBinding binding;
        binding.virtualKey = LOBYTE(value);
        const BYTE flags = HIBYTE(value);
        if ((flags & HOTKEYF_CONTROL) != 0) binding.modifiers |= MOD_CONTROL;
        if ((flags & HOTKEYF_ALT) != 0) binding.modifiers |= MOD_ALT;
        if ((flags & HOTKEYF_SHIFT) != 0) binding.modifiers |= MOD_SHIFT;
        return binding;
    }

    static WORD bindingForControl(const HotkeyBinding& binding)
    {
        BYTE flags = 0;
        if ((binding.modifiers & MOD_CONTROL) != 0) flags |= HOTKEYF_CONTROL;
        if ((binding.modifiers & MOD_ALT) != 0) flags |= HOTKEYF_ALT;
        if ((binding.modifiers & MOD_SHIFT) != 0) flags |= HOTKEYF_SHIFT;
        return MAKEWORD(binding.virtualKey, flags);
    }

    bool saveSettingsFromWindow()
    {
        const HotkeyBinding start = bindingFromControl(GetDlgItem(settingsWindow_, kSettingsStart));
        const HotkeyBinding stop = bindingFromControl(GetDlgItem(settingsWindow_, kSettingsStop));
        const HotkeyBinding visibility = {MOD_CONTROL | MOD_ALT, 'H'};
        const HotkeyBinding copy = {MOD_CONTROL | MOD_ALT, 'C'};
        if (start.virtualKey == 0 || stop.virtualKey == 0) {
            MessageBoxW(settingsWindow_, L"开始和停止监听都必须设置快捷键。", L"言流快捷键", MB_OK | MB_ICONWARNING);
            return false;
        }
        if (sameHotkey(start, stop) || sameHotkey(start, visibility) || sameHotkey(start, copy) ||
            sameHotkey(stop, visibility) || sameHotkey(stop, copy)) {
            MessageBoxW(settingsWindow_, L"快捷键互相冲突，请为开始和停止监听选择不同组合。",
                L"言流快捷键", MB_OK | MB_ICONWARNING);
            return false;
        }
        if (!registerConfiguredHotkeys(start, stop, true)) {
            MessageBoxW(settingsWindow_, L"快捷键已被其他程序占用，请换一个组合。",
                L"言流快捷键", MB_OK | MB_ICONWARNING);
            return false;
        }
        startHotkey_ = start;
        stopHotkey_ = stop;
        persistSettings();
        DestroyWindow(settingsWindow_);
        return true;
    }

    static LRESULT CALLBACK settingsProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        YanFlowApp* self = reinterpret_cast<YanFlowApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<YanFlowApp*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->settingsWindow_ = window;
        }
        if (self == nullptr) return DefWindowProcW(window, message, wParam, lParam);
        if (message == WM_COMMAND) {
            if (LOWORD(wParam) == kSettingsSave) self->saveSettingsFromWindow();
            if (LOWORD(wParam) == kSettingsCancel) DestroyWindow(window);
            return 0;
        }
        if (message == WM_CLOSE) {
            DestroyWindow(window);
            return 0;
        }
        if (message == WM_DESTROY) {
            self->settingsWindow_ = nullptr;
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void showSettings()
    {
        if (settingsWindow_ != nullptr) {
            ShowWindow(settingsWindow_, SW_RESTORE);
            SetForegroundWindow(settingsWindow_);
            return;
        }
        WNDCLASSEXW settingsClass = {};
        settingsClass.cbSize = sizeof(settingsClass);
        settingsClass.hInstance = instance_;
        settingsClass.lpfnWndProc = &YanFlowApp::settingsProcedure;
        settingsClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        settingsClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        settingsClass.hIcon = applicationIcon_;
        settingsClass.hIconSm = applicationSmallIcon_;
        settingsClass.lpszClassName = L"YanFlowHotkeySettings";
        RegisterClassExW(&settingsClass);
        settingsWindow_ = CreateWindowExW(WS_EX_DLGMODALFRAME, settingsClass.lpszClassName,
            L"言流快捷键设置", WS_CAPTION | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, 390, 225, window_, nullptr, instance_, this);
        if (settingsWindow_ == nullptr) return;
        CreateWindowExW(0, L"STATIC", L"开始监听", WS_CHILD | WS_VISIBLE,
            28, 28, 90, 24, settingsWindow_, nullptr, instance_, nullptr);
        HWND startControl = CreateWindowExW(WS_EX_CLIENTEDGE, HOTKEY_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 125, 24, 220, 28, settingsWindow_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsStart)), instance_, nullptr);
        CreateWindowExW(0, L"STATIC", L"停止监听", WS_CHILD | WS_VISIBLE,
            28, 72, 90, 24, settingsWindow_, nullptr, instance_, nullptr);
        HWND stopControl = CreateWindowExW(WS_EX_CLIENTEDGE, HOTKEY_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 125, 68, 220, 28, settingsWindow_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsStop)), instance_, nullptr);
        CreateWindowExW(0, L"STATIC", L"默认：开始 Ctrl+Alt+Space，停止 Ctrl+Alt+S",
            WS_CHILD | WS_VISIBLE, 28, 111, 320, 24, settingsWindow_, nullptr, instance_, nullptr);
        CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            175, 143, 80, 30, settingsWindow_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsSave)), instance_, nullptr);
        CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            265, 143, 80, 30, settingsWindow_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsCancel)), instance_, nullptr);
        SendMessageW(startControl, HKM_SETHOTKEY, bindingForControl(startHotkey_), 0);
        SendMessageW(stopControl, HKM_SETHOTKEY, bindingForControl(stopHotkey_), 0);
        RECT owner = {};
        RECT dialog = {};
        GetWindowRect(window_, &owner);
        GetWindowRect(settingsWindow_, &dialog);
        SetWindowPos(settingsWindow_, HWND_TOPMOST,
            owner.left - (dialog.right - dialog.left - (owner.right - owner.left)) / 2,
            owner.top - (dialog.bottom - dialog.top - (owner.bottom - owner.top)) / 2,
            0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(settingsWindow_);
    }

    void showContextMenu()
    {
        HMENU menu = CreatePopupMenu();
        const std::wstring listeningLabel = (listening_.load() ? L"停止聆听\t" : L"开始聆听\t") +
            hotkeyLabel(listening_.load() ? stopHotkey_ : startHotkey_) +
            ((listening_.load() ? stopHotkeyRegistered_ : startHotkeyRegistered_) ? L"" : L"（冲突，请设置）");
        AppendMenuW(menu, MF_STRING, kCommandToggle, listeningLabel.c_str());
        AppendMenuW(menu, MF_STRING | (fallbackText_.empty() ? MF_GRAYED : 0), kCommandCopy,
            L"复制文本\tCtrl+Alt+C");
        AppendMenuW(menu, MF_STRING | (!fallbackIsTranscript_ || fallbackText_.empty() ? MF_GRAYED : 0),
            kCommandWrite, L"写入桌面 TXT");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCommandSettings, L"快捷键设置...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCommandExit, L"退出言流");
        POINT cursor = {};
        GetCursorPos(&cursor);
        SetForegroundWindow(window_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window_, nullptr);
        DestroyMenu(menu);
    }

    void paint()
    {
        PAINTSTRUCT paintStruct = {};
        HDC target = BeginPaint(window_, &paintStruct);
        RECT rect = {};
        GetClientRect(window_, &rect);
        HDC memory = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, rect.right, rect.bottom);
        HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
        HBRUSH background = CreateSolidBrush(RGB(24, 27, 38));
        FillRect(memory, &rect, background);
        DeleteObject(background);

        if (floatingIcon_ != nullptr) {
            DrawIconEx(memory, 10, 10, floatingIcon_, 52, 52, 0, nullptr, DI_NORMAL);
        } else {
            HBRUSH circle = CreateSolidBrush(RGB(30, 86, 214));
            SelectObject(memory, circle);
            SelectObject(memory, GetStockObject(NULL_PEN));
            Ellipse(memory, 10, 10, 62, 62);
            DeleteObject(circle);
            HPEN micPen = CreatePen(PS_SOLID, 3, RGB(255, 255, 255));
            SelectObject(memory, micPen);
            MoveToEx(memory, 29, 24, nullptr);
            LineTo(memory, 29, 38);
            Arc(memory, 25, 30, 47, 47, 25, 35, 47, 35);
            MoveToEx(memory, 36, 44, nullptr);
            LineTo(memory, 36, 49);
            DeleteObject(micPen);
        }

        if (state_ == ListeningState::Listening || state_ == ListeningState::Recognizing) {
            const COLORREF waveColor = state_ == ListeningState::Recognizing
                ? RGB(255, 190, 70) : RGB(255, 94, 126);
            HPEN wavePen = CreatePen(PS_SOLID, 3, waveColor);
            SelectObject(memory, wavePen);
            const int energy = std::max(2, static_cast<int>(displayLevel_ * 12 / 1000));
            for (int index = 0; index < 5; index++) {
                const int pulse = (animationPhase_ + index * 2) % 10;
                const int animated = state_ == ListeningState::Recognizing ? 3 + std::abs(5 - pulse) : energy;
                const int height = std::min(15, animated + (index == 2 ? 4 : index % 2));
                const int x = 22 + index * 7;
                MoveToEx(memory, x, 36 - height / 2, nullptr);
                LineTo(memory, x, 37 + height / 2);
            }
            DeleteObject(wavePen);
        }

        if (rect.right > 72) {
            SetBkMode(memory, TRANSPARENT);
            SetTextColor(memory, RGB(238, 241, 250));
            HFONT font = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH, L"Microsoft YaHei UI");
            HGDIOBJ oldFont = SelectObject(memory, font);
            RECT textRect = {82, 14, rect.right - (fallbackIsTranscript_ ? 130 : 70), rect.bottom - 14};
            DrawTextW(memory, fallbackText_.c_str(), -1, &textRect,
                DT_LEFT | DT_VCENTER | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
            SelectObject(memory, oldFont);
            DeleteObject(font);

            HPEN divider = CreatePen(PS_SOLID, 1, RGB(61, 68, 88));
            SelectObject(memory, divider);
            const int actionLeft = rect.right - (fallbackIsTranscript_ ? 120 : 60);
            MoveToEx(memory, actionLeft, 20, nullptr);
            LineTo(memory, actionLeft, rect.bottom - 20);
            DeleteObject(divider);
            SetTextColor(memory, copiedFeedback_ ? RGB(112, 224, 173) : RGB(164, 211, 255));
            HFONT actionFont = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH, L"Microsoft YaHei UI");
            oldFont = SelectObject(memory, actionFont);
            RECT actionRect = {actionLeft + 2, 16, actionLeft + 58, rect.bottom - 16};
            DrawTextW(memory, copiedFeedback_ ? L"已复制" : L"复制", -1, &actionRect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (fallbackIsTranscript_) {
                divider = CreatePen(PS_SOLID, 1, RGB(61, 68, 88));
                SelectObject(memory, divider);
                MoveToEx(memory, rect.right - 60, 20, nullptr);
                LineTo(memory, rect.right - 60, rect.bottom - 20);
                DeleteObject(divider);
                SetTextColor(memory, RGB(255, 199, 122));
                RECT writeRect = {rect.right - 58, 16, rect.right - 4, rect.bottom - 16};
                DrawTextW(memory, L"写入", -1, &writeRect,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            SelectObject(memory, oldFont);
            DeleteObject(actionFont);
        }
        BitBlt(target, 0, 0, rect.right, rect.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memory);
        EndPaint(window_, &paintStruct);
    }

    void endpointLoop()
    {
        std::array<int16_t, kFramesPerBuffer> frame = {};
        std::deque<int16_t> preRoll;
        std::vector<int16_t> utterance;
        bool speech = false;
        int voicedFrames = 0;
        int silentFrames = 0;
        float noiseFloor = 0.004f;
        bool wasListening = false;
        while (workerRunning_.load(std::memory_order_acquire)) {
            if (!listening_.load(std::memory_order_acquire)) {
                if (wasListening && speech && utterance.size() >= static_cast<size_t>(kSampleRate * 0.35f)) {
                    enqueueUtterance(std::move(utterance), false);
                }
                speech = false;
                voicedFrames = 0;
                silentFrames = 0;
                preRoll.clear();
                utterance.clear();
                wasListening = false;
                Sleep(20);
                continue;
            }
            wasListening = true;
            const size_t count = capture_.read(frame.data(), frame.size());
            if (count < frame.size()) {
                Sleep(4);
                continue;
            }
            double squares = 0.0;
            for (int16_t sample : frame) {
                const float normalized = static_cast<float>(sample) / 32768.0f;
                squares += normalized * normalized;
            }
            const float rms = static_cast<float>(std::sqrt(squares / frame.size()));
            const float threshold = std::max(0.0035f, noiseFloor * 2.5f);
            const bool voiced = rms >= threshold;
            if (!speech) {
                if (!voiced) {
                    noiseFloor = noiseFloor * 0.98f + std::min(rms, 0.03f) * 0.02f;
                }
                preRoll.insert(preRoll.end(), frame.begin(), frame.end());
                while (preRoll.size() > static_cast<size_t>(kSampleRate * 0.30f)) preRoll.pop_front();
                voicedFrames = voiced ? voicedFrames + 1 : 0;
                if (voicedFrames >= 3) {
                    speech = true;
                    utterance.assign(preRoll.begin(), preRoll.end());
                    preRoll.clear();
                    silentFrames = 0;
                }
            } else {
                utterance.insert(utterance.end(), frame.begin(), frame.end());
                silentFrames = voiced ? 0 : silentFrames + 1;
                const bool endpoint = silentFrames >= 28;
                const bool maximum = utterance.size() >= kMaximumUtteranceSamples;
                if (endpoint || maximum) {
                    const size_t tail = static_cast<size_t>(std::min(silentFrames, 12) * kFramesPerBuffer);
                    if (tail < utterance.size()) utterance.resize(utterance.size() - tail);
                    if (utterance.size() >= static_cast<size_t>(kSampleRate * 0.35f)) {
                        enqueueUtterance(std::move(utterance), false);
                    }
                    utterance.clear();
                    speech = false;
                    voicedFrames = 0;
                    silentFrames = 0;
                }
            }
        }
    }

    struct QueuedUtterance {
        std::vector<int16_t> samples;
        bool useVad = false;
    };

    void enqueueUtterance(std::vector<int16_t>&& samples, bool useVad = true)
    {
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (utterances_.size() >= 3) utterances_.pop_front();
            QueuedUtterance utterance;
            utterance.samples = std::move(samples);
            utterance.useVad = useVad;
            utterances_.push_back(std::move(utterance));
        }
        queueChanged_.notify_one();
    }

    void inferenceLoop()
    {
        if (smokeMilliseconds_ <= 0 && smokeMilliseconds_ != -3) {
            std::wstring error;
            if (!asrWorker_.warmup(executableDirectory(), error)) {
                PostMessageW(window_, kMessageStatus, static_cast<WPARAM>(ListeningState::Error),
                    reinterpret_cast<LPARAM>(new std::wstring(error)));
            }
        }
        while (workerRunning_.load(std::memory_order_acquire)) {
            QueuedUtterance utterance;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueChanged_.wait(lock, [this] {
                    return !workerRunning_.load(std::memory_order_acquire) || !utterances_.empty();
                });
                if (!workerRunning_.load(std::memory_order_acquire)) break;
                utterance = std::move(utterances_.front());
                utterances_.pop_front();
            }
            PostMessageW(window_, kMessageStatus, static_cast<WPARAM>(ListeningState::Recognizing), 0);
            std::wstring text;
            std::wstring error;
            if (transcribe(utterance.samples, utterance.useVad, text, error) && !text.empty()) {
                PostMessageW(window_, kMessageResult, 0, reinterpret_cast<LPARAM>(new std::wstring(text)));
            } else if (error.empty()) {
                error = L"已检测到语音，但未识别出文本。请靠近麦克风后重试。";
                PostMessageW(window_, kMessageStatus, static_cast<WPARAM>(ListeningState::Error),
                    reinterpret_cast<LPARAM>(new std::wstring(error)));
            } else if (!error.empty()) {
                PostMessageW(window_, kMessageStatus, static_cast<WPARAM>(ListeningState::Error),
                    reinterpret_cast<LPARAM>(new std::wstring(error)));
            }
            if (listening_.load(std::memory_order_acquire)) {
                PostMessageW(window_, kMessageStatus, static_cast<WPARAM>(ListeningState::Listening), 0);
            }
        }
    }

    bool transcribe(const std::vector<int16_t>& samples, bool useVad, std::wstring& text, std::wstring& error)
    {
        const DWORD smokeTimeout = smokeMilliseconds_ < 0 ? 600000 : 0;
        return asrWorker_.transcribe(executableDirectory(), samples, useVad, text, error, smokeTimeout);
    }

    void acceptResult(std::wstring* result)
    {
        if (result == nullptr) return;
        std::wstring text = *result;
        delete result;
        if (smokeMilliseconds_ != -2 && smokeMilliseconds_ != -4) {
            HWND foreground = GetForegroundWindow();
            if (foreground != nullptr && foreground != window_) {
                targetWindow_ = foreground;
                targetWasEditable_ = focusedElementAcceptsText();
            }
        }
        const bool targetValid = targetWindow_ != nullptr && IsWindow(targetWindow_);
        const bool foregroundMatch = GetForegroundWindow() == targetWindow_;
        const bool injected = targetValid && targetWasEditable_ && foregroundMatch && injectText(text);
        if (injected) {
            fallbackText_.clear();
            fallbackIsTranscript_ = false;
            collapseBubble();
        } else {
            if (fallbackIsTranscript_ && !fallbackText_.empty()) {
                fallbackText_.push_back(L' ');
                fallbackText_.append(text);
            } else {
                fallbackText_ = text;
            }
            fallbackIsTranscript_ = true;
            expandBubble();
        }
        InvalidateRect(window_, nullptr, FALSE);
        if (smokeMilliseconds_ == -1) {
            const std::wstring diagnosticPath = joinPath(executableDirectory(), L"yanflow-e2e-diagnostic.txt");
            FILE* diagnostic = nullptr;
            if (_wfopen_s(&diagnostic, diagnosticPath.c_str(), L"wb") == 0 && diagnostic != nullptr) {
                fprintf(diagnostic, "target_valid=%d editable=%d foreground_match=%d injected=%d text_units=%zu\n",
                    targetValid ? 1 : 0, targetWasEditable_ ? 1 : 0, foregroundMatch ? 1 : 0,
                    injected ? 1 : 0, text.size());
                fclose(diagnostic);
            }
            exitCode_ = injected ? 0 : 61;
            DestroyWindow(window_);
        } else if (smokeMilliseconds_ == -2) {
            const LPARAM copyPoint = MAKELPARAM(kBubbleWidth - 90, kBubbleHeight / 2);
            SendMessageW(window_, WM_LBUTTONDOWN, MK_LBUTTON, copyPoint);
            SendMessageW(window_, WM_LBUTTONUP, 0, copyPoint);
            RECT rect = {};
            GetWindowRect(window_, &rect);
            bool clipboardMatches = false;
            if (OpenClipboard(window_)) {
                HANDLE data = GetClipboardData(CF_UNICODETEXT);
                if (data != nullptr) {
                    const wchar_t* clipboardText = static_cast<const wchar_t*>(GlobalLock(data));
                    if (clipboardText != nullptr) {
                        clipboardMatches = fallbackText_ == clipboardText;
                        GlobalUnlock(data);
                    }
                }
                CloseClipboard();
            }
            HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
            MONITORINFO monitorInfo = {sizeof(MONITORINFO)};
            GetMonitorInfoW(monitor, &monitorInfo);
            const bool fullyVisible = rect.left >= monitorInfo.rcWork.left && rect.top >= monitorInfo.rcWork.top &&
                rect.right <= monitorInfo.rcWork.right && rect.bottom <= monitorInfo.rcWork.bottom;
            exitCode_ = !injected && rect.right - rect.left == kBubbleWidth &&
                rect.bottom - rect.top == kBubbleHeight && fullyVisible && clipboardMatches &&
                copiedFeedback_ && !listening_.load(std::memory_order_acquire) ? 0 : 30;
            DestroyWindow(window_);
        } else if (smokeMilliseconds_ == -4) {
            if (!smokeTranscript_.empty()) smokeTranscript_.push_back(L' ');
            smokeTranscript_.append(text);
            fallbackText_ = smokeTranscript_;
            copyFallback();
            if (!injected && smokeTranscript_.find(L"\u6ee8\u6d77\u65b0\u533a\u6709\u623f") != std::wstring::npos) {
                exitCode_ = 0;
                DestroyWindow(window_);
            }
        }
    }

    bool injectText(const std::wstring& text)
    {
        if (text.empty() || targetWindow_ == nullptr) return false;
        if (GetForegroundWindow() != targetWindow_) {
            return false;
        }
        std::vector<INPUT> inputs;
        inputs.reserve(text.size() * 2);
        for (wchar_t unit : text) {
            INPUT down = {};
            down.type = INPUT_KEYBOARD;
            down.ki.wScan = unit;
            down.ki.dwFlags = KEYEVENTF_UNICODE;
            INPUT up = down;
            up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            inputs.push_back(down);
            inputs.push_back(up);
        }
        return SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) == inputs.size();
    }

    bool copyFallback()
    {
        if (fallbackText_.empty() || !OpenClipboard(window_)) return false;
        EmptyClipboard();
        const size_t bytes = (fallbackText_.size() + 1) * sizeof(wchar_t);
        HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, bytes);
        bool copied = false;
        if (data != nullptr) {
            void* memory = GlobalLock(data);
            memcpy(memory, fallbackText_.c_str(), bytes);
            GlobalUnlock(data);
            if (SetClipboardData(CF_UNICODETEXT, data) == nullptr) {
                GlobalFree(data);
            } else {
                copied = true;
            }
        }
        CloseClipboard();
        return copied;
    }

    std::wstring transcriptFileStem() const
    {
        std::wstring stem;
        size_t index = 0;
        int characters = 0;
        while (index < fallbackText_.size() && characters < 8) {
            wchar_t unit = fallbackText_[index++];
            if (unit == L'\r' || unit == L'\n' || unit == L'\t') unit = L' ';
            if (wcschr(L"\\/:*?\"<>|", unit) != nullptr || unit < 32) unit = L'_';
            stem.push_back(unit);
            if (unit >= 0xD800 && unit <= 0xDBFF && index < fallbackText_.size() &&
                fallbackText_[index] >= 0xDC00 && fallbackText_[index] <= 0xDFFF) {
                stem.push_back(fallbackText_[index++]);
            }
            characters++;
        }
        while (!stem.empty() && (stem.back() == L' ' || stem.back() == L'.')) stem.pop_back();
        if (stem.empty()) stem = L"言流转录";
        std::wstring upper = stem;
        std::transform(upper.begin(), upper.end(), upper.begin(), towupper);
        if (upper == L"CON" || upper == L"PRN" || upper == L"AUX" || upper == L"NUL" ||
            (upper.size() == 4 && (upper.rfind(L"COM", 0) == 0 || upper.rfind(L"LPT", 0) == 0) &&
                upper[3] >= L'1' && upper[3] <= L'9')) {
            stem.insert(stem.begin(), L'_');
        }
        return stem;
    }

    bool writeFallbackToDesktop(bool showErrors)
    {
        if (!fallbackIsTranscript_ || fallbackText_.empty()) return false;
        std::wstring directory;
        if (smokeMilliseconds_ > 0) {
            std::array<wchar_t, MAX_PATH + 1> temporary = {};
            const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
            if (length > 0 && length < temporary.size()) directory.assign(temporary.data(), length);
            while (!directory.empty() && (directory.back() == L'\\' || directory.back() == L'/')) {
                directory.pop_back();
            }
        } else {
            PWSTR desktop = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_CREATE, nullptr, &desktop))) {
                directory = desktop;
                CoTaskMemFree(desktop);
            }
        }
        if (directory.empty()) {
            if (showErrors) MessageBoxW(window_, L"无法定位桌面目录。", L"写入失败", MB_OK | MB_ICONERROR);
            return false;
        }
        const std::wstring stem = transcriptFileStem();
        HANDLE file = INVALID_HANDLE_VALUE;
        std::wstring path;
        for (int suffix = 0; suffix < 1000 && file == INVALID_HANDLE_VALUE; suffix++) {
            const std::wstring suffixText = suffix == 0
                ? L"" : L" (" + std::to_wstring(suffix + 1) + L")";
            path = joinPath(directory, stem + suffixText + L".txt");
            file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) break;
        }
        if (file == INVALID_HANDLE_VALUE) {
            if (showErrors) MessageBoxW(window_, L"无法在桌面创建文本文件。", L"写入失败", MB_OK | MB_ICONERROR);
            return false;
        }
        const std::string utf8 = wideToUtf8(fallbackText_);
        const uint8_t bom[] = {0xEF, 0xBB, 0xBF};
        DWORD written = 0;
        const bool bomWritten = WriteFile(file, bom, sizeof(bom), &written, nullptr) != FALSE && written == sizeof(bom);
        const bool textWritten = utf8.empty() ||
            (WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) != FALSE &&
                written == utf8.size());
        CloseHandle(file);
        if (!bomWritten || !textWritten) {
            DeleteFileW(path.c_str());
            if (showErrors) MessageBoxW(window_, L"文本文件写入不完整，请重试。", L"写入失败", MB_OK | MB_ICONERROR);
            return false;
        }
        lastWrittenPath_ = path;
        fallbackText_.clear();
        fallbackIsTranscript_ = false;
        copiedFeedback_ = false;
        collapseBubble();
        InvalidateRect(window_, nullptr, FALSE);
        return true;
    }

    bool runWriteSmoke()
    {
        fallbackText_ = L"滨海新区有房，继续测试写入。";
        fallbackIsTranscript_ = true;
        expandBubble();
        if (!writeFallbackToDesktop(false) || lastWrittenPath_.empty()) return false;
        const size_t slash = lastWrittenPath_.find_last_of(L"\\/");
        const std::wstring fileName = slash == std::wstring::npos
            ? lastWrittenPath_ : lastWrittenPath_.substr(slash + 1);
        const std::wstring expectedStem = L"滨海新区有房，继";
        const bool namedFromEightCharacters = fileName.rfind(expectedStem, 0) == 0 &&
            fileName.size() >= 4 && fileName.compare(fileName.size() - 4, 4, L".txt") == 0;
        HANDLE file = CreateFileW(lastWrittenPath_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        bool hasUtf8BomAndText = false;
        if (file != INVALID_HANDLE_VALUE) {
            std::array<uint8_t, 256> contents = {};
            DWORD bytes = 0;
            if (ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &bytes, nullptr) && bytes > 3) {
                hasUtf8BomAndText = contents[0] == 0xEF && contents[1] == 0xBB && contents[2] == 0xBF;
            }
            CloseHandle(file);
        }
        DeleteFileW(lastWrittenPath_.c_str());
        lastWrittenPath_.clear();
        return namedFromEightCharacters && hasUtf8BomAndText && fallbackText_.empty() &&
            !fallbackIsTranscript_ && !bubbleExpanded_;
    }

    void expandBubble()
    {
        resizeBubble(kBubbleWidth, kBubbleHeight, true);
    }

    void collapseBubble()
    {
        resizeBubble(kCollapsedSize, kCollapsedSize, false);
    }

    void resizeBubble(int width, int height, bool expanded)
    {
        RECT rect = {};
        GetWindowRect(window_, &rect);
        if (bubbleExpanded_ == expanded && rect.right - rect.left == width && rect.bottom - rect.top == height) {
            return;
        }
        HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo = {sizeof(MONITORINFO)};
        GetMonitorInfoW(monitor, &monitorInfo);
        int right = rect.right;
        int top = rect.top;
        int left = right - width;
        if (left < monitorInfo.rcWork.left) {
            left = monitorInfo.rcWork.left;
        }
        if (left + width > monitorInfo.rcWork.right) {
            left = monitorInfo.rcWork.right - width;
        }
        if (top + height > monitorInfo.rcWork.bottom) {
            top = monitorInfo.rcWork.bottom - height;
        }
        top = std::max(top, static_cast<int>(monitorInfo.rcWork.top));
        SetWindowRgn(window_, CreateRoundRectRgn(0, 0, width, height, 24, 24), TRUE);
        SetWindowPos(window_, HWND_TOPMOST, left, top, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        bubbleExpanded_ = expanded;
    }

    void shutdownWorkers()
    {
        listening_.store(false, std::memory_order_release);
        capture_.stop();
        workerRunning_.store(false, std::memory_order_release);
        queueChanged_.notify_all();
        if (endpointThread_.joinable()) endpointThread_.join();
        if (inferenceThread_.joinable()) inferenceThread_.join();
        asrWorker_.stop();
    }

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND settingsWindow_ = nullptr;
    HWND targetWindow_ = nullptr;
    HICON applicationIcon_ = nullptr;
    HICON applicationSmallIcon_ = nullptr;
    HICON floatingIcon_ = nullptr;
    IUIAutomation* automation_ = nullptr;
    AudioCapture capture_;
    PersistentAsrWorker asrWorker_;
    std::thread endpointThread_;
    std::thread inferenceThread_;
    std::atomic<bool> workerRunning_ = false;
    std::atomic<bool> listening_ = false;
    std::mutex queueMutex_;
    std::condition_variable queueChanged_;
    std::deque<QueuedUtterance> utterances_;
    ListeningState state_ = ListeningState::Idle;
    std::wstring fallbackText_;
    std::wstring smokeTranscript_;
    std::wstring lastWrittenPath_;
    uint32_t displayLevel_ = 0;
    int animationPhase_ = 0;
    bool targetWasEditable_ = false;
    bool fallbackIsTranscript_ = false;
    bool bubbleExpanded_ = false;
    bool copiedFeedback_ = false;
    HotkeyBinding startHotkey_ = {MOD_CONTROL | MOD_ALT, VK_SPACE};
    HotkeyBinding stopHotkey_ = {MOD_CONTROL | MOD_ALT, 'S'};
    bool startHotkeyRegistered_ = false;
    bool stopHotkeyRegistered_ = false;
    int smokeMilliseconds_ = 0;
    int exitCode_ = 0;
    POINT dragOrigin_ = {};
    POINT dragCursor_ = {};
    bool dragged_ = false;
};

} // namespace

Int php_yanflow_run(Int smokeMilliseconds)
{
    YanFlowApp app(static_cast<int>(smokeMilliseconds));
    return static_cast<Int>(app.run());
}

Int php_yanflow_asr_smoke()
{
    const std::wstring root = executableDirectory();
    const std::wstring path = joinPath(root, L"yanflow-asr-smoke.wav");
    std::vector<int16_t> samples;
    if (!readPcm16Wave(path, samples)) return 20;
    PersistentAsrWorker worker;
    std::wstring text;
    std::wstring error;
    if (!worker.transcribe(root, samples, true, text, error, 600000)) return 24;
    return text.find(L"\u6ee8\u6d77\u65b0\u533a\u6709\u623f") == std::wstring::npos ? 25 : 0;
}
