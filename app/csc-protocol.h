#pragma once
#include <cstdint>
namespace yanflow {
constexpr uint32_t kCscReady = 0x31435343;
constexpr uint32_t kCscVersion = 1;
constexpr uint32_t kCscMaximumCharacters = 4096;
#pragma pack(push, 1)
struct CscProposal {
    uint32_t offset, original, replacement;
    double confidence, originalConfidence, runnerUpConfidence;
};
#pragma pack(pop)
static_assert(sizeof(CscProposal) == 36, "CSC wire format");
}
