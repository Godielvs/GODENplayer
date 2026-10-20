#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

// Only frequency anchors are cached. Every frame still samples the measured
// amplitudes, held peaks, opacity and temporal shadow of its original bands.
class neko_render_sampling_t final
{
public:
    struct mapping_t
    {
        std::size_t first = 0;
        std::size_t last = 0; // Exclusive aggregation bound, or upper + 1.
        double fraction = 0.;
        double lo = 0.;
        double center = 0.;
        double correctionDb = 0.;
    };

    template<typename Bands, typename Correction>
    bool update(const Bands& source, std::size_t count, Correction correction)
    {
        bool same = source.size() == sources_.size() &&
            count == mappings_.size();
        if (same) {
            for (std::size_t i = 0; i < source.size(); ++i) {
                if (source[i].Lo != sources_[i].lo ||
                        source[i].Center != sources_[i].center) {
                    same = false;
                    break;
                }
            }
        }
        if (same) return false;
        sources_.resize(source.size());
        for (std::size_t i = 0; i < source.size(); ++i)
            sources_[i] = {source[i].Lo, source[i].Center};
        if (source.empty() || count == 0) {
            mappings_.clear();
            return true;
        }
        mappings_.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            auto& m = mappings_[i];
            if (count <= source.size()) {
                m.first = i * source.size() / count;
                m.last = std::max(m.first + 1,
                    (i + 1) * source.size() / count);
                m.fraction = 0.;
                m.lo = source[m.first].Lo;
                m.center = std::sqrt(source[m.first].Center *
                    source[m.last - 1].Center);
            } else {
                const double position = count == 1 || source.size() == 1
                    ? 0. : double(i) * double(source.size() - 1) /
                        double(count - 1);
                m.first = std::size_t(std::floor(position));
                const auto upper = std::min(m.first + 1, source.size() - 1);
                m.last = upper + 1;
                m.fraction = position - double(m.first);
                m.lo = std::exp(std::lerp(std::log(source[m.first].Lo),
                    std::log(source[upper].Lo), m.fraction));
                m.center = std::exp(std::lerp(std::log(source[m.first].Center),
                    std::log(source[upper].Center), m.fraction));
            }
            m.correctionDb = correction(m.center);
        }
        ++revision_;
        return true;
    }

    const mapping_t& operator[](std::size_t i) const noexcept
    {
        return mappings_[i];
    }
    std::size_t size() const noexcept { return mappings_.size(); }
    std::size_t revision() const noexcept { return revision_; }

private:
    struct source_t { double lo, center; };
    std::vector<source_t> sources_;
    std::vector<mapping_t> mappings_;
    std::size_t revision_ = 0;
};
