#pragma once

#include "sdk_compat.h"

#include <filesystem>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nekorox::component {

struct LocalTrackContent {
    std::uint64_t revision = 0;
    std::wstring biography;
    std::wstring review;
    std::vector<std::filesystem::path> artistImages;
};

class LocalContentCache final {
public:
    explicit LocalContentCache(UINT refreshMessage);
    ~LocalContentCache();

    void attach(HWND window);
    void detach();
    std::shared_ptr<const LocalTrackContent> getOrRequest(
        const metadb_handle_ptr& track,
        const std::wstring& artist,
        const std::wstring& album,
        const std::wstring& title);

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace nekorox::component
