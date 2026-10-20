#pragma once
// SPDX-License-Identifier: MIT
// Public, value-only interface to an independent image-generating program.
// No SDK objects, renderer internals, pointers, intermediate images or passes.
#include <array>
#include <cstddef>
#include <bit>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace goden::frames {
inline constexpr std::uint32_t Magic=0x46524747, Version=1, SurfaceEdge=512;
// Visible admits motion; Work separately admits rendering. A visible but
// motion-ineligible surface can still request its first static image.
enum Flags:std::uint32_t { Playing=1, Visible=2, Music=4, CoverColors=8, AudioValid=16, Work=32 };
struct Input {
    std::uint64_t sequence=0, generation=0, restart=0, track=0, stream=0, audioSequence=0;
    double seconds=0;
    float width=0,height=0,intensity=1.25f,gate=0;
    std::uint32_t flags=0,sampleRate=48000;
    std::array<float,16> colors{};
    // 64 logarithmic frequency intervals (25 Hz to min(18 kHz, .48*rate)),
    // linear display energy [0,1], onset [0,8], relative activity [-1,1].
    std::array<float,64> energy{},onsets{},activity{};
};
struct Output {
    std::uint64_t sequence=0,generation=0,frames=0,audioSequence=0,bytes=0;
    std::uint32_t width=0,height=0;
    std::int32_t result=0;
    double cpuUs=0,gpuP95Us=0;
};
// The inherited mutex protects every access to this shared mailbox. A writer
// replaces the single pending input; there is no unbounded command/frame queue.
struct Mailbox {
    std::uint32_t magic=Magic,version=Version,bytes=sizeof(Mailbox),reserved=0;
    std::uint32_t adapterLow=0;std::int32_t adapterHigh=0;
    std::uint64_t acknowledged=0;
    // Latest visibility-loss event is latched independently of the coalesced
    // input, so a quick hide/show cannot discard the suspension timestamp.
    std::uint64_t suspension=0;double suspendedAt=0;
    Input input{};Output output{};
};
static_assert(std::is_trivially_copyable_v<Mailbox>);
static_assert(std::endian::native==std::endian::little);
static_assert(sizeof(float)==4&&sizeof(double)==8&&std::numeric_limits<float>::is_iec559&&std::numeric_limits<double>::is_iec559);
static_assert(sizeof(Input)==912&&alignof(Input)==8&&offsetof(Input,seconds)==48&&offsetof(Input,colors)==80&&offsetof(Input,energy)==144);
static_assert(sizeof(Output)==72&&sizeof(Mailbox)==1032&&offsetof(Mailbox,input)==48);
inline bool valid(const Mailbox& box)noexcept {
    return box.magic==Magic&&box.version==Version&&box.bytes==sizeof(Mailbox);
}
} // namespace goden::frames
