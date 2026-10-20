#pragma once

#include "lyrics_http_client.h"
#include "lyrics_provider_types.h"

#include <chrono>
#include <mutex>

namespace nekorox::component {

[[nodiscard]] bool parseLrclibResponseForTests(
    std::string_view body,
    std::vector<LyricsCandidateV2>& output);

class LrclibProvider final {
public:
    [[nodiscard]] LyricsProviderResult lookup(
        const LyricsRequestContext& request,
        std::stop_token stop);

private:
    [[nodiscard]] LyricsHttpResponse request(
        std::wstring path,
        std::stop_token stop);

    LyricsHttpClient http_;
    std::mutex rateMutex_;
    std::chrono::steady_clock::time_point nextRequest_{};
    std::chrono::steady_clock::time_point serverBackoff_{};
    unsigned consecutiveServerFailures_ = 0;
};

} // namespace nekorox::component
