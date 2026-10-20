#include "listen_presentation_model.h"

#include "text_util.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace nekorox::component {
namespace {

std::string_view object(
    std::string_view json,
    std::string_view name) noexcept
{
    const auto key = std::string("\"") + std::string(name) + "\"";
    auto begin = json.find(key);
    if (begin == std::string_view::npos) return {};
    begin = json.find('{', begin + key.size());
    if (begin == std::string_view::npos) return {};
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (auto index = begin; index < json.size(); ++index) {
        const auto value = json[index];
        if (quoted) {
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == '"') quoted = false;
            continue;
        }
        if (value == '"') quoted = true;
        else if (value == '{') ++depth;
        else if (value == '}' && --depth == 0) {
            return json.substr(begin, index - begin + 1U);
        }
    }
    return {};
}

std::string_view array(
    std::string_view json,
    std::string_view name) noexcept
{
    const auto key = std::string("\"") + std::string(name) + "\"";
    auto begin = json.find(key);
    if (begin == std::string_view::npos) return {};
    begin = json.find('[', begin + key.size());
    if (begin == std::string_view::npos) return {};
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (auto index = begin; index < json.size(); ++index) {
        const auto value = json[index];
        if (quoted) {
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == '"') quoted = false;
            continue;
        }
        if (value == '"') quoted = true;
        else if (value == '[') ++depth;
        else if (value == ']' && --depth == 0) {
            return json.substr(begin, index - begin + 1U);
        }
    }
    return {};
}

std::vector<std::string_view> objects(std::string_view source)
{
    std::vector<std::string_view> result;
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    std::size_t begin = std::string_view::npos;
    for (std::size_t index = 0; index < source.size(); ++index) {
        const auto value = source[index];
        if (quoted) {
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == '"') quoted = false;
            continue;
        }
        if (value == '"') quoted = true;
        else if (value == '{') {
            if (depth++ == 0) begin = index;
        } else if (value == '}' && --depth == 0 &&
            begin != std::string_view::npos) {
            result.push_back(source.substr(begin, index - begin + 1U));
            begin = std::string_view::npos;
        }
    }
    return result;
}

std::optional<double> number(
    std::string_view json,
    std::string_view field) noexcept
{
    const auto key = std::string("\"") + std::string(field) + "\"";
    auto position = json.find(key);
    if (position == std::string_view::npos) return std::nullopt;
    position = json.find(':', position + key.size());
    if (position == std::string_view::npos) return std::nullopt;
    ++position;
    while (position < json.size() &&
        (json[position] == ' ' || json[position] == '\t')) ++position;
    const auto length = std::min<std::size_t>(48U, json.size() - position);
    std::string value(json.substr(position, length));
    char* end = nullptr;
    const auto parsed = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || !std::isfinite(parsed)) return std::nullopt;
    return parsed;
}

std::optional<double> objectNumber(
    std::string_view json,
    std::string_view objectName,
    std::string_view field) noexcept
{
    return number(object(json, objectName), field);
}

std::wstring objectString(
    std::string_view json,
    std::string_view objectName,
    std::string_view field)
{
    const auto source = object(json, objectName);
    const auto key = std::string("\"") + std::string(field) + "\"";
    auto position = source.find(key);
    if (position == std::string_view::npos) return {};
    position = source.find(':', position + key.size());
    position = source.find('"', position);
    if (position == std::string_view::npos) return {};
    const auto end = source.find('"', position + 1U);
    if (end == std::string_view::npos) return {};
    return utf8ToWide(std::string(
        source.substr(position + 1U, end - position - 1U)));
}

std::wstring dbText(const std::optional<double>& value)
{
    if (!value) return L"\u2014";
    wchar_t text[24]{};
    std::swprintf(text, std::size(text), L"%.1f dB", *value);
    return text;
}

std::wstring signedText(const std::optional<double>& value)
{
    if (!value) return L"\u2014";
    wchar_t text[24]{};
    std::swprintf(text, std::size(text), L"%+.2f", *value);
    return text;
}

std::wstring percent(const std::optional<double>& value)
{
    return value
        ? std::to_wstring(static_cast<int>(std::lround(*value * 100.0))) + L"%"
        : L"\u2014";
}

} // namespace

