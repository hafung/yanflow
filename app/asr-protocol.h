#pragma once
#include <cstdint>
namespace yanflow {
constexpr uint32_t kAsrProtocolVersion = 2;
constexpr uint32_t kAsrUseVad = 1;
constexpr uint32_t kAsrTimedTokens = 2;
constexpr uint32_t kAsrShortHold = 4;
// Protocol v2 adds optional timed tokens and the neural short-hold path.
struct AsrTokenHeader { uint32_t beginSample, endSample, bytes; };
static_assert(sizeof(AsrTokenHeader) == 12, "ASR token wire format");
}
