#pragma once

#include "sdk_compat.h"

#include <columns_ui-sdk/ui_extension.h>

#include <memory>

namespace nekorox::component {

class WorkspacePanel : public uie::container_uie_window_v3 {
public:
    WorkspacePanel();
    ~WorkspacePanel();

    const GUID& get_extension_guid() const override;
    void get_name(pfc::string_base& output) const override;
    void get_category(pfc::string_base& output) const override;
    unsigned get_type() const override;

private:
    class Impl;

    uie::container_window_v3_config get_window_config() override;
    LRESULT on_message(HWND window, UINT message, WPARAM wParam, LPARAM lParam) override;

    std::unique_ptr<Impl> impl_;
};

} // namespace nekorox::component
