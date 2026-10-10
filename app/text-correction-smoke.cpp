#include "text-correction.h"
#include "hold-hotkey.h"
#include <limits>

namespace yanflow {
int runTextCorrectionSmoke()
{
    TextDictionary dictionary;
    if (dictionary.load(L"case\topenai\tOpenAI\ncase\tasr\tASR\n"
        L"term\t麦克伯特\tMacBERT\t纠错\nphonetic\t热此\t热词\t语音识别\n"
        L"term\t言流\tYanFlow\nterm\t言流工具\tYanFlow tool\n") != 6) return 80;
    if (dictionary.apply(L"openai 的 asr 与麦克伯特纠错").text != L"OpenAI 的 ASR 与MacBERT纠错") return 81;
    if (dictionary.apply(L"oasr asr2 my_openai openai.com `openai asr` \"麦克伯特\" C:\\openai\\asr.cpp").text !=
        L"oasr asr2 my_openai openai.com `openai asr` \"麦克伯特\" C:\\openai\\asr.cpp") return 82;
    if (dictionary.apply(L"热此非常有用，语音识别").text != L"热词非常有用，语音识别" ||
        dictionary.apply(L"热此非常有用").text != L"热此非常有用" ||
        dictionary.apply(L"麦克伯特").text != L"麦克伯特") return 83;
    if (dictionary.apply(L"热此非常有用。语音识别是另一个话题。").text != L"热此非常有用。语音识别是另一个话题。") return 83;
    if (dictionary.apply(L"言流工具").text != L"YanFlow tool") return 84;
    TextDictionary protectedTerms;
    protectedTerms.load(L"term\t好心\t好兴\nterm\t高兴\t高兴\n");
    if (protectedTerms.apply(L"高兴").text != L"高兴") return 85;
    TextDictionary ambiguous;
    if (ambiguous.load(L"term\t别名\t甲词\nterm\t别名\t乙词\nphonetic\t你好\t您好\ncase\tfoo\tBar\n") != 0 ||
        ambiguous.apply(L"别名你好 foo").text != L"别名你好 foo") return 86;
    TextDictionary cascading;
    cascading.load(L"term\t甲乙\t丙丁\nterm\t丙丁\t戊己\n");
    if (cascading.apply(L"甲乙").text != L"丙丁") return 87;
    auto input = dictionary.apply(L"今天新情很好，OpenAI ASR 123 `新情` \"新情\"");
    const CharacterProposal good{2, L'新', L'心', 0.999, 0.0001, 0.0002};
    if (applyChineseCorrections(input, {good}) != L"今天心情很好，OpenAI ASR 123 `新情` \"新情\"") return 88;
    auto low = good; low.confidence = 0.98;
    auto nan = good; nan.confidence = std::numeric_limits<double>::quiet_NaN();
    auto original = good; original.originalConfidence = 0.2;
    if (applyChineseCorrections(input, {low, nan, original}) != input.text) return 89;
    if (applyChineseCorrections(input, {{input.text.find(L'`') + 1, L'新', L'心', 1, 0, 0},
        {input.text.find(L'O'), L'O', L'哦', 1, 0, 0}}) != input.text) return 90;
    if (applyChineseCorrections(input, {good, good}) != input.text ||
        applyChineseCorrections(input, {good, {3, L'情', L'晴', 1, 0, 0}}) != input.text) return 91;
    auto code = dictionary.apply(L"const auto 新情 = openai(); // 新情");
    if (code.text != L"const auto 新情 = openai(); // 新情" ||
        applyChineseCorrections(code, {{11, L'新', L'心', 1, 0, 0}}) != code.text) return 92;
    HoldHotkey keys;
    using A = HoldHotkey::Action;
    if (keys.key(0xa2, true) != A::None || keys.key(0x5b, true) != A::Press ||
        keys.key(0x5b, true) != A::None || keys.key(0xa2, false) != A::Release ||
        keys.key(0xa2, true) != A::None) return 93;
    keys.key(0xa2, false); keys.key(0x5b, false);
    if (keys.key(0x5c, true) != A::None || keys.key(0xa3, true) != A::Press ||
        keys.key(0x5c, false) != A::Release) return 94;
    keys.reset(); keys.key(0xa2, true); keys.key(0xa4, true);
    if (keys.key(0x5b, true) != A::None) return 95;
    keys.reset(); keys.key(0x5b, true, true);
    if (keys.key(0xa2, true, true) != A::None || keys.active()) return 96;
    keys.reset(); keys.key(0xa2, true); keys.key(0x5b, true);
    if (keys.key('S', true) != A::Release || keys.key('S', false) != A::None) return 97;
    keys.reset(); keys.key(0xa2, true); keys.key(0x5b, true); keys.key(0xa3, true);
    if (keys.key(0xa2, false) != A::None || keys.key(0xa3, false) != A::Release) return 98;
    return 0;
}
} // namespace yanflow
