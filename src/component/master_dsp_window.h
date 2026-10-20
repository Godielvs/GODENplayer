#pragma once

#include <memory>
#include <windows.h>

namespace nekorox::component {

class MasterDspWindow final {
public:
    static MasterDspWindow& instance();
    void show(HWND owner = nullptr);
    void close();

private:
    MasterDspWindow();
    ~MasterDspWindow();
    MasterDspWindow(const MasterDspWindow&) = delete;
    MasterDspWindow& operator=(const MasterDspWindow&) = delete;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nekorox::component
