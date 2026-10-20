#pragma once

#include <array>
#include <cstddef>
#include <type_traits>

namespace nekorox {

struct alignas(16) FlowGpuConstants final {
    std::array<float, 4> geometry{};
    std::array<float, 4> levels{};
    std::array<float, 4> hits{};
    std::array<float, 4> phases{};
    std::array<float, 4> colorLow{};
    std::array<float, 4> colorMid{};
    std::array<float, 4> colorHigh{};
    std::array<float, 4> colorBase{};
    std::array<float, 4> lyricsRect{};
    std::array<float, 4> trackRect{};
    std::array<float, 4> controls{};
    std::array<float, 4> textureBounds{};
};

static_assert(std::is_standard_layout_v<FlowGpuConstants>);
static_assert(sizeof(FlowGpuConstants) == 192U);
static_assert(offsetof(FlowGpuConstants, textureBounds) == 176U);

} // namespace nekorox
