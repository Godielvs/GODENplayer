#pragma once

#include <windows.h>

#include <memory>

namespace nekorox::component {

class DawWindow final {
public:
    static DawWindow& instance();
    void show(HWND owner = nullptr);
    void close();
    void shutdown();

private:
    DawWindow();
    ~DawWindow();
    DawWindow(const DawWindow&) = delete;
    DawWindow& operator=(const DawWindow&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nekorox::component
