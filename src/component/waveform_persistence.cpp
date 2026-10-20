#include "waveform_persistence.h"

#include "nekorox/database.h"
#include "nekorox/track_identity.h"
#include "text_util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>

namespace nekorox::component {
namespace {

constexpr char kWaveSeekAnalyzerVersion[] = "nekorox-waveseek-v2";
constexpr char kWaveSeekLayerName[] = "waveform-pyramid-v2";

std::int64_t utcNowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t durationMs(double seconds) noexcept
{
    if (!std::isfinite(seconds) || seconds <= 0.0) return 0;
    constexpr auto maximum =
        static_cast<double>(std::numeric_limits<std::int64_t>::max()) /
        1000.0;
    return static_cast<std::int64_t>(
        std::llround(std::min(seconds, maximum) * 1000.0));
}

std::uint64_t fileTimeTicks(
    const std::filesystem::file_time_type& value) noexcept
{
    return static_cast<std::uint64_t>(
        value.time_since_epoch().count());
}

bool identifyTrack(
    const WaveformCacheKey& key,
    PersistedTrack& track,
    std::string& error)
{
    std::filesystem::path path;
    try {
        const auto native =
            filesystem::g_get_native_path(key.path.c_str());
        path = utf8ToWide(native.c_str());
    } catch (const std::exception& exception) {
        error = std::string("WaveSeek native path failed: ") +
            exception.what();
        return false;
    }

    std::error_code statusError;
    const auto size = std::filesystem::file_size(path, statusError);
    if (statusError) {
        error = "WaveSeek file size failed: " +
            statusError.message();
        return false;
    }
    const auto modified =
        std::filesystem::last_write_time(path, statusError);
    if (statusError) {
        error = "WaveSeek file timestamp failed: " +
            statusError.message();
        return false;
    }

    try {
        const auto identity = makeTrackId({
            path,
            key.subsong,
            size,
            fileTimeTicks(modified),
        });
        track.trackId = identity.value;
        track.path = identity.normalizedPathUtf8;
        track.partialFingerprint = identity.partialFingerprint;
    } catch (const std::exception& exception) {
        error = std::string("WaveSeek track identity failed: ") +
            exception.what();
        return false;
    }

    track.subsong = key.subsong;
    track.fileSize = size;
    track.modifiedTicks = fileTimeTicks(modified);
    track.durationMs = durationMs(key.durationSeconds);
    track.title = wideToUtf8(path.stem().wstring());
    track.lastSeenUtcMs = utcNowMs();
    return true;
}

class SqliteWaveformPersistence final
    : public WaveformPersistenceHook {
public:
    bool load(
        const WaveformCacheKey& key,
        std::vector<std::byte>& encoded,
        std::string& error) override
    {
        encoded.clear();
        PersistedTrack track;
        if (!identifyTrack(key, track, error) ||
            !ensureOpen(error)) {
            return false;
        }

        PersistedAnalysisBundle bundle;
        if (!database_.readAnalysisBundle(
                track.trackId,
                kWaveSeekAnalyzerVersion,
                bundle,
                error)) {
            return false;
        }
        if (!bundle.found) {
            error.clear();
            return false;
        }
        const auto iterator = std::find_if(
            bundle.layers.begin(),
            bundle.layers.end(),
            [](const PersistedAnalysisLayer& layer) {
                return layer.name == kWaveSeekLayerName;
            });
        if (iterator == bundle.layers.end()) {
            error = "WaveSeek cache layer is missing";
            return false;
        }
        encoded = iterator->data;
        return true;
    }

    bool store(
        const WaveformCacheKey& key,
        std::span<const std::byte> encoded,
        std::string& error) override
    {
        if (encoded.empty()) {
            error = "WaveSeek cache payload is empty";
            return false;
        }

        PersistedTrack track;
        if (!identifyTrack(key, track, error) ||
            !ensureOpen(error)) {
            return false;
        }

        PersistedAnalysisBundle bundle;
        bundle.trackId = track.trackId;
        bundle.analyzerVersion = kWaveSeekAnalyzerVersion;
        bundle.state = "complete";
        bundle.manifestJson =
            "{\"schema\":\"WaveformPyramidV2\",\"version\":2," 
            "\"detail\":\"stereo-density-bands-transient\"}";
        bundle.updatedUtcMs = utcNowMs();
        bundle.lastAccessUtcMs = bundle.updatedUtcMs;
        bundle.layers.push_back({
            kWaveSeekLayerName,
            10,
            std::vector<std::byte>(encoded.begin(), encoded.end()),
        });
        return database_.storeAnalysisBundle(
            track,
            bundle,
            kAnalysisCacheLimitBytes,
            error);
    }

private:
    bool ensureOpen(std::string& error)
    {
        if (database_.isOpen()) return true;
        try {
            const auto profile =
                core_api::pathInProfile("nekorox/nekorox.db");
            const auto native =
                filesystem::g_get_native_path(profile.c_str());
            return database_.open(
                utf8ToWide(native.c_str()),
                error);
        } catch (const std::exception& exception) {
            error = std::string("WaveSeek database path failed: ") +
                exception.what();
            return false;
        }
    }

    NekoDatabase database_;
};

} // namespace

std::shared_ptr<WaveformPersistenceHook>
createWaveformPersistenceHook()
{
    return std::make_shared<SqliteWaveformPersistence>();
}

} // namespace nekorox::component
