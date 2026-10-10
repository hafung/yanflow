#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#include "../csc-protocol.h"
#include "../text-correction.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace {
struct Token { int64_t id; size_t offset; };
constexpr size_t kNoOffset = static_cast<size_t>(-1);
std::wstring wide(const std::string& value)
{
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("Invalid UTF-8 vocabulary");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}
bool asciiWord(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9'); }
class Model {
public:
    Model() : environment_(ORT_LOGGING_LEVEL_ERROR, "yanflow-csc")
    {
        std::ifstream file("vocab.txt", std::ios::binary);
        if (!file) throw std::runtime_error("Missing vocab.txt");
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto token = wide(line);
            ids_[token] = static_cast<int64_t>(vocabulary_.size()); vocabulary_.push_back(token);
        }
        if (vocabulary_.size() < 1000 || !ids_.count(L"[CLS]") || !ids_.count(L"[SEP]") || !ids_.count(L"[UNK]"))
            throw std::runtime_error("Invalid vocabulary");
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(2); options.SetInterOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        session_ = std::make_unique<Ort::Session>(environment_, L"model.onnx", options);
        if (session_->GetInputCount() != 3 || session_->GetOutputCount() != 1)
            throw std::runtime_error("Unexpected MacBERT graph");
        Ort::AllocatorWithDefaultOptions allocator;
        for (size_t i = 0; i < 3; ++i) {
            const auto name = session_->GetInputNameAllocated(i, allocator);
            if (std::string(name.get()) != names_[i]) throw std::runtime_error("Unexpected input order");
        }
        const auto output = session_->GetOutputNameAllocated(0, allocator);
        if (std::string(output.get()) != "logits") throw std::runtime_error("Unexpected output name");
    }
    std::vector<yanflow::CscProposal> propose(const std::wstring& text)
    {
        std::vector<Token> tokens;
        // Chinese characters are individual BERT tokens; ASCII words use greedy
        // WordPiece. Preserve original UTF-16 offsets instead of decoding text.
        for (size_t at = 0; at < text.size();) {
            if (text[at] <= L' ') { ++at; continue; }
            if (!asciiWord(text[at])) {
                const auto found = ids_.find(text.substr(at, 1));
                tokens.push_back({found == ids_.end() ? ids_.at(L"[UNK]") : found->second,
                    yanflow::isHan(text[at]) && found != ids_.end() ? at : kNoOffset});
                ++at; continue;
            }
            size_t end = at + 1;
            while (end < text.size() && asciiWord(text[end])) ++end;
            auto word = text.substr(at, end - at);
            for (auto& c : word) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
            std::vector<Token> pieces;
            bool unknown = word.size() > 100;
            for (size_t start = 0; !unknown && start < word.size();) {
                size_t stop = word.size();
                auto found = ids_.end();
                for (; stop > start; --stop) {
                    found = ids_.find((start ? L"##" : L"") + word.substr(start, stop - start));
                    if (found != ids_.end()) break;
                }
                if (found == ids_.end()) { unknown = true; break; }
                pieces.push_back({found->second, kNoOffset}); start = stop;
            }
            if (unknown) tokens.push_back({ids_.at(L"[UNK]"), kNoOffset});
            else tokens.insert(tokens.end(), pieces.begin(), pieces.end());
            at = end;
        }
        std::vector<yanflow::CscProposal> proposals;
        for (size_t begin = 0; begin < tokens.size(); begin += 256) {
            const size_t end = std::min(tokens.size(), begin + 256);
            std::vector<int64_t> input{ids_.at(L"[CLS]")};
            for (size_t i = begin; i < end; ++i) input.push_back(tokens[i].id);
            input.push_back(ids_.at(L"[SEP]"));
            std::vector<int64_t> mask(input.size(), 1), types(input.size(), 0);
            const std::array<int64_t, 2> shape{1, static_cast<int64_t>(input.size())};
            const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            std::array<Ort::Value, 3> values{
                Ort::Value::CreateTensor<int64_t>(memory, input.data(), input.size(), shape.data(), shape.size()),
                Ort::Value::CreateTensor<int64_t>(memory, mask.data(), mask.size(), shape.data(), shape.size()),
                Ort::Value::CreateTensor<int64_t>(memory, types.data(), types.size(), shape.data(), shape.size())};
            const char* outputName = "logits";
            auto output = session_->Run(Ort::RunOptions{nullptr}, names_.data(), values.data(), values.size(), &outputName, 1);
            const auto info = output[0].GetTensorTypeAndShapeInfo();
            const auto dims = info.GetShape();
            if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || dims.size() != 3 ||
                dims[0] != 1 || dims[1] != static_cast<int64_t>(input.size()) || dims[2] != static_cast<int64_t>(vocabulary_.size()))
                throw std::runtime_error("Invalid logits shape");
            const float* logits = output[0].GetTensorData<float>();
            for (size_t i = begin; i < end; ++i) {
                if (tokens[i].offset == kNoOffset) continue;
                const auto* row = logits + (i - begin + 1) * vocabulary_.size();
                size_t best = 0, second = 0;
                for (size_t v = 1; v < vocabulary_.size(); ++v) if (row[v] > row[best]) best = v;
                if (static_cast<int64_t>(best) == tokens[i].id || vocabulary_[best].size() != 1 || !yanflow::isHan(vocabulary_[best][0])) continue;
                second = best == 0 ? 1 : 0;
                double sum = 0;
                for (size_t v = 0; v < vocabulary_.size(); ++v) {
                    sum += std::exp(static_cast<double>(row[v]) - row[best]);
                    if (v != best && row[v] > row[second]) second = v;
                }
                const double confidence = 1.0 / sum;
                if (!std::isfinite(confidence) || confidence < 0.995) continue;
                proposals.push_back({static_cast<uint32_t>(tokens[i].offset), static_cast<uint32_t>(text[tokens[i].offset]),
                    static_cast<uint32_t>(vocabulary_[best][0]), confidence,
                    std::exp(static_cast<double>(row[tokens[i].id]) - row[best]) / sum,
                    std::exp(static_cast<double>(row[second]) - row[best]) / sum});
            }
        }
        return proposals;
    }
