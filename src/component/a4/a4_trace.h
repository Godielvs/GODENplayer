#pragma once
#include <cstdint>
namespace nekorox::component::a4 {
// No strings, metadata or PCM are retained. ETW is off unless a session enables it.
void trace(const char* stage,std::uint64_t revision,std::int64_t code=0,
           double value0=0,double value1=0) noexcept;
}
