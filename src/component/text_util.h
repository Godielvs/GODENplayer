#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace nekorox::component {

std::wstring utf8ToWide(std::string_view value);
std::string wideToUtf8(std::wstring_view value);
std::wstring trimText(std::wstring value);
std::string personaAliasKey(std::wstring_view value);
std::wstring readLocalTextFile(
    const std::filesystem::path& path,
    std::size_t maximumBytes = 512U * 1024U);
int compareText(std::wstring_view left, std::wstring_view right) noexcept;

} // namespace nekorox::component
