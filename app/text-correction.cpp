#include "text-correction.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace yanflow {
namespace {
bool asciiLetter(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }
bool digit(wchar_t c) { return c >= L'0' && c <= L'9'; }
bool word(wchar_t c) { return asciiLetter(c) || digit(c) || c == L'_'; }
wchar_t lower(wchar_t c) { return c >= L'A' && c <= L'Z' ? c + (L'a' - L'A') : c; }
std::wstring folded(std::wstring text) { for (auto& c : text) c = lower(c); return text; }
bool boundary(const std::wstring& text, size_t at, size_t length, const std::wstring& term)
{
    return !(word(term.front()) && at > 0 && word(text[at - 1])) &&
        !(word(term.back()) && at + length < text.size() && word(text[at + length]));
}
bool hasContext(const std::wstring& text, size_t at, const std::wstring& context)
{
    if (context.empty()) return true;
    const size_t previous = at ? text.find_last_of(L"。.!?！？\n;", at - 1) : std::wstring::npos;
    const size_t next = text.find_first_of(L"。.!?！？\n;", at);
    const size_t begin = std::max(previous == std::wstring::npos ? 0 : previous + 1, at > 80 ? at - 80 : 0);
    const size_t end = std::min(next == std::wstring::npos ? text.size() : next, at + 80);
    return text.substr(begin, end - begin).find(context) != std::wstring::npos;
}
std::vector<bool> codeMask(const std::wstring& text)
{
    std::vector<bool> mask(text.size(), false);
    // Quotes/backticks include Chinese comments and string literals. An unclosed
    // quote protects the rest: incomplete code must not be silently rewritten.
    wchar_t quote = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (quote) {
            mask[i] = true;
            if (c == quote && (i == 0 || text[i - 1] != L'\\')) quote = 0;
        } else if (c == L'`' || c == L'\"' || (c == L'\'' &&
            !(i > 0 && i + 1 < text.size() && asciiLetter(text[i - 1]) && asciiLetter(text[i + 1])))) {
            quote = c; mask[i] = true;
        }
    }
    for (size_t begin = 0; begin < text.size();) {
        size_t end = begin;
        while (end < text.size() && (word(text[end]) || isHan(text[end]) ||
            std::wstring(L"./\\:@-=+(){}[]#$%").find(text[end]) != std::wstring::npos)) ++end;
        if (end == begin) { ++begin; continue; }
        const std::wstring token = text.substr(begin, end - begin);
        bool technical = token.find_first_of(L"/\\@_=(){}[]#$%") != std::wstring::npos;
        // Domains, filenames, member access, URLs and drive paths.
        technical = technical || (token.find(L'.') != std::wstring::npos &&
            std::any_of(token.begin(), token.end(), asciiLetter)) || token.find(L':') != std::wstring::npos;
        if (technical) std::fill(mask.begin() + begin, mask.begin() + end, true);
        begin = end;
    }
    // Obvious code statements: protect the complete line, including comments.
    for (size_t begin = 0; begin < text.size();) {
        const size_t end = text.find(L'\n', begin);
        const size_t stop = end == std::wstring::npos ? text.size() : end;
        const auto line = text.substr(begin, stop - begin);
        if (line.find_first_of(L"={};") != std::wstring::npos) {
            std::fill(mask.begin() + begin, mask.begin() + stop, true);
        }
        begin = stop + 1;
    }
    return mask;
}
} // namespace

bool isHan(wchar_t c) { return c >= 0x4e00 && c <= 0x9fff; }

size_t TextDictionary::load(const std::wstring& tsv)
{
    rules_.clear();
    std::wistringstream stream(tsv);
    std::wstring line;
    std::unordered_set<std::wstring> ambiguous;
    std::unordered_map<std::wstring, std::wstring> destinations;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.front() == 0xfeff) line.erase(line.begin());
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty() || line.front() == L'#') continue;
        std::vector<std::wstring> fields;
        size_t start = 0;
        do {
            const size_t tab = line.find(L'\t', start);
            fields.push_back(line.substr(start, tab == std::wstring::npos ? tab : tab - start));
            if (tab == std::wstring::npos) break;
            start = tab + 1;
        } while (true);
        if (fields.size() < 3 || fields.size() > 4 || fields[1].empty() || fields[2].empty() ||
            fields[1].size() > 128 || fields[2].size() > 128 || rules_.size() >= 2048) continue;
        DictionaryRule rule{fields[0], fields[1], fields[2], fields.size() == 4 ? fields[3] : L""};
        if (rule.kind == L"case") {
            if (folded(rule.source) != folded(rule.target) ||
                !std::all_of(rule.source.begin(), rule.source.end(), [](wchar_t c) { return c > 0 && c < 128; })) continue;
        } else if (rule.kind == L"term" || rule.kind == L"phonetic") {
            if (rule.kind == L"phonetic" && (rule.context.empty() || rule.source.size() < 2 ||
                !std::all_of(rule.source.begin(), rule.source.end(), isHan))) continue;
        } else continue;
        // A source has one meaning. Conflicting destinations disable every entry
        // for that source, including conflicts between case and term rules.
        const auto key = folded(rule.source);
        const auto found = destinations.find(key);
        if (found != destinations.end() && found->second != rule.target) ambiguous.insert(key);
        else destinations.emplace(key, rule.target);
        rules_.push_back(std::move(rule));
    }
    rules_.erase(std::remove_if(rules_.begin(), rules_.end(), [&](const auto& rule) {
        return ambiguous.count(folded(rule.source)) != 0;
    }), rules_.end());
    std::stable_sort(rules_.begin(), rules_.end(), [](const auto& a, const auto& b) {
        return a.source.size() > b.source.size();
    });
    return rules_.size();
}

