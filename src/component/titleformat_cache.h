#pragma once

#include "sdk_compat.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace nekorox::component {

// Metadata formatting is outside the realtime PCM callback. Each owning
// thread reuses its scripts; configurable expressions have a bounded cache.
inline titleformat_object::ptr cachedTitleFormat(const char* expression)
{
    thread_local std::vector<std::pair<std::string, titleformat_object::ptr>> scripts;
    const auto found = std::find_if(scripts.begin(), scripts.end(),
        [expression](const auto& entry) { return entry.first == expression; });
    if (found != scripts.end()) return found->second;
    titleformat_object::ptr script;
    titleformat_compiler::get()->compile_safe(script, expression);
    if (scripts.size() == 32) scripts.erase(scripts.begin());
    scripts.emplace_back(expression, script);
    return script;
}

} // namespace nekorox::component
