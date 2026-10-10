#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace yanflow {
constexpr size_t kAudioRate = 16000;
constexpr size_t kInferenceLimit = kAudioRate * 8;
constexpr size_t kBoundaryOverlap = kAudioRate / 5; // 200 ms each side
struct AudioBoundary { size_t sample; bool quiet; };
struct AudioChunk { size_t begin, end, ownedBegin, ownedEnd; };
struct TimedToken { std::wstring text; uint32_t beginSample, endSample; };
class TranscriptBoundaryMerger {
public:
    std::wstring append(const std::vector<TimedToken>& tokens, size_t clipBegin, size_t clipEnd,
        size_t ownedBegin, size_t ownedEnd);
    void reset();
private:
    struct PlacedToken { std::wstring text; size_t begin, end; bool emitted; };
    std::vector<PlacedToken> tail_;
    size_t previousOwnedEnd_ = 0, previousClipEnd_ = 0;
};
AudioBoundary chooseAudioBoundary(const std::vector<int16_t>& audio);
std::vector<AudioChunk> planAudioChunks(const std::vector<int16_t>& audio);
std::wstring ownedTranscript(const std::vector<TimedToken>& tokens, size_t begin, size_t end);
bool submitSpeech(bool held, size_t samples, size_t voicedFrames, bool continuation = false);
void padShortRecording(std::vector<int16_t>& audio);
int runAudioSegmentationSmoke();
}
