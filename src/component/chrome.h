#pragma once

#include <cstdint>

namespace nekorox::component {

class ChromeController final {
public:
    static ChromeController& instance();
    void start();
    void stop();
    void refreshAccent();

private:
    ChromeController() = default;
    void* window_ = nullptr;
    std::intptr_t originalStyle_ = 0;
};

} // namespace nekorox::component
