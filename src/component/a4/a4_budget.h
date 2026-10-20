#pragma once
// GPU timings are diagnostic only. Quality changes are always explicit user
// choices; expensive timing windows never degrade or stop a shader.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace nekorox::component::a4 {
enum class QualityTier : unsigned { full=0, light=1, fieldOnly=2, staticFallback=3 };
enum class QualityMode : unsigned { full=0, automatic=1, light=2, fieldOnly=3 };
enum class GpuStage : unsigned { field=0, prefilter=1, scene=2, finish=3, count=4 };
constexpr unsigned stageBit(GpuStage s) noexcept { return 1U << static_cast<unsigned>(s); }
constexpr std::size_t stageCount=static_cast<std::size_t>(GpuStage::count);
// Adaptive quality keeps a progressively restrained film layer until the final
// static fallback.  Otherwise the normal Full -> Light -> FieldOnly settling is
// seen by the user as the noise abruptly disappearing after startup.
inline float grainAmplitudeForTier(QualityTier tier,float requested) noexcept {
    const auto clamped=std::clamp(requested,0.f,.09f);
    switch(tier) {
    case QualityTier::full:return clamped;
    case QualityTier::light:return clamped*.55f;
    case QualityTier::fieldOnly:return clamped*.32f;
    default:return 0.f;
    }
}
struct QualifiedGpuSample {
    std::uint64_t frameId=0, epoch=0;
    unsigned presentMask=0, requiredMask=0;
    bool valid=false, eligible=false;
    std::array<double,stageCount> gpuUs{}, cpuUs{};
    double inclusiveUs=0;
};
struct BudgetSnapshot {
    QualityTier tier=QualityTier::full;
    std::uint64_t epoch=1, accepted=0, ignored=0, completedWindows=0;
    unsigned inWindow=0, expensiveWindows=0;
    double decorativeP95Us=0;
    std::array<double,stageCount> stageP95Us{};
};
class DecorativeBudget final {
public:
    explicit DecorativeBudget(bool /*automaticDowngrade*/=false,
        QualityTier configuredTier=QualityTier::full) noexcept
        : configuredTier_(configuredTier) { state_.tier=configuredTier_; }
    static constexpr unsigned samplesPerWindow=30;
    static constexpr unsigned warmupSamples=8;
    static constexpr unsigned windowsBeforeDowngrade=3;
    static constexpr double fieldLimitUs=500, decorativeLimitUs=1000;
    [[nodiscard]] const BudgetSnapshot& snapshot() const noexcept { return state_; }
    [[nodiscard]] QualityTier tier() const noexcept { return state_.tier; }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return state_.epoch; }
    [[nodiscard]] bool latched() const noexcept { return tier()==QualityTier::staticFallback; }
    // Never rewind epoch. Pending query results from earlier attempts remain inert.
    void restart(bool restoreConfiguredQuality) noexcept {
        if(restoreConfiguredQuality) state_.tier=configuredTier_;
        ++state_.epoch; if(state_.epoch==0) ++state_.epoch;
        state_.inWindow=state_.expensiveWindows=0;
        state_.decorativeP95Us=0; state_.stageP95Us={};
        warmupLeft_=warmupSamples; lastFrameId_=0;
    }
    bool configure(bool /*automaticDowngrade*/,QualityTier configuredTier) noexcept {
        if(configuredTier==QualityTier::staticFallback)
            configuredTier=QualityTier::fieldOnly;
        if(configuredTier_==configuredTier)return false;
        configuredTier_=configuredTier;
        state_.tier=configuredTier_;
        restart(false);
        return true;
    }
    bool requireAtLeast(QualityTier tier) noexcept {
        if(static_cast<unsigned>(tier)<=static_cast<unsigned>(state_.tier))return false;
        state_.tier=tier;restart(false);return true;
    }
    // Records qualified timings. It never changes quality automatically.
    bool submit(const QualifiedGpuSample& s) noexcept {
        if(latched() || !s.valid || !s.eligible || s.epoch!=state_.epoch ||
            s.frameId==0 || s.frameId<=lastFrameId_ || s.requiredMask==0 ||
            (s.presentMask&s.requiredMask)!=s.requiredMask) {
            ++state_.ignored; return false;
        }
        double total=0;
        for(std::size_t i=0;i<stageCount;++i) {
            if((s.presentMask&(1U<<i))==0) continue;
            if(!std::isfinite(s.gpuUs[i]) || s.gpuUs[i]<0 || s.gpuUs[i]>1e7) {
                ++state_.ignored; return false;
            }
            total+=s.gpuUs[i];
        }
        lastFrameId_=s.frameId;
        if(warmupLeft_) { --warmupLeft_; ++state_.ignored; return false; }
        const auto n=state_.inWindow;
        totals_[n]=total;
        for(std::size_t i=0;i<stageCount;++i) stages_[i][n]=s.gpuUs[i];
        ++state_.accepted; ++state_.inWindow;
        if(state_.inWindow<samplesPerWindow) return false;
        state_.decorativeP95Us=percentile(totals_);
        for(std::size_t i=0;i<stageCount;++i) state_.stageP95Us[i]=percentile(stages_[i]);
        ++state_.completedWindows; state_.inWindow=0;
        const bool expensive=state_.decorativeP95Us>decorativeLimitUs ||
            state_.stageP95Us[0]>fieldLimitUs;
        state_.expensiveWindows=expensive?state_.expensiveWindows+1:0;
        return false;
    }
private:
    static double percentile(std::array<double,samplesPerWindow> values) noexcept {
        std::sort(values.begin(),values.end());
        return values[(samplesPerWindow*95U+99U)/100U-1U];
    }
    BudgetSnapshot state_{};
    std::uint64_t lastFrameId_=0;
    unsigned warmupLeft_=warmupSamples;
    QualityTier configuredTier_=QualityTier::full;
    std::array<double,samplesPerWindow> totals_{};
    std::array<std::array<double,samplesPerWindow>,stageCount> stages_{};
};
} // namespace nekorox::component::a4
