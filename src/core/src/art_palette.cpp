#include "nekorox/art_palette.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace nekorox {
namespace {

constexpr double kPi = 3.14159265358979323846;

struct Oklab {
    double lightness = 0.0;
    double a = 0.0;
    double b = 0.0;
};

struct HueBin {
    double lightness = 0.0;
    double a = 0.0;
    double b = 0.0;
    double weight = 0.0;
    double score = 0.0;
};

double srgbToLinear(double value) noexcept
{
    value = std::clamp(value, 0.0, 1.0);
    return value <= 0.04045
        ? value / 12.92
        : std::pow((value + 0.055) / 1.055, 2.4);
}

double linearToSrgb(double value) noexcept
{
    value = std::clamp(value, 0.0, 1.0);
    return value <= 0.0031308
        ? value * 12.92
        : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

Oklab rgbToOklab(RgbColor color) noexcept
{
    const auto red = srgbToLinear(color.red);
    const auto green = srgbToLinear(color.green);
    const auto blue = srgbToLinear(color.blue);
    const auto l = std::cbrt(
        0.4122214708 * red + 0.5363325363 * green + 0.0514459929 * blue);
    const auto m = std::cbrt(
        0.2119034982 * red + 0.6806995451 * green + 0.1073969566 * blue);
    const auto s = std::cbrt(
        0.0883024619 * red + 0.2817188376 * green + 0.6299787005 * blue);
    return {
        0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
        1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
    };
}

RgbColor oklabToRgb(Oklab color) noexcept
{
    const auto l_ = color.lightness + 0.3963377774 * color.a + 0.2158037573 * color.b;
    const auto m_ = color.lightness - 0.1055613458 * color.a - 0.0638541728 * color.b;
    const auto s_ = color.lightness - 0.0894841775 * color.a - 1.2914855480 * color.b;
    const auto l = l_ * l_ * l_;
    const auto m = m_ * m_ * m_;
    const auto s = s_ * s_ * s_;
    return {
        static_cast<float>(linearToSrgb(
            4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s)),
        static_cast<float>(linearToSrgb(
            -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s)),
        static_cast<float>(linearToSrgb(
            -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s)),
    };
}

double hueDegrees(const Oklab& color) noexcept
{
    auto degrees = std::atan2(color.b, color.a) * 180.0 / kPi;
    if (degrees < 0.0) degrees += 360.0;
    return degrees;
}

double hueDistance(double left, double right) noexcept
{
    const auto distance = std::abs(left - right);
    return std::min(distance, 360.0 - distance);
}

RgbColor accessibleAccent(const Oklab& source, double minimumLightness)
{
    const auto hue = std::atan2(source.b, source.a);
    const auto sourceChroma = std::hypot(source.a, source.b);
    auto lightness = std::clamp(source.lightness, minimumLightness, 0.86);
    auto chroma = std::clamp(sourceChroma, 0.075, 0.18);
    const RgbColor background{0.035f, 0.039f, 0.039f};
    for (int attempt = 0; attempt < 10; ++attempt) {
        const auto color = oklabToRgb({
            lightness, std::cos(hue) * chroma, std::sin(hue) * chroma});
        if (contrastRatio(color, background) >= 4.5) return color;
        lightness = std::min(0.92, lightness + 0.025);
        chroma *= 0.94;
    }
    return {0.0f, 0.847f, 0.816f};
}

Oklab averaged(const HueBin& bin) noexcept
{
    if (bin.weight <= 0.0) return {};
    return {bin.lightness / bin.weight, bin.a / bin.weight, bin.b / bin.weight};
}

double luminance(RgbColor color) noexcept
{
    return 0.2126 * srgbToLinear(color.red) +
        0.7152 * srgbToLinear(color.green) +
        0.0722 * srgbToLinear(color.blue);
}

// A bounded, deterministic material palette. Medoids retain sampled pigments;
// population chooses the body colour, while small chromatic marks remain accents.
// No accessibility hue rotation or saturation boost enters this path.
void materialPalette(ArtPalette& out, const std::vector<RgbColor>& pixels,
                     const std::vector<Oklab>& labs)
{
    if(pixels.empty())return;
    constexpr std::size_t count=6;
    const auto distance=[](const Oklab&a,const Oklab&b){
        const double l=a.lightness-b.lightness,x=a.a-b.a,y=a.b-b.b;
        return l*l+x*x+y*y;
    };
    std::array<Oklab,count> centers{};
    centers[0]=labs[labs.size()/2];
    for(std::size_t k=1;k<count;++k){
        double farthest=-1.;
        for(const auto& candidate:labs){
            double nearest=1e9;
            for(std::size_t p=0;p<k;++p)nearest=std::min(nearest,distance(candidate,centers[p]));
            if(nearest>farthest){farthest=nearest;centers[k]=candidate;}
        }
    }
    std::array<std::size_t,count> populations{};
    std::vector<std::size_t> owners(labs.size());
    for(unsigned pass=0;pass<6;++pass){
        std::array<Oklab,count> sum{};populations={};
        for(std::size_t i=0;i<labs.size();++i){
            std::size_t owner=0;double nearest=1e9;
            for(std::size_t k=0;k<count;++k){const auto d=distance(labs[i],centers[k]);
                if(d<nearest){nearest=d;owner=k;}}
            owners[i]=owner;++populations[owner];sum[owner].lightness+=labs[i].lightness;
            sum[owner].a+=labs[i].a;sum[owner].b+=labs[i].b;
        }
        for(std::size_t k=0;k<count;++k)if(populations[k]){
            const double n=double(populations[k]);centers[k]={sum[k].lightness/n,sum[k].a/n,sum[k].b/n};
        }
    }
    std::array<RgbColor,count> pigment{};
    std::array<double,count> nearest;nearest.fill(1e9);
    for(std::size_t i=0;i<labs.size();++i){const auto k=owners[i];const auto d=distance(labs[i],centers[k]);
        if(d<nearest[k]){nearest[k]=d;pigment[k]=pixels[i];}}
    const auto representative=[&](std::size_t k){return populations[k]>=std::max<std::size_t>(1,pixels.size()/200);};
    std::size_t dominant=0,secondary=0,accent=0,base=0;double best=-1.;
    for(std::size_t k=0;k<count;++k)if(representative(k)){
        const double score=double(populations[k])*(centers[k].lightness<.16?.08:1.);
        if(score>best){best=score;dominant=k;}
    }
    secondary=accent=base=dominant;best=-1.;
    for(std::size_t k=0;k<count;++k)if(representative(k)&&k!=dominant&&centers[k].lightness>.12){
        const auto& body=centers[dominant];const auto& candidate=centers[k];
        const double cb=std::hypot(body.a,body.b),cc=std::hypot(candidate.a,candidate.b);
        const double affinity=cb>.025&&cc>.025?
            (body.a*candidate.a+body.b*candidate.b)/(cb*cc):1.;
        // Minor contrasting marks belong to the accent. They must not tint
        // the broad midrange material just because they are much lighter.
        if(affinity<.5&&double(populations[k])<.45*double(populations[dominant]))continue;
        const double score=double(populations[k])*
            (.03+std::min(.20,std::sqrt(distance(candidate,body))))*
            (.15+.85*std::max(0.,affinity));
        if(score>best){best=score;secondary=k;}
    }
    best=-1.;out.materialMonochrome=true;
    for(std::size_t k=0;k<count;++k)if(representative(k)){
        const auto chroma=std::hypot(centers[k].a,centers[k].b);
        if(chroma>.025)out.materialMonochrome=false;
        if(centers[k].lightness<centers[base].lightness)base=k;
        const double score=(.025+chroma)*std::sqrt(double(populations[k]))*
            (k==dominant||k==secondary?.15:1.)*(.15+centers[k].lightness)*
            (.05+std::sqrt(distance(centers[k],centers[dominant])));
        if(score>best){best=score;accent=k;}
    }
    if(out.materialMonochrome){
        accent=dominant;
        for(std::size_t k=0;k<count;++k)if(representative(k)&&centers[k].lightness>centers[accent].lightness)accent=k;
    }
    const auto scale=[](RgbColor c,float amount){
        const float peak=std::max({c.red,c.green,c.blue});
        amount=std::min(amount,1.f/std::max(peak,1e-6f));
        return RgbColor{c.red*amount,c.green*amount,c.blue*amount};
    };
    auto body=pigment[dominant];
    // An all-black cover still has a neutral, quiet material in silence.
    if(std::max({body.red,body.green,body.blue})<.0001f)body={.02f,.02f,.02f};
    out.material={body,secondary==dominant?scale(body,.70f):pigment[secondary],
        accent==dominant?scale(body,1.25f):pigment[accent],
        base==dominant?scale(body,.04f):pigment[base]};
    if(out.materialMonochrome)for(auto& color:out.material){
        const float light=.2126f*color.red+.7152f*color.green+.0722f*color.blue;
        color={light,light,light};
    }
    out.materialValid=true;
}

} // namespace

ArtPalette extractArtPalette(
    std::span<const std::uint8_t> pixels,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t stride)
{
    ArtPalette result;
    if (width == 0 || height == 0 || stride < width * 4U ||
        pixels.size() < static_cast<std::size_t>(stride) * height) {
        return result;
    }

    constexpr std::size_t kBins = 24;
    std::array<HueBin, kBins> bins{};
    std::vector<RgbColor> flowPixels;
    std::vector<Oklab> flowLabs;
    flowPixels.reserve(4096);
    flowLabs.reserve(4096);
    std::vector<RgbColor> materialPixels;
    std::vector<Oklab> materialLabs;
    materialPixels.reserve(4096);materialLabs.reserve(4096);
    constexpr std::uint32_t kMaximumSamples = 4096;
    const auto aspect = static_cast<double>(width) /
        static_cast<double>(height);
    const auto columns = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::floor(
            std::sqrt(kMaximumSamples * aspect))), 1U, width);
    const auto rows = std::clamp<std::uint32_t>(
        kMaximumSamples / columns, 1U, height);
    std::size_t accepted = 0;
    // Sample the centre of every stratum. Unlike the old raster-prefix cap,
    // this covers every quadrant (including 1xN strips) with <=4096 samples.
    for (std::uint32_t rowIndex = 0; rowIndex < rows; ++rowIndex) {
        const auto y = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(rowIndex) * 2U + 1U) * height /
            (static_cast<std::uint64_t>(rows) * 2U));
        const auto* row = pixels.data() + static_cast<std::size_t>(y) * stride;
        for (std::uint32_t columnIndex = 0; columnIndex < columns;
                ++columnIndex) {
            const auto x = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(columnIndex) * 2U + 1U) * width /
                (static_cast<std::uint64_t>(columns) * 2U));
            const auto* pixel = row + static_cast<std::size_t>(x) * 4U;
            const auto alpha = static_cast<double>(pixel[3]) / 255.0;
            if (alpha < 0.5) continue;
            const RgbColor rgb{
                static_cast<float>(std::clamp(pixel[2] / (255.0 * alpha), 0.0, 1.0)),
                static_cast<float>(std::clamp(pixel[1] / (255.0 * alpha), 0.0, 1.0)),
                static_cast<float>(std::clamp(pixel[0] / (255.0 * alpha), 0.0, 1.0)),
            };
            const auto lab = rgbToOklab(rgb);
            materialPixels.push_back({static_cast<float>(srgbToLinear(rgb.red)),
                static_cast<float>(srgbToLinear(rgb.green)),static_cast<float>(srgbToLinear(rgb.blue))});
            materialLabs.push_back(lab);
            if (lab.lightness >= 0.08 &&
                    !(lab.lightness > 0.97 &&
                      std::hypot(lab.a, lab.b) < 0.018)) {
                flowPixels.push_back({
                    static_cast<float>(srgbToLinear(rgb.red)),
                    static_cast<float>(srgbToLinear(rgb.green)),
                    static_cast<float>(srgbToLinear(rgb.blue))});
                flowLabs.push_back(lab);
            }
            const auto chroma = std::hypot(lab.a, lab.b);
            if (lab.lightness < 0.16 || lab.lightness > 0.94 || chroma < 0.025) continue;
            const auto hue = hueDegrees(lab);
            const auto index = std::min<std::size_t>(kBins - 1,
                static_cast<std::size_t>(hue / (360.0 / kBins)));
            const auto weight = alpha * (0.35 + chroma * 4.0) *
                (0.55 + std::sin(std::clamp(lab.lightness, 0.0, 1.0) * kPi));
            auto& bin = bins[index];
            bin.lightness += lab.lightness * weight;
            bin.a += lab.a * weight;
            bin.b += lab.b * weight;
            bin.weight += weight;
            bin.score += weight * (0.55 + chroma * 3.0);
            ++accepted;
        }
    }
    materialPalette(result,materialPixels,materialLabs);
    // Four deterministic OKLab clusters feed the background. Representative
    // clusters must cover at least 0.5% of samples; monochrome artwork is
    // separated only by luminance instead of inventing unrelated hues.
    if (!flowPixels.empty()) {
        std::array<Oklab, 4> centers{};
        centers[0] = *std::max_element(flowLabs.begin(), flowLabs.end(),
            [](const Oklab& a, const Oklab& b) {
                return std::hypot(a.a, a.b) < std::hypot(b.a, b.b);
            });
        for (std::size_t cluster = 1; cluster < centers.size(); ++cluster) {
            auto best = -1.0;
            for (const auto& candidate : flowLabs) {
                auto distance = std::numeric_limits<double>::max();
                for (std::size_t prior = 0; prior < cluster; ++prior) {
                    const auto dl = candidate.lightness - centers[prior].lightness;
                    const auto da = candidate.a - centers[prior].a;
                    const auto db = candidate.b - centers[prior].b;
                    distance = std::min(distance, dl*dl + da*da + db*db);
                }
                if (distance > best) { best = distance; centers[cluster] = candidate; }
            }
        }
        std::array<RgbColor, 4> colors{};
        std::array<std::size_t, 4> populations{};
        for (int pass = 0; pass < 6; ++pass) {
            std::array<Oklab, 4> sums{};
            colors = {};
            populations = {};
            for (std::size_t i = 0; i < flowLabs.size(); ++i) {
                std::size_t chosen = 0;
                auto best = std::numeric_limits<double>::max();
                for (std::size_t cluster = 0; cluster < centers.size(); ++cluster) {
                    const auto dl = flowLabs[i].lightness-centers[cluster].lightness;
                    const auto da = flowLabs[i].a-centers[cluster].a;
                    const auto db = flowLabs[i].b-centers[cluster].b;
                    const auto distance = dl*dl+da*da+db*db;
                    if (distance < best) { best = distance; chosen = cluster; }
                }
                sums[chosen].lightness += flowLabs[i].lightness;
                sums[chosen].a += flowLabs[i].a;
                sums[chosen].b += flowLabs[i].b;
                colors[chosen].red += flowPixels[i].red;
                colors[chosen].green += flowPixels[i].green;
                colors[chosen].blue += flowPixels[i].blue;
                ++populations[chosen];
            }
            for (std::size_t cluster = 0; cluster < centers.size(); ++cluster) {
                if (populations[cluster] == 0) continue;
                const auto n = static_cast<double>(populations[cluster]);
                centers[cluster] = {sums[cluster].lightness/n,
                    sums[cluster].a/n, sums[cluster].b/n};
                colors[cluster] = {static_cast<float>(colors[cluster].red/n),
                    static_cast<float>(colors[cluster].green/n),
                    static_cast<float>(colors[cluster].blue/n)};
            }
        }
        const auto representative = [&](std::size_t index) {
            return populations[index] >= std::max<std::size_t>(
                1, flowPixels.size()/200);
        };
        const auto distance = [&](std::size_t a, std::size_t b) {
            const auto dl=centers[a].lightness-centers[b].lightness;
            const auto da=centers[a].a-centers[b].a;
            const auto db=centers[a].b-centers[b].b;
            return dl*dl+da*da+db*db;
        };
        std::size_t a=0,b=0,c=0; auto best=-1.0;
        for(std::size_t k=0;k<4;++k) if(representative(k)) {
            const auto population=std::sqrt(
                static_cast<double>(populations[k])/flowPixels.size());
            const auto chroma=std::hypot(centers[k].a,centers[k].b);
            // Large near-black fields are useful as a base, but must not erase
            // thin chromatic artwork. flowBase already carries the dark field,
            // so give a representative chromatic cluster an area-independent
            // term when choosing the material's leading pigment.
            const auto score=population*(0.020+chroma)+0.35*chroma;
            if(score>best){best=score;a=k;}
        }
        best=-1.0; for(std::size_t k=0;k<4;++k) if(k!=a&&representative(k)) {
            const auto population=std::sqrt(
                static_cast<double>(populations[k])/flowPixels.size());
            const auto chroma=std::hypot(centers[k].a,centers[k].b);
            const auto score=distance(k,a)*population*(0.35+3.0*chroma)+0.08*chroma;
            if(score>best){best=score;b=k;}
        }
        best=-1.0; for(std::size_t k=0;k<4;++k) if(k!=a&&k!=b&&representative(k)) {
            const auto population=std::sqrt(
                static_cast<double>(populations[k])/flowPixels.size());
            const auto chroma=std::hypot(centers[k].a,centers[k].b);
            const auto score=std::min(distance(k,a),distance(k,b))*population*
                (0.35+3.0*chroma)+0.08*chroma;
            if(score>best){best=score;c=k;}
        }
        const auto scale=[](RgbColor v,float amount){return RgbColor{
            std::clamp(v.red*amount,0.0f,1.0f),
            std::clamp(v.green*amount,0.0f,1.0f),
            std::clamp(v.blue*amount,0.0f,1.0f)};};
        result.flowValid=true;
        result.flowMonochrome=true;
        for(std::size_t k=0;k<4;++k) if(representative(k)&&
            std::hypot(centers[k].a,centers[k].b)>0.025) result.flowMonochrome=false;
        result.flowLow=colors[a];
        result.flowMid=(b==a||distance(a,b)<0.001)?scale(colors[a],0.58f):colors[b];
        result.flowHigh=(c==a||c==b)?scale(colors[a],1.25f):colors[c];
        result.flowBase=scale(colors[a],0.025f);
    }
    if (accepted < 8) return result;

    const auto primaryIterator = std::max_element(
        bins.begin(), bins.end(), [](const HueBin& left, const HueBin& right) {
            return left.score < right.score;
        });
    if (primaryIterator == bins.end() || primaryIterator->weight <= 0.0) return result;
    const auto primaryLab = averaged(*primaryIterator);
    const auto primaryHue = hueDegrees(primaryLab);

    const HueBin* secondaryBin = nullptr;
    double secondaryScore = -std::numeric_limits<double>::infinity();
    for (const auto& bin : bins) {
        if (bin.weight <= 0.0) continue;
        const auto candidate = averaged(bin);
        const auto separation = hueDistance(primaryHue, hueDegrees(candidate));
        if (separation < 42.0) continue;
        const auto score = bin.score * (0.75 + separation / 360.0);
        if (score > secondaryScore) {
            secondaryScore = score;
            secondaryBin = &bin;
        }
    }
    auto secondaryLab = secondaryBin != nullptr
        ? averaged(*secondaryBin)
        : Oklab{primaryLab.lightness,
            std::cos((primaryHue + 135.0) * kPi / 180.0) * 0.10,
            std::sin((primaryHue + 135.0) * kPi / 180.0) * 0.10};

    result.valid = true;
    result.primary = accessibleAccent(primaryLab, 0.70);
    result.secondary = accessibleAccent(secondaryLab, 0.68);
    const auto highlightLab = Oklab{
        0.88, primaryLab.a * 0.36, primaryLab.b * 0.36};
    result.highlight = accessibleAccent(highlightLab, 0.82);
    return result;
}

