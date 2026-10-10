#pragma once
#include <string>
#include <utility>
#include <vector>
bool yanflow_short_vad_segments(const std::string& model, const std::vector<float>& audio,
    int maximumMilliseconds, std::vector<std::pair<int,int>>& segments, int threads);
