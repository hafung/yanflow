#pragma once
#include "audio-segmentation.h"

namespace yanflow {
// Inference-thread-only cache. Audio equality is checked, not inferred from a
// timestamp/hash: changed tails, sessions or request flags always miss.
class PreparedAsr {
public:
    bool get(uint64_t session, size_t begin, uint32_t flags,
        const std::vector<int16_t>& samples, std::wstring& text, std::vector<TimedToken>& tokens) const;
    void put(uint64_t session, size_t begin, uint32_t flags, std::vector<int16_t>&& samples,
        std::wstring&& text, std::vector<TimedToken>&& tokens);
    void reset(uint64_t session);
private:
    struct Entry {
        size_t begin;
        uint32_t flags;
        std::vector<int16_t> samples;
        std::wstring text;
        std::vector<TimedToken> tokens;
    };
    uint64_t session_ = 0;
    std::vector<Entry> entries_;
};
// Only the last window can change when more PCM arrives. Never prepare it.
std::vector<AudioChunk> prepareableAudioChunks(const std::vector<int16_t>& audio);
int runPreparedAsrSmoke();
}
