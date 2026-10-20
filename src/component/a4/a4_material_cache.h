#pragma once
#include "a4_model.h"
#include <memory>
namespace nekorox::component::a4 {
// Call on the artwork worker with the canonical 256-edge image, not a UI variant.
std::shared_ptr<const Material> materialFor(std::span<const std::uint8_t>,unsigned,unsigned,
    std::uint64_t,bool alloyV2=false);
Vec3 quietAccent(const Material&) noexcept; // encoded sRGB, never white
} // namespace nekorox::component::a4
