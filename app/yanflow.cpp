#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "yanflow.h"
#include "hold-hotkey.h"
#include "text-correction.h"
#include "text-pipeline.h"
#include "audio-segmentation.h"
#include "asr-protocol.h"
#include "dictionary-store.h"
#include "text-delivery.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <ole2.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <shellapi.h>
#include <uiautomation.h>
#include <wtsapi32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr UINT kMessageResult = WM_APP + 1;
constexpr UINT kMessageStatus = WM_APP + 2;
constexpr UINT kMessageHold = WM_APP + 3;
constexpr UINT kMessageStreamingDone = WM_APP + 4;
constexpr UINT kMessageCsc = WM_APP + 5;
constexpr UINT kMessageListeningToggle = WM_APP + 6;
constexpr UINT_PTR kTimerSmoke = 1;
constexpr UINT_PTR kTimerPipelineStop = 2;
constexpr UINT_PTR kTimerIdle = 5;
constexpr UINT_PTR kTimerHold = 6;
constexpr UINT_PTR kTimerStreamingFeed = 7;
constexpr int kHotkeyStart = 1;
constexpr int kHotkeyStop = 2;
constexpr int kHotkeyVisibility = 3;
constexpr int kHotkeyCopy = 4;
constexpr int kCommandToggle = 100;
constexpr int kCommandCopy = 101;
constexpr int kCommandWrite = 102;
constexpr int kCommandSettings = 103;
constexpr int kCommandExit = 104;
constexpr int kCommandDictionary = 105;
constexpr int kCommandCsc = 106;
constexpr int kCommandCompare = 107;
constexpr int kCommandProfileAuto = 108;
constexpr int kCommandProfileGeneral = 109;
constexpr int kCommandProfileDevelopment = 110;
constexpr int kCommandAppDictionary = 111;
constexpr int kCommandLearn = 112;
constexpr int kCommandDevelopmentDictionary = 113;
constexpr int kCommandCscDetails = 114;
constexpr int kLearnPreview = 300;
constexpr int kLearnSource = 301;
constexpr int kLearnTarget = 302;
constexpr int kLearnContext = 303;
constexpr int kLearnScope = 304;
constexpr int kLearnSave = 305;
constexpr int kLearnCancel = 306;
constexpr int kSettingsStart = 200;
constexpr int kSettingsStop = 201;
constexpr int kSettingsSave = 202;
constexpr int kSettingsCancel = 203;
constexpr int kSettingsHold = 204;
constexpr int kSampleRate = 16000;
constexpr int kFramesPerBuffer = 320;
constexpr int kAudioBuffers = 6;
constexpr size_t kRingSamples = kSampleRate * 30;
constexpr size_t kMaximumUtteranceSamples = kSampleRate * 8;
constexpr uint64_t kIdleTimeoutMilliseconds = 30000;
constexpr uint64_t kMaximumHoldMilliseconds = 60000;
constexpr int kCollapsedSize = 64;
constexpr int kBubbleWidth = 480;
constexpr int kBubbleMinimumHeight = 88;
constexpr int kBubbleMaximumHeight = 216;

RECT bubbleActionRect(bool write, int width, int height, bool transcript = true)
{
    const int top = transcript ? (height - 64) / 2 : (height - 28) / 2;
    return {width - 68, top + (write ? 36 : 0), width - 12, top + (write ? 36 : 0) + 28};
}

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

struct RecognitionResult {
    std::wstring raw, final, application;
    uint64_t serial = 0, timestamp = 0;
    std::wstring correctionStatus, correctionError, manualCorrection;
    yanflow::TextDelivery delivery;
};
struct CscStatus { unsigned int revision; bool ready; std::wstring error; };

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

    size_t read(int16_t* destination, size_t capacity, bool drain = false)
    {
        const uint64_t readIndex = read_.load(std::memory_order_relaxed);
        const uint64_t writeIndex = write_.load(std::memory_order_acquire);
        if (!drain && writeIndex - readIndex < capacity) {
            return 0;
        }
        const size_t count = static_cast<size_t>(std::min<uint64_t>(capacity, writeIndex - readIndex));
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
        for (size_t index = 0; index < count; index++) {
            if (writeIndex - readIndex >= ring_.size()) {
                dropped_.fetch_add(count - index, std::memory_order_relaxed);
                break;
            }
            ring_[writeIndex % ring_.size()] = samples[index];
            writeIndex++;
        }
        write_.store(writeIndex, std::memory_order_release);
        captured_.fetch_add(count, std::memory_order_release);
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
    MMRESULT lastOpenResult_ = MMSYSERR_NOERROR;
};

class PersistentAsrWorker {
public:
    ~PersistentAsrWorker()
    {
        stop();
    }

    bool transcribe(const std::wstring& root, const std::vector<int16_t>& samples, bool useVad,
        std::wstring& text, std::wstring& error, std::vector<yanflow::TimedToken>* tokens = nullptr, bool shortHold = false)
    {
        for (int attempt = 0; attempt < 2; attempt++) {
            if (process_ == nullptr && !start(root, error)) {
                return false;
            }
            const ExchangeResult result = exchange(samples, useVad, text, tokens, shortHold);
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
            magic != kReadyMagic || version != yanflow::kAsrProtocolVersion || status != 0) {
            stop();
            error = L"离线识别引擎初始化失败。";
            return false;
        }
        return true;
    }

