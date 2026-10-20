#include "../third_party/spectrum-analyzer/NekoRenderSampling.h"
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace {
struct Band { double Lo, Center, Value, Peak; };
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(std::string("Spectrum cache: ") + message);
}
bool exact(double a, double b) {
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}
}

void runSpectrumRenderCacheTests()
{
    // Resize across aggregation/interpolation boundaries, including a source
    // with only one supported frequency. Unsupported amplitudes remain data.
    for (auto sourceCount : {1u, 17u, 120u}) {
        std::vector<Band> bands;
        for (unsigned i = 0; i < sourceCount; ++i) {
            const double center = 16.3516 * std::pow(2., double(i) / 12.);
            bands.push_back({center * .97, center, 0., 0.});
        }
        neko_render_sampling_t cache;
        for (auto count : {1u, 12u, 119u, 120u, 121u, 133u, 150u, 240u, 360u}) {
            unsigned correctionCalls = 0;
            const auto correction = [&](double center) {
                ++correctionCalls; return std::log(center) * .125;
            };
            check(cache.update(bands, count, correction), "resize invalidation");
            check(cache.size() == count && correctionCalls == count,
                "all frequency corrections computed once");
            for (unsigned i = 0; i < count; ++i) {
                const auto& m = cache[i];
                check(m.first < bands.size() && m.last > m.first &&
                    m.last <= bands.size(), "bounded source support");
                double lo, center;
                if (count <= sourceCount) {
                    const auto first = i * sourceCount / count;
                    const auto last = std::max(first + 1, (i + 1) * sourceCount / count);
                    check(m.first == first && m.last == last, "aggregation coverage");
                    lo = bands[first].Lo;
                    center = std::sqrt(bands[first].Center * bands[last - 1].Center);
                } else {
                    const double p = sourceCount == 1 ? 0. :
                        double(i) * double(sourceCount - 1) / double(count - 1);
                    const auto first = std::size_t(std::floor(p));
                    const auto upper = std::min(first + 1, bands.size() - 1);
                    const double t = p - double(first);
                    lo = std::exp(std::lerp(std::log(bands[first].Lo), std::log(bands[upper].Lo), t));
                    center = std::exp(std::lerp(std::log(bands[first].Center), std::log(bands[upper].Center), t));
                }
                check(exact(m.lo, lo) && exact(m.center, center), "unchanged frequency anchors");
                check(exact(m.correctionDb, std::log(center) * .125), "unchanged correction");
            }
            check(cache[0].first == 0 && cache[count - 1].last == bands.size(),
                "full frequency axis preserved");
            const auto* address = &cache[0];
            const auto revision = cache.revision();
            correctionCalls = 0;
            for (unsigned frame = 0; frame < 1000; ++frame) {
                // Changing measured audio must neither invalidate frequency
                // metadata nor be cached in place of the current observation.
                bands[frame % bands.size()].Value = double(frame % 7) / 7.;
                bands[frame % bands.size()].Peak = double(frame % 11) / 11.;
                check(!cache.update(bands, count, correction), "audio-independent cache hit");
            }
            check(correctionCalls == 0 && &cache[0] == address &&
                cache.revision() == revision, "stable storage in steady playback");
        }
        const auto middle = bands.size() / 2;
        bands[middle].Center *= 1.002;
        check(cache.update(bands, 360, [](double c) { return c; }),
            "interior tuning change invalidates");
        bands[middle].Lo *= .999;
        check(cache.update(bands, 360, [](double c) { return c; }),
            "interior frequency boundary invalidates");
        cache.update(bands, 0, [](double c) { return c; });
        check(cache.size() == 0, "zero width clears mappings");
        bands.clear();
        cache.update(bands, 12, [](double c) { return c; });
        check(cache.size() == 0, "missing source never invents support");
    }
}
