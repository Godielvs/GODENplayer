#pragma once

#include "nekorox/contracts.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace nekorox {

// Directly consumable by the D3D renderer. Geometry is deliberately raw:
// x is the left channel and y is the right channel.
struct CrtBeamSegment {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float charge = 0.0f;
    float ageSeconds = 0.0f;
};

static_assert(sizeof(float) == 4);
static_assert(sizeof(CrtBeamSegment) == 24);
static_assert(offsetof(CrtBeamSegment, x0) == 0);
static_assert(offsetof(CrtBeamSegment, y0) == 4);
static_assert(offsetof(CrtBeamSegment, x1) == 8);
static_assert(offsetof(CrtBeamSegment, y1) == 12);
static_assert(offsetof(CrtBeamSegment, charge) == 16);
static_assert(offsetof(CrtBeamSegment, ageSeconds) == 20);
static_assert(std::is_standard_layout_v<CrtBeamSegment>);
static_assert(std::is_trivially_copyable_v<CrtBeamSegment>);

struct CrtXyStats {
    std::uint64_t samples = 0;
    std::uint64_t segments = 0;
    std::uint64_t brokenPaths = 0;
};

class CrtXyModel final {
public:
    void reset() noexcept;

    // The returned view remains valid until the next ingest() or reset().
    // Invalid frames return an empty view and leave path/statistics unchanged.
    [[nodiscard]] std::span<const CrtBeamSegment> ingest(
        const RealtimeFrameV1& frame,
        bool discontinuity,
        double frameAgeSeconds = 0.0) noexcept;

    [[nodiscard]] const CrtXyStats& stats() const noexcept {
        return stats_;
    }
    [[nodiscard]] bool hasOpenPath() const noexcept { return hasPrevious_; }

private:
    std::array<CrtBeamSegment, kRealtimeMaxFrames> segments_{};
    CrtXyStats stats_{};
    std::size_t segmentCount_ = 0;
    float previousX_ = 0.0f;
    float previousY_ = 0.0f;
    bool hasPrevious_ = false;
};

struct CrtXyRasterSpec {
    std::size_t width = 0;
    std::size_t height = 0;
    double minimumX = -1.0;
    double maximumX = 1.0;
    double minimumY = -1.0;
    double maximumY = 1.0;
};

struct CrtXyReferenceRaster {
    std::size_t width = 0;
    std::size_t height = 0;
    std::vector<double> energy{};
    double inputCharge = 0.0;
    double visibleCharge = 0.0;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] double pixel(
        std::size_t x, std::size_t y) const noexcept;
    [[nodiscard]] double totalEnergy() const noexcept;
};

[[nodiscard]] bool validCrtXyRasterSpec(
    const CrtXyRasterSpec& spec) noexcept;

// Scalar double-precision reference implementation. It allocates and is
// intended for tests and non-realtime validation, not audio ingestion.
[[nodiscard]] CrtXyReferenceRaster rasterizeCrtXyReference(
    std::span<const CrtBeamSegment> segments,
    const CrtXyRasterSpec& spec);

[[nodiscard]] bool validCrtXyDecay(
    double deltaSeconds, double tauSeconds) noexcept;
[[nodiscard]] bool crtXyDecayFactor(
    double deltaSeconds, double tauSeconds, double& factor) noexcept;
[[nodiscard]] bool decayCrtXyRaster(
    std::span<double> energy,
    double deltaSeconds,
    double tauSeconds) noexcept;

} // namespace nekorox