std::shared_ptr<const ListenPresentationModelV3>
buildListenPresentationModelV3(
    const LibraryTrack* track,
    const DeepAnalysisSnapshot& analysis,
    const std::vector<LibraryTrack>& library,
    std::uint64_t libraryGeneration)
{
    auto result = std::make_shared<ListenPresentationModelV3>();
    result->libraryGeneration = libraryGeneration;
    result->analysisRequestId = analysis.requestId;
    result->analysisStatus = analysis.status;
    result->analysisStage = analysis.stage;
    result->analysisProgress = analysis.overallProgress;
    if (track != nullptr) {
        result->trackPath = track->path;
        result->formatLine = track->codec.empty()
            ? L"Local audio" : track->codec;
        const auto append = [&](std::wstring value) {
            if (value.empty()) return;
            if (!result->formatLine.empty()) result->formatLine += L"  |  ";
            result->formatLine += std::move(value);
        };
        if (track->sampleRate > 0) {
            append(std::to_wstring(track->sampleRate) + L" Hz");
        }
        if (track->bitsPerSample > 0) {
            append(std::to_wstring(track->bitsPerSample) + L" bit");
        }

        std::vector<const LibraryTrack*> album;
        album.reserve(24);
        for (const auto& candidate : library) {
            if (candidate.album == track->album &&
                candidate.albumArtist == track->albumArtist) {
                album.push_back(&candidate);
            }
        }
        std::sort(album.begin(), album.end(), [](const auto* left, const auto* right) {
            if (left->disc != right->disc) return left->disc < right->disc;
            return left->track < right->track;
        });
        result->albumRows.reserve(album.size());
        for (const auto* candidate : album) {
            result->albumRows.push_back({
                candidate->disc,
                candidate->track,
                candidate->path == track->path,
                candidate->title});
        }
    }

    switch (analysis.status) {
    case DeepAnalysisStatus::running:
        result->analysisStatusText = analysisStageLabel(analysis.stage);
        result->analysisStatusText += L"  ";
        result->analysisStatusText += std::to_wstring(static_cast<int>(
            std::lround(std::clamp(analysis.overallProgress, 0.0f, 1.0f) *
                100.0f))) + L"%";
        break;
    case DeepAnalysisStatus::error:
        result->analysisStatusText = analysis.error.empty()
            ? L"Analysis failed. Retry to try again."
            : analysis.error;
        break;
    case DeepAnalysisStatus::cancelled:
        result->analysisStatusText = L"Analysis cancelled";
        break;
    case DeepAnalysisStatus::idle:
        result->analysisStatusText = L"Analyze to see track metrics";
        break;
    case DeepAnalysisStatus::complete:
        break;
    }

    const auto json = analysis.resultJson
        ? std::string_view(*analysis.resultJson) : std::string_view{};
    result->analyzed = analysis.status == DeepAnalysisStatus::complete &&
        !json.empty();
    result->dossierValues.fill(L"\u2014");
    result->overviewValueText.fill(L"\u2014");
    result->portraitValueText.fill(L"\u2014");
    if (json.empty()) return result;

    for (const auto section : objects(array(json, "sections"))) {
        const auto start = number(section, "start");
        const auto end = number(section, "end");
        if (!start || !end || *end <= *start) continue;
        result->sections.push_back({
            *start,
            *end,
            static_cast<float>(std::clamp(
                number(section, "energyLevel").value_or(0.5), 0.0, 1.0))});
    }
    for (const auto chord : objects(array(json, "chords"))) {
        if (const auto start = number(chord, "start")) {
            result->chordStarts.push_back(*start);
        }
    }

    const auto bpm = number(json, "bpm");
    const auto key = objectString(json, "key", "shortName");
    const auto signature = object(json, "timeSignature");
    const auto numerator = number(signature, "numerator");
    const auto denominator = number(signature, "denominator");
    const auto peakDb = objectNumber(json, "dynamics", "peakDb");
    const auto rmsDb = objectNumber(json, "dynamics", "rmsDb");
    const auto rangeDb = objectNumber(json, "dynamics", "dynamicRangeDb");
    const auto crest = objectNumber(json, "dynamics", "crestFactor");
    const auto warmth = objectNumber(json, "timbre", "warmth");
    const auto brightness = objectNumber(json, "timbre", "brightness");
    const auto tempo = objectNumber(json, "rhythm", "tempoStability");
    const auto godenV2 = object(json, "godenV2");
    const auto correlation = objectNumber(godenV2, "stereo", "correlation");
    const auto midEnergy = objectNumber(godenV2, "stereo", "midEnergy");
    const auto sideEnergy = objectNumber(godenV2, "stereo", "sideEnergy");
    std::optional<double> sideMidDb;
    if (midEnergy && sideEnergy && *midEnergy > 1.0e-12) {
        sideMidDb = 20.0 * std::log10(
            std::max(1.0e-12, *sideEnergy / *midEnergy));
    }
    result->dossierValues[0] = bpm
        ? std::to_wstring(static_cast<int>(std::lround(*bpm))) : L"\u2014";
    result->dossierValues[1] = key.empty() ? L"\u2014" : key;
    result->dossierValues[2] = numerator && denominator
        ? std::to_wstring(static_cast<int>(*numerator)) + L"/" +
            std::to_wstring(static_cast<int>(*denominator))
        : L"\u2014";
    result->dossierValues[3] = dbText(peakDb);
    result->dossierValues[4] = dbText(rmsDb);
    result->dossierValues[5] = dbText(rangeDb);
    result->dossierValues[6] = dbText(crest);
    result->dossierValues[7] = percent(warmth);
    result->dossierValues[8] = percent(brightness);
    result->dossierValues[9] = percent(tempo);
    result->dossierValues[10] = signedText(correlation);
    result->dossierValues[11] = dbText(sideMidDb);

    const std::array<std::optional<double>, 3> overview{
        rmsDb ? std::optional<double>(std::clamp(
            (*rmsDb + 48.0) / 42.0, 0.0, 1.0)) : std::nullopt,
        sideMidDb ? std::optional<double>(std::clamp(
            (*sideMidDb + 30.0) / 36.0, 0.0, 1.0))
            : correlation ? std::optional<double>(std::clamp(
                (1.0 - *correlation) * 0.5, 0.0, 1.0)) : std::nullopt,
        brightness ? std::optional<double>(std::clamp(
            *brightness, 0.0, 1.0)) : std::nullopt,
    };
    for (std::size_t index = 0; index < overview.size(); ++index) {
        result->overviewValid[index] = overview[index].has_value();
        result->overviewValues[index] = static_cast<float>(
            overview[index].value_or(0.0));
    }
    if (rmsDb) result->overviewValueText[0] = L"RMS " + dbText(rmsDb);
    if (sideMidDb) result->overviewValueText[1] = L"S/M " + dbText(sideMidDb);
    else if (correlation) result->overviewValueText[1] =
        L"CORR " + signedText(correlation);
    if (brightness) result->overviewValueText[2] =
        L"BRIGHTNESS " + percent(brightness);

    const auto density = objectNumber(json, "timbre", "density");
    const auto roughness = objectNumber(json, "timbre", "roughness");
    const auto complexity = objectNumber(json, "timbre", "complexity");
    const auto regularity = objectNumber(json, "rhythm", "patternRegularity");
    const auto stability = objectNumber(json, "melody", "pitchStability");
    const std::array<std::optional<double>, 8> portrait{
        rangeDb ? std::optional<double>(std::clamp(*rangeDb / 30.0, 0.0, 1.0)) : std::nullopt,
        brightness, warmth, density, roughness, complexity,
        regularity ? std::optional<double>(1.0 - *regularity) : std::nullopt,
        stability,
    };
    for (std::size_t index = 0; index < portrait.size(); ++index) {
        result->portraitValid[index] = portrait[index].has_value();
        result->portraitValues[index] = static_cast<float>(std::clamp(
            portrait[index].value_or(0.5), 0.0, 1.0));
        if (portrait[index]) {
            wchar_t text[24]{};
            if (index == 0 && rangeDb) {
                std::swprintf(text, std::size(text), L"%.1f dB", *rangeDb);
            } else {
                std::swprintf(text, std::size(text), L"%.2f",
                    result->portraitValues[index]);
            }
            result->portraitValueText[index] = text;
        }
    }
    return result;
}

} // namespace nekorox::component
