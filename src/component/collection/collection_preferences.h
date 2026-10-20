#pragma once
#include <string_view>
#include "collection_graph.h"
#include <windows.h>
namespace nekorox::component::collection {
bool selected()noexcept;
Selection selection()noexcept;
const wchar_t* title()noexcept;
UINT qaMessage() noexcept;
void appendMenu(HMENU)noexcept;
bool handleMenu(UINT)noexcept;
bool favorite(std::string_view id)noexcept;
void toggleFavorite(std::string_view id);
} // namespace
