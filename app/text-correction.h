#pragma once
#include <string>
#include <vector>

namespace yanflow {
struct DictionaryRule {
    std::wstring kind, source, target, context;
};
struct CorrectionResult {
    std::wstring text;
    std::vector<bool> protectedCharacters;
};
struct CharacterProposal {
    size_t offset;
    wchar_t original, replacement;
    double confidence, originalConfidence, runnerUpConfidence;
};
class TextDictionary {
public:
    // Invalid or ambiguous entries are ignored, never guessed.
    size_t load(const std::wstring& tsv);
    size_t overlay(const std::wstring& tsv);
    CorrectionResult apply(const std::wstring& text) const;
private:
    std::vector<DictionaryRule> rules_;
};
bool isHan(wchar_t character);
std::wstring applyChineseCorrections(const CorrectionResult& input,
    const std::vector<CharacterProposal>& proposals);
int runTextCorrectionSmoke();
} // namespace yanflow
