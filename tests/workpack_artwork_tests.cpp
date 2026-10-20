#include "artwork_broker_policy.h"

#include <cstdint>
#include <stdexcept>
#include "nekorox/artwork_resampler.h"
#include "nekorox/presented_animation_clock.h"
#include "nekorox/analysis_source_revision.h"
#include <fstream>
#include <windows.h>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

} // namespace

void runWorkpackArtworkTests()
{
    using namespace nekorox::component;
    const auto revisionFile = std::filesystem::temp_directory_path() /
        (L"goden-analysis-revision-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    { std::ofstream stream(revisionFile, std::ios::binary); stream << "original"; }
    const auto revision = nekorox::analysisSourceRevision(revisionFile);
    require(revision.has_value() && revision == nekorox::analysisSourceRevision(revisionFile),
        "unchanged analysis source revision was not stable");
    { std::ofstream stream(revisionFile, std::ios::binary); stream << "changed contents"; }
    require(revision != nekorox::analysisSourceRevision(revisionFile),
        "changed analysis source retained its admitted revision");
    std::filesystem::remove(revisionFile);
    require(!nekorox::analysisSourceRevision(revisionFile), "missing analysis source was accepted");
    nekorox::PresentedAnimationClock coverClock, fieldClock;
    coverClock.reset(1); fieldClock.reset(1);
    require(!coverClock.accept(1, false, 1.0), "rejected frame started artwork clock");
    require(coverClock.elapsedMilliseconds(100.0) == 0, "unpresented animation consumed time");
    require(coverClock.accept(1, true, 100.0) && fieldClock.accept(1, true, 100.0), "first accepted generation did not start");
    require(coverClock.startSeconds() == fieldClock.startSeconds(), "shared accepted time diverged");
    require(coverClock.elapsedMilliseconds(100.125) == 125.0, "accepted animation clock trajectory changed");
    require(!coverClock.accept(1, true, 200.0) && coverClock.startSeconds() == 100.0, "duplicate ack restarted animation");
    coverClock.reset(2);
    require(!coverClock.accept(1, true, 200.0) && !coverClock.started(), "stale generation started successor");
    require(coverClock.accept(2, true, 201.0), "successor could not be presented");
    require(artworkDecodeFitsBudget(12U*1024*1024,2048U*2048*4,256U*256*4),
        "large cover that fits alone was rejected");
    require(!artworkDecodeFitsBudget(32U*1024*1024,2048U*2048*4,256U*256*4),
        "intrinsically oversized artwork must not enter transient retry loops");
    require(!artworkDecodeFitsBudget(SIZE_MAX,SIZE_MAX,4),
        "artwork peak accounting wrapped around");
    std::vector<std::uint8_t> image(720U*720U*4U,255U);
    unsigned checks=0;bool cancelled=false;
    try{(void)nekorox::resampleArtworkPbgraV1(image,720,720,80,80,[&]{
        if(++checks==5)throw std::runtime_error("cancel resample");});}
    catch(const std::runtime_error&){cancelled=true;}
    require(cancelled&&checks==5,"superseded artwork did not stop during resampling");
    image.resize(32U*32U*4U);
    const auto original=nekorox::resampleArtworkPbgraV1(image,32,32,16,16);
    checks=0;const auto observed=nekorox::resampleArtworkPbgraV1(image,32,32,16,16,[&]{++checks;});
    require(checks>0&&original.bgra==observed.bgra,"cancellation checks changed reconstructed pixels");

    require(kArtworkMaximumPendingRequests == 16U,
        "artwork pending queue contract changed");
    require(kArtworkMaximumEntries == 256U,
        "artwork entry cap contract changed");
    require(artworkBudgetCanAdd(32U, 32U, 64U),
        "exact temporary budget should be accepted");
    require(!artworkBudgetCanAdd(33U, 32U, 64U),
        "temporary budget overflow should be rejected");
    require(!artworkBudgetCanAdd(0U, 65U, 64U),
        "single oversize allocation should be rejected");
    require(artworkPriorityDisplaces(2U, 0U),
        "visible main art must displace queued prefetch");
    require(artworkPriorityDisplaces(2U, 1U),
        "visible row art must displace queued prefetch");
    require(!artworkPriorityDisplaces(1U, 1U),
        "equal-priority work must not churn a full queue");
    require(!artworkPriorityDisplaces(0U, 2U),
        "prefetch must not displace visible main art");
    require(artworkBgraBytes(2048U, 2048U) == 16U * 1024U * 1024U,
        "2048 BGRA accounting mismatch");
    require(artworkBgraBytes(0U, 2048U) == 0U,
        "zero-sized artwork must be rejected");
    require(artworkBgraBytes(UINT32_MAX, UINT32_MAX) == 0U,
        "malformed dimensions must overflow fail-closed");
    require(artworkBudgetCanAdd(48U * 1024U * 1024U,
            48U * 1024U * 1024U, kArtworkRendererGpuBudgetBytes),
        "renderer should accept an exact 96 MiB transition");
    require(!artworkBudgetCanAdd(64U * 1024U * 1024U,
            64U * 1024U * 1024U, kArtworkRendererGpuBudgetBytes),
        "renderer must reject a 128 MiB transition pair");
}
