#pragma once
#include <memory>
#include <string>
#include <vector>
namespace yanflow {
class TextPipeline {
public:
    TextPipeline();
    ~TextPipeline();
    std::wstring correct(const std::wstring& raw, const std::wstring& root,
        const std::wstring& dictionaryPath, bool enableCsc,
        const std::vector<std::wstring>& overlays = {});
    bool cscFailed() const;
    const std::wstring& cscError() const;
    bool warmupCsc(const std::wstring& root);
    void resetCsc();
private:
    struct State;
    std::unique_ptr<State> state_;
};
std::wstring readUtf8File(const std::wstring& path);
bool readUtf8FileChecked(const std::wstring& path, std::wstring& text);
bool writeUtf8File(const std::wstring& path, const std::wstring& text);
int correctTextFile(const std::wstring& input, const std::wstring& output, bool enableCsc);
int runCscPipelineSmoke();
}
