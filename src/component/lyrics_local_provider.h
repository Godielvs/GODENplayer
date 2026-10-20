#pragma once

#include "lyrics_provider_types.h"
#include <memory>
#include <stop_token>

namespace nekorox::component {

class LyricsLocalProvider final {
public:
    LyricsLocalProvider();
    [[nodiscard]] LyricsProviderResult lookup(
        const LyricsRequestContext& request, std::stop_token stop = {}) const;
    [[nodiscard]] std::uint64_t indexBuilds() const noexcept;
private:
    struct Index;
    std::shared_ptr<Index> index_;
};

} // namespace nekorox::component
