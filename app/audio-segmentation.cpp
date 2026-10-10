#include "audio-segmentation.h"
#include <algorithm>

namespace yanflow {
AudioBoundary chooseAudioBoundary(const std::vector<int16_t>& audio)
{
    const size_t limit = std::min(audio.size(), kInferenceLimit - 2 * kBoundaryOverlap);
    const size_t earliest = kAudioRate * 6;
    constexpr size_t frame = kAudioRate / 50;
    size_t quietStart = 0, quietFrames = 0, chosen = 0;
    for (size_t at = earliest; at + frame <= limit; at += frame) {
        double energy = 0;
        for (size_t i = at; i < at + frame; ++i) energy += static_cast<double>(audio[i]) * audio[i];
        if (energy / frame <= 82.0 * 82.0) {
            if (!quietFrames) quietStart = at;
            ++quietFrames;
            if (quietFrames >= 4) chosen = quietStart + (quietFrames * frame) / 2;
        } else quietFrames = 0;
    }
    return {chosen ? chosen : limit, chosen != 0};
}

std::vector<AudioChunk> planAudioChunks(const std::vector<int16_t>& audio)
{
    std::vector<AudioChunk> chunks;
    size_t coreBegin = 0, leftOverlap = 0;
    while (coreBegin < audio.size()) {
        const size_t begin = coreBegin - leftOverlap;
        if (audio.size() - begin <= kInferenceLimit) {
            chunks.push_back({begin, audio.size(), coreBegin, audio.size()}); break;
        }
        // Analyze at most eight seconds; no full-recording copy or scan per slice.
        const size_t stop = std::min(audio.size(), coreBegin + kInferenceLimit);
        const std::vector<int16_t> window(audio.begin() + coreBegin, audio.begin() + stop);
        const auto boundary = chooseAudioBoundary(window);
        const size_t coreEnd = coreBegin + boundary.sample;
        const size_t rightOverlap = boundary.quiet ? 0 : kBoundaryOverlap;
        chunks.push_back({begin, coreEnd + rightOverlap, coreBegin, coreEnd});
        coreBegin = coreEnd; leftOverlap = rightOverlap;
    }
    return chunks;
}

std::wstring ownedTranscript(const std::vector<TimedToken>& tokens, size_t begin, size_t end)
{
    std::wstring text;
    for (const auto& token : tokens) {
        if (token.beginSample > token.endSample) continue;
        // Token positions refer to the actual overlapping PCM. Never remove
        // repeated words based on spelling alone. CTC time resolution is 60 ms.
        const size_t center = token.beginSample + (token.endSample - token.beginSample) / 2;
        if (center >= begin && center < end) text += token.text;
    }
    return text;
}

void TranscriptBoundaryMerger::reset()
{
    tail_.clear(); previousOwnedEnd_ = previousClipEnd_ = 0;
}
std::wstring TranscriptBoundaryMerger::append(const std::vector<TimedToken>& tokens, size_t clipBegin,
    size_t clipEnd, size_t ownedBegin, size_t ownedEnd)
{
    if (clipBegin > ownedBegin || ownedBegin >= ownedEnd || ownedEnd > clipEnd) { reset(); return L""; }
    if (ownedBegin != previousOwnedEnd_) reset();
    std::vector<PlacedToken> current;
    for (const auto& token : tokens) {
        if (token.beginSample > token.endSample || token.endSample > clipEnd - clipBegin) continue;
        const size_t begin = clipBegin + token.beginSample, end = clipBegin + token.endSample;
        const size_t center = begin + (end - begin) / 2;
        current.push_back({token.text, begin, end, center >= ownedBegin && center < ownedEnd});
    }
    const size_t sharedEnd = std::min(clipEnd, previousClipEnd_);
    if (clipBegin < sharedEnd && !tail_.empty()) {
        // CTC frame spikes can move between independently decoded windows.
        // Match exact token pieces one-to-one only around shared PCM, with a
        // bounded 320 ms displacement and 120 ms edge allowance. Never apply
        // general text suffix de-duplication to the complete transcript.
        constexpr size_t allowance = kAudioRate * 120 / 1000;
        constexpr size_t maximumShift = kAudioRate * 320 / 1000;
        const size_t start = clipBegin > allowance ? clipBegin - allowance : 0;
        const size_t finish = sharedEnd + allowance;
        std::vector<size_t> left, right;
        for (size_t i = 0; i < tail_.size(); ++i) if (tail_[i].end >= start && tail_[i].begin <= finish) left.push_back(i);
        for (size_t i = 0; i < current.size(); ++i) if (current[i].end >= start && current[i].begin <= finish) right.push_back(i);
        const auto pairScore = [&](size_t a, size_t b) -> int64_t {
            const auto& p = tail_[left[a]]; const auto& q = current[right[b]];
            if (p.text != q.text || p.text.find_first_not_of(L' ') == std::wstring::npos) return 0;
            const auto countLabel = [](const auto& list, const auto& indexes, const auto& text) {
                return std::count_if(indexes.begin(), indexes.end(), [&](size_t index) { return list[index].text == text; });
            };
            const auto countLeft = countLabel(tail_, left, p.text), countRight = countLabel(current, right, q.text);
            // Disagreement on repeated tokens is ambiguous; retain them.
            if ((countLeft > 1 || countRight > 1) && countLeft != countRight) return 0;
            const size_t x = p.begin + (p.end - p.begin) / 2, y = q.begin + (q.end - q.begin) / 2;
            const size_t distance = x > y ? x - y : y - x;
            return distance <= maximumShift ? 1000000 - static_cast<int64_t>(distance) : 0;
        };
        if (left.size() <= 32 && right.size() <= 32) {
            const size_t columns = right.size() + 1;
            std::vector<int64_t> scores((left.size() + 1) * columns, 0);
            for (size_t i = 1; i <= left.size(); ++i) for (size_t j = 1; j <= right.size(); ++j) {
                const auto pair = pairScore(i - 1, j - 1);
                scores[i * columns + j] = std::max(scores[(i - 1) * columns + j], scores[i * columns + j - 1]);
                if (pair) scores[i * columns + j] = std::max(scores[i * columns + j], scores[(i - 1) * columns + j - 1] + pair);
            }
            size_t i = left.size(), j = right.size();
            while (i && j) {
                const auto pair = pairScore(i - 1, j - 1);
                if (pair && scores[i * columns + j] == scores[(i - 1) * columns + j - 1] + pair) {
                    const auto& prior = tail_[left[i - 1]];
                    auto& next = current[right[j - 1]];
                    if (prior.emitted) next.emitted = false;
                    else if (!next.emitted && next.begin < ownedEnd) next.emitted = true;
                    --i; --j;
                } else if (scores[(i - 1) * columns + j] >= scores[i * columns + j - 1]) --i;
                else --j;
            }
        }
    }
    std::wstring text;
    for (const auto& token : current) if (token.emitted) text += token.text;
    tail_.clear();
    const size_t minimum = ownedEnd > kBoundaryOverlap + kAudioRate * 320 / 1000
        ? ownedEnd - kBoundaryOverlap - kAudioRate * 320 / 1000 : 0;
    for (auto& token : current) if (token.end >= minimum) tail_.push_back(std::move(token));
    if (tail_.size() > 64) tail_.clear();
    previousOwnedEnd_ = ownedEnd; previousClipEnd_ = clipEnd;
    return text;
}

bool submitSpeech(bool held, size_t samples, size_t voicedFrames, bool continuation)
{
    if (held) return samples >= kAudioRate * 80 / 1000 && voicedFrames >= 3;
    return samples >= (continuation ? kAudioRate / 20 : kAudioRate * 350 / 1000);
}
void padShortRecording(std::vector<int16_t>& audio)
{
    if (audio.size() >= kAudioRate * 350 / 1000) return;
    // Offline padding gives feature extraction context, without waiting on the
    // microphone or changing pitch/speed. The worker still checks real speech.
    audio.insert(audio.begin(), kAudioRate / 10, 0);
    audio.resize(std::max(audio.size(), kAudioRate * 400 / 1000), 0);
}

int runAudioSegmentationSmoke()
{
    std::vector<int16_t> quiet(21 * kAudioRate, 1200);
    std::fill(quiet.begin() + 7 * kAudioRate, quiet.begin() + 7 * kAudioRate + kAudioRate / 5, int16_t{0});
    const auto boundary = chooseAudioBoundary(quiet);
    if (!boundary.quiet || boundary.sample < 7 * kAudioRate || boundary.sample > 7 * kAudioRate + kAudioRate / 5) return 110;
    for (const auto& audio : {quiet, std::vector<int16_t>(60 * kAudioRate, 1200)}) {
        const auto chunks = planAudioChunks(audio);
        size_t owned = 0;
        for (const auto& chunk : chunks) {
            if (chunk.ownedBegin != owned || chunk.end - chunk.begin > kInferenceLimit ||
                chunk.begin > chunk.ownedBegin || chunk.end < chunk.ownedEnd || chunk.ownedEnd <= owned) return 111;
            owned = chunk.ownedEnd;
        }
        if (owned != audio.size()) return 112;
    }
    if (planAudioChunks({}).size() != 0 || planAudioChunks(std::vector<int16_t>(kInferenceLimit, 5)).size() != 1) return 113;
    const std::vector<TimedToken> repeated{{L"看", 0, 800}, {L"看", 900, 1600}, {L"好", 1700, 2200}};
    if (ownedTranscript(repeated, 0, 1600) != L"看看" || ownedTranscript(repeated, 1600, 2500) != L"好") return 114;
    if (!submitSpeech(true, 1600, 5) || submitSpeech(false, 1600, 5) || submitSpeech(true, 1600, 1) ||
        !submitSpeech(false, 1600, 5, true)) return 115;
    std::vector<int16_t> word(1600, 1234); padShortRecording(word);
    if (word.size() != 6400 || word[1600] != 1234 || word[3199] != 1234 || word[3200] != 0) return 116;
    TranscriptBoundaryMerger merger;
    // Reproduce the measured 260 ms CTC displacement of the duplicated 问.
    if (merger.append({{L"问", 117120, 118080}}, 0, 124800, 0, 121600) != L"问" ||
        merger.append({{L"问", 2880, 3840}, {L"我", 7000, 8000}}, 118400, 246400, 121600, 243200) != L"我") return 117;
    merger.reset();
    if (merger.append({{L"看", 7800, 8400}, {L"看", 9600, 10200}}, 0, 12800, 0, 9200) != L"看" ||
        merger.append({{L"看", 1000, 1600}, {L"看", 2800, 3400}}, 7000, 16000, 9200, 16000) != L"看") return 118;
    merger.reset();
    merger.append({{L"看", 7800, 8400}}, 0, 12800, 0, 9200);
    if (merger.append({{L"看", 1000, 1600}, {L"看", 2800, 3400}}, 7000, 16000, 9200, 16000) != L"看") return 119;
    merger.reset();
    if (merger.append({{L"好", 7000, 7600}}, 0, 8000, 0, 8000) != L"好" ||
        merger.append({{L"好", 0, 600}}, 8000, 16000, 8000, 16000) != L"好") return 119;
    return 0;
}
}
