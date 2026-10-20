#pragma once

#include <chrono>
#include <cstdint>
#include <stop_token>
#include <string>

namespace nekorox::component {

struct LyricsHttpResponse {
    std::uint32_t status = 0;
    std::string body;
    std::chrono::seconds retryAfter{0};
    std::string error;
    bool cancelled = false;
};

class LyricsHttpClient final {
public:
    [[nodiscard]] LyricsHttpResponse getLrclib(
        std::wstring_view pathAndQuery,
        std::stop_token stop) const;
};

} // namespace nekorox::component