size_t TextDictionary::overlay(const std::wstring& tsv)
{
    TextDictionary extra;
    extra.load(tsv);
    std::unordered_set<std::wstring> overridden;
    for (const auto& rule : extra.rules_) overridden.insert(folded(rule.source));
    rules_.erase(std::remove_if(rules_.begin(), rules_.end(), [&](const auto& previous) {
        return overridden.count(folded(previous.source)) != 0;
    }), rules_.end());
    const size_t added = extra.rules_.size();
    rules_.insert(rules_.begin(), extra.rules_.begin(), extra.rules_.end());
    // Keep the more specific layer when the combined dictionary reaches its cap.
    if (rules_.size() > 2048) rules_.resize(2048);
    std::stable_sort(rules_.begin(), rules_.end(), [](const auto& a, const auto& b) { return a.source.size() > b.source.size(); });
    return added;
}

CorrectionResult TextDictionary::apply(const std::wstring& text) const
{
    auto protectedInput = codeMask(text);
    // Correct canonical terms are immutable even if another rule matches inside.
    for (const auto& rule : rules_) {
        size_t at = 0;
        while ((at = text.find(rule.target, at)) != std::wstring::npos) {
            if (boundary(text, at, rule.target.size(), rule.target))
                std::fill(protectedInput.begin() + at, protectedInput.begin() + at + rule.target.size(), true);
            ++at;
        }
    }
    CorrectionResult result;
    for (size_t at = 0; at < text.size();) {
        const DictionaryRule* match = nullptr;
        if (!protectedInput[at]) for (const auto& rule : rules_) {
            if (rule.kind == L"case" ? lower(text[at]) != lower(rule.source.front()) : text[at] != rule.source.front()) continue;
            if (at + rule.source.size() > text.size() ||
                !boundary(text, at, rule.source.size(), rule.source) ||
                !hasContext(text, at, rule.context)) continue;
            if (std::any_of(protectedInput.begin() + at, protectedInput.begin() + at + rule.source.size(),
                [](bool value) { return value; })) continue;
            const bool matched = rule.kind == L"case" ? std::equal(rule.source.begin(), rule.source.end(), text.begin() + at,
                [](wchar_t a, wchar_t b) { return lower(a) == lower(b); }) : text.compare(at, rule.source.size(), rule.source) == 0;
            if (matched) {
                match = &rule; break;
            }
        }
        if (match) {
            result.text += match->target;
            result.protectedCharacters.insert(result.protectedCharacters.end(), match->target.size(), true);
            at += match->source.size();
        } else {
            result.text += text[at];
            result.protectedCharacters.push_back(protectedInput[at] || !isHan(text[at]));
            ++at;
        }
    }
    return result;
}

std::wstring applyChineseCorrections(const CorrectionResult& input, const std::vector<CharacterProposal>& proposals)
{
    if (input.protectedCharacters.size() != input.text.size()) return input.text;
    std::wstring result = input.text;
    size_t hanCount = 0;
    for (size_t i = 0; i < result.size(); ++i) if (isHan(result[i]) && !input.protectedCharacters[i]) ++hanCount;
    std::vector<CharacterProposal> accepted;
    for (const auto& p : proposals) {
        if (p.offset >= result.size() || input.protectedCharacters[p.offset] ||
            result[p.offset] != p.original || !isHan(p.original) || !isHan(p.replacement) ||
            p.original == p.replacement || !std::isfinite(p.confidence) ||
            !std::isfinite(p.originalConfidence) || !std::isfinite(p.runnerUpConfidence) ||
            p.confidence < 0.995 || p.confidence > 1.0 || p.originalConfidence < 0.0 ||
            p.originalConfidence > 0.001 || p.runnerUpConfidence < 0.0 ||
            p.runnerUpConfidence > 1.0 || p.confidence - p.runnerUpConfidence < 0.20) continue;
        if (std::any_of(accepted.begin(), accepted.end(), [&](const auto& other) { return other.offset == p.offset; }))
            return input.text;
        accepted.push_back(p);
    }
    // Excessive edits reject the entire proposal rather than cherry-picking it.
    if (accepted.size() > std::min<size_t>(2, std::max<size_t>(1, hanCount / 10))) return input.text;
    for (const auto& p : accepted) result[p.offset] = p.replacement;
    return result;
}
} // namespace yanflow