double contrastRatio(RgbColor foreground, RgbColor background) noexcept
{
    const auto first = luminance(foreground);
    const auto second = luminance(background);
    const auto lighter = std::max(first, second);
    const auto darker = std::min(first, second);
    return (lighter + 0.05) / (darker + 0.05);
}

RgbColor interpolateOklch(
    RgbColor from,
    RgbColor to,
    float progress) noexcept
{
    const auto amount = std::clamp(
        static_cast<double>(progress), 0.0, 1.0);
    if (amount <= 0.0) return from;
    if (amount >= 1.0) return to;

    const auto first = rgbToOklab(from);
    const auto second = rgbToOklab(to);
    const auto firstChroma = std::hypot(first.a, first.b);
    const auto secondChroma = std::hypot(second.a, second.b);
    auto firstHue = std::atan2(first.b, first.a);
    auto secondHue = std::atan2(second.b, second.a);
    if (firstChroma < 1e-5) firstHue = secondHue;
    if (secondChroma < 1e-5) secondHue = firstHue;
    auto hueDelta = std::remainder(secondHue - firstHue, 2.0 * kPi);

    auto interpolated = Oklab{
        std::lerp(first.lightness, second.lightness, amount),
        0.0,
        0.0,
    };
    const auto chroma =
        std::lerp(firstChroma, secondChroma, amount);
    const auto hue = firstHue + hueDelta * amount;
    interpolated.a = std::cos(hue) * chroma;
    interpolated.b = std::sin(hue) * chroma;

    const RgbColor background{0.035f, 0.039f, 0.039f};
    auto result = oklabToRgb(interpolated);
    for (int attempt = 0;
         attempt < 8 &&
         contrastRatio(result, background) < 4.5;
         ++attempt) {
        interpolated.lightness =
            std::min(0.94, interpolated.lightness + 0.02);
        interpolated.a *= 0.98;
        interpolated.b *= 0.98;
        result = oklabToRgb(interpolated);
    }
    return result;
}

} // namespace nekorox