private:
    Ort::Env environment_;
    std::unique_ptr<Ort::Session> session_;
    std::vector<std::wstring> vocabulary_;
    std::unordered_map<std::wstring, int64_t> ids_;
    std::array<const char*, 3> names_{"input_ids", "attention_mask", "token_type_ids"};
};
}

int main(int argc, char** argv)
{
    _setmode(_fileno(stdin), _O_BINARY); _setmode(_fileno(stdout), _O_BINARY);
    try {
        wchar_t executable[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        if (!length || length >= std::size(executable)) throw std::runtime_error("CSC executable path unavailable");
        std::wstring directory(executable, length);
        directory.resize(directory.find_last_of(L"\\/"));
        if (!SetCurrentDirectoryW(directory.c_str())) throw std::runtime_error("CSC model directory unavailable");
        // Windows may carry an older onnxruntime.dll. A unique app-local name
        // and explicit search scope prevent accidental system/runtime substitution.
        const auto library = directory + L"\\yanflow-onnxruntime.dll";
        const HMODULE runtime = LoadLibraryExW(library.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!runtime) throw std::runtime_error("App-local ONNX Runtime could not load");
        using GetApiBase = const OrtApiBase* (ORT_API_CALL*)();
        const auto getApiBase = reinterpret_cast<GetApiBase>(GetProcAddress(runtime, "OrtGetApiBase"));
        if (!getApiBase) throw std::runtime_error("Missing ONNX Runtime API");
        const auto api = getApiBase()->GetApi(ORT_API_VERSION);
        if (!api) throw std::runtime_error("Incompatible ONNX Runtime API");
        Ort::InitApi(api);
        Model model;
        if (argc == 2 && std::string(argv[1]) == "--smoke") {
            const auto text = L"今天新情很好，OpenAI ASR 123 `新情`";
            yanflow::TextDictionary dictionary;
            const auto first = dictionary.apply(text);
            const auto wire = model.propose(text);
            std::vector<yanflow::CharacterProposal> proposals;
            for (const auto& p : wire) proposals.push_back({p.offset, static_cast<wchar_t>(p.original), static_cast<wchar_t>(p.replacement),
                p.confidence, p.originalConfidence, p.runnerUpConfidence});
            const auto result = yanflow::applyChineseCorrections(first, proposals);
            if (result.size() != first.text.size() || result.substr(7) != first.text.substr(7)) return 20;
            fprintf(stderr, "PASS MacBERT graph/tokenizer/guard smoke: proposals=%zu accepted=%d\n", wire.size(), result != first.text);
            return 0;
        }
        const uint32_t ready[2]{yanflow::kCscReady, yanflow::kCscVersion};
        if (fwrite(ready, sizeof(ready), 1, stdout) != 1 || fflush(stdout)) return 4;
        uint32_t count = 0;
        while (fread(&count, sizeof(count), 1, stdin) == 1) {
            if (count > yanflow::kCscMaximumCharacters) return 5;
            std::wstring text(count, L'\0');
            if (count && fread(text.data(), sizeof(wchar_t), count, stdin) != count) return 6;
            const auto proposals = model.propose(text);
            const uint32_t size = static_cast<uint32_t>(proposals.size());
            if (fwrite(&size, sizeof(size), 1, stdout) != 1 ||
                (!proposals.empty() && fwrite(proposals.data(), sizeof(yanflow::CscProposal), size, stdout) != size) || fflush(stdout)) return 7;
        }
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "yanflow-csc: %s\n", error.what()); return 3;
    }
}
