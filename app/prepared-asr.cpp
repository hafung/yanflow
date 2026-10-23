#include "prepared-asr.h"
#include "asr-protocol.h"
#include <algorithm>
#include <utility>

namespace yanflow {
void PreparedAsr::reset(uint64_t session)
{
    entries_.clear(); session_ = session;
}
bool PreparedAsr::get(uint64_t session, size_t begin, uint32_t flags,
    const std::vector<int16_t>& samples, std::wstring& text, std::vector<TimedToken>& tokens) const
{
    if (session != session_) return false;
    for (const auto& entry : entries_) {
        if (entry.begin == begin && entry.flags == flags && entry.samples == samples) {
            text = entry.text; tokens = entry.tokens; return true;
        }
    }
    return false;
}
void PreparedAsr::put(uint64_t session, size_t begin, uint32_t flags, std::vector<int16_t>&& samples,
    std::wstring&& text, std::vector<TimedToken>&& tokens)
{
    if (session != session_) reset(session);
    // At most ten completed windows in a held recording (60s, >=6s/core).
    // Keep an explicit bound even if recording limits change later.
    if (samples.size() > kInferenceLimit || entries_.size() >= 12) return;
    entries_.push_back({begin, flags, std::move(samples), std::move(text), std::move(tokens)});
}
std::vector<AudioChunk> prepareableAudioChunks(const std::vector<int16_t>& audio)
{
    auto chunks = planAudioChunks(audio);
    if (!chunks.empty()) chunks.pop_back();
    return chunks;
}
int runPreparedAsrSmoke()
{
    // A quiet boundary and a forced boundary must both match the final plan,
    // including 200ms overlaps. Test the exact eight-second transition too.
    for (bool quiet : {false, true}) {
        std::vector<int16_t> audio(kAudioRate * 60, 1200);
        if (quiet) for (size_t at = kAudioRate * 7; at + kAudioRate / 5 < audio.size(); at += kAudioRate * 7)
            std::fill(audio.begin() + at, audio.begin() + at + kAudioRate / 5, int16_t{0});
        const auto final = planAudioChunks(audio);
        for (size_t size : {size_t(0), kInferenceLimit, kInferenceLimit + 1, kAudioRate * 17, audio.size()}) {
            const std::vector<int16_t> partial(audio.begin(), audio.begin() + size);
            const auto ready = prepareableAudioChunks(partial);
            if (size <= kInferenceLimit && !ready.empty()) return 220;
            if (ready.size() >= final.size()) return 221;
            for (size_t i = 0; i < ready.size(); ++i) {
                const auto& a = ready[i]; const auto& b = final[i];
                if (a.begin != b.begin || a.end != b.end || a.ownedBegin != b.ownedBegin || a.ownedEnd != b.ownedEnd) return 222;
            }
        }
    }
    PreparedAsr cache;
    const uint32_t flags = kAsrUseVad | kAsrTimedTokens;
    const std::vector<int16_t> pcm{1, 2, 3};
    std::wstring text; std::vector<TimedToken> tokens;
    cache.put(7, 16, flags, std::vector<int16_t>(pcm), L"看看", {{L"看", 0, 1}, {L"看", 1, 3}});
    if (!cache.get(7, 16, flags, pcm, text, tokens) || text != L"看看" || tokens.size() != 2 || tokens[1].endSample != 3) return 223;
    for (uint32_t changed : {0u, kAsrTimedTokens, kAsrUseVad, flags | kAsrShortHold})
        if (cache.get(7, 16, changed, pcm, text, tokens)) return 224;
    if (cache.get(8, 16, flags, pcm, text, tokens) || cache.get(7, 17, flags, pcm, text, tokens) ||
        cache.get(7, 16, flags, {1, 2, 4}, text, tokens) || cache.get(7, 16, flags, {1, 2, 3, 0}, text, tokens)) return 225;
    // Successful silence is reusable too; a new session never retains it.
    cache.put(8, 0, flags, std::vector<int16_t>(pcm), L"", {});
    if (!cache.get(8, 0, flags, pcm, text, tokens) || !text.empty() || !tokens.empty() || cache.get(7, 16, flags, pcm, text, tokens)) return 226;
    cache.reset(8);
    if (cache.get(8, 0, flags, pcm, text, tokens)) return 227;
    return 0;
}
}