    ExchangeResult exchange(const std::vector<int16_t>& samples, bool useVad, std::wstring& text,
        std::vector<yanflow::TimedToken>* tokens, bool shortHold)
    {
        if (samples.size() > static_cast<size_t>(kSampleRate * 60)) {
            return ExchangeResult::InferenceFailed;
        }
        const uint32_t sampleCount = static_cast<uint32_t>(samples.size());
        const uint32_t flags = (useVad ? yanflow::kAsrUseVad : 0u) | (tokens ? yanflow::kAsrTimedTokens : 0u) |
            (shortHold && useVad ? yanflow::kAsrShortHold : 0u);
        if (!writeValue(kRequestMagic) || !writeValue(sampleCount) || !writeValue(flags) ||
            !writeAll(samples.data(), samples.size() * sizeof(int16_t))) {
            return ExchangeResult::TransportFailed;
        }
        uint32_t magic = 0;
        uint32_t status = 1;
        uint32_t bytes = 0;
        uint64_t elapsedMicroseconds = 0;
        const uint64_t audioMilliseconds = samples.size() * 1000ull / kSampleRate;
        const DWORD responseTimeout = static_cast<DWORD>(std::min<uint64_t>(180000,
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
        if (tokens) {
            tokens->clear();
            uint32_t count = 0;
            if (!readValue(count, 1000) || count > 4096) return ExchangeResult::TransportFailed;
            uint32_t previousBegin = 0;
            for (uint32_t i = 0; i < count; ++i) {
                yanflow::AsrTokenHeader header{};
                if (!readValue(header, 1000) || header.bytes > 4096 || header.beginSample > header.endSample ||
                    header.endSample > sampleCount || header.beginSample < previousBegin) return ExchangeResult::TransportFailed;
                std::string piece(header.bytes, '\0');
                if (!readAll(piece.data(), piece.size(), 1000)) return ExchangeResult::TransportFailed;
                const std::string marker = "\xe2\x96\x81";
                size_t at = 0;
                while ((at = piece.find(marker, at)) != std::string::npos) { piece.replace(at, marker.size(), " "); ++at; }
                const auto value = utf8ToWide(piece);
                if (!piece.empty() && value.empty()) return ExchangeResult::TransportFailed;
                tokens->push_back({value, header.beginSample, header.endSample});
                previousBegin = header.beginSample;
            }
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
        if (smokeMilliseconds_ == -8) {
            wchar_t temporary[MAX_PATH]{};
            if (!GetTempPathW(MAX_PATH, temporary)) return 130;
            dictionarySmokeRoot_ = std::wstring(temporary) + L"YanFlow-learn-" + std::to_wstring(GetCurrentProcessId());
            if (!CreateDirectoryW(dictionarySmokeRoot_.c_str(), nullptr)) return 131;
        }
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
            (applicationIcon_ == nullptr || applicationSmallIcon_ == nullptr ||
             floatingIcon_ == nullptr || listeningIcon_ == nullptr)) {
            exitCode_ = 70;
        }
        loadSettings();
        if (smokeMilliseconds_ == -12 || smokeMilliseconds_ == -13) cscEnabled_.store(false);
        if (smokeMilliseconds_ > 0 && !runComparisonSmoke()) exitCode_ = 158;
        workerRunning_.store(true, std::memory_order_release);
        endpointThread_ = std::thread(&YanFlowApp::endpointLoop, this);
        inferenceThread_ = std::thread(&YanFlowApp::inferenceLoop, this);
        hookOwner_ = this;
        keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &YanFlowApp::keyboardProcedure, instance_, 0);
        WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION);
        registerConfiguredHotkeys(startHotkey_, stopHotkey_, false);
        RegisterHotKey(window_, kHotkeyVisibility, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'H');
        RegisterHotKey(window_, kHotkeyCopy, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'C');
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        UpdateWindow(window_);
        SetTimer(window_, kTimerIdle, 1000, nullptr);
        SetTimer(window_, kTimerHold, 50, nullptr);
        if (smokeMilliseconds_ < 0) exitCode_ = 60;
        if (smokeMilliseconds_ == -12) {
            fallbackText_ = L"没有可写的输入框时，识别文字会留在这里。\n可以复制，也可以写入桌面 TXT。";
            fallbackIsTranscript_ = true;
            bool saved = saveUiPreview(L"yanflow-ui-collapsed.bmp", false) && saveUiPreview(L"yanflow-ui-bubble.bmp", true);
            fallbackText_ = L"好";
            const auto shortBubbleSize = measureBubble();
            saved = saved && saveUiPreview(L"yanflow-ui-short.bmp", true);
            fallbackText_ = std::wstring(160, L'多');
            const auto longBubbleSize = measureBubble();
            saved = saved && saveUiPreview(L"yanflow-ui-long.bmp", true);
            copiedFeedback_ = true;
            saved = saved && saveUiPreview(L"yanflow-ui-copied.bmp", true);
            exitCode_ = saved && shortBubbleSize.cx < longBubbleSize.cx && shortBubbleSize.cy < longBubbleSize.cy && longBubbleSize.cx <= kBubbleWidth &&
                longBubbleSize.cy <= kBubbleMaximumHeight ? 0 : 159;
            SetTimer(window_, kTimerSmoke, 200, nullptr);
        } else if (smokeMilliseconds_ == -13) {
            exitCode_ = runUiStateSmoke();
            SetTimer(window_, kTimerSmoke, 200, nullptr);
        } else if (smokeMilliseconds_ == -8) {
            const bool passed = runLearnSmoke();
            exitCode_ = passed ? 0 : 132;
            SetTimer(window_, kTimerSmoke, 200, nullptr);
        } else if (smokeMilliseconds_ == -3) {
            toggleListening();
            SetTimer(window_, kTimerSmoke, 1500, nullptr);
        } else if (smokeMilliseconds_ == -6) {
            lastSpeechTick_.store(GetTickCount64(), std::memory_order_release);
            listening_.store(true, std::memory_order_release);
            state_ = ListeningState::Listening;
            SetTimer(window_, kTimerSmoke, 4500, nullptr);
        } else if (smokeMilliseconds_ == -4 || smokeMilliseconds_ == -7 || smokeMilliseconds_ == -9 || smokeMilliseconds_ == -10 || smokeMilliseconds_ == -11) {
            std::vector<int16_t> samples;
            if (readPcm16Wave(joinPath(executableDirectory(), L"yanflow-asr-smoke.wav"), samples)) {
                for (int16_t& sample : samples) {
                    if (smokeMilliseconds_ != -9) sample = static_cast<int16_t>(sample / 4);
                }
                if (smokeMilliseconds_ == -7 || smokeMilliseconds_ == -9 || smokeMilliseconds_ == -10) {
                    // A full second of trailing silence must not trigger ASR
                    // before release, even after the normal 560 ms endpoint.
                    if (smokeMilliseconds_ == -7) samples.resize(samples.size() + kSampleRate + 7, 0);
                    holdSession_.store(true, std::memory_order_release);
                }
                listening_.store(true, std::memory_order_release);
                lastSpeechTick_.store(GetTickCount64(), std::memory_order_release);
                if (smokeMilliseconds_ == -11) {
                    streamingSmokeSamples_ = std::move(samples);
                    streamingSmokeFrame_.reserve(kSampleRate / 10);
                    SetTimer(window_, kTimerStreamingFeed, 100, nullptr);
                } else capture_.injectForTest(samples);
            }
            if (smokeMilliseconds_ != -11) SetTimer(window_, kTimerPipelineStop, 500, nullptr);
            SetTimer(window_, kTimerSmoke, smokeMilliseconds_ == -11 ? 45000 : 10000, nullptr);
        } else if (smokeMilliseconds_ == -1 || smokeMilliseconds_ == -2 || smokeMilliseconds_ == -5) {
            if (smokeMilliseconds_ == -1) {
                rememberTarget();
                std::lock_guard<std::mutex> lock(queueMutex_);
                sessionApplication_ = applicationForWindow(targetWindow_);
            }
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
            if (learnWindow_ && IsDialogMessageW(learnWindow_, &message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        shutdownWorkers();
        if (!dictionarySmokeRoot_.empty()) {
            const auto apps = dictionarySmokeRoot_ + L"\\dictionaries\\apps";
            DeleteFileW((apps + L"\\yanflow-learning-smoke.exe.tsv").c_str());
            RemoveDirectoryW(apps.c_str());
            RemoveDirectoryW((dictionarySmokeRoot_ + L"\\dictionaries").c_str());
            DeleteFileW((dictionarySmokeRoot_ + L"\\settings.ini").c_str());
            RemoveDirectoryW(dictionarySmokeRoot_.c_str());
        }
        releaseAutomation();
        CoUninitialize();
        return exitCode_;
    }

private:
    static LRESULT CALLBACK keyboardProcedure(int code, WPARAM message, LPARAM parameter)
    {
        if (code == HC_ACTION && hookOwner_ != nullptr) {
            const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(parameter);
            const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
            if ((down || up) && (key->flags & LLKHF_INJECTED) == 0) {
                const bool toggle = hookOwner_->toggleKeys_.key(key->vkCode, down);
                const auto action = hookOwner_->holdEnabled_ ? hookOwner_->holdKeys_.key(key->vkCode, down) : yanflow::HoldHotkey::Action::None;
                if (toggle || action == yanflow::HoldHotkey::Action::Press) {
                    // Pass both modifier downs/ups through so Windows and the
                    // foreground app never see a stuck modifier. An unassigned
                    // injected key masks Start-menu activation on Win release.
                    INPUT mask[2]{};
                    mask[0].type = mask[1].type = INPUT_KEYBOARD;
                    mask[0].ki.wVk = mask[1].ki.wVk = 0xe8;
                    mask[1].ki.dwFlags = KEYEVENTF_KEYUP;
                    SendInput(2, mask, sizeof(INPUT));
                }
                if (toggle) PostMessageW(hookOwner_->window_, kMessageListeningToggle, 0, 0);
                else if (action != yanflow::HoldHotkey::Action::None) {
                    // The hook only tracks keys and posts; audio work runs on the UI/worker threads.
                    PostMessageW(hookOwner_->window_, kMessageHold,
                        action == yanflow::HoldHotkey::Action::Press ? 1 : 0, 0);
                }
            }
        }
        return CallNextHookEx(nullptr, code, message, parameter);
    }

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
            kCollapsedSize, kCollapsedSize, LR_DEFAULTCOLOR | LR_SHARED));
        listeningIcon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(3), IMAGE_ICON,
            kCollapsedSize, kCollapsedSize, LR_DEFAULTCOLOR | LR_SHARED));
        return true;
    }

    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_LBUTTONDOWN:
            if (bubbleExpanded_) {
                const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                const auto copy = actionRect(false);
                const auto write = actionRect(true);
                if (PtInRect(&copy, point)) {
                    if (copiedFeedback_) dismissFallback();
                    else copyFallback();
                    InvalidateRect(window_, nullptr, FALSE); return 0;
                }
                if (fallbackIsTranscript_ && PtInRect(&write, point)) {
                    writeFallbackToDesktop(true); return 0;
                }
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
            if (wParam == kHotkeyStart) { if (sameHotkey(startHotkey_, stopHotkey_)) toggleListening(); else startListening(); }
            if (wParam == kHotkeyStop) stopListening();
            if (wParam == kHotkeyVisibility) toggleVisibility();
            if (wParam == kHotkeyCopy) copyFallback();
            return 0;
        case kMessageListeningToggle:
            if (settingsWindow_ || learnWindow_) return 0;
            if (holdSession_.load() && listening_.load()) {
                // Ctrl+Win may already have started a hold before Shift arrives.
                // Convert this same capture to real-time; do not stop/restart it.
                holdSession_.store(false);
            } else toggleListening();
            return 0;
        case kMessageHold:
            if (wParam != 0) {
                if (holdEnabled_ && holdKeys_.active() && settingsWindow_ == nullptr && learnWindow_ == nullptr && !listening_.load() &&
                    (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                    ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000)) {
                    startListening(true);
                }
            } else if (holdSession_.load(std::memory_order_acquire)) stopListening();
            return 0;
        case WM_WTSSESSION_CHANGE:
            if (wParam == WTS_SESSION_LOCK || wParam == WTS_REMOTE_DISCONNECT || wParam == WTS_CONSOLE_DISCONNECT) stopListening();
            return 0;
        case WM_POWERBROADCAST:
            if (wParam == PBT_APMSUSPEND) stopListening();
            return TRUE;
        case WM_COMMAND:
            if (LOWORD(wParam) == kCommandToggle) toggleListening();
            if (LOWORD(wParam) == kCommandCopy) copyFallback();
            if (LOWORD(wParam) == kCommandWrite) writeFallbackToDesktop(true);
            if (LOWORD(wParam) == kCommandSettings) showSettings();
            if (LOWORD(wParam) == kCommandDictionary) editDictionary();
            if (LOWORD(wParam) == kCommandAppDictionary) editApplicationDictionary();
            if (LOWORD(wParam) == kCommandDevelopmentDictionary) editDevelopmentDictionary();
            if (LOWORD(wParam) == kCommandLearn) showLearnWindow();
            if (LOWORD(wParam) >= kCommandProfileAuto && LOWORD(wParam) <= kCommandProfileDevelopment && !menuApplication_.empty()) {
                const wchar_t* value = LOWORD(wParam) == kCommandProfileGeneral ? L"general"
                    : LOWORD(wParam) == kCommandProfileDevelopment ? L"development" : L"auto";
                WritePrivateProfileStringW(L"DictionaryApplications", menuApplication_.c_str(), value, settingsPath().c_str());
            }
            if (LOWORD(wParam) == kCommandCompare) saveRecognitionComparison();
            if (LOWORD(wParam) == kCommandCsc) {
                {
                    std::lock_guard<std::mutex> lock(queueMutex_);
                    cscEnabled_.store(!cscEnabled_.load());
                    cscRevision_.fetch_add(1);
                }
                cscFailed_.store(false); cscReady_.store(false); cscError_.clear();
                queueChanged_.notify_one();
                persistSettings();
            }
            if (LOWORD(wParam) == kCommandCscDetails) {
                MessageBoxW(window_, cscError_.empty() ? L"MacBERT 模型随完整安装包提供。开启后会在后台加载。" : cscError_.c_str(),
                    L"MacBERT 加载诊断", MB_OK | MB_ICONINFORMATION);
            }
            if (LOWORD(wParam) == kCommandExit) DestroyWindow(window_);
            return 0;
        case kMessageCsc: {
            auto* status = reinterpret_cast<CscStatus*>(lParam);
            if (status->revision == cscRevision_.load()) {
                cscReady_.store(status->ready); cscFailed_.store(!status->error.empty()); cscError_ = status->error;
            }
            delete status; return 0;
        }
        case kMessageResult:
            acceptResult(reinterpret_cast<RecognitionResult*>(lParam));
            return 0;
        case kMessageStreamingDone:
            finishStreamingSmoke();
            return 0;
        case kMessageStatus:
            if (smokeMilliseconds_ == -11 && static_cast<ListeningState>(wParam) == ListeningState::Error) streamingSmokeError_ = true;
            if (static_cast<ListeningState>(wParam) == ListeningState::Idle && listening_.load(std::memory_order_acquire)) return 0;
            if (static_cast<ListeningState>(wParam) == ListeningState::Listening &&
                !listening_.load(std::memory_order_acquire)) return 0;
            state_ = static_cast<ListeningState>(wParam);
            if (lParam != 0) {
                std::wstring* messageText = reinterpret_cast<std::wstring*>(lParam);
                fallbackText_ = *messageText;
                fallbackIsTranscript_ = false; copiedFeedback_ = false;
                delete messageText;
                expandBubble();
            }
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_TIMER:
            if (wParam == kTimerStreamingFeed && smokeMilliseconds_ == -11) {
                const size_t end = std::min(streamingSmokeSamples_.size(), streamingSmokePosition_ + kSampleRate / 10);
                streamingSmokeFrame_.assign(streamingSmokeSamples_.begin() + streamingSmokePosition_, streamingSmokeSamples_.begin() + end);
                capture_.injectForTest(streamingSmokeFrame_);
                streamingSmokePosition_ = end;
                if (end == streamingSmokeSamples_.size()) {
                    KillTimer(window_, kTimerStreamingFeed);
                    stopListening();
                }
                return 0;
            }
            if (wParam == kTimerHold) {
                if (smokeMilliseconds_ == -11) finishStreamingSmoke();
                if (smokeMilliseconds_ != -7 && smokeMilliseconds_ != -9 && smokeMilliseconds_ != -10 && holdSession_.load(std::memory_order_acquire) && listening_.load() &&
                    (!(GetAsyncKeyState(VK_CONTROL) & 0x8000) ||
                     !((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) ||
                     GetTickCount64() - holdStartedTick_ >= kMaximumHoldMilliseconds)) stopListening();
                return 0;
            }
            if (wParam == kTimerIdle) {
                const uint64_t now = GetTickCount64();
                const uint64_t idleTimeout = smokeMilliseconds_ == -6 ? 2000 : kIdleTimeoutMilliseconds;
                if (!holdSession_.load(std::memory_order_acquire) && listening_.load(std::memory_order_acquire) &&
                    now - lastSpeechTick_.load(std::memory_order_acquire) >= idleTimeout) {
                    // Give a phrase at the boundary one chance to finish VAD/ASR.
                    // Continuous non-speech noise cannot keep the mic open indefinitely.
                    if (idleGraceDeadline_ == 0 &&
                        now - lastCandidateTick_.load(std::memory_order_acquire) <= 3000) {
                        idleGraceDeadline_ = now + 8000;
                    }
                    if (idleGraceDeadline_ == 0 || now >= idleGraceDeadline_) stopListening();
                }
                return 0;
            }
            if (wParam == kTimerPipelineStop && (smokeMilliseconds_ == -4 || smokeMilliseconds_ == -7 || smokeMilliseconds_ == -9 || smokeMilliseconds_ == -10 || smokeMilliseconds_ == -11)) {
                KillTimer(window_, kTimerPipelineStop);
                if (smokeMilliseconds_ == -7 || smokeMilliseconds_ == -9 || smokeMilliseconds_ == -10 || smokeMilliseconds_ == -11) {
                    holdSmokeDeferred_ = submittedUtterances_.load() == 0;
                    stopListening();
                    return 0;
                }
                listening_.store(false, std::memory_order_release);
                state_ = ListeningState::Idle;
                return 0;
            }
            if (smokeMilliseconds_ == -3) {
                exitCode_ = state_ == ListeningState::Error ? 41
                    : capture_.capturedSamples() >= static_cast<uint64_t>(kSampleRate / 2) ? 0 : 40;
            } else if (smokeMilliseconds_ == -6) {
                exitCode_ = !listening_.load(std::memory_order_acquire) && state_ == ListeningState::Idle
                    ? 0 : 42;
            }
            DestroyWindow(window_);
            return 0;
        case WM_PAINT:
            paint();
            return 0;
        case WM_DESTROY:
            if (keyboardHook_ != nullptr) UnhookWindowsHookEx(keyboardHook_);
            keyboardHook_ = nullptr;
            hookOwner_ = nullptr;
            WTSUnRegisterSessionNotification(window_);
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
        automationCreationStatus_ = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&automation_));
    }

    void releaseAutomation()
    {
        if (automation_ != nullptr) {
            automation_->Release();
            automation_ = nullptr;
        }
    }

    // Native Edit controls remain identifiable when a UI Automation provider
    // is absent or reports stale focus. Unknown controls still require UIA.
    static int nativeEditState(HWND control)
    {
        if (!control || !IsWindow(control)) return -1;
        wchar_t name[256]{};
        if (!GetClassNameW(control, name, static_cast<int>(std::size(name)))) return -1;
        if (std::wcscmp(name, L"Edit") != 0 && std::wcscmp(name, L"EDIT") != 0 &&
            std::wcsncmp(name, L"WindowsForms10.EDIT.", 20) != 0 && _wcsnicmp(name, L"RichEdit", 8) != 0) return -1;
        return IsWindowEnabled(control) && !(GetWindowLongPtrW(control, GWL_STYLE) & ES_READONLY) ? 1 : 0;
    }

    bool focusedElementAcceptsText()
    {
        lastNativeFocusState_ = -1;
        lastNativeFocusClass_.clear(); lastForegroundClass_.clear();
        HWND foreground = GetForegroundWindow();
        wchar_t className[256]{};
        GetClassNameW(foreground, className, static_cast<int>(std::size(className)));
        lastForegroundClass_ = className;
        GUITHREADINFO info{}; info.cbSize = sizeof(info);
        const DWORD thread = GetWindowThreadProcessId(foreground, nullptr);
        lastNativeThreadInfo_ = thread && GetGUIThreadInfo(thread, &info);
        if (info.hwndFocus) {
            className[0] = 0;
            GetClassNameW(info.hwndFocus, className, static_cast<int>(std::size(className)));
            lastNativeFocusClass_ = className;
        }
        if (lastNativeThreadInfo_ && info.hwndFocus && IsChild(foreground, info.hwndFocus))
            lastNativeFocusState_ = nativeEditState(info.hwndFocus);
        lastAutomationFocusStatus_ = E_FAIL;
        lastAutomationControlType_ = 0;
        IUIAutomationElement* element = nullptr;
        if (automation_) lastAutomationFocusStatus_ = automation_->GetFocusedElement(&element);
        if (SUCCEEDED(lastAutomationFocusStatus_) && element) {
            lastAutomationFocusStatus_ = element->get_CurrentControlType(&lastAutomationControlType_);
        }
        if (element) element->Release();
        if (lastNativeFocusState_ >= 0) return lastNativeFocusState_ == 1;
        return SUCCEEDED(lastAutomationFocusStatus_) &&
            (lastAutomationControlType_ == UIA_EditControlTypeId || lastAutomationControlType_ == UIA_DocumentControlTypeId);
    }

    void rememberTarget()
    {
        if (smokeMilliseconds_ != -2 && smokeMilliseconds_ != -4 && smokeMilliseconds_ != -7 && smokeMilliseconds_ != -9 && smokeMilliseconds_ != -10 && smokeMilliseconds_ != -11 && smokeMilliseconds_ != -13) {
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

    void startListening(bool hold = false)
    {
        if (learnWindow_ || listening_.load(std::memory_order_acquire) || captureDrainPending_.load(std::memory_order_acquire)) return;
        copiedFeedback_ = false;
        rememberTarget();
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            sessionApplication_ = applicationForWindow(targetWindow_);
        }
        holdSession_.store(hold, std::memory_order_release);
        holdStartedTick_ = GetTickCount64();
        if (!capture_.start()) {
            holdSession_.store(false, std::memory_order_release);
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
            lastSpeechTick_.store(GetTickCount64(), std::memory_order_release);
            lastCandidateTick_.store(0, std::memory_order_release);
            idleGraceDeadline_ = 0;
            listening_.store(true, std::memory_order_release);
            state_ = ListeningState::Listening;
            collapseBubble();
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    void stopListening()
    {
        if (!listening_.load(std::memory_order_acquire)) return;
        captureDrainPending_.store(true, std::memory_order_release);
        capture_.stop();
        listening_.store(false, std::memory_order_release);
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
        if (!dictionarySmokeRoot_.empty()) return dictionarySmokeRoot_ + L"\\settings.ini";
        PWSTR roaming = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &roaming))) {
            return joinPath(executableDirectory(), L"yanflow.ini");
        }
        const std::wstring directory = joinPath(roaming, L"YanFlow");
        CoTaskMemFree(roaming);
        CreateDirectoryW(directory.c_str(), nullptr);
        return joinPath(directory, L"settings.ini");
    }

    std::wstring dictionaryPath()
    {
        const auto settings = settingsPath();
        const auto userDictionary = settings.substr(0, settings.find_last_of(L"\\/")) + L"\\dictionary.tsv";
        return GetFileAttributesW(userDictionary.c_str()) == INVALID_FILE_ATTRIBUTES
            ? joinPath(executableDirectory(), L"dictionary.tsv") : userDictionary;
    }

    std::wstring userDirectory()
    {
        const auto path = settingsPath();
        return path.substr(0, path.find_last_of(L"\\/"));
    }

    std::wstring applicationForWindow(HWND window)
    {
        DWORD pid = 0;
        if (!window || window == window_ || !GetWindowThreadProcessId(window, &pid)) return L"";
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return L"";
        std::array<wchar_t, 32768> path{};
        DWORD size = static_cast<DWORD>(path.size());
        const BOOL read = QueryFullProcessImageNameW(process, 0, path.data(), &size);
        CloseHandle(process);
        return read ? yanflow::applicationName(std::wstring(path.data(), size)) : L"";
    }

    yanflow::DictionaryProfile dictionaryProfile(const std::wstring& application)
    {
        wchar_t profile[32]{};
        GetPrivateProfileStringW(L"DictionaryApplications", application.c_str(), L"auto", profile,
            static_cast<DWORD>(std::size(profile)), settingsPath().c_str());
        if (std::wcscmp(profile, L"general") == 0) return yanflow::DictionaryProfile::General;
        if (std::wcscmp(profile, L"development") == 0) return yanflow::DictionaryProfile::Development;
        return yanflow::DictionaryProfile::Automatic;
    }

    std::vector<std::wstring> dictionaryLayers(const std::wstring& application)
    {
        return yanflow::dictionaryOverlays(userDirectory(), executableDirectory(), application, dictionaryProfile(application));
    }

    std::wstring applicationDictionaryPath(const std::wstring& application)
    {
        const auto name = yanflow::applicationName(application);
        return name.empty() ? L"" : userDirectory() + L"\\dictionaries\\apps\\" + name + L".tsv";
    }

    void openDictionaryEditor(const std::wstring& path, const std::wstring& templateText)
    {
        if (path.empty()) return;
        const auto parent = path.substr(0, path.find_last_of(L"\\/"));
        const auto upper = parent.find_last_of(L"\\/");
        if (upper != std::wstring::npos) CreateDirectoryW(parent.substr(0, upper).c_str(), nullptr);
        CreateDirectoryW(parent.c_str(), nullptr);
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && !yanflow::writeUtf8File(path, templateText)) {
            MessageBoxW(window_, L"无法创建词典，请检查写入权限。", L"言流词典", MB_OK | MB_ICONWARNING); return;
        }
        ShellExecuteW(window_, L"open", L"notepad.exe", quoteArgument(path).c_str(), nullptr, SW_SHOWNORMAL);
    }

    void editApplicationDictionary()
    {
        openDictionaryEditor(applicationDictionaryPath(menuApplication_),
            L"# Application dictionary; overrides matching aliases in general/development dictionaries.\n# kind\tsource\tcanonical\tcontext\n");
    }
    void editDevelopmentDictionary()
    {
        openDictionaryEditor(userDirectory() + L"\\dictionaries\\development.tsv",
            yanflow::readUtf8File(joinPath(executableDirectory(), L"dictionary-development.tsv")));
    }

    void editDictionary()
    {
        const auto settings = settingsPath();
        const auto userDictionary = settings.substr(0, settings.find_last_of(L"\\/")) + L"\\dictionary.tsv";
        if (GetFileAttributesW(userDictionary.c_str()) == INVALID_FILE_ATTRIBUTES &&
            !CopyFileW(joinPath(executableDirectory(), L"dictionary.tsv").c_str(), userDictionary.c_str(), TRUE)) {
            MessageBoxW(window_, L"无法创建自定义词典，请检查目录写入权限。", L"言流词典", MB_OK | MB_ICONWARNING);
            return;
        }
        const auto argument = quoteArgument(userDictionary);
        ShellExecuteW(window_, L"open", L"notepad.exe", argument.c_str(), nullptr, SW_SHOWNORMAL);
    }

    static std::wstring controlText(HWND window, int id)
    {
        const HWND control = GetDlgItem(window, id);
        const int length = GetWindowTextLengthW(control);
        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        GetWindowTextW(control, text.data(), length + 1);
        text.resize(static_cast<size_t>(length));
        return text;
    }

    static bool copyText(HWND owner, const std::wstring& text)
    {
        if (text.empty()) return false;
        bool opened = false;
        for (int attempt = 0; attempt < 8; ++attempt) {
            if (OpenClipboard(owner)) { opened = true; break; }
            if (attempt < 7) Sleep(10);
        }
        if (!opened) return false;
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, bytes);
        bool copied = false;
        if (data) {
            if (void* memory = GlobalLock(data)) {
                memcpy(memory, text.c_str(), bytes); GlobalUnlock(data);
                if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, data)) copied = true;
            }
            if (!copied) GlobalFree(data);
        }
        CloseClipboard(); return copied;
    }

    bool rememberCorrectionFromWindow()
    {
        yanflow::DictionaryRule rule{L"term", controlText(learnWindow_, kLearnSource),
            controlText(learnWindow_, kLearnTarget), controlText(learnWindow_, kLearnContext)};
        const auto asciiFold = [](std::wstring value) {
            for (auto& c : value) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
            return value;
        };
        if (asciiFold(rule.source) == asciiFold(rule.target)) rule.kind = L"case";
        yanflow::TextDictionary check;
        if (check.load(rule.kind + L"\t" + rule.source + L"\t" + rule.target + L"\t" + rule.context) != 1) {
            MessageBoxW(learnWindow_, L"请填写有效的误识别词和正确词。", L"记住纠正", MB_OK | MB_ICONWARNING); return false;
        }
        const bool general = SendMessageW(GetDlgItem(learnWindow_, kLearnScope), CB_GETCURSEL, 0, 0) == 1 || learningResult_.application.empty();
        const auto tsv = rule.kind + L"\t" + rule.source + L"\t" + rule.target + L"\t" + rule.context;
        if (check.apply(learningResult_.raw).text == learningResult_.raw) {
            MessageBoxW(learnWindow_, L"原词须来自上方的原始识别，并符合上下文；代码片段受到保护。",
                L"记住纠正", MB_OK | MB_ICONWARNING); return false;
        }
        yanflow::TextDictionary baseline, proposed;
        const auto global = yanflow::readUtf8File(dictionaryPath());
        baseline.load(global); proposed.load(global);
        if (general) proposed.overlay(tsv);
        for (const auto& layer : dictionaryLayers(learningResult_.application)) {
            const auto contents = yanflow::readUtf8File(layer);
            baseline.overlay(contents); proposed.overlay(contents);
        }
        if (!general) proposed.overlay(tsv);
        const auto originalAutomatic = baseline.apply(learningResult_.raw).text;
        const auto proposedAutomatic = proposed.apply(learningResult_.raw).text;
        if (proposedAutomatic == originalAutomatic) {
            MessageBoxW(learnWindow_, L"规则未产生变化；该应用可能已有更具体的映射。请选择仅此应用，或编辑应用词库。",
                L"记住纠正", MB_OK | MB_ICONWARNING); return false;
        }
        std::wstring corrected = check.apply(learningResult_.final).text;
        if (corrected == learningResult_.final && originalAutomatic == learningResult_.final) corrected = proposedAutomatic;
        if (corrected == learningResult_.final || corrected == learningResult_.raw) {
            MessageBoxW(learnWindow_, L"规则未能修改这次识别结果。请检查原词、上下文；代码片段受到保护。",
                L"记住纠正", MB_OK | MB_ICONWARNING); return false;
        }
        const auto path = general ? userDirectory() + L"\\dictionary.tsv" : applicationDictionaryPath(learningResult_.application);
        // Creating the first personal general dictionary must retain the packaged defaults.
        if (general && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
            !CopyFileW(joinPath(executableDirectory(), L"dictionary.tsv").c_str(), path.c_str(), TRUE)) {
            MessageBoxW(learnWindow_, L"无法创建通用词典，请检查写入权限。", L"记住纠正", MB_OK | MB_ICONWARNING); return false;
        }
        std::wstring error;
        if (!yanflow::rememberDictionaryRule(path, rule, error)) {
            MessageBoxW(learnWindow_, error.c_str(), L"记住纠正", MB_OK | MB_ICONWARNING); return false;
        }
        const bool sameResult = lastRecognition_.serial == learningResult_.serial;
        for (auto& item : recentRecognitions_)
            if (item.serial == learningResult_.serial) item.manualCorrection = corrected;
        if (sameResult) {
            lastManualCorrection_ = corrected; lastRecognition_.manualCorrection = corrected;
        }
        if (sameResult && fallbackIsTranscript_ && fallbackText_.size() >= learningResult_.final.size() &&
            fallbackText_.compare(fallbackText_.size() - learningResult_.final.size(), learningResult_.final.size(), learningResult_.final) == 0) {
            fallbackText_.replace(fallbackText_.size() - learningResult_.final.size(), learningResult_.final.size(), corrected);
        } else if (fallbackText_.empty() || !fallbackIsTranscript_) {
            fallbackText_ = corrected; fallbackIsTranscript_ = true;
        }
        copiedFeedback_ = copyText(window_, corrected);

        expandBubble(); InvalidateRect(window_, nullptr, FALSE);
        DestroyWindow(learnWindow_);
        if (!copiedFeedback_) MessageBoxW(window_, L"规则已保存；剪贴板暂时不可用，可从气泡重新复制正确文本。",
            L"已记住纠正", MB_OK | MB_ICONINFORMATION);
        return true;
    }

    static LRESULT CALLBACK learnProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<YanFlowApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = reinterpret_cast<YanFlowApp*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); self->learnWindow_ = window;
        }
        if (!self) return DefWindowProcW(window, message, wParam, lParam);
        if (message == DM_GETDEFID) return MAKELONG(kLearnSave, DC_HASDEFID);
        if (message == WM_COMMAND) {
            if (LOWORD(wParam) == kLearnSave) self->rememberCorrectionFromWindow();
            if (LOWORD(wParam) == kLearnCancel || LOWORD(wParam) == IDCANCEL) DestroyWindow(window);
            return 0;
        }
        if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
        if (message == WM_DESTROY) { self->learnWindow_ = nullptr; return 0; }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void showLearnWindow()
    {
        if (lastRecognition_.raw.empty()) return;
        if (learnWindow_) { SetForegroundWindow(learnWindow_); return; }
        stopListening();
        learningResult_ = lastRecognition_;
        if (!lastManualCorrection_.empty()) learningResult_.final = lastManualCorrection_;
        WNDCLASSEXW type{};
        type.cbSize = sizeof(type); type.hInstance = instance_; type.lpfnWndProc = &YanFlowApp::learnProcedure;
        type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        type.hIcon = applicationIcon_; type.lpszClassName = L"YanFlowRememberCorrection";
        RegisterClassExW(&type);
        learnWindow_ = CreateWindowExW(WS_EX_DLGMODALFRAME, type.lpszClassName, L"纠正并记住", WS_CAPTION | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, 560, 430, window_, nullptr, instance_, this);
        if (!learnWindow_) return;
        const auto label = [&](const wchar_t* text, int top) {
            CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, 24, top, 480, 22, learnWindow_, nullptr, instance_, nullptr);
        };
        const auto edit = [&](int id, int left, int top, int width, int height, const std::wstring& text, DWORD style) {
            return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                left, top, width, height, learnWindow_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        };
        label(L"最近原始识别（只读，可复制）", 18);
        edit(kLearnPreview, 24, 44, 490, 64, learningResult_.raw, ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL);
        label(L"误识别词                         正确词", 120);
        const auto raw = learningResult_.raw.size() <= 128 ? learningResult_.raw : L"";
        const auto final = learningResult_.final.size() <= 128 ? learningResult_.final : L"";
        HWND source = edit(kLearnSource, 24, 146, 235, 28, raw, ES_AUTOHSCROLL);
        HWND target = edit(kLearnTarget, 279, 146, 235, 28, final, ES_AUTOHSCROLL);
        label(L"上下文（建议填写；中文单字替换必填）", 190);
        HWND context = edit(kLearnContext, 24, 216, 490, 28, L"", ES_AUTOHSCROLL);
        for (HWND control : {source, target, context}) SendMessageW(control, EM_SETLIMITTEXT, 128, 0);
        label(L"适用范围", 260);
        HWND scope = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            120, 255, 394, 120, learnWindow_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLearnScope)), instance_, nullptr);
        const auto application = learningResult_.application.empty() ? L"此应用不可识别，使用通用词库" : L"仅此应用：" + learningResult_.application;
        SendMessageW(scope, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(application.c_str()));
        SendMessageW(scope, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"通用（所有应用）"));
        SendMessageW(scope, CB_SETCURSEL, learningResult_.application.empty() ? 1 : 0, 0);
        label(L"保存规则并复制本次正确文本；已落字的内容可粘贴更正。", 296);
        CreateWindowExW(0, L"BUTTON", L"记住并复制", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            284, 332, 130, 32, learnWindow_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLearnSave)), instance_, nullptr);
        CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            434, 332, 80, 32, learnWindow_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLearnCancel)), instance_, nullptr);
        EnumChildWindows(learnWindow_, [](HWND control, LPARAM) -> BOOL {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE); return TRUE;
        }, 0);
        ShowWindow(learnWindow_, SW_SHOW); SetForegroundWindow(learnWindow_);
        SetFocus(target); SendMessageW(target, EM_SETSEL, 0, -1);
    }

    bool runLearnSmoke()
    {
        const auto settings = settingsPath();
        WritePrivateProfileStringW(L"Hotkeys", L"SchemaVersion", L"1", settings.c_str());
        WritePrivateProfileStringW(L"Hotkeys", L"StartModifiers", L"3", settings.c_str());
        WritePrivateProfileStringW(L"Hotkeys", L"StartKey", L"32", settings.c_str());
        WritePrivateProfileStringW(L"Hotkeys", L"StopModifiers", L"3", settings.c_str());
        WritePrivateProfileStringW(L"Hotkeys", L"StopKey", L"83", settings.c_str());
        loadSettings();
        if (startHotkey_.virtualKey || stopHotkey_.virtualKey || !holdEnabled_) return false;
        WritePrivateProfileStringW(L"Hotkeys", L"SchemaVersion", L"1", settings.c_str());
        WritePrivateProfileStringW(L"Hotkeys", L"StartModifiers", L"3", settings.c_str());
        WritePrivateProfileStringW(L"Hotkeys", L"StartKey", L"117", settings.c_str());
        WritePrivateProfileStringW(L"Hotkeys", L"StopModifiers", L"6", settings.c_str());
        WritePrivateProfileStringW(L"Hotkeys", L"StopKey", L"118", settings.c_str());
        loadSettings();
        if (startHotkey_.virtualKey != VK_F6 || stopHotkey_.virtualKey != VK_F7 ||
            startHotkey_.modifiers != (MOD_CONTROL | MOD_ALT) || stopHotkey_.modifiers != (MOD_CONTROL | MOD_SHIFT)) return false;
        lastRecognition_ = {L"语音识别支持热此", L"语音识别支持热此", L"yanflow-learning-smoke.exe"};
        recentRecognitions_.push_back(lastRecognition_);
        showLearnWindow();
        if (!learnWindow_) return false;
        if (applicationForWindow(learnWindow_) != L"yanflow.exe") return false;
        SetWindowTextW(GetDlgItem(learnWindow_, kLearnSource), L"热此");
        SetWindowTextW(GetDlgItem(learnWindow_, kLearnTarget), L"热词");
        SetWindowTextW(GetDlgItem(learnWindow_, kLearnContext), L"语音识别");
        SendMessageW(learnWindow_, WM_COMMAND, kLearnSave, 0);
        if (learnWindow_ || fallbackText_ != L"语音识别支持热词" ||
            lastRecognition_.final != L"语音识别支持热此" || recentRecognitions_.back().final != lastRecognition_.final ||
            recentRecognitions_.back().manualCorrection != fallbackText_) return false;
        yanflow::TextPipeline pipeline;
        const auto raw = L"语音识别支持热此";
        const auto learned = pipeline.correct(raw, executableDirectory(), dictionaryPath(), false,
            dictionaryLayers(L"yanflow-learning-smoke.exe"));
        const auto other = pipeline.correct(raw, executableDirectory(), dictionaryPath(), false,
            dictionaryLayers(L"other-app.exe"));
        if (learned != L"语音识别支持热词" || other != raw) return false;
        if (!WritePrivateProfileStringW(L"DictionaryApplications", L"other-app.exe", L"development", settingsPath().c_str()) ||
            dictionaryProfile(L"other-app.exe") != yanflow::DictionaryProfile::Development) return false;
        return copiedFeedback_;
    }

    void loadSettings()
    {
        const std::wstring path = settingsPath();
        holdEnabled_ = GetPrivateProfileIntW(L"Hotkeys", L"HoldEnabled", 1, path.c_str()) != 0;
        cscEnabled_.store(GetPrivateProfileIntW(L"Correction", L"MacBERTEnabled", 0, path.c_str()) != 0);
        startHotkey_.modifiers = GetPrivateProfileIntW(L"Hotkeys", L"StartModifiers", 0, path.c_str());
        startHotkey_.virtualKey = GetPrivateProfileIntW(L"Hotkeys", L"StartKey", 0, path.c_str());
        stopHotkey_.modifiers = GetPrivateProfileIntW(L"Hotkeys", L"StopModifiers", 0, path.c_str());
        stopHotkey_.virtualKey = GetPrivateProfileIntW(L"Hotkeys", L"StopKey", 0, path.c_str());
        startHotkey_.modifiers &= MOD_CONTROL | MOD_ALT | MOD_SHIFT;
        stopHotkey_.modifiers &= MOD_CONTROL | MOD_ALT | MOD_SHIFT;
        if (GetPrivateProfileIntW(L"Hotkeys", L"SchemaVersion", 0, path.c_str()) < 2) {
            if (sameHotkey(startHotkey_, HotkeyBinding{MOD_CONTROL | MOD_ALT, VK_SPACE}) &&
                sameHotkey(stopHotkey_, HotkeyBinding{MOD_CONTROL | MOD_ALT, 'S'})) {
                startHotkey_ = {}; stopHotkey_ = {};
            }
            persistSettings();
        }
    }

    void persistSettings()
    {
        const std::wstring path = settingsPath();
        WritePrivateProfileStringW(L"Hotkeys", L"SchemaVersion", L"2", path.c_str());
        WritePrivateProfileStringW(L"Hotkeys", L"HoldEnabled", holdEnabled_ ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"Correction", L"MacBERTEnabled", cscEnabled_.load() ? L"1" : L"0", path.c_str());
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
        const auto registerKeys = [this](const HotkeyBinding& first, const HotkeyBinding& second) {
            const bool shared = first.virtualKey && sameHotkey(first, second);
            const bool firstOk = !first.virtualKey || RegisterHotKey(window_, kHotkeyStart, first.modifiers | MOD_NOREPEAT, first.virtualKey);
            const bool secondOk = !second.virtualKey || shared || (firstOk && RegisterHotKey(window_, kHotkeyStop, second.modifiers | MOD_NOREPEAT, second.virtualKey));
            startHotkeyRegistered_ = firstOk && first.virtualKey;
            stopHotkeyRegistered_ = secondOk && second.virtualKey;
            return firstOk && secondOk;
        };
        if (replaceExisting) { UnregisterHotKey(window_, kHotkeyStart); UnregisterHotKey(window_, kHotkeyStop); }
        if (registerKeys(start, stop)) return true;
        UnregisterHotKey(window_, kHotkeyStart); UnregisterHotKey(window_, kHotkeyStop);
        if (replaceExisting) registerKeys(startHotkey_, stopHotkey_);
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
        if ((start.virtualKey && (sameHotkey(start, visibility) || sameHotkey(start, copy))) ||
            (stop.virtualKey && (sameHotkey(stop, visibility) || sameHotkey(stop, copy)))) {
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
        holdEnabled_ = SendMessageW(GetDlgItem(settingsWindow_, kSettingsHold), BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (!holdEnabled_ && holdSession_.load()) stopListening();
        holdKeys_.reset(); toggleKeys_.reset();
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
            CW_USEDEFAULT, CW_USEDEFAULT, 420, 300, window_, nullptr, instance_, this);
        if (settingsWindow_ == nullptr) return;
        CreateWindowExW(0, L"STATIC", L"Ctrl+Win+Shift：切换实时监听", WS_CHILD | WS_VISIBLE,
            28, 12, 360, 24, settingsWindow_, nullptr, instance_, nullptr);
        CreateWindowExW(0, L"STATIC", L"额外开始", WS_CHILD | WS_VISIBLE,
            28, 56, 90, 24, settingsWindow_, nullptr, instance_, nullptr);
        HWND startControl = CreateWindowExW(WS_EX_CLIENTEDGE, HOTKEY_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 125, 52, 220, 28, settingsWindow_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsStart)), instance_, nullptr);
        CreateWindowExW(0, L"STATIC", L"额外停止", WS_CHILD | WS_VISIBLE,
            28, 100, 90, 24, settingsWindow_, nullptr, instance_, nullptr);
        HWND stopControl = CreateWindowExW(WS_EX_CLIENTEDGE, HOTKEY_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 125, 96, 220, 28, settingsWindow_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsStop)), instance_, nullptr);
        HWND holdControl = CreateWindowExW(0, L"BUTTON",
            keyboardHook_ ? L"Ctrl+Win：按住录音，松开识别" : L"Ctrl+Win：键盘监听不可用，请重启",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            28, 139, 360, 24, settingsWindow_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsHold)), instance_, nullptr);
        SendMessageW(holdControl, BM_SETCHECK, holdEnabled_ ? BST_CHECKED : BST_UNCHECKED, 0);
        CreateWindowExW(0, L"STATIC", L"额外快捷键可留空；按住模式最长 60 秒",
            WS_CHILD | WS_VISIBLE, 28, 173, 360, 24, settingsWindow_, nullptr, instance_, nullptr);
        CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            175, 215, 80, 30, settingsWindow_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsSave)), instance_, nullptr);
        CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            265, 215, 80, 30, settingsWindow_,
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
        menuApplication_ = applicationForWindow(GetForegroundWindow());
        if (menuApplication_.empty()) menuApplication_ = lastRecognition_.application;
        HMENU menu = CreatePopupMenu();
        const std::wstring listeningLabel = (listening_.load() ? L"关闭实时监听\t" : L"开启实时监听\t") +
            std::wstring(keyboardHook_ ? L"Ctrl+Win+Shift" : L"键盘监听不可用");
        AppendMenuW(menu, MF_STRING, kCommandToggle, listeningLabel.c_str());
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, holdEnabled_
            ? (keyboardHook_ ? L"按住录音\tCtrl+Win" : L"按住录音：键盘监听不可用") : L"按住录音：已关闭");
        AppendMenuW(menu, MF_STRING | (fallbackText_.empty() ? MF_GRAYED : 0), kCommandCopy,
            L"复制文本\tCtrl+Alt+C");
        AppendMenuW(menu, MF_STRING | (!fallbackIsTranscript_ || fallbackText_.empty() ? MF_GRAYED : 0),
            kCommandWrite, L"写入桌面 TXT");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCommandSettings, L"快捷键设置...");
        AppendMenuW(menu, MF_STRING, kCommandDictionary, L"编辑自定义词典...");
        HMENU dictionaries = CreatePopupMenu();
        const auto profile = dictionaryProfile(menuApplication_);
        const UINT available = menuApplication_.empty() ? MF_GRAYED : 0;
        AppendMenuW(dictionaries, MF_STRING | available | (profile == yanflow::DictionaryProfile::Automatic ? MF_CHECKED : 0),
            kCommandProfileAuto, L"自动选择（开发 / 通用）");
        AppendMenuW(dictionaries, MF_STRING | available | (profile == yanflow::DictionaryProfile::General ? MF_CHECKED : 0),
            kCommandProfileGeneral, L"通用词库");
        AppendMenuW(dictionaries, MF_STRING | available | (profile == yanflow::DictionaryProfile::Development ? MF_CHECKED : 0),
            kCommandProfileDevelopment, L"通用＋开发词库");
        AppendMenuW(dictionaries, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(dictionaries, MF_STRING | available, kCommandAppDictionary, L"编辑此应用专用词库...");
        AppendMenuW(dictionaries, MF_STRING, kCommandDevelopmentDictionary, L"编辑开发词库...");
        const auto title = menuApplication_.empty() ? L"应用词库" : L"应用词库：" + menuApplication_;
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(dictionaries), title.c_str());
        AppendMenuW(menu, MF_STRING | (lastRecognition_.raw.empty() ? MF_GRAYED : 0), kCommandLearn, L"纠正并记住...");
        AppendMenuW(menu, MF_STRING | (cscEnabled_.load() ? MF_CHECKED : 0), kCommandCsc,
            cscFailed_.load() ? L"MacBERT 中文纠错（加载失败，已回退）" :
                !cscEnabled_.load() ? L"MacBERT 中文纠错（已关闭）" : cscReady_.load() ? L"MacBERT 中文纠错（已就绪）" : L"MacBERT 中文纠错（正在加载）");
        if (cscFailed_.load()) AppendMenuW(menu, MF_STRING, kCommandCscDetails, L"查看 MacBERT 加载失败原因...");
        AppendMenuW(menu, MF_STRING | (lastRecognition_.raw.empty() ? MF_GRAYED : 0), kCommandCompare,
            (L"保存最近识别对照（" + std::to_wstring(recentRecognitions_.size()) + L"条）...").c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCommandExit, L"退出言流");
        POINT cursor = {};
        GetCursorPos(&cursor);
        SetForegroundWindow(window_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window_, nullptr);
        DestroyMenu(menu);
    }

    void paintContent(HDC memory, RECT rect)
    {
        HBRUSH background = CreateSolidBrush(RGB(24, 27, 38));
        FillRect(memory, &rect, background);
        DeleteObject(background);

        HICON currentIcon = (state_ == ListeningState::Listening ||
            state_ == ListeningState::Recognizing) ? listeningIcon_ : floatingIcon_;
        const bool expanded = rect.right > kCollapsedSize;
        const int iconSize = expanded ? 40 : kCollapsedSize;
        const int iconLeft = expanded ? 16 : 0;
        const int iconTop = (rect.bottom - iconSize) / 2;
        if (currentIcon != nullptr) {
            DrawIconEx(memory, iconLeft, iconTop, currentIcon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
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

        if (expanded) {
            SetBkMode(memory, TRANSPARENT);
            SetTextColor(memory, RGB(238, 241, 250));
            HFONT font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH, L"Microsoft YaHei UI");
            HGDIOBJ oldFont = SelectObject(memory, font);
            RECT textRect = {72, 12, rect.right - 80, rect.bottom - 12};
            RECT measured = textRect;
            DrawTextW(memory, fallbackText_.c_str(), -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            textRect.top += std::max(0L, (textRect.bottom - textRect.top - (measured.bottom - measured.top)) / 2);
            DrawTextW(memory, fallbackText_.c_str(), -1, &textRect,
                DT_LEFT | DT_VCENTER | DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(memory, oldFont);
            DeleteObject(font);

            HFONT actionFont = CreateFontW(-13, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH, L"Microsoft YaHei UI");
            oldFont = SelectObject(memory, actionFont);
            const auto action = [&](bool write) {
                auto button = bubbleActionRect(write, rect.right, rect.bottom, fallbackIsTranscript_);
                HBRUSH brush = CreateSolidBrush(RGB(36, 42, 57));
                auto oldBrush = SelectObject(memory, brush);
                auto oldPen = SelectObject(memory, GetStockObject(NULL_PEN));
                RoundRect(memory, button.left, button.top, button.right, button.bottom, 10, 10);
                SelectObject(memory, oldPen); SelectObject(memory, oldBrush); DeleteObject(brush);
                SetTextColor(memory, write ? RGB(255, 199, 122) : copiedFeedback_ ? RGB(112, 224, 173) : RGB(164, 211, 255));
                DrawTextW(memory, write ? L"写入" : copiedFeedback_ ? L"关闭" : L"复制", -1, &button,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            };
            action(false);
            if (fallbackIsTranscript_) action(true);
            SelectObject(memory, oldFont);
            DeleteObject(actionFont);
        }
    }

    void paint()
    {
        PAINTSTRUCT paintStruct{};
        HDC target = BeginPaint(window_, &paintStruct);
        RECT rect{}; GetClientRect(window_, &rect);
        HDC memory = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, rect.right, rect.bottom);
        auto oldBitmap = SelectObject(memory, bitmap);
        paintContent(memory, rect);
        BitBlt(target, 0, 0, rect.right, rect.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, oldBitmap); DeleteObject(bitmap); DeleteDC(memory);
        EndPaint(window_, &paintStruct);
    }

    bool saveUiPreview(const wchar_t* name, bool expanded)
    {
        const SIZE size = expanded ? measureBubble() : SIZE{kCollapsedSize, kCollapsedSize};
        const int width = size.cx;
        const int height = size.cy;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        HDC memory = CreateCompatibleDC(nullptr);
        HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!memory || !bitmap || !pixels) { if (bitmap) DeleteObject(bitmap); if (memory) DeleteDC(memory); return false; }
        auto old = SelectObject(memory, bitmap);
        paintContent(memory, RECT{0, 0, width, height});
        GdiFlush();
        BITMAPFILEHEADER header{};
        header.bfType = 0x4d42; header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
        const DWORD bytes = width * height * 4;
        header.bfSize = header.bfOffBits + bytes;
        FILE* file = nullptr;
        const auto path = joinPath(executableDirectory(), name);
        const bool opened = _wfopen_s(&file, path.c_str(), L"wb") == 0 && file;
        bool saved = opened && fwrite(&header, sizeof(header), 1, file) == 1 &&
            fwrite(&info.bmiHeader, sizeof(info.bmiHeader), 1, file) == 1 && fwrite(pixels, bytes, 1, file) == 1;
        if (file && fclose(file)) saved = false;
        SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory);
        return saved;
    }

    void endpointLoop()
    {
        std::array<int16_t, kFramesPerBuffer> frame{};
        std::deque<int16_t> preRoll;
        std::vector<int16_t> utterance;
        bool speech = false, wasListening = false, continuation = false;
        int voicedFrames = 0, silentFrames = 0;
        size_t speechVoicedFrames = 0, ownedBegin = 0, streamOffset = 0;
        float noiseFloor = 0.004f, previousInput = 0.0f, previousFiltered = 0.0f;
        const auto flush = [&](bool held) {
            if (held && speechVoicedFrames < 18 && silentFrames > 5) {
                const size_t tail = static_cast<size_t>(silentFrames - 5) * kFramesPerBuffer;
                if (tail < utterance.size() - ownedBegin) utterance.resize(utterance.size() - tail);
            }
            const size_t ownedSamples = utterance.size() > ownedBegin ? utterance.size() - ownedBegin : 0;
            if (speech && yanflow::submitSpeech(held, ownedSamples, speechVoicedFrames, continuation)) {
                const bool shortHold = held && utterance.size() <= kSampleRate && speechVoicedFrames < 18;
                if (shortHold) yanflow::padShortRecording(utterance);
                enqueueUtterance(std::move(utterance), true, ownedBegin, static_cast<size_t>(-1), shortHold, continuation, streamOffset);
            }
        };
        while (workerRunning_.load(std::memory_order_acquire)) {
            const bool recording = listening_.load(std::memory_order_acquire);
            const bool held = holdSession_.load(std::memory_order_acquire);
            const size_t count = recording || captureDrainPending_.load(std::memory_order_acquire)
                ? capture_.read(frame.data(), frame.size(), !recording) : 0;
            if (!recording && count == 0) {
                if (wasListening) flush(held);
                speech = false; voicedFrames = silentFrames = 0;
                speechVoicedFrames = ownedBegin = streamOffset = 0; continuation = false;
                preRoll.clear(); utterance.clear(); noiseFloor = 0.004f;
                previousInput = previousFiltered = 0.0f; wasListening = false;
                if (captureDrainPending_.load(std::memory_order_acquire)) holdSession_.store(false, std::memory_order_release);
                captureDrainPending_.store(false, std::memory_order_release);
                Sleep(20); continue;
            }
            wasListening = true;
            if (!count) { Sleep(4); continue; }
            double squares = 0;
            for (size_t i = 0; i < count; ++i) {
                const float input = static_cast<float>(frame[i]) / 32768.0f;
                const float filtered = input - previousInput + 0.954f * previousFiltered;
                previousInput = input; previousFiltered = filtered;
                frame[i] = static_cast<int16_t>(std::clamp(filtered * 32768.0f, -32768.0f, 32767.0f));
                squares += filtered * filtered;
            }
            const float rms = static_cast<float>(std::sqrt(squares / count));
            const bool voiced = rms >= std::max(0.0035f, noiseFloor * 2.5f);
            if (!speech) {
                if (!voiced) noiseFloor = noiseFloor * 0.98f + std::min(rms, 0.03f) * 0.02f;
                preRoll.insert(preRoll.end(), frame.begin(), frame.begin() + count);
                while (preRoll.size() > static_cast<size_t>(kSampleRate * 0.30f)) preRoll.pop_front();
                voicedFrames = voiced ? voicedFrames + 1 : 0;
                if (voicedFrames >= (held ? 2 : 3)) {
                    speech = true; speechVoicedFrames = static_cast<size_t>(voicedFrames);
                    lastCandidateTick_.store(GetTickCount64(), std::memory_order_release);
                    utterance.assign(preRoll.begin(), preRoll.end()); preRoll.clear(); silentFrames = 0;
                }
            } else {
                utterance.insert(utterance.end(), frame.begin(), frame.begin() + count);
                if (voiced) { ++speechVoicedFrames; lastCandidateTick_.store(GetTickCount64(), std::memory_order_release); }
                silentFrames = voiced ? 0 : silentFrames + 1;
                if (held && utterance.size() >= static_cast<size_t>(kSampleRate * 60)) {
                    utterance.resize(kSampleRate * 60); PostMessageW(window_, kMessageHold, 0, 0);
                }
                if (!held && silentFrames >= 28) {
                    const size_t tail = static_cast<size_t>(std::min(silentFrames, 12) * kFramesPerBuffer);
                    if (tail < utterance.size() - ownedBegin) utterance.resize(utterance.size() - tail);
                    flush(false); utterance.clear(); speech = false; voicedFrames = silentFrames = 0;
                    speechVoicedFrames = ownedBegin = streamOffset = 0; continuation = false;
                } else if (!held && utterance.size() >= kMaximumUtteranceSamples) {
                    const auto boundary = yanflow::chooseAudioBoundary(utterance);
                    const size_t overlap = boundary.quiet ? 0 : yanflow::kBoundaryOverlap;
                    std::vector<int16_t> clip(utterance.begin(), utterance.begin() + boundary.sample + overlap);
                    enqueueUtterance(std::move(clip), true, ownedBegin, boundary.sample, false, continuation, streamOffset);
                    utterance.erase(utterance.begin(), utterance.begin() + boundary.sample - overlap);
                    streamOffset += boundary.sample - overlap;
                    ownedBegin = overlap; continuation = true;
                }
            }
        }
    }

    struct QueuedUtterance {
        std::vector<int16_t> samples;
        bool useVad = false;
        bool shortHold = false, continuation = false;
        size_t ownedBegin = 0, ownedEnd = 0;
        std::wstring application;
        uint64_t serial = 0;
        size_t streamOffset = 0;
    };

    void enqueueUtterance(std::vector<int16_t>&& samples, bool useVad = true,
        size_t ownedBegin = 0, size_t ownedEnd = static_cast<size_t>(-1), bool shortHold = false, bool continuation = false,
        size_t streamOffset = 0)
    {
        const auto serial = submittedUtterances_.fetch_add(1) + 1;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (utterances_.size() >= 3) utterances_.pop_front();
            QueuedUtterance utterance;
            utterance.samples = std::move(samples);
            utterance.useVad = useVad;
            utterance.shortHold = shortHold;
            utterance.continuation = continuation;
            utterance.ownedBegin = ownedBegin;
            utterance.ownedEnd = std::min(ownedEnd, utterance.samples.size());
            utterance.application = sessionApplication_;
            utterance.serial = serial;
            utterance.streamOffset = streamOffset;
            utterances_.push_back(std::move(utterance));
        }
        queueChanged_.notify_one();
    }

    void inferenceLoop()
    {
        unsigned int cscRevision = ~0u;
        if (smokeMilliseconds_ <= 0 && smokeMilliseconds_ != -3 && smokeMilliseconds_ != -8 && smokeMilliseconds_ != -12 && smokeMilliseconds_ != -13) {
            std::wstring error;
            if (!asrWorker_.warmup(executableDirectory(), error)) {
                PostMessageW(window_, kMessageStatus, static_cast<WPARAM>(ListeningState::Error),
                    reinterpret_cast<LPARAM>(new std::wstring(error)));
            }
        }
        const auto reportCsc = [this](unsigned int revision, bool ready) {
            auto* status = new CscStatus{revision, ready, textPipeline_.cscError()};
            if (!PostMessageW(window_, kMessageCsc, 0, reinterpret_cast<LPARAM>(status))) delete status;
        };
        while (workerRunning_.load(std::memory_order_acquire)) {
            const auto requestedRevision = cscRevision_.load();
            if (requestedRevision != cscRevision) {
                textPipeline_.resetCsc(); cscRevision = requestedRevision;
                const bool ready = cscEnabled_.load() && textPipeline_.warmupCsc(executableDirectory());
                reportCsc(cscRevision, ready);
            }
            QueuedUtterance utterance;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueChanged_.wait(lock, [this, cscRevision] {
                    return !workerRunning_.load(std::memory_order_acquire) || !utterances_.empty() || cscRevision_.load() != cscRevision;
                });
                if (!workerRunning_.load(std::memory_order_acquire)) break;
                if (cscRevision_.load() != cscRevision) continue;
                utterance = std::move(utterances_.front());
                utterances_.pop_front();
            }
            PostMessageW(window_, kMessageStatus, static_cast<WPARAM>(ListeningState::Recognizing), 0);
            std::wstring text;
            std::wstring error;
            if (transcribe(utterance, text, error) && !text.empty()) {
                auto* result = new RecognitionResult;
                result->raw = text;
                result->application = utterance.application;
                result->serial = utterance.serial;
                FILETIME time{}; GetSystemTimePreciseAsFileTime(&time);
                result->timestamp = (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
                const auto revision = cscRevision_.load();
                if (revision != cscRevision) { textPipeline_.resetCsc(); cscRevision = revision; }
                const bool enabled = cscEnabled_.load();
                result->final = textPipeline_.correct(text, executableDirectory(), dictionaryPath(), enabled,
                    dictionaryLayers(utterance.application));
                result->correctionStatus = !enabled ? L"disabled" : textPipeline_.cscFailed() ? L"failed" :
                    text.size() > 4096 ? L"skipped_length" : L"ready";
                result->correctionError = textPipeline_.cscError();
                reportCsc(cscRevision, enabled && !textPipeline_.cscFailed());
                if (!PostMessageW(window_, kMessageResult, 0, reinterpret_cast<LPARAM>(result))) delete result;
            } else if (!error.empty()) {
                PostMessageW(window_, kMessageStatus, static_cast<WPARAM>(ListeningState::Error),
                    reinterpret_cast<LPARAM>(new std::wstring(error)));
            }
            if (listening_.load(std::memory_order_acquire)) {
                PostMessageW(window_, kMessageStatus, static_cast<WPARAM>(ListeningState::Listening), 0);
            } else if (error.empty()) {
                PostMessageW(window_, kMessageStatus, static_cast<WPARAM>(ListeningState::Idle), 0);
            }
            if (smokeMilliseconds_ == -11) {
                processedStreamingUtterances_.fetch_add(1);
                PostMessageW(window_, kMessageStreamingDone, 0, 0);
            }
        }
    }

    bool transcribe(const QueuedUtterance& utterance, std::wstring& text, std::wstring& error)
    {
        if (!utterance.continuation || utterance.serial != previousTranscriptSerial_ + 1) transcriptMerger_.reset();
        previousTranscriptSerial_ = utterance.serial;
        const auto& samples = utterance.samples;
        text.clear();
        const auto chunks = yanflow::planAudioChunks(samples);
        if (smokeMilliseconds_ == -10 || smokeMilliseconds_ == -11) {
            std::wstring rows = smokeMilliseconds_ == -11 ? streamingSmokeChunks_ : L"begin\tend\townedBegin\townedEnd\n";
            for (const auto& chunk : chunks) rows += std::to_wstring(utterance.streamOffset + chunk.begin) + L"\t" +
                std::to_wstring(utterance.streamOffset + chunk.end) + L"\t" +
                std::to_wstring(utterance.streamOffset + std::max(chunk.ownedBegin, utterance.ownedBegin)) + L"\t" +
                std::to_wstring(utterance.streamOffset + std::min(chunk.ownedEnd, utterance.ownedEnd)) + L"\n";
            if (smokeMilliseconds_ == -11) streamingSmokeChunks_ = rows;
            if (!yanflow::writeUtf8File(joinPath(executableDirectory(), L"yanflow-long-chunks.tsv"), rows)) return false;
        }
        for (const auto& chunk : chunks) {
            std::vector<int16_t> clip(samples.begin() + chunk.begin, samples.begin() + chunk.end);
            std::wstring part;
            std::vector<yanflow::TimedToken> tokens;
            const size_t ownedBegin = std::max(chunk.ownedBegin, utterance.ownedBegin);
            const size_t ownedEnd = std::min(chunk.ownedEnd, utterance.ownedEnd);
            const bool timing = chunks.size() > 1 || utterance.continuation || ownedBegin != chunk.begin || ownedEnd != chunk.end;
            if (!asrWorker_.transcribe(executableDirectory(), clip, utterance.useVad, part, error,
                timing ? &tokens : nullptr, utterance.shortHold)) { transcriptMerger_.reset(); return false; }
            if (timing) {
                if (!part.empty() && tokens.empty()) { error = L"识别引擎没有返回分段时间信息，请重启后重试。"; return false; }
                part = transcriptMerger_.append(tokens, utterance.streamOffset + chunk.begin, utterance.streamOffset + chunk.end,
                    utterance.streamOffset + ownedBegin, utterance.streamOffset + ownedEnd);
            } else {
                transcriptMerger_.reset();
            }
            text += part;
        }
        if (!utterance.continuation) {
            const auto begin = text.find_first_not_of(L' ');
            text = begin == std::wstring::npos ? L"" : text.substr(begin);
        }
        const auto end = text.find_last_not_of(L' ');
        if (end != std::wstring::npos) text.resize(end + 1);
        return true;
    }

    void acceptResult(RecognitionResult* result)
    {
        if (result == nullptr) return;
        lastRecognition_ = std::move(*result);
        lastManualCorrection_.clear();
        const std::wstring& text = lastRecognition_.final;
        delete result;
        state_ = listening_.load(std::memory_order_acquire) ? ListeningState::Listening : ListeningState::Idle;
        if (listening_.load(std::memory_order_acquire)) {
            lastSpeechTick_.store(GetTickCount64(), std::memory_order_release);
            idleGraceDeadline_ = 0;
        }
        if (smokeMilliseconds_ != -2 && smokeMilliseconds_ != -4 && smokeMilliseconds_ != -7 && smokeMilliseconds_ != -9 && smokeMilliseconds_ != -10 && smokeMilliseconds_ != -11 && smokeMilliseconds_ != -13) {
            HWND foreground = GetForegroundWindow();
            if (foreground != nullptr && foreground != window_ &&
                (lastRecognition_.application.empty() || applicationForWindow(foreground) == lastRecognition_.application)) {
                targetWindow_ = foreground;
                targetWasEditable_ = focusedElementAcceptsText();
            }
        }
        const bool targetValid = targetWindow_ != nullptr && IsWindow(targetWindow_);
        const bool foregroundMatch = GetForegroundWindow() == targetWindow_;
        if (targetValid && targetWasEditable_ && foregroundMatch)
            lastRecognition_.delivery = yanflow::deliverText(targetWindow_, window_, text);
        const bool injected = lastRecognition_.delivery.submitted;
        retainRecognition(lastRecognition_);
        copiedFeedback_ = false;
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
                fprintf(diagnostic, "native_edit_state=%d uia_create=0x%08lx uia_focus=0x%08lx uia_type=%d\n",
                    lastNativeFocusState_, static_cast<unsigned long>(automationCreationStatus_),
                    static_cast<unsigned long>(lastAutomationFocusStatus_), lastAutomationControlType_);
                fprintf(diagnostic, "gui_thread_info=%d foreground_class=%s focus_class=%s\n", lastNativeThreadInfo_ ? 1 : 0,
                    wideToUtf8(lastForegroundClass_).c_str(), wideToUtf8(lastNativeFocusClass_).c_str());
                fclose(diagnostic);
            }
            yanflow::writeUtf8File(joinPath(executableDirectory(), L"yanflow-e2e-comparison.jsonl"), recognitionComparisonJson());
            exitCode_ = injected ? 0 : 61;
            DestroyWindow(window_);
        } else if (smokeMilliseconds_ == -2) {
            const auto copyRect = actionRect(false);
            const LPARAM copyPoint = MAKELPARAM((copyRect.left + copyRect.right) / 2, (copyRect.top + copyRect.bottom) / 2);
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
            const bool valid = !injected && rect.right - rect.left <= kBubbleWidth && rect.right - rect.left >= 200 &&
                rect.bottom - rect.top >= kBubbleMinimumHeight && rect.bottom - rect.top <= kBubbleMaximumHeight &&
                fullyVisible && clipboardMatches && copiedFeedback_ && !listening_.load();
            SendMessageW(window_, WM_LBUTTONDOWN, MK_LBUTTON, copyPoint);
            SendMessageW(window_, WM_LBUTTONUP, 0, copyPoint);
            exitCode_ = valid && !bubbleExpanded_ && fallbackText_.empty() && !copiedFeedback_ ? 0 : 30;
            DestroyWindow(window_);
        } else if (smokeMilliseconds_ == -11) {
            smokeTranscript_ += text;
            ++deliveredStreamingResults_;
        } else if (smokeMilliseconds_ == -9 || smokeMilliseconds_ == -10) {
            const auto file = smokeMilliseconds_ == -9 ? L"yanflow-short-result.txt" : L"yanflow-long-result.txt";
            const bool saved = yanflow::writeUtf8File(joinPath(executableDirectory(), file), text);
            exitCode_ = saved && holdSmokeDeferred_ && submittedUtterances_.load() == 1 ? 0 : 133;
            DestroyWindow(window_);
        } else if (smokeMilliseconds_ == -4 || smokeMilliseconds_ == -7) {
            if (!smokeTranscript_.empty()) smokeTranscript_.push_back(L' ');
            smokeTranscript_.append(text);
            fallbackText_ = smokeTranscript_;
            copyFallback();
            if (!injected && smokeTranscript_.find(L"\u6ee8\u6d77\u65b0\u533a\u6709\u623f") != std::wstring::npos) {
                if (smokeMilliseconds_ == -7 && (!holdSmokeDeferred_ || captureDrainPending_.load())) {
                    exitCode_ = 99;
                    DestroyWindow(window_);
                    return;
                }
                exitCode_ = 0;
                DestroyWindow(window_);
            }
        }
    }

    void finishStreamingSmoke()
    {
        if (smokeMilliseconds_ != -11 || listening_.load() || captureDrainPending_.load() ||
            submittedUtterances_.load() < 3 || processedStreamingUtterances_.load() != submittedUtterances_.load() ||
            deliveredStreamingResults_ != processedStreamingUtterances_.load()) return;
        const bool saved = yanflow::writeUtf8File(joinPath(executableDirectory(), L"yanflow-long-result.txt"), smokeTranscript_);
        exitCode_ = saved && !streamingSmokeError_ ? 0 : 134;
        DestroyWindow(window_);
    }

    int runUiStateSmoke()
    {
        targetWindow_ = nullptr; targetWasEditable_ = false;
        // This exercises delivery-state handling only, never a foreground app.
        const auto result = [this](bool submitted, const wchar_t* status) {
            auto* item = new RecognitionResult;
            item->raw = item->final = L"已写入的文字";
            item->application = L"yanflow-ui-state-smoke.exe";
            item->serial = recentRecognitions_.size() + 1;
            item->delivery = {submitted, status, L"test", submitted ? item->final : L""};
            acceptResult(item);
        };
        result(true, L"verified");
        if (bubbleExpanded_ || !fallbackText_.empty()) return 170;
        result(true, L"submitted_unverified");
        if (bubbleExpanded_ || !fallbackText_.empty()) return 171;
        result(true, L"mismatch");
        if (bubbleExpanded_ || !fallbackText_.empty()) return 172;
        result(false, L"bubble");
        if (!bubbleExpanded_ || fallbackText_.empty() || copiedFeedback_) return 173;
        auto rect = actionRect(false);
        const auto point = MAKELPARAM((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
        SendMessageW(window_, WM_LBUTTONDOWN, MK_LBUTTON, point);
        if (!copiedFeedback_ || !bubbleExpanded_) return 174;
        result(false, L"bubble");
        if (copiedFeedback_) return 175; // New text must not be dismissed as already copied.
        rect = actionRect(false);
        const auto next = MAKELPARAM((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
        SendMessageW(window_, WM_LBUTTONDOWN, MK_LBUTTON, next);
        SendMessageW(window_, WM_LBUTTONDOWN, MK_LBUTTON, next);
        if (bubbleExpanded_ || !fallbackText_.empty()) return 176;
        // Model the two-key hold that arrives before the third toggle modifier.
        listening_.store(true); holdSession_.store(true);
        SendMessageW(window_, kMessageListeningToggle, 0, 0);
        if (!listening_.load() || holdSession_.load()) return 177;
        SendMessageW(window_, kMessageListeningToggle, 0, 0);
        captureDrainPending_.store(false);
        return !listening_.load() && !holdSession_.load() ? 0 : 178;
    }

    void retainRecognition(const RecognitionResult& result)
    {
        recentRecognitions_.push_back(result);
        if (recentRecognitions_.size() > 20) recentRecognitions_.pop_front();
    }

    bool runComparisonSmoke()
    {
        for (uint64_t i = 1; i <= 22; ++i) {
            RecognitionResult item{L"note ZS的最新版板是什？", L"note ZS的最新版板是什？", L"notepad.exe"};
            item.serial = i;
            item.delivery = {true, L"verified", L"unicode_edit", item.final};
            retainRecognition(item);
        }
        recentRecognitions_.back().manualCorrection = L"Node.js 的最新版本是什么？\n\"引用\"";
        const auto json = recognitionComparisonJson();
        const bool passed = recentRecognitions_.size() == 20 && recentRecognitions_.front().serial == 3 &&
            json.find(L"\"recognition_id\":3,") < json.find(L"\"recognition_id\":22,") &&
            json.find(L"Node.js 的最新版本是什么？\\u000a\\\"引用\\\"") != std::wstring::npos &&
            json.find(L"\"observed_inserted\":\"note ZS的最新版板是什？\"") != std::wstring::npos;
        recentRecognitions_.clear(); return passed;
    }

    std::wstring recognitionComparisonJson() const
    {
        const auto escape = [](const std::wstring& value) {
            std::wstring json = L"\"";
            for (wchar_t c : value) {
                if (c == L'\"' || c == L'\\') { json += L'\\'; json += c; }
                else if (c < 0x20) {
                    wchar_t encoded[7]{};
                    swprintf_s(encoded, L"\\u%04x", static_cast<unsigned int>(c)); json += encoded;
                } else json += c;
            }
            return json + L"\"";
        };
        std::wstring json;
        for (const auto& item : recentRecognitions_) {
            json += L"{\"recognition_id\":" + std::to_wstring(item.serial) + L",\"timestamp_filetime\":" + std::to_wstring(item.timestamp) +
                L",\"raw\":" + escape(item.raw) + L",\"final\":" + escape(item.final) + L",\"application\":" + escape(item.application) +
                L",\"macbert_status\":" + escape(item.correctionStatus) + L",\"macbert_error\":" + escape(item.correctionError) +
                L",\"delivery_status\":" + escape(item.delivery.status) + L",\"delivery_method\":" + escape(item.delivery.method) +
                L",\"observed_inserted\":" + escape(item.delivery.observed) + L",\"manual_correction\":" + escape(item.manualCorrection) + L",\"expected\":\"\"}\n";
        }
        return json;
    }

    void saveRecognitionComparison()
    {
        if (lastRecognition_.raw.empty()) return;
        PWSTR desktop = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_CREATE, nullptr, &desktop))) return;
        FILETIME timestamp{};
        GetSystemTimePreciseAsFileTime(&timestamp);
        ULARGE_INTEGER stamp{};
        stamp.LowPart = timestamp.dwLowDateTime; stamp.HighPart = timestamp.dwHighDateTime;
        const auto path = joinPath(desktop, L"YanFlow-correction-" + std::to_wstring(stamp.QuadPart) + L".jsonl");
        CoTaskMemFree(desktop);
        const auto json = recognitionComparisonJson();
        const bool saved = yanflow::writeUtf8File(path, json);
        MessageBoxW(window_, saved ? (L"已保存本次运行最近最多 20 条识别对照到：\n" + path + L"\n请填写人工校对的 expected 文本后评测。").c_str()
            : L"无法保存识别对照，请检查桌面写入权限。", L"言流识别对照", MB_OK | (saved ? MB_ICONINFORMATION : MB_ICONWARNING));
    }

    bool copyFallback()
    {
        const bool copied = copyText(window_, fallbackText_);
        if (copied) { copiedFeedback_ = true; InvalidateRect(window_, nullptr, FALSE); }
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
        HWND writable = CreateWindowExW(0, L"EDIT", L"", WS_CHILD, 0, 0, 10, 10, window_, nullptr, instance_, nullptr);
        HWND readOnly = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_READONLY, 0, 0, 10, 10, window_, nullptr, instance_, nullptr);
        HWND unknown = CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 10, 10, window_, nullptr, instance_, nullptr);
        const bool nativeStates = nativeEditState(writable) == 1 && nativeEditState(readOnly) == 0 && nativeEditState(unknown) == -1;
        EnableWindow(writable, FALSE);
        const bool disabledRejected = nativeEditState(writable) == 0;
        DestroyWindow(writable); DestroyWindow(readOnly); DestroyWindow(unknown);
        if (!nativeStates || !disabledRejected) return false;
        fallbackText_ = L"滨海新区有房，继续测试写入。";
        fallbackIsTranscript_ = true;
        expandBubble();
        const auto writeRect = actionRect(true);
        const LPARAM writePoint = MAKELPARAM((writeRect.left + writeRect.right) / 2, (writeRect.top + writeRect.bottom) / 2);
        SendMessageW(window_, WM_LBUTTONDOWN, MK_LBUTTON, writePoint);
        SendMessageW(window_, WM_LBUTTONUP, 0, writePoint);
        if (lastWrittenPath_.empty()) return false;
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

    RECT actionRect(bool write) const
    {
        RECT client{}; GetClientRect(window_, &client);
        return bubbleActionRect(write, client.right, client.bottom, fallbackIsTranscript_);
    }

    SIZE measureBubble() const
    {
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
        const LONG maximumWidth = std::min<LONG>(kBubbleWidth, monitor.rcWork.right - monitor.rcWork.left);
        const LONG maximumHeight = std::min<LONG>(kBubbleMaximumHeight, monitor.rcWork.bottom - monitor.rcWork.top);
        HDC dc = GetDC(window_);
        HFONT font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        auto previous = SelectObject(dc, font);
        LONG naturalWidth = 0;
        for (size_t begin = 0; begin < fallbackText_.size();) {
            const size_t end = fallbackText_.find(L'\n', begin);
            const size_t stop = end == std::wstring::npos ? fallbackText_.size() : end;
            SIZE line{}; GetTextExtentPoint32W(dc, fallbackText_.data() + begin, static_cast<int>(stop - begin), &line);
            naturalWidth = std::max(naturalWidth, line.cx);
            begin = stop + 1;
        }
        const LONG width = std::min(maximumWidth, std::max(200L, naturalWidth + 152));
        RECT text{0, 0, std::max(16L, width - 152), 0};
        DrawTextW(dc, fallbackText_.c_str(), -1, &text, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        const LONG height = std::min(maximumHeight, std::max<LONG>(kBubbleMinimumHeight, text.bottom + 24));
        SelectObject(dc, previous); DeleteObject(font); ReleaseDC(window_, dc);
        return SIZE{width, height};
    }

    void dismissFallback()
    {
        fallbackText_.clear(); fallbackIsTranscript_ = false; copiedFeedback_ = false;
        collapseBubble(); InvalidateRect(window_, nullptr, FALSE);
    }

    void expandBubble()
    {
        const auto size = measureBubble();
        resizeBubble(size.cx, size.cy, true);
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
        int top = rect.bottom - height;
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
    inline static YanFlowApp* hookOwner_ = nullptr;
    HHOOK keyboardHook_ = nullptr;
    yanflow::HoldHotkey holdKeys_;
    yanflow::ListeningToggleHotkey toggleKeys_;
    bool holdEnabled_ = true;
    uint64_t holdStartedTick_ = 0;
    HWND window_ = nullptr;
    HWND settingsWindow_ = nullptr;
    HWND learnWindow_ = nullptr;
    HWND targetWindow_ = nullptr;
    HICON applicationIcon_ = nullptr;
    HICON applicationSmallIcon_ = nullptr;
    HICON floatingIcon_ = nullptr;
    HICON listeningIcon_ = nullptr;
    IUIAutomation* automation_ = nullptr;
    HRESULT automationCreationStatus_ = E_FAIL, lastAutomationFocusStatus_ = E_FAIL;
    CONTROLTYPEID lastAutomationControlType_ = 0;
    int lastNativeFocusState_ = -1;
    bool lastNativeThreadInfo_ = false;
    std::wstring lastNativeFocusClass_, lastForegroundClass_;
    AudioCapture capture_;
    PersistentAsrWorker asrWorker_;
    yanflow::TextPipeline textPipeline_;
    yanflow::TranscriptBoundaryMerger transcriptMerger_;
    uint64_t previousTranscriptSerial_ = 0;
    std::atomic<bool> cscEnabled_ = false;
    std::atomic<bool> cscFailed_ = false, cscReady_ = false;
    std::wstring cscError_;
    std::atomic<unsigned int> cscRevision_ = 0;
    std::thread endpointThread_;
    std::thread inferenceThread_;
    std::atomic<bool> workerRunning_ = false;
    std::atomic<bool> listening_ = false;
    std::atomic<bool> holdSession_ = false;
    std::atomic<bool> captureDrainPending_ = false;
    std::atomic<size_t> submittedUtterances_ = 0;
    bool holdSmokeDeferred_ = false;
    std::atomic<size_t> processedStreamingUtterances_ = 0;
    size_t deliveredStreamingResults_ = 0, streamingSmokePosition_ = 0;
    std::vector<int16_t> streamingSmokeSamples_, streamingSmokeFrame_;
    bool streamingSmokeError_ = false;
    std::wstring streamingSmokeChunks_ = L"begin\tend\townedBegin\townedEnd\n";
    std::atomic<uint64_t> lastSpeechTick_ = 0;
    std::atomic<uint64_t> lastCandidateTick_ = 0;
    uint64_t idleGraceDeadline_ = 0;
    std::mutex queueMutex_;
    std::condition_variable queueChanged_;
    std::deque<QueuedUtterance> utterances_;
    std::wstring sessionApplication_, menuApplication_;
    std::wstring dictionarySmokeRoot_;
    RecognitionResult learningResult_;
    ListeningState state_ = ListeningState::Idle;
    std::wstring fallbackText_;
    RecognitionResult lastRecognition_;
    std::deque<RecognitionResult> recentRecognitions_;
    std::wstring lastManualCorrection_;
    std::wstring smokeTranscript_;
    std::wstring lastWrittenPath_;
    bool targetWasEditable_ = false;
    bool fallbackIsTranscript_ = false;
    bool bubbleExpanded_ = false;
    bool copiedFeedback_ = false;
    HotkeyBinding startHotkey_{};
    HotkeyBinding stopHotkey_{};
    bool startHotkeyRegistered_ = false;
    bool stopHotkeyRegistered_ = false;
    int smokeMilliseconds_ = 0;
    int exitCode_ = 0;
    POINT dragOrigin_ = {};
    POINT dragCursor_ = {};
    bool dragged_ = false;
};

} // namespace

int runYanFlow(int smokeMilliseconds)
{
    YanFlowApp app(smokeMilliseconds);
    return app.run();
}

int runAsrSmoke()
{
    const std::wstring root = executableDirectory();
    const std::wstring path = joinPath(root, L"yanflow-asr-smoke.wav");
    std::vector<int16_t> samples;
    if (!readPcm16Wave(path, samples)) return 20;
    PersistentAsrWorker worker;
    std::wstring text;
    std::wstring error;
    if (!worker.transcribe(root, samples, true, text, error)) return 24;
    return text.find(L"\u6ee8\u6d77\u65b0\u533a\u6709\u623f") == std::wstring::npos ? 25 : 0;
}
