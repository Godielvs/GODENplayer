#include "master_dsp_controller.h"

#include "master_dsp_engine.h"

#include "rollout_flags.h"
#include "sdk_compat.h"
#include "text_util.h"
#include "ui_invalidation_bus.h"

#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "util/json.h"

#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace nekorox::component {
namespace {

constexpr GUID kMasterDspEnabledGuid = {0xf2d53c91, 0xbb3e, 0x4377,
    {0xa4, 0xe1, 0x79, 0xac, 0x9c, 0xd1, 0x8b, 0x2d}};
constexpr GUID kMasterDspBypassGuid = {0x7190ee9b, 0x4b8b, 0x45bf,
    {0xae, 0x73, 0xac, 0xd3, 0x43, 0xe8, 0xfb, 0x50}};
constexpr GUID kMasterDspBlobGuid = {0x61497568, 0x3170, 0x49e5,
    {0x9f, 0x0c, 0x4c, 0x14, 0x71, 0xca, 0x5d, 0x82}};
constexpr GUID kMasterDspBlobV2Guid = {0x59fa35b4, 0x1eb6, 0x4fe3,
    {0x88, 0x79, 0x2a, 0x55, 0x12, 0x42, 0x5d, 0xf4}};

cfg_bool g_masterDspEnabled(kMasterDspEnabledGuid, false);
cfg_bool g_masterDspBypass(kMasterDspBypassGuid, false);
cfg_string g_masterDspLegacyBlob(kMasterDspBlobGuid, "");
cfg_var_modern::cfg_blob g_masterDspBlob(kMasterDspBlobV2Guid);

constexpr std::uint32_t kBlobMagic = 0x31524447U; // GDR1
constexpr std::uint32_t kBlobMagicV2 = 0x32524447U; // GDR2
constexpr std::size_t kMaxModules = 32;
constexpr std::size_t kMaxStringBytes = 256U << 10U;
constexpr std::size_t kMaxRackBlobBytes = 8U << 20U;
constexpr std::size_t kMaxPresetBytes = 256U << 20U;
constexpr std::size_t kMaxIrSourceBytes = 256U << 20U;
constexpr std::size_t kMaxPresetManifestBytes = 1U << 20U;
constexpr std::size_t kMaxPresetAssetsBytes = 512U << 20U;
constexpr std::size_t kMaxApoSourceBytes = 4U << 20U;
constexpr std::size_t kMaxApoLineBytes = 8U << 10U;
constexpr std::uint64_t kIrCacheBytes = 2ULL << 30U;
constexpr std::uint32_t kPresetMagicV2 = 0x32504447U; // GDP2

std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept
{
    std::uint32_t result = 0xffffffffU;
    for (const auto byte : bytes) {
        result ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            result = (result >> 1U) ^
                (0xedb88320U & (0U - (result & 1U)));
    }
    return ~result;
}

template <typename T>
void appendPod(std::vector<std::uint8_t>& bytes, const T& value)
{
    const auto* begin = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), begin, begin + sizeof(value));
}

void appendString(std::vector<std::uint8_t>& bytes, const std::string& value)
{
    if (value.size() > kMaxStringBytes)
        throw std::length_error("DSP module configuration exceeds 256 KiB");
    const auto size = static_cast<std::uint32_t>(value.size());
    appendPod(bytes, size);
    bytes.insert(bytes.end(), value.begin(), value.begin() + size);
}

template <typename T>
bool readPod(const std::uint8_t*& cursor, const std::uint8_t* end, T& value)
{
    if (static_cast<std::size_t>(end - cursor) < sizeof(value)) return false;
    std::memcpy(&value, cursor, sizeof(value));
    cursor += sizeof(value);
    return true;
}

bool readString(
    const std::uint8_t*& cursor,
    const std::uint8_t* end,
    std::string& value)
{
    std::uint32_t size = 0;
    if (!readPod(cursor, end, size) || size > kMaxStringBytes ||
        static_cast<std::size_t>(end - cursor) < size) {
        return false;
    }
    value.assign(reinterpret_cast<const char*>(cursor), size);
    cursor += size;
    return true;
}

std::string hexEncode(const std::vector<std::uint8_t>& bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result(bytes.size() * 2U, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        result[index * 2U] = digits[bytes[index] >> 4U];
        result[index * 2U + 1U] = digits[bytes[index] & 15U];
    }
    return result;
}

std::vector<std::uint8_t> hexDecode(std::string_view text)
{
    if ((text.size() & 1U) != 0U) return {};
    auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> result(text.size() / 2U);
    for (std::size_t index = 0; index < result.size(); ++index) {
        const auto high = nibble(text[index * 2U]);
        const auto low = nibble(text[index * 2U + 1U]);
        if (high < 0 || low < 0) return {};
        result[index] = static_cast<std::uint8_t>((high << 4U) | low);
    }
    return result;
}

bool validAssetKey(const std::string& key)
{
    const std::filesystem::path name(key);
    if (name.filename() != name ||
        (key.size() != 68U && key.size() != 69U)) return false;
    for (std::size_t index = 0; index < 64U; ++index) {
        const auto value = static_cast<unsigned char>(key[index]);
        if (!std::isxdigit(value)) return false;
    }
    auto extension = name.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension == ".wav" || extension == ".flac";
}

void appendRack(
    std::vector<std::uint8_t>& bytes,
    const goden_audio::MasterDspRackV1& rack)
{
    appendPod(bytes, rack.schemaVersion);
    if (rack.modules.size() > kMaxModules)
        throw std::length_error("DSP rack exceeds 32 modules");
    const auto count = static_cast<std::uint32_t>(rack.modules.size());
    appendPod(bytes, count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto& module = rack.modules[index];
        appendString(bytes, module.uuid);
        appendString(bytes, module.type);
        appendString(bytes, module.parametersJson);
        appendString(bytes, module.irContentKey);
        appendString(bytes, module.opaqueFuturePayload);
        appendPod(bytes, module.version);
        const std::uint8_t flags =
            (module.bypassed ? 1U : 0U) |
            (module.terminal ? 2U : 0U);
        appendPod(bytes, flags);
    }
}

bool readRack(
    const std::uint8_t*& cursor,
    const std::uint8_t* end,
    goden_audio::MasterDspRackV1& rack)
{
    std::uint32_t count = 0;
    if (!readPod(cursor, end, rack.schemaVersion) ||
        !readPod(cursor, end, count) || count > kMaxModules) {
        return false;
    }
    rack.modules.clear();
    rack.modules.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        goden_audio::MasterDspModuleV1 module;
        std::uint8_t flags = 0;
        if (!readString(cursor, end, module.uuid) ||
            !readString(cursor, end, module.type) ||
            !readString(cursor, end, module.parametersJson) ||
            !readString(cursor, end, module.irContentKey) ||
            !readString(cursor, end, module.opaqueFuturePayload) ||
            !readPod(cursor, end, module.version) ||
            !readPod(cursor, end, flags)) {
            return false;
        }
        module.bypassed = (flags & 1U) != 0;
        module.terminal = (flags & 2U) != 0;
        rack.modules.push_back(std::move(module));
    }
    return true;
}

std::string newUuid()
{
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return "module";
    wchar_t text[40]{};
    StringFromGUID2(guid, text, static_cast<int>(std::size(text)));
    const auto needed = WideCharToMultiByte(
        CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(needed, 0)), '\0');
    if (needed > 1) {
        WideCharToMultiByte(CP_UTF8, 0, text, -1,
            result.data(), needed, nullptr, nullptr);
        result.pop_back();
    }
    return result;
}

void refreshModuleIdentities(goden_audio::MasterDspRackV1& rack)
{
    for (auto& module : rack.modules) module.uuid = newUuid();
}

bool normalizeModuleIdentities(goden_audio::MasterDspRackV1& rack,
    std::unordered_set<std::string>& identities)
{
    bool changed = false;
    for (auto& module : rack.modules) {
        while (module.uuid.empty() || !identities.insert(module.uuid).second) {
            module.uuid = newUuid();
            changed = true;
        }
    }
    return changed;
}

std::string titleFromType(const std::string& type)
{
    const auto separator = type.find_last_of('.');
    auto value = separator == std::string::npos
        ? type : type.substr(separator + 1);
    std::string output;
    output.reserve(value.size() + 8);
    for (char ch : value) {
        if (!output.empty() && ch >= 'A' && ch <= 'Z') output.push_back(' ');
        output.push_back(ch);
    }
    if (!output.empty()) output[0] = static_cast<char>(std::toupper(output[0]));
    return output;
}

std::string categoryFromType(const std::string& type)
{
    const auto separator = type.find('.');
    const auto root = type.substr(0, separator);
    if (root == "eq") return "Correction / EQ";
    if (root == "dynamics") return "Dynamics";
    if (root == "multiband") return "Multiband";
    if (root == "saturation") return "Color";
    if (root == "spectral") return "Spectral";
    if (root == "stereo") return "Stereo / spatial";
    if (root == "maximizer") return "Maximizer / output";
    if (root == "effects") {
        if (type.find("reverb") != std::string::npos ||
            type.find("roomMorph") != std::string::npos) return "Reverb";
        return type.find("delay") != std::string::npos
            ? "Delay / modulation" : "Modulation";
    }
    return "Utility";
}

std::unordered_set<std::string> realtimeParameterKeys(
    const std::string& type, const std::string& parametersJson)
{
    if (type == "utility.trim" || type == "utility.output") return {"gainDb"};
    if (type == "utility.balance") return {"balance"};
    if (type == "utility.crossfeed") return {"amount"};
    if (type.rfind("utility.", 0) == 0 ||
            type == "effects.reverb.convolution") return {};
    try {
        if (type == "autoeq.equalizerApo") {
            // AutoEQ partitions its public 128-band namespace into groups of
            // 24 libsonare bands. Query each real group and translate its
            // local RT-safe keys back to the public band number. The old code
            // queried one unsplit processor, so the controller published
            // commands that AutoEqNode could not route (and bands >=24 often
            // fell back to slow graph compiles).
            constexpr std::size_t groupSize = 24;
            constexpr std::size_t groupCount = (128 + groupSize - 1) / groupSize;
            std::array<sonare::util::json::Object, groupCount> localObjects;
            std::array<std::vector<std::pair<std::string, std::string>>,
                groupCount> keyMaps;
            const auto document = sonare::util::json::parse_strict(
                parametersJson.empty() ? "{}" : parametersJson);
            for (const auto& [globalKey, value] : document.as_object()) {
                if (globalKey == "preampDb") continue;
                if (globalKey.rfind("band", 0) != 0) continue;
                const auto dot = globalKey.find('.', 4);
                if (dot == std::string::npos || dot == 4) continue;
                std::size_t index = 0;
                const auto parsed = std::from_chars(
                    globalKey.data() + 4, globalKey.data() + dot, index);
                if (parsed.ec != std::errc{} ||
                        parsed.ptr != globalKey.data() + dot || index >= 128)
                    continue;
                const auto group = index / groupSize;
                const auto localKey = "band" +
                    std::to_string(index % groupSize) + globalKey.substr(dot);
                localObjects[group][localKey] = value;
                keyMaps[group].emplace_back(globalKey, localKey);
            }

            std::unordered_set<std::string> result{"preampDb"};
            for (std::size_t group = 0; group < groupCount; ++group) {
                if (keyMaps[group].empty()) continue;
                const auto localJson = sonare::util::json::dump(
                    sonare::util::json::Value(localObjects[group]));
                const auto processor = sonare::mastering::api::make_insert(
                    "eq.parametric", localJson);
                if (!processor) continue;
                std::unordered_set<std::string> localSafe;
                for (const auto& descriptor : processor->parameter_descriptors())
                    if (processor->parameter_is_realtime_safe(descriptor.id))
                        localSafe.insert(descriptor.key);
                for (const auto& [globalKey, localKey] : keyMaps[group])
                    if (localSafe.contains(localKey)) result.insert(globalKey);
            }
            return result;
        }

        const auto processor = sonare::mastering::api::make_insert(
            type, parametersJson);
        if (!processor) return {};
        std::unordered_set<std::string> result;
        for (const auto& descriptor : processor->parameter_descriptors()) {
            if (processor->parameter_is_realtime_safe(descriptor.id))
                result.insert(descriptor.key);
        }
        return result;
    } catch (...) {
        return {};
    }
}

std::filesystem::path dspRoot()
{
    const auto native = filesystem::g_get_native_path(core_api::get_profile_path());
    return std::filesystem::path(utf8ToWide(native.c_str())) / L"nekorox" / L"master-dsp";
}

void trimIrCache(const goden_audio::MasterDspStateV1& state)
{
    std::unordered_set<std::wstring> pinned;
    for (const auto* rack : {&state.rackA, &state.rackB})
        for (const auto& module : rack->modules)
            if (!module.irContentKey.empty())
                pinned.insert(std::filesystem::path(module.irContentKey).wstring());
    struct Entry {
        std::filesystem::path path;
        std::uint64_t size = 0;
        std::filesystem::file_time_type modified{};
    };
    std::vector<Entry> removable;
    std::uint64_t total = 0;
    std::error_code error;
    const auto directory = dspRoot() / L"ir";
    for (std::filesystem::directory_iterator iterator(directory, error), end;
            !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_regular_file(error)) continue;
        const auto size = iterator->file_size(error);
        if (error) break;
        total += size;
        if (!pinned.contains(iterator->path().filename().wstring()))
            removable.push_back({iterator->path(), size,
                iterator->last_write_time(error)});
    }
    std::sort(removable.begin(), removable.end(),
        [](const auto& left, const auto& right) {
            return left.modified < right.modified;
        });
    for (const auto& entry : removable) {
        if (total <= kIrCacheBytes) break;
        if (std::filesystem::remove(entry.path, error)) total -= entry.size;
        error.clear();
    }
}

std::string sha256File(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD resultSize = 0;
    std::vector<std::uint8_t> object;
    std::array<std::uint8_t, 32> digest{};
    if (BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
            &resultSize, 0) != 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    object.resize(objectSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize,
            nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::array<char, 64 * 1024> buffer{};
    while (stream) {
        stream.read(buffer.data(), buffer.size());
        const auto count = stream.gcount();
        if (count > 0 && BCryptHashData(hash,
                reinterpret_cast<PUCHAR>(buffer.data()),
                static_cast<ULONG>(count), 0) != 0) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return {};
        }
    }
    const auto status = BCryptFinishHash(
        hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status != 0) return {};
    std::ostringstream text;
    text << std::hex << std::setfill('0');
    for (const auto byte : digest) text << std::setw(2) << unsigned(byte);
    return text.str();
}

bool isTerminalType(const std::string& type) noexcept
{
    return type == "utility.src" || type == "utility.output" ||
        type == "maximizer.truePeakLimiter" || type == "utility.dither";
}

int terminalRank(const std::string& type) noexcept
{
    if (type == "utility.src") return 0;
    if (type == "utility.output") return 1;
    if (type == "maximizer.truePeakLimiter") return 2;
    if (type == "utility.dither") return 3;
    return 4;
}

bool normalizeTerminalStages(goden_audio::MasterDspRackV1& rack)
{
    bool changed = false;
    std::unordered_set<int> terminalClasses;
    for (auto iterator = rack.modules.begin(); iterator != rack.modules.end();) {
        iterator->terminal = iterator->terminal || isTerminalType(iterator->type);
        if (iterator->type == "utility.src" && !iterator->bypassed) {
            iterator->bypassed = true;
            changed = true;
        }
        if (!iterator->terminal) {
            ++iterator;
            continue;
        }
        const auto rank = terminalRank(iterator->type);
        if (rank < 4 && !terminalClasses.insert(rank).second) {
            iterator = rack.modules.erase(iterator);
            changed = true;
            continue;
        }
        ++iterator;
    }
    const auto before = rack.modules;
    std::stable_sort(rack.modules.begin(), rack.modules.end(),
        [](const auto& left, const auto& right) {
            if (left.terminal != right.terminal) return !left.terminal;
            if (!left.terminal) return false;
            return terminalRank(left.type) < terminalRank(right.type);
        });
    if (before.size() == rack.modules.size()) {
        for (std::size_t index = 0; index < before.size(); ++index) {
            if (before[index].uuid != rack.modules[index].uuid) {
                changed = true;
                break;
            }
        }
    }
    return changed;
}

bool consolidateLegacyAutoEqPreamp(
    goden_audio::MasterDspRackV1& rack, const std::string& presetName)
{
    if (presetName != "Imported AutoEQ") return false;
    for (std::size_t index = 1; index < rack.modules.size(); ++index) {
        auto& autoEq = rack.modules[index];
        const auto& trim = rack.modules[index - 1U];
        if (autoEq.type != "autoeq.equalizerApo" ||
                trim.type != "utility.trim") continue;
        try {
            const auto trimJson = sonare::util::json::parse_strict(
                trim.parametersJson.empty() ? "{}" : trim.parametersJson);
            const auto autoEqJson = sonare::util::json::parse_strict(
                autoEq.parametersJson.empty() ? "{}" : autoEq.parametersJson);
            const auto* trimGain = trimJson.find("gainDb");
            const auto* preamp = autoEqJson.find("preampDb");
            if (!trimGain || !preamp || !trimGain->is_number() ||
                    !preamp->is_number() || std::abs(trimGain->as_number() -
                        preamp->as_number()) > 1.0e-6) continue;
            rack.modules.erase(rack.modules.begin() +
                static_cast<std::ptrdiff_t>(index - 1U));
            return true;
        } catch (...) {
        }
    }
    return false;
}

goden_audio::MasterDspTerminalClass terminalClassFor(
    const std::string& type) noexcept
{
    using Class = goden_audio::MasterDspTerminalClass;
    if (type == "utility.src") return Class::sampleRateConversion;
    if (type == "utility.output") return Class::outputTrim;
    if (type == "maximizer.truePeakLimiter") return Class::truePeakLimiter;
    if (type == "utility.dither") return Class::dither;
    return Class::none;
}

std::string parameterLeaf(const std::string& key)
{
    const auto dot = key.find_last_of('.');
    return dot == std::string::npos ? key : key.substr(dot + 1U);
}

goden_audio::MasterDspParameterDescriptorV2 makeParameter(
    const std::string& key, double minimum, double maximum,
    double defaultValue, double step, const char* unit = "",
    goden_audio::MasterDspParameterScale scale =
        goden_audio::MasterDspParameterScale::linear,
    goden_audio::MasterDspParameterKind kind =
        goden_audio::MasterDspParameterKind::continuous,
    goden_audio::MasterDspParameterUpdate update =
        goden_audio::MasterDspParameterUpdate::realtime)
{
    goden_audio::MasterDspParameterDescriptorV2 value;
    value.key = key;
    value.displayName = titleFromType(parameterLeaf(key));
    value.unit = unit;
    value.minimum = minimum;
    value.maximum = maximum;
    value.hardMinimum = minimum;
    value.hardMaximum = maximum;
    value.defaultValue = defaultValue;
    value.step = step;
    value.scale = scale;
    value.kind = kind;
    value.update = update;
    return value;
}

std::optional<goden_audio::MasterDspParameterDescriptorV2>
comfortDescriptorForParameter(const std::string& type, const std::string& key,
    bool realtimeSafe)
{
    using Kind = goden_audio::MasterDspParameterKind;
    using Scale = goden_audio::MasterDspParameterScale;
    using Update = goden_audio::MasterDspParameterUpdate;
    const auto leaf = parameterLeaf(key);
    const auto update = realtimeSafe ? Update::realtime : Update::structural;
    const auto boolean = std::unordered_set<std::string>{
        "enabled", "autoMakeup", "autoThreshold", "link",
        "linkedDetection", "invert", "polarity", "bypassed", "dynamic",
        "soloed", "pingPong", "preserveEnergy", "constantPower",
        "outputLimiter", "usePartitionedConvolution", "delayRight",
        "sidechainHpfEnabled", "applyGainAtInputRate", "enableShelf",
        "proportionalQ", "highPassEnabled", "lowPassEnabled",
        "bandPassEnabled", "notchEnabled", "lowEnabled", "highEnabled"};
    if (boolean.contains(leaf)) {
        auto value = makeParameter(key, 0, 1, 0, 1, "", Scale::linear,
            Kind::boolean, update);
        return value;
    }
    if (leaf == "type" && key.find("band") != std::string::npos) {
        const auto autoEq = type == "autoeq.equalizerApo";
        auto value = makeParameter(key, 0, autoEq ? 2 : 8, 0, 1, "", Scale::linear,
            Kind::enumeration, update);
        value.enumLabels = {"Peak", "Low shelf", "High shelf"};
        if (!autoEq) value.enumLabels.insert(value.enumLabels.end(),
            {"Low pass", "High pass", "Band pass", "Notch", "Tilt", "Flat tilt"});
        return value;
    }
    if (leaf == "coeffMode") {
        auto value = makeParameter(key, 0, 1, 0, 1, "", Scale::linear,
            Kind::enumeration, update);
        value.enumLabels = {"RBJ", "Vicanek"};
        return value;
    }
    if (leaf == "placement") {
        auto value = makeParameter(key, 0, 4, 0, 1, "", Scale::linear,
            Kind::enumeration, update);
        value.enumLabels = {"Stereo", "Left", "Right", "Mid", "Side"};
        return value;
    }
    if (leaf == "channel") {
        auto value = makeParameter(key, 0, 7, 0, 1, "", Scale::linear,
            Kind::enumeration, Update::structural);
        value.enumLabels = {"Left", "Right", "Center", "LFE",
            "Surround left", "Surround right", "Back left", "Back right"};
        return value;
    }
    if (leaf == "phase" && type == "stereo.autoPan")
        return makeParameter(key, 0, 1, 0, 0.001, "cycle", Scale::linear,
            Kind::continuous, update);
    if (leaf == "phase" || leaf == "phaseMode") {
        auto value = makeParameter(key, 0, 3, 0, 1, "", Scale::linear,
            Kind::enumeration, update);
        value.enumLabels = {"Inherit", "Zero latency", "Natural", "Linear"};
        return value;
    }
    if (leaf == "ditherType" || (type == "utility.dither" && leaf == "type")) {
        auto value = makeParameter(key, 0, 3, 2, 1, "", Scale::linear,
            Kind::enumeration, Update::structural);
        value.enumLabels = {"Off", "RPDF", "TPDF", "Noise shaped"};
        return value;
    }
    if (leaf == "fftSize" || leaf == "kernelSize" ||
            leaf == "firKernelSize" || leaf == "partitionSize")
        return makeParameter(key, 64, 32768, 1024, 1, "samples",
            Scale::logarithmic, Kind::integer, Update::structural);
    if (leaf == "oversampleFactor" &&
            (type == "saturation.tube" ||
             type == "maximizer.truePeakLimiter")) {
        const auto truePeak = type == "maximizer.truePeakLimiter";
        auto value = makeParameter(key, 1, truePeak ? 16 : 8, 4, 1, "x",
            Scale::linear, Kind::enumeration, Update::structural);
        value.enumLabels = truePeak
            ? std::vector<std::string>{"1x", "2x", "4x", "8x", "16x"}
            : std::vector<std::string>{"1x", "2x", "4x", "8x"};
        value.enumValues = truePeak
            ? std::vector<double>{1, 2, 4, 8, 16}
            : std::vector<double>{1, 2, 4, 8};
        return value;
    }
    if (leaf == "oversampleFactor" || leaf == "downsampleFactor" ||
            leaf == "stages" || leaf == "ismOrder" || leaf == "order")
        return makeParameter(key, 1, 32, 1, 1, "", Scale::linear,
            Kind::integer, Update::structural);
    if (leaf == "mode" || leaf == "algorithm" || leaf == "detector" ||
            leaf == "slope" || leaf == "highPassSlope" ||
            leaf == "lowPassSlope" || leaf == "ampModel" ||
            leaf == "cabModel" || leaf == "componentModel" ||
            leaf == "curve")
        return makeParameter(key, 0, 8, 0, 1, "", Scale::linear,
            Kind::enumeration, Update::structural);
    if (leaf == "rateHz" || leaf == "modRateHz" || leaf == "rateFastHz" ||
            leaf == "rateSlowHz")
        return makeParameter(key, 0.01, 20, 1, 0.01, "Hz",
            Scale::logarithmic, Kind::continuous, update);
    if (leaf == "sidechainHpfHz")
        return makeParameter(key, 20, 5000, 100, 1, "Hz",
            Scale::logarithmic, Kind::continuous, update);
    if (leaf == "keyHpfHz")
        return makeParameter(key, 0, 5000, 0, 1, "Hz",
            Scale::logarithmic, Kind::continuous, update);
    if (leaf.ends_with("FrequencyHz") || leaf.ends_with("FreqHz") ||
            leaf == "frequencyHz" || leaf == "cutoffHz" ||
            leaf == "pivotHz" || leaf == "carrierHz" ||
            leaf == "minHz" || leaf == "maxHz" || leaf == "toneHz" ||
            leaf == "densityHz")
        return makeParameter(key, 20, 20000, 1000, 1, "Hz",
            Scale::logarithmic, Kind::continuous, update);
    if (leaf == "q" || leaf.ends_with("Q") || leaf == "resonance")
        return makeParameter(key, 0.1, 20, 0.707, 0.01, "",
            Scale::logarithmic, Kind::continuous, update);
    if (leaf.find("threshold") != std::string::npos ||
            leaf.find("Threshold") != std::string::npos)
        return makeParameter(key, -120, 6, -18, 0.1, "dB",
            Scale::decibels, Kind::continuous, update);
    if (leaf.find("ceiling") != std::string::npos ||
            leaf.find("Ceiling") != std::string::npos)
        return makeParameter(key, -24, 0, -1, 0.05, "dBTP",
            Scale::decibels, Kind::continuous, update);
    if (leaf.ends_with("GainDb") || leaf.ends_with("gainDb") ||
            leaf == "preampDb" || leaf == "driveDb" ||
            leaf.ends_with("BoostDb") || leaf.ends_with("CutDb") ||
            leaf.ends_with("RangeDb") || leaf == "rangeDb" ||
            leaf == "targetDb" || leaf == "noiseFloorDb" ||
            leaf == "tiltDb" || leaf == "headBumpDb")
        return makeParameter(key, -60, 30, 0, 0.1, "dB",
            Scale::decibels, Kind::continuous, update);
    if (leaf.find("attack") != std::string::npos ||
            leaf.find("Attack") != std::string::npos)
        return makeParameter(key, 0.01, 2000, 10, 0.1, "ms",
            Scale::logarithmic, Kind::continuous, update);
    if (leaf.find("release") != std::string::npos ||
            leaf.find("Release") != std::string::npos)
        return makeParameter(key, 0.1, 10000, 100, 0.1, "ms",
            Scale::logarithmic, Kind::continuous, update);
    if (leaf.find("hold") != std::string::npos ||
            leaf.find("Hold") != std::string::npos)
        return makeParameter(key, 0, 5000, 0, 0.1, "ms",
            Scale::linear, Kind::continuous, update);
    if (leaf == "pdrTimeMs")
        return makeParameter(key, 0, 500, 0, 0.1, "ms",
            Scale::linear, Kind::continuous, update);
    if (leaf == "pdrReleaseScale")
        return makeParameter(key, 1, 4, 1, 0.01, "x",
            Scale::logarithmic, Kind::continuous, update);
    if (leaf == "depthMs")
        return makeParameter(key, 0,
            type == "effects.modulation.flanger" ? 100 : 50,
            type == "effects.modulation.chorus" ? 6 : 2,
            0.01, "ms", Scale::linear, Kind::continuous, update);
    if (leaf == "centerDelayMs" &&
            (type == "effects.modulation.chorus" ||
             type == "effects.modulation.flanger"))
        return makeParameter(key, 0,
            type == "effects.modulation.flanger" ? 100 : 50,
            type == "effects.modulation.chorus" ? 14 : 3,
            0.01, "ms", Scale::linear, Kind::continuous, update);
    if ((leaf == "delayTimeLMs" || leaf == "delayTimeRMs") &&
            type == "effects.delay.stereo")
        return makeParameter(key, 0, 4000, 250, 0.1, "ms", Scale::linear,
            Kind::continuous, update);
    if (leaf == "delaySamples")
        return makeParameter(key, 0, 8192, 0, 1, "samples", Scale::linear,
            Kind::integer, Update::structural);
    if (leaf == "fractionalDelaySamples")
        return makeParameter(key, 0, 0.999, 0, 0.001, "samples", Scale::linear,
            Kind::continuous, update);
    if (leaf.find("delay") != std::string::npos ||
            leaf.find("Delay") != std::string::npos ||
            leaf.find("lookahead") != std::string::npos ||
            leaf.find("Lookahead") != std::string::npos ||
            leaf == "preDelayMs" || leaf == "timeMs")
        return makeParameter(key, 0, 2000, 0, 0.1,
            leaf.ends_with("Us") ? "us" : "ms", Scale::linear,
            Kind::continuous, update);
    if (leaf.find("ratio") != std::string::npos ||
            leaf.find("Ratio") != std::string::npos)
        return makeParameter(key, 1, 100, 2, 0.05, ":1",
            Scale::logarithmic, Kind::continuous, update);
    if (leaf.find("knee") != std::string::npos ||
            leaf.find("Knee") != std::string::npos)
        return makeParameter(key, 0, 24, 6, 0.1, "dB",
            Scale::linear, Kind::continuous, update);
    if (leaf == "bits" &&
            (type == "utility.bitDepth" || type == "utility.dither"))
        return makeParameter(key, 2, 32, 24, 1, "bit", Scale::linear,
            Kind::integer, Update::structural);
    if (leaf == "bitDepth" && type == "saturation.bitcrusher")
        return makeParameter(key, 1, 24, 12, 1, "bit", Scale::linear,
            Kind::integer, Update::structural);
    if (leaf == "bitDepth" || leaf == "bits")
        return makeParameter(key, 8, 32, 24, 1, "bit", Scale::linear,
            Kind::integer, Update::structural);
    if (leaf == "modDepthSamples")
        return makeParameter(key, 0, 64, 6, 0.1, "samples", Scale::linear,
            Kind::continuous, update);
    if (leaf == "semitones")
        return makeParameter(key, -24, 24, 0, 0.01, "st", Scale::linear,
            Kind::continuous, update);
    if (leaf.find("width") != std::string::npos ||
            leaf.find("Width") != std::string::npos || leaf == "stereoSpread")
        return makeParameter(key, 0, 2, 1, 0.01, "", Scale::linear,
            Kind::continuous, update);
    if (leaf == "balance" || leaf == "outputPan")
        return makeParameter(key, -1, 1, 0, 0.01, "", Scale::linear,
            Kind::continuous, update);
    if (leaf == "dryWet" || leaf == "mix" || leaf == "amount" ||
            leaf == "depth" || leaf.ends_with("Amount") ||
            leaf == "sensitivity" || leaf == "tremolo" ||
            leaf == "damping" || leaf == "absorption" ||
            leaf == "saturation" || leaf == "bias" || leaf == "power" ||
            leaf == "sag" || leaf == "nfb" || leaf == "gapLoss" ||
            leaf == "hysteresis" || leaf == "evenOddMix" ||
            leaf == "transientTightness" || leaf == "decorrelationAmount" ||
            leaf == "preserveEnergy")
        return makeParameter(key, 0, 1, 0, 0.005, "", Scale::linear,
            Kind::continuous, update);
    if (leaf == "feedback")
        return makeParameter(key,
            type == "effects.modulation.flanger" ? -0.95 : 0.0,
            0.95, type == "effects.modulation.flanger" ? 0.3 : 0.25,
            0.005, "", Scale::linear, Kind::continuous, update);
    if (leaf == "hfDamping")
        return makeParameter(key, 0, 1, 0.5, 0.005, "", Scale::linear,
            Kind::continuous, update);
    if (leaf == "speedIps")
        return makeParameter(key, 1.875, 60, 15, 0.125, "ips",
            Scale::logarithmic, Kind::continuous, update);
    if (leaf == "ceiling")
        return makeParameter(key, 0.01, 1, 1, 0.001, "FS",
            Scale::linear, Kind::continuous, update);
    if (leaf == "biasV")
        return makeParameter(key, -5, 5, -1.6, 0.01, "V",
            Scale::linear, Kind::continuous, update);
    if (leaf == "harmonicDrive")
        return makeParameter(key, 0, 1, 1, 0.01, "", Scale::linear,
            Kind::continuous, update);
    if (leaf == "drive")
        return makeParameter(key, 0.01, 20, 2, 0.01, "x",
            Scale::logarithmic, Kind::continuous, update);
    if (leaf == "seed")
        return makeParameter(key, 0, 4294967295.0, 0, 1, "", Scale::linear,
            Kind::integer, Update::structural);
    if (leaf == "decay" && type == "effects.reverb.plate")
        return makeParameter(key, 0, 0.98, 0.5, 0.005, "", Scale::linear,
            Kind::continuous, update);
    if (leaf == "decay" && type == "effects.reverb.fdn")
        return makeParameter(key, 0.001, 1.5, 0.55, 0.005, "", Scale::linear,
            Kind::continuous, update);
    if (leaf == "decay" || leaf == "decaySec" || leaf == "reverbTimeS" ||
            leaf == "maxSeconds")
        return makeParameter(key, 0.05, 60, 1.5, 0.01, "s",
            Scale::logarithmic, Kind::continuous, update);
    if (leaf == "resolution" || leaf == "gainScale" ||
            leaf == "proportionalQStrength" || leaf == "slopeDbOct")
        return makeParameter(key, 0, 24, 1, 0.01, "", Scale::linear,
            Kind::continuous, update);
    return std::nullopt;
}

void expandHardRange(const std::string& type,
    goden_audio::MasterDspParameterDescriptorV2& value)
{
    using Kind = goden_audio::MasterDspParameterKind;
    if (value.kind == Kind::boolean || value.kind == Kind::enumeration ||
            value.kind == Kind::asset) return;

    const auto leaf = parameterLeaf(value.key);
    auto set = [&](double minimum, double maximum) {
        // A hard range must always contain the complete mouse/slider range.
        value.hardMinimum = std::min(minimum, value.minimum);
        value.hardMaximum = std::max(maximum, value.maximum);
    };

    // Utility processors are implemented by Goden rather than libsonare, so
    // their hard limits mirror the audited bounds in UtilityNode exactly.
    if ((type == "utility.trim" || type == "utility.output") &&
            leaf == "gainDb") {
        set(-120.0, 48.0);
        return;
    }
    if ((type == "utility.channelDelay" || type == "utility.crossfeed") &&
            leaf == "delayUs") {
        // 10 ms remains below UtilityNode's 4096-frame allocation at 384 kHz.
        set(0.0, 10000.0);
        return;
    }
    if (type.rfind("utility.", 0) == 0) return;

    // AutoEQ has a Goden wrapper around groups of 24 libsonare EQ bands.
    // Keep its preamp finite and deliberately less permissive on boost than
    // attenuation; the bands themselves use the generic EQ limits below.
    if (type == "autoeq.equalizerApo" && leaf == "preampDb") {
        set(-120.0, 48.0);
        return;
    }

    if (leaf.find("threshold") != std::string::npos ||
            leaf.find("Threshold") != std::string::npos) {
        set(-144.0, 24.0);
    } else if (leaf.find("ceiling") != std::string::npos ||
            leaf.find("Ceiling") != std::string::npos) {
        set(-60.0, 0.0);
    } else if (leaf.ends_with("GainDb") || leaf.ends_with("gainDb") ||
            leaf == "preampDb" || leaf == "driveDb" ||
            leaf.ends_with("BoostDb") || leaf.ends_with("CutDb") ||
            leaf.ends_with("RangeDb") || leaf == "rangeDb" ||
            leaf == "targetDb" || leaf == "noiseFloorDb" ||
            leaf == "tiltDb" || leaf == "headBumpDb") {
        set(-120.0, 48.0);
    } else if (leaf.find("attack") != std::string::npos ||
            leaf.find("Attack") != std::string::npos) {
        set(0.0, 60000.0);
    } else if (leaf.find("release") != std::string::npos ||
            leaf.find("Release") != std::string::npos) {
        set(0.0, 120000.0);
    } else if (leaf.find("hold") != std::string::npos ||
            leaf.find("Hold") != std::string::npos) {
        set(0.0, 120000.0);
    } else if (leaf == "pdrTimeMs") {
        set(0.0, 5000.0);
    } else if (leaf == "pdrReleaseScale") {
        set(1.0, 32.0);
    } else if (leaf == "rateHz" || leaf == "modRateHz" ||
            leaf == "rateFastHz" || leaf == "rateSlowHz") {
        set(0.0, 100.0);
    } else if (leaf.ends_with("FrequencyHz") || leaf.ends_with("FreqHz") ||
            leaf == "frequencyHz" || leaf == "cutoffHz" ||
            leaf == "pivotHz" || leaf == "carrierHz" || leaf == "minHz" ||
            leaf == "maxHz" || leaf == "toneHz" || leaf == "densityHz") {
        set(1.0, 22000.0);
    } else if (leaf == "q" || leaf.ends_with("Q") ||
            leaf == "resonance") {
        set(0.01, 100.0);
    } else if (leaf.find("ratio") != std::string::npos ||
            leaf.find("Ratio") != std::string::npos) {
        set(1.0, 1000.0);
    } else if (leaf.find("knee") != std::string::npos ||
            leaf.find("Knee") != std::string::npos) {
        set(0.0, 60.0);
    } else if ((leaf == "delayTimeLMs" || leaf == "delayTimeRMs") &&
            type == "effects.delay.stereo") {
        set(0.0, 10000.0);
    } else if (leaf == "delaySamples") {
        set(0.0, 32768.0);
    } else if (leaf == "semitones") {
        set(-96.0, 96.0);
    } else if (leaf == "feedback") {
        set(-0.999, 0.999);
    } else if (leaf == "speedIps") {
        set(0.1, 240.0);
    } else if (leaf == "ceiling") {
        set(0.001, 4.0);
    } else if (leaf == "biasV") {
        set(-20.0, 20.0);
    } else if (leaf == "drive") {
        set(0.001, 100.0);
    }
}

std::optional<goden_audio::MasterDspParameterDescriptorV2>
descriptorForParameter(const std::string& type, const std::string& key,
    bool realtimeSafe)
{
    auto result = comfortDescriptorForParameter(type, key, realtimeSafe);
    if (result) {
        expandHardRange(type, *result);
        if (type == "autoeq.equalizerApo" &&
                parameterLeaf(key) == "preampDb") {
            result->displayName = "Manual preamp";
        }
    }
    return result;
}

bool coreQualifiedType(const std::string& type)
{
    static const std::unordered_set<std::string> qualified{
        "eq.tilt", "eq.parametric", "eq.cutFilter", "eq.bandPass",
        "eq.shelving", "eq.graphic", "dynamics.compressor",
        "dynamics.limiter", "dynamics.brickwallLimiter", "dynamics.deesser",
        "dynamics.gate", "saturation.softClipper", "saturation.tape",
        "saturation.tube", "saturation.exciter", "spectral.airBand",
        "spectral.lowEndFocus", "spectral.presenceEnhancer",
        "stereo.autoPan", "stereo.imager", "stereo.monoMaker",
        "stereo.phaseAlign", "stereo.stereoBalance", "maximizer.maximizer",
        "maximizer.truePeakLimiter", "effects.reverb.convolution",
        "effects.reverb.fdn", "effects.reverb.plate",
        "effects.modulation.chorus", "effects.modulation.flanger",
        "effects.delay.stereo"};
    return qualified.contains(type);
}

} // namespace

MasterDspController& MasterDspController::instance()
{
    static MasterDspController controller;
    return controller;
}

void MasterDspController::start()
{
    std::lock_guard lock(mutex_);
    if (started_) return;
    loadLocked();
    started_ = true;
}

void MasterDspController::stop()
{
    std::lock_guard lock(mutex_);
    if (!started_) return;
    persistLocked();
    started_ = false;
}

goden_audio::MasterDspStateV1 MasterDspController::snapshot() const
{
    std::lock_guard lock(mutex_);
    auto copy = state_;
    copy.revision = revision_.load(std::memory_order_acquire);
    return copy;
}

goden_audio::MasterDspTelemetryV1 MasterDspController::telemetrySnapshot() const
{
    std::lock_guard lock(mutex_);
    return state_.telemetry;
}

goden_audio::MasterDspStateV2 MasterDspController::snapshotV2() const
{
    std::lock_guard lock(mutex_);
    goden_audio::MasterDspStateV2 copy;
    copy.compatible = state_;
    copy.compatible.revision = revision_.load(std::memory_order_acquire);
    copy.faults = faults_;
    try {
        std::vector<std::uint8_t> payload;
        appendString(payload, state_.presetName);
        appendRack(payload, state_.rackA);
        appendRack(payload, state_.rackB);
        copy.crc32 = crc32(payload);
    } catch (...) {
        copy.crc32 = 0;
    }
    return copy;
}

std::uint64_t MasterDspController::revision() const noexcept
{
    return revision_.load(std::memory_order_acquire);
}

bool MasterDspController::enabledRt() const noexcept
{
    return enabled_.load(std::memory_order_acquire);
}

bool MasterDspController::bypassedRt() const noexcept
{
    return bypassed_.load(std::memory_order_acquire);
}

std::size_t MasterDspController::activeModuleCountRt() const noexcept
{
    return activeModuleCount_.load(std::memory_order_acquire);
}

std::size_t MasterDspController::moduleCountRt() const noexcept
{
    return moduleCount_.load(std::memory_order_acquire);
}

void MasterDspController::setTelemetryDemand(bool demanded) noexcept
{
    const auto previous = telemetryDemand_.exchange(
        demanded, std::memory_order_acq_rel);
    if (previous != demanded) goden_audio::notifyMasterDspCompilerWorkers();
}

bool MasterDspController::telemetryDemandRt() const noexcept
{
    return telemetryDemand_.load(std::memory_order_acquire);
}

goden_audio::MasterDspRackV1& MasterDspController::selectedRackLocked() noexcept
{
    return state_.selectedB ? state_.rackB : state_.rackA;
}

const goden_audio::MasterDspRackV1&
MasterDspController::selectedRackLocked() const noexcept
{
    return state_.selectedB ? state_.rackB : state_.rackA;
}

void MasterDspController::setEnabled(bool enabled)
{
    {
        std::lock_guard lock(mutex_);
        state_.enabled = enabled;
        g_masterDspEnabled = enabled;
        changedLocked(false, false, false);
    }
    publishUi();
}

void MasterDspController::setBypassed(bool bypassed)
{
    {
        std::lock_guard lock(mutex_);
        state_.bypassed = bypassed;
        g_masterDspBypass = bypassed;
        changedLocked(false, false, false);
    }
    publishUi();
}

void MasterDspController::toggleEnabled() { setEnabled(!enabledRt()); }
void MasterDspController::toggleBypass() { setBypassed(!bypassedRt()); }

void MasterDspController::selectRackB(bool selectedB)
{
    {
        std::lock_guard lock(mutex_);
        if (state_.selectedB == selectedB) return;
        if (selectedB && !state_.rackBInitialized) {
            state_.rackB = state_.rackA;
            refreshModuleIdentities(state_.rackB);
            state_.rackBInitialized = true;
        }
        state_.selectedB = selectedB;
        changedLocked();
    }
    publishUi();
}

void MasterDspController::copyAToB()
{
    {
        std::lock_guard lock(mutex_);
        state_.rackB = state_.rackA;
        refreshModuleIdentities(state_.rackB);
        state_.rackBInitialized = true;
        changedLocked();
    }
    publishUi();
}

void MasterDspController::copyBToA()
{
    {
        std::lock_guard lock(mutex_);
        if (!state_.rackBInitialized) return;
        state_.rackA = state_.rackB;
        refreshModuleIdentities(state_.rackA);
        changedLocked();
    }
    publishUi();
}

bool MasterDspController::undo()
{
    {
        std::lock_guard lock(mutex_);
        if (undo_.empty()) return false;
        redo_.push_back(committedState_);
        const auto enabled = state_.enabled;
        const auto bypassed = state_.bypassed;
        state_ = std::move(undo_.back());
        undo_.pop_back();
        state_.enabled = enabled;
        state_.bypassed = bypassed;
        changedLocked(true, false);
    }
    publishUi();
    return true;
}

bool MasterDspController::redo()
{
    {
        std::lock_guard lock(mutex_);
        if (redo_.empty()) return false;
        undo_.push_back(committedState_);
        const auto enabled = state_.enabled;
        const auto bypassed = state_.bypassed;
        state_ = std::move(redo_.back());
        redo_.pop_back();
        state_.enabled = enabled;
        state_.bypassed = bypassed;
        changedLocked(true, false);
    }
    publishUi();
    return true;
}

bool MasterDspController::canUndo() const
{
    std::lock_guard lock(mutex_);
    return !undo_.empty();
}

bool MasterDspController::canRedo() const
{
    std::lock_guard lock(mutex_);
    return !redo_.empty();
}

bool MasterDspController::addModule(const std::string& type, bool terminal)
{
    const auto available = catalog();
    const auto offered = std::find_if(available.begin(), available.end(),
        [&](const auto& entry) { return entry.type == type; });
    if (offered == available.end()) return false;
    {
        std::lock_guard lock(mutex_);
        auto& rack = selectedRackLocked();
        if (rack.modules.size() >= kMaxModules || type.empty()) return false;
        goden_audio::MasterDspModuleV1 module;
        module.uuid = newUuid();
        module.type = type;
        module.terminal = terminal || isTerminalType(type);
        if (module.terminal) {
            const auto terminalClass = terminalClassFor(type);
            if (std::any_of(rack.modules.begin(), rack.modules.end(),
                    [&](const auto& value) {
                        return terminalClassFor(value.type) == terminalClass;
                    })) return false;
            rack.modules.push_back(std::move(module));
            (void)normalizeTerminalStages(rack);
        } else {
            const auto terminalBegin = std::find_if(rack.modules.begin(),
                rack.modules.end(), [](const auto& value) { return value.terminal; });
            rack.modules.insert(terminalBegin, std::move(module));
        }
        changedLocked();
    }
    publishUi();
    return true;
}

bool MasterDspController::removeModule(const std::string& uuid)
{
    {
        std::lock_guard lock(mutex_);
        auto& modules = selectedRackLocked().modules;
        const auto found = std::find_if(modules.begin(), modules.end(),
            [&](const auto& value) { return value.uuid == uuid; });
        if (found == modules.end()) return false;
        modules.erase(found);
        changedLocked();
    }
    publishUi();
    return true;
}

bool MasterDspController::moveModule(const std::string& uuid, int delta)
{
    if (delta == 0) return true;
    {
        std::lock_guard lock(mutex_);
        auto& modules = selectedRackLocked().modules;
        const auto found = std::find_if(modules.begin(), modules.end(),
            [&](const auto& value) { return value.uuid == uuid; });
        if (found == modules.end() || found->terminal) return false;
        const auto index = static_cast<std::ptrdiff_t>(found - modules.begin());
        const auto target = index + static_cast<std::ptrdiff_t>(delta);
        if (target < 0 || target >= static_cast<std::ptrdiff_t>(modules.size()) ||
            modules[static_cast<std::size_t>(target)].terminal) return false;
        if (target < index) {
            std::rotate(modules.begin() + target, modules.begin() + index,
                modules.begin() + index + 1);
        } else {
            std::rotate(modules.begin() + index, modules.begin() + index + 1,
                modules.begin() + target + 1);
        }
        changedLocked();
    }
    publishUi();
    return true;
}

bool MasterDspController::setModuleBypassed(
    const std::string& uuid, bool bypassed)
{
    {
        std::lock_guard lock(mutex_);
        auto& modules = selectedRackLocked().modules;
        const auto found = std::find_if(modules.begin(), modules.end(),
            [&](const auto& value) { return value.uuid == uuid; });
        if (found == modules.end()) return false;
        if (found->bypassed == bypassed) return true;
        found->bypassed = bypassed;
        changedLocked(true, true, false);
    }
    goden_audio::publishMasterDspBypassCommand(uuid, bypassed);
    publishUi();
    return true;
}

bool MasterDspController::setModuleParameters(
    const std::string& uuid, std::string json)
{
    json.erase(json.begin(), std::find_if(json.begin(), json.end(),
        [](unsigned char ch) { return !std::isspace(ch); }));
    while (!json.empty() && std::isspace(
        static_cast<unsigned char>(json.back()))) json.pop_back();
    if (json.size() > kMaxStringBytes || json.empty() ||
        json.front() != '{' || json.back() != '}') return false;
    sonare::util::json::Value parsed;
    try {
        parsed = sonare::util::json::parse_strict(json);
        if (!parsed.is_object()) return false;
    } catch (...) {
        return false;
    }
    std::vector<std::pair<std::string, float>> commands;
    bool realtimeOnly = true;
    bool anyChange = false;
    {
        std::lock_guard lock(mutex_);
        auto& modules = selectedRackLocked().modules;
        const auto found = std::find_if(modules.begin(), modules.end(),
            [&](const auto& value) { return value.uuid == uuid; });
        if (found == modules.end()) return false;
        for (const auto& [key, value] : parsed.as_object()) {
            if (!value.is_number() && !value.is_bool()) return false;
            const auto descriptor = parameterDescriptor(found->type, key);
            if (!descriptor) return false;
            const auto numeric = value.is_number() ? value.as_number()
                : (value.as_bool() ? 1.0 : 0.0);
            if (!descriptor->accepts(numeric)) return false;
            if (found->type == "autoeq.equalizerApo" && key.rfind("band", 0) == 0) {
                const auto dot = key.find('.', 4);
                if (dot == std::string::npos) return false;
                std::size_t index = 0;
                const auto converted = std::from_chars(
                    key.data() + 4, key.data() + dot, index);
                if (converted.ec != std::errc{} || converted.ptr != key.data() + dot ||
                        index >= 128) return false;
            }
        }
        const auto safeKeys = realtimeParameterKeys(
            found->type, found->parametersJson);
        try {
            const auto previous = sonare::util::json::parse_strict(
                found->parametersJson.empty() ? "{}" : found->parametersJson);
            std::unordered_set<std::string> keys;
            for (const auto& [key, value] : previous.as_object()) keys.insert(key);
            for (const auto& [key, value] : parsed.as_object()) keys.insert(key);
            for (const auto& key : keys) {
                const auto* before = previous.find(key);
                const auto* after = parsed.find(key);
                if (before && after && sonare::util::json::dump(*before) ==
                        sonare::util::json::dump(*after)) continue;
                anyChange = true;
                const auto descriptor = after
                    ? parameterDescriptor(found->type, key) : std::nullopt;
                const auto afterNumeric = after && after->is_number()
                    ? after->as_number()
                    : (after && after->is_bool()
                        ? (after->as_bool() ? 1.0 : 0.0) : 0.0);
                // Extreme manual values are never pushed into an existing
                // realtime object. They take the graph-shadow path so the
                // processor constructor can validate the complete state and
                // the old graph remains audible if it rejects the edit.
                const auto outsideComfort = descriptor && after &&
                    !descriptor->isInsideComfortRange(afterNumeric);
                if (!before || !after || !descriptor || outsideComfort ||
                        safeKeys.find(key) == safeKeys.end() ||
                        (!after->is_number() && !after->is_bool())) {
                    realtimeOnly = false;
                    break;
                }
                commands.emplace_back(key, after->is_number()
                    ? after->as_float() : (after->as_bool() ? 1.0f : 0.0f));
            }
        } catch (...) {
            realtimeOnly = false;
            anyChange = true;
        }
        if (!anyChange) return true;
        if (!realtimeOnly && found->type.rfind("utility.", 0) != 0 &&
                found->type != "autoeq.equalizerApo" &&
                found->type != "effects.reverb.convolution") {
            try {
                std::vector<std::string> unknown;
                const auto candidate = sonare::mastering::api::make_insert(
                    found->type, json, &unknown);
                if (!candidate || !unknown.empty()) return false;
            } catch (...) {
                return false;
            }
        }
        found->parametersJson = std::move(json);
        const auto persist = !parameterGestureActive_;
        changedLocked(persist, !parameterGestureActive_, !realtimeOnly);
        if (parameterGestureActive_) parameterGestureChanged_ = true;
    }
    if (realtimeOnly) for (const auto& [key, value] : commands)
        goden_audio::publishMasterDspParameterCommand(uuid, key, value);
    publishUi();
    return true;
}

void MasterDspController::beginParameterGesture()
{
    std::lock_guard lock(mutex_);
    if (parameterGestureActive_) return;
    parameterGestureActive_ = true;
    parameterGestureChanged_ = false;
    parameterGestureInitialState_ = state_;
}

void MasterDspController::endParameterGesture()
{
    bool changed = false;
    {
        std::lock_guard lock(mutex_);
        if (!parameterGestureActive_) return;
        changed = parameterGestureChanged_;
        parameterGestureActive_ = false;
        parameterGestureChanged_ = false;
        if (!changed) return;
        if (historyReady_) {
            undo_.push_back(std::move(parameterGestureInitialState_));
            if (undo_.size() > 64) undo_.erase(undo_.begin());
            redo_.clear();
        }
        committedState_ = state_;
        persistLocked();
    }
    // The Undo button becomes available only when the gesture commits.
    publishUi();
}

void MasterDspController::clearRack()
{
    {
        std::lock_guard lock(mutex_);
        selectedRackLocked().modules.clear();
        state_.presetName = "Null";
        changedLocked();
    }
    publishUi();
}

std::vector<goden_audio::MasterDspCatalogEntryV1>
MasterDspController::catalog() const
{
    std::vector<goden_audio::MasterDspCatalogEntryV1> result;
    const auto qualifiedCatalog = rolloutFlagEnabled(
        RolloutFlag::masterDspCatalogQualifiedV2);
    const auto fullCatalog = rolloutFlagEnabled(
        RolloutFlag::masterDspFullCatalogV1);
    for (const auto& descriptor : catalogV2()) {
        if (descriptor.qualification == goden_audio::MasterDspQualification::hidden)
            continue;
        const auto foundation = descriptor.category == "Utility" ||
            descriptor.type == "autoeq.equalizerApo";
        // Both switches are real kill switches. Qualification-off keeps the
        // small engine-native foundation; full-catalog-off provides the same
        // emergency fallback without ever exposing hidden processors.
        if ((!qualifiedCatalog || !fullCatalog) && !foundation) continue;
        goden_audio::MasterDspCatalogEntryV1 entry;
        entry.type = descriptor.type;
        entry.displayName = descriptor.displayName;
        entry.category = descriptor.category;
        entry.terminal = descriptor.terminalClass !=
            goden_audio::MasterDspTerminalClass::none;
        entry.requiresAsset = descriptor.requiresAsset;
        for (const auto& parameter : descriptor.parameters) {
            if (parameter.visible) entry.parameterNames.push_back(parameter.key);
        }
        result.push_back(std::move(entry));
    }
    return result;
}

const std::vector<goden_audio::MasterDspProcessorDescriptorV2>&
MasterDspController::catalogV2() const
{
    using Descriptor = goden_audio::MasterDspProcessorDescriptorV2;
    using Qualification = goden_audio::MasterDspQualification;
    static std::once_flag once;
    static std::vector<Descriptor> cached;
    std::call_once(once, [&] {
    std::vector<Descriptor> result;
    auto addUtility = [&](const char* type, std::initializer_list<const char*> keys) {
        Descriptor descriptor;
        descriptor.type = type;
        descriptor.displayName = type == std::string_view("utility.bitDepth")
            ? "Undithered Quantizer" : titleFromType(type);
        descriptor.category = "Utility";
        descriptor.terminalClass = terminalClassFor(type);
        descriptor.qualification = type == std::string_view("utility.src")
            ? Qualification::hidden : Qualification::core;
        for (const auto* key : keys) {
            if (auto parameter = descriptorForParameter(type, key, true))
                descriptor.parameters.push_back(std::move(*parameter));
        }
        result.push_back(std::move(descriptor));
    };
    addUtility("utility.trim", {"gainDb"});
    addUtility("utility.output", {"gainDb"});
    addUtility("utility.balance", {"balance"});
    addUtility("utility.polarity", {});
    addUtility("utility.swap", {});
    addUtility("utility.mono", {});
    addUtility("utility.channelDelay", {"channel", "delayUs"});
    addUtility("utility.dcBlocker", {});
    addUtility("utility.crossfeed", {"amount", "delayUs", "cutoffHz"});
    addUtility("utility.monoBass", {"frequencyHz"});
    addUtility("utility.bitDepth", {"bits"});
    addUtility("utility.dither", {"bits", "type"});

    Descriptor autoEq;
    autoEq.type = "autoeq.equalizerApo";
    autoEq.displayName = "AutoEQ / Equalizer APO";
    autoEq.category = "Audiophile";
    autoEq.qualification = Qualification::core;
    for (const auto* key : {"preampDb", "bandN.type", "bandN.frequencyHz",
             "bandN.gainDb", "bandN.q", "bandN.bypassed"}) {
        if (auto parameter = descriptorForParameter(autoEq.type, key, true))
            autoEq.parameters.push_back(std::move(*parameter));
    }
    result.push_back(std::move(autoEq));

    for (const auto& type : sonare::mastering::api::insert_factory_names()) {
        if (type == "dynamics.sidechainRouter" || type == "utility.src") continue;
        Descriptor descriptor;
        descriptor.type = type;
        descriptor.displayName = titleFromType(type);
        descriptor.category = categoryFromType(type);
        descriptor.terminalClass = terminalClassFor(type);
        descriptor.requiresAsset = type == "effects.reverb.convolution";
        if (sonare::mastering::api::channel_policy(type) ==
                sonare::mastering::api::ChannelPolicy::StereoPairOnly)
            descriptor.multichannelPolicy =
                goden_audio::MasterDspMultichannelPolicy::frontStereoDryRemainder;

        std::unordered_map<std::string, bool> realtime;
        std::unordered_map<std::string, double> defaults;
        try {
            const auto info = sonare::util::json::parse_strict(
                sonare::mastering::api::insert_param_info_json(type));
            if (info.is_array()) for (const auto& item : info.as_array()) {
                const auto* name = item.find("name");
                const auto* safe = item.find("rtSafe");
                if (name && name->is_string()) realtime[name->as_string()] =
                    safe && safe->is_bool() && safe->as_bool();
            }
            const auto defaultValues = sonare::util::json::parse_strict(
                sonare::mastering::api::insert_param_defaults_json(type));
            if (defaultValues.is_object()) {
                for (const auto& [key, value] : defaultValues.as_object()) {
                    if (value.is_number() && std::isfinite(value.as_number()))
                        defaults[key] = value.as_number();
                }
            }
        } catch (...) {
        }
        bool complete = true;
        for (const auto& key : sonare::mastering::api::insert_param_names(type)) {
            const auto safe = realtime.find(key);
            auto parameter = descriptorForParameter(type, key,
                safe != realtime.end() && safe->second);
            if (!parameter) {
                complete = false;
                continue;
            }
            if (const auto found = defaults.find(key); found != defaults.end()) {
                if (found->second < parameter->minimum ||
                        found->second > parameter->maximum) {
                    // A range that cannot represent the processor's own
                    // native default is not a qualified UI contract.
                    complete = false;
                    continue;
                }
                parameter->defaultValue = found->second;
            }
            descriptor.parameters.push_back(std::move(*parameter));
        }
        descriptor.qualification = coreQualifiedType(type) && complete
            ? Qualification::core : Qualification::hidden;
        result.push_back(std::move(descriptor));
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return std::tie(left.category, left.displayName) <
            std::tie(right.category, right.displayName);
    });
    cached = std::move(result);
    });
    return cached;
}

std::optional<goden_audio::MasterDspParameterDescriptorV2>
MasterDspController::parameterDescriptor(
    const std::string& type, const std::string& key) const
{
    for (const auto& processor : catalogV2()) {
        if (processor.type != type) continue;
        for (const auto& descriptor : processor.parameters) {
            if (descriptor.key == key) return descriptor;
            const auto marker = descriptor.key.find("bandN.");
            if (marker != std::string::npos) {
                const auto suffix = descriptor.key.substr(marker + 5U);
                if (key.rfind("band", 0) == 0 && key.ends_with(suffix)) {
                    auto resolved = descriptor;
                    resolved.key = key;
                    return resolved;
                }
            }
        }
        break;
    }
    if (type == "autoeq.equalizerApo" && key.rfind("band", 0) == 0)
        return descriptorForParameter(type, key, true);
    return std::nullopt;
}

std::vector<std::string> MasterDspController::factoryPresetNames() const
{
    return {"Null", "Headphone Crossfeed", "AutoEQ Template",
        "Room Correction", "Master Safety", "Transparent Control",
        "Tape and Space", "Wide Ambient"};
}

bool MasterDspController::loadFactoryPreset(const std::string& name)
{
    goden_audio::MasterDspRackV1 rack;
    auto add = [&](const char* type, const char* params = "{}", bool terminal = false) {
        goden_audio::MasterDspModuleV1 module;
        module.uuid = newUuid();
        module.type = type;
        module.parametersJson = params;
        module.terminal = terminal || isTerminalType(type);
        rack.modules.push_back(std::move(module));
    };
    if (name == "Null") {
    } else if (name == "Headphone Crossfeed") {
        add("utility.crossfeed", "{\"amount\":0.32,\"delayUs\":280,\"cutoffHz\":700}");
    } else if (name == "AutoEQ Template") {
        add("utility.trim", "{\"gainDb\":-6}");
        add("eq.parametric");
    } else if (name == "Room Correction") {
        add("eq.linearPhase");
        add("effects.reverb.convolution");
    } else if (name == "Master Safety") {
        add("maximizer.truePeakLimiter", "{\"ceilingDb\":-1}", true);
    } else if (name == "Transparent Control") {
        add("eq.minimumPhase");
        add("dynamics.compressor", "{\"ratio\":1.5,\"thresholdDb\":-18}");
        add("maximizer.truePeakLimiter", "{\"ceilingDb\":-1}", true);
    } else if (name == "Tape and Space") {
        add("saturation.tape", "{\"driveDb\":2,\"mix\":0.35}");
        add("effects.reverb.plate", "{\"dryWet\":0.12}");
    } else if (name == "Wide Ambient") {
        add("stereo.imager", "{\"width\":1.2}");
        add("effects.delay.stereo", "{\"dryWet\":0.12}");
        add("effects.reverb.fdn", "{\"dryWet\":0.16}");
    } else {
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        selectedRackLocked() = std::move(rack);
        state_.presetName = name;
        changedLocked();
    }
    publishUi();
    return true;
}

bool MasterDspController::importEqualizerApo(
    const std::wstring& path, std::string* error)
{
    std::error_code sizeError;
    const auto sourceBytes = std::filesystem::file_size(
        std::filesystem::path(path), sizeError);
    if (sizeError || sourceBytes == 0 || sourceBytes > kMaxApoSourceBytes) {
        if (error) *error =
            "Equalizer APO imports must be non-empty and no larger than 4 MiB.";
        return false;
    }
    std::ifstream stream{std::filesystem::path(path)};
    if (!stream) {
        if (error) *error = "Could not open the Equalizer APO file.";
        return false;
    }
    double preamp = 0.0;
    struct Band { int type = 0; double fc = 1000; double gain = 0; double q = 1; };
    std::vector<Band> bands;
    std::string line;
    while (std::getline(stream, line)) {
        if (line.size() > kMaxApoLineBytes) {
            if (error) *error =
                "An Equalizer APO line exceeds the 8 KiB safety limit.";
            return false;
        }
        std::istringstream words(line);
        std::string first;
        words >> first;
        if (first == "Preamp:") {
            words >> preamp;
            if (!words || !std::isfinite(preamp) || preamp < -60.0 || preamp > 30.0) {
                if (error) *error = "Preamp must be a finite value from -60 to +30 dB.";
                return false;
            }
        } else if (first == "Filter") {
            std::string number, on, kind;
            words >> number >> on >> kind;
            if (on != "ON") continue;
            if (kind != "PK" && kind != "LS" && kind != "HS") {
                if (error) *error = "Only PK, LS and HS filters are supported.";
                return false;
            }
            Band band{kind == "LS" ? 1 : (kind == "HS" ? 2 : 0)};
            std::string token;
            while (words >> token) {
                if (token == "Fc") words >> band.fc;
                else if (token == "Gain") words >> band.gain;
                else if (token == "Q") words >> band.q;
            }
            if (!std::isfinite(band.fc) || !std::isfinite(band.gain) ||
                    !std::isfinite(band.q) || band.fc < 20.0 ||
                    band.fc > 20000.0 || band.gain < -60.0 ||
                    band.gain > 30.0 || band.q < 0.1 || band.q > 20.0) {
                if (error) *error = "A filter is outside the supported frequency, gain or Q range.";
                return false;
            }
            if (bands.size() >= 128U) {
                if (error) *error = "Equalizer APO imports are limited to 128 filters.";
                return false;
            }
            bands.push_back(band);
        }
    }
    if (stream.bad()) {
        if (error) *error = "The Equalizer APO file could not be read completely.";
        return false;
    }
    std::ostringstream json;
    json << "{\"preampDb\":" << preamp;
    for (std::size_t index = 0; index < bands.size(); ++index) {
        json << ",\"band" << index << ".type\":" << bands[index].type
             << ",\"band" << index << ".frequencyHz\":" << bands[index].fc
             << ",\"band" << index << ".gainDb\":" << bands[index].gain
             << ",\"band" << index << ".q\":" << bands[index].q;
    }
    json << '}';
    {
        std::lock_guard lock(mutex_);
        auto& rack = selectedRackLocked();
        if (rack.modules.size() + 1U > kMaxModules) {
            if (error) *error = "The rack already contains 32 modules.";
            return false;
        }
        auto terminal = std::find_if(rack.modules.begin(), rack.modules.end(),
            [](const auto& value) { return value.terminal; });
        goden_audio::MasterDspModuleV1 module;
        module.uuid = newUuid();
        module.type = "autoeq.equalizerApo";
        module.parametersJson = json.str();
        rack.modules.insert(terminal, std::move(module));
        state_.presetName = "Imported AutoEQ";
        changedLocked();
    }
    publishUi();
    return true;
}

bool MasterDspController::importImpulseResponse(
    const std::wstring& path, std::string* error)
{
    const std::filesystem::path source(path);
    std::error_code ioError;
    const auto sourceSize = std::filesystem::file_size(source, ioError);
    if (ioError || sourceSize == 0 || sourceSize > kMaxIrSourceBytes) {
        if (error) *error = "Impulse responses must be non-empty and no larger than 256 MiB.";
        return false;
    }
    auto extensionText = source.extension().wstring();
    std::transform(extensionText.begin(), extensionText.end(),
        extensionText.begin(), towlower);
    if (extensionText != L".wav" && extensionText != L".flac") {
        if (error) *error = "Impulse responses must be WAV or FLAC files.";
        return false;
    }
    const auto hash = sha256File(source);
    if (hash.empty()) {
        if (error) *error = "Could not read the impulse response.";
        return false;
    }
    const auto directory = dspRoot() / L"ir";
    std::filesystem::create_directories(directory, ioError);
    if (ioError) {
        if (error) *error = "Could not create the impulse-response cache.";
        return false;
    }
    const std::filesystem::path extension(extensionText);
    const auto target = directory /
        (std::filesystem::path(hash).wstring() + extension.wstring());
    if (!std::filesystem::exists(target, ioError)) {
        const auto staging = directory /
            (std::filesystem::path(hash).wstring() + L".staging");
        std::filesystem::copy_file(source, staging,
            std::filesystem::copy_options::overwrite_existing, ioError);
        if (!ioError && sha256File(staging) == hash)
            std::filesystem::rename(staging, target, ioError);
        if (ioError) {
            std::error_code ignored;
            std::filesystem::remove(staging, ignored);
        }
    }
    if (ioError) {
        if (error) *error = "Could not cache the impulse response.";
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        auto& rack = selectedRackLocked();
        if (rack.modules.size() >= kMaxModules) {
            if (error) *error = "The rack already contains 32 modules.";
            return false;
        }
        goden_audio::MasterDspModuleV1 module;
        module.uuid = newUuid();
        module.type = "effects.reverb.convolution";
        module.irContentKey = hash + extension.string();
        const auto terminal = std::find_if(rack.modules.begin(), rack.modules.end(),
            [](const auto& value) { return value.terminal; });
        rack.modules.insert(terminal, std::move(module));
        state_.presetName = "Imported IR";
        changedLocked();
    }
    publishUi();
    trimIrCache(snapshot());
    return true;
}

bool MasterDspController::exportPreset(
    const std::wstring& path, std::string* error) const
{
    const auto state = snapshot();
    const auto& rack = state.selectedB ? state.rackB : state.rackA;
    sonare::util::json::Object root;
    root["schema"] = "goden.master-dsp-preset.v2";
    root["name"] = state.presetName;
    sonare::util::json::Array modules;
    sonare::util::json::Array assetNames;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> assets;
    std::size_t aggregateAssets = 0;
    try {
        for (const auto& module : rack.modules) {
            if (!module.irContentKey.empty() &&
                    std::none_of(assets.begin(), assets.end(), [&](const auto& item) {
                        return item.first == module.irContentKey;
                    })) {
                if (!validAssetKey(module.irContentKey))
                    throw std::runtime_error("unsafe impulse-response asset name");
                const auto assetPath = dspRoot() / L"ir" /
                    std::filesystem::path(module.irContentKey);
                const auto size = std::filesystem::file_size(assetPath);
                if (size == 0 || size > kMaxIrSourceBytes ||
                        aggregateAssets + size > kMaxPresetAssetsBytes)
                    throw std::runtime_error("impulse-response asset limit exceeded");
                std::ifstream asset(assetPath, std::ios::binary);
                if (!asset) {
                    if (error) *error = "A referenced impulse response is missing.";
                    return false;
                }
                std::vector<std::uint8_t> bytes(
                    (std::istreambuf_iterator<char>(asset)), {});
                aggregateAssets += bytes.size();
                assetNames.emplace_back(module.irContentKey);
                assets.emplace_back(module.irContentKey, std::move(bytes));
            }
            if (!module.opaqueFuturePayload.empty()) {
                modules.emplace_back(sonare::util::json::parse_strict(
                    module.opaqueFuturePayload));
                continue;
            }
            sonare::util::json::Object object;
            object["uuid"] = module.uuid;
            object["type"] = module.type;
            object["bypass"] = module.bypassed;
            object["terminal"] = module.terminal;
            object["parameters"] = sonare::util::json::parse_strict(
                module.parametersJson.empty() ? "{}" : module.parametersJson);
            object["ir"] = module.irContentKey;
            modules.emplace_back(std::move(object));
        }
    } catch (const std::exception& exception) {
        if (error) *error = std::string("Preset contains invalid parameters: ") +
            exception.what();
        return false;
    }
    root["modules"] = std::move(modules);
    root["assets"] = std::move(assetNames);
    const auto manifest = sonare::util::json::dump_pretty(
        sonare::util::json::Value(std::move(root)), 2);
    if (manifest.size() > kMaxPresetManifestBytes) {
        if (error) *error = "Preset manifest exceeds 1 MiB.";
        return false;
    }
    std::ofstream stream(std::filesystem::path(path),
        std::ios::binary | std::ios::trunc);
    if (!stream) {
        if (error) *error = "Could not create the preset file.";
        return false;
    }
    const auto magic = kPresetMagicV2;
    const auto manifestSize = static_cast<std::uint32_t>(manifest.size());
    const auto assetCount = static_cast<std::uint32_t>(assets.size());
    const auto manifestCrc = crc32(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(manifest.data()), manifest.size()));
    stream.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    stream.write(reinterpret_cast<const char*>(&manifestSize), sizeof(manifestSize));
    stream.write(reinterpret_cast<const char*>(&assetCount), sizeof(assetCount));
    stream.write(reinterpret_cast<const char*>(&manifestCrc), sizeof(manifestCrc));
    stream.write(manifest.data(), static_cast<std::streamsize>(manifest.size()));
    for (const auto& [key, bytes] : assets) {
        const auto keySize = static_cast<std::uint32_t>(key.size());
        const auto byteCount = static_cast<std::uint64_t>(bytes.size());
        stream.write(reinterpret_cast<const char*>(&keySize), sizeof(keySize));
        stream.write(key.data(), static_cast<std::streamsize>(key.size()));
        stream.write(reinterpret_cast<const char*>(&byteCount), sizeof(byteCount));
        stream.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream) {
        if (error) *error = "Could not finish writing the preset.";
        return false;
    }
    return true;
}

bool MasterDspController::importPreset(
    const std::wstring& path, std::string* error)
{
    std::error_code sizeError;
    const auto presetSize = std::filesystem::file_size(
        std::filesystem::path(path), sizeError);
    if (sizeError || presetSize == 0 ||
            presetSize > kMaxPresetAssetsBytes + kMaxPresetManifestBytes + (1U << 20U)) {
        if (error) *error = "DSP preset exceeds the V2 container limit.";
        return false;
    }
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    if (!stream) {
        if (error) *error = "Could not open the preset file.";
        return false;
    }
    std::string text;
    std::unordered_map<std::string, std::vector<std::uint8_t>> embeddedAssets;
    std::uint32_t fileMagic = 0;
    stream.read(reinterpret_cast<char*>(&fileMagic), sizeof(fileMagic));
    if (stream && fileMagic == kPresetMagicV2) {
        std::uint32_t manifestSize = 0;
        std::uint32_t assetCount = 0;
        std::uint32_t expectedCrc = 0;
        stream.read(reinterpret_cast<char*>(&manifestSize), sizeof(manifestSize));
        stream.read(reinterpret_cast<char*>(&assetCount), sizeof(assetCount));
        stream.read(reinterpret_cast<char*>(&expectedCrc), sizeof(expectedCrc));
        if (!stream || manifestSize > kMaxPresetManifestBytes || assetCount > 32U) {
            if (error) *error = "DSP preset has an invalid V2 header.";
            return false;
        }
        text.resize(manifestSize);
        stream.read(text.data(), static_cast<std::streamsize>(text.size()));
        const auto actualCrc = crc32(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        if (!stream || actualCrc != expectedCrc) {
            if (error) *error = "DSP preset manifest checksum failed.";
            return false;
        }
        std::size_t aggregate = 0;
        for (std::uint32_t index = 0; index < assetCount; ++index) {
            std::uint32_t keySize = 0;
            std::uint64_t byteCount = 0;
            stream.read(reinterpret_cast<char*>(&keySize), sizeof(keySize));
            if (!stream || keySize == 0 || keySize > 256U) {
                if (error) *error = "DSP preset contains an invalid asset name.";
                return false;
            }
            std::string key(keySize, '\0');
            stream.read(key.data(), static_cast<std::streamsize>(key.size()));
            stream.read(reinterpret_cast<char*>(&byteCount), sizeof(byteCount));
            if (!stream || !validAssetKey(key) || byteCount == 0 ||
                    byteCount > kMaxIrSourceBytes ||
                    aggregate + byteCount > kMaxPresetAssetsBytes) {
                if (error) *error = "DSP preset contains an invalid or oversized asset.";
                return false;
            }
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(byteCount));
            stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
            if (!stream || !embeddedAssets.emplace(key, std::move(bytes)).second) {
                if (error) *error = "DSP preset contains a duplicate or truncated asset.";
                return false;
            }
            aggregate += static_cast<std::size_t>(byteCount);
        }
        char trailing = 0;
        if (stream.read(&trailing, 1)) {
            if (error) *error = "DSP preset has trailing data.";
            return false;
        }
    } else {
        if (presetSize > kMaxPresetBytes) {
            if (error) *error = "Legacy DSP presets are limited to 256 MiB.";
            return false;
        }
        stream.clear();
        stream.seekg(0);
        text.assign(std::istreambuf_iterator<char>(stream), {});
    }
    goden_audio::MasterDspRackV1 rack;
    std::string presetName = "Imported preset";
    std::filesystem::path stagingDirectory;
    std::vector<std::filesystem::path> committedAssets;
    try {
        const auto document = sonare::util::json::parse_strict(text);
        if (!document.is_object()) throw std::runtime_error("root must be an object");
        const auto* schema = document.find("schema");
        if (schema == nullptr || !schema->is_string() ||
                (schema->as_string() != "goden.master-dsp-preset.v1" &&
                 schema->as_string() != "goden.master-dsp-preset.v2")) {
            throw std::runtime_error("unsupported preset schema");
        }
        if (const auto* name = document.find("name"); name && name->is_string()) {
            presetName = name->as_string();
        }
        const auto* moduleArray = document.find("modules");
        if (moduleArray == nullptr || !moduleArray->is_array()) {
            throw std::runtime_error("modules array is missing");
        }
        std::unordered_set<std::string> known;
        for (const auto& entry : catalog()) known.insert(entry.type);
        for (const auto& value : moduleArray->as_array()) {
            if (!value.is_object()) throw std::runtime_error("module must be an object");
            if (rack.modules.size() >= kMaxModules)
                throw std::runtime_error("preset exceeds 32 modules");
            goden_audio::MasterDspModuleV1 module;
            const auto* type = value.find("type");
            const auto* parameters = value.find("parameters");
            const auto* bypass = value.find("bypass");
            const auto* terminal = value.find("terminal");
            const auto* ir = value.find("ir");
            if (type == nullptr || !type->is_string() || type->as_string().empty())
                throw std::runtime_error("module type is missing");
            // UUIDs are live-control routing identities, not portable preset
            // identifiers. Regenerate them on every import so repeated imports
            // and the A/B bank can never receive each other's RT commands.
            module.uuid = newUuid();
            module.type = type->as_string();
            module.parametersJson = parameters
                ? sonare::util::json::dump(*parameters) : "{}";
            if (module.parametersJson.size() > kMaxStringBytes)
                throw std::runtime_error("module configuration exceeds 256 KiB");
            module.bypassed = bypass && bypass->is_bool() && bypass->as_bool();
            module.terminal = isTerminalType(module.type) ||
                (terminal && terminal->is_bool() && terminal->as_bool());
            module.irContentKey = ir && ir->is_string() ? ir->as_string() : "";
            if (!module.irContentKey.empty() && !validAssetKey(module.irContentKey))
                throw std::runtime_error("unsafe impulse-response asset name");
            if (known.find(module.type) == known.end()) {
                module.bypassed = true;
                auto preserved = value.as_object();
                preserved["uuid"] = module.uuid;
                module.opaqueFuturePayload = sonare::util::json::dump(
                    sonare::util::json::Value(std::move(preserved)));
            }
            rack.modules.push_back(std::move(module));
        }
        (void)normalizeTerminalStages(rack);
        if (const auto* assetMap = document.find("assets");
                assetMap && assetMap->is_object()) {
            std::size_t aggregate = 0;
            for (const auto& [key, encoded] : assetMap->as_object()) {
                if (!validAssetKey(key) || !encoded.is_string())
                    throw std::runtime_error("legacy preset contains an unsafe asset");
                const auto bytes = hexDecode(encoded.as_string());
                if (bytes.empty() || bytes.size() > kMaxIrSourceBytes ||
                        aggregate + bytes.size() > kMaxPresetAssetsBytes)
                    throw std::runtime_error("legacy preset asset limit exceeded");
                aggregate += bytes.size();
                if (!embeddedAssets.emplace(key, bytes).second)
                    throw std::runtime_error("duplicate preset asset");
            }
        }
        if (!embeddedAssets.empty()) {
            std::error_code ioError;
            const auto directory = dspRoot() / L"ir";
            stagingDirectory = dspRoot() / L"import-staging" /
                std::filesystem::path(newUuid());
            std::filesystem::create_directories(stagingDirectory, ioError);
            std::filesystem::create_directories(directory, ioError);
            if (ioError) throw std::runtime_error("could not create IR import staging");
            for (const auto& [key, bytes] : embeddedAssets) {
                const auto staged = stagingDirectory / std::filesystem::path(key);
                std::ofstream asset(staged, std::ios::binary | std::ios::trunc);
                asset.write(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
                asset.close();
                if (!asset || sha256File(staged) != key.substr(0, 64U))
                    throw std::runtime_error("impulse-response hash validation failed");
            }
            for (const auto& [key, bytes] : embeddedAssets) {
                const auto target = directory / std::filesystem::path(key);
                if (std::filesystem::exists(target, ioError)) continue;
                std::filesystem::rename(
                    stagingDirectory / std::filesystem::path(key), target, ioError);
                if (ioError) throw std::runtime_error("could not publish IR asset");
                committedAssets.push_back(target);
            }
            std::filesystem::remove_all(stagingDirectory, ioError);
            stagingDirectory.clear();
        }
        for (auto& module : rack.modules) {
            if (module.irContentKey.empty()) continue;
            const std::filesystem::path safeName(module.irContentKey);
            if (safeName.filename() != safeName ||
                    !std::filesystem::exists(dspRoot() / L"ir" / safeName)) {
                module.bypassed = true;
            }
        }
    } catch (const std::exception& exception) {
        std::error_code ignored;
        if (!stagingDirectory.empty())
            std::filesystem::remove_all(stagingDirectory, ignored);
        for (const auto& asset : committedAssets)
            std::filesystem::remove(asset, ignored);
        if (error) *error = std::string("Could not import preset: ") +
            exception.what();
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        selectedRackLocked() = std::move(rack);
        state_.presetName = std::move(presetName);
        changedLocked();
    }
    publishUi();
    trimIrCache(snapshot());
    return true;
}

void MasterDspController::publishPreparing(bool preparing) noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (preparing) state_.status = goden_audio::MasterDspStatus::preparing;
        else if (state_.enabled) state_.status = state_.bypassed
            ? goden_audio::MasterDspStatus::bypassed
            : (activeModuleCount_.load(std::memory_order_relaxed) == 0
                ? goden_audio::MasterDspStatus::dry
                : goden_audio::MasterDspStatus::active);
    }
    publishUi();
}

void MasterDspController::publishTelemetry(
    const goden_audio::MasterDspTelemetryV1& telemetry) noexcept
{
    std::lock_guard lock(mutex_);
    if (telemetry.revision >= state_.telemetry.revision)
        state_.telemetry = telemetry;
}

void MasterDspController::publishFault(std::string text) noexcept
{
    goden_audio::MasterDspFaultV2 fault;
    fault.code = 0x1000U;
    fault.rackRevision = revision_.load(std::memory_order_acquire);
    fault.severity = goden_audio::MasterDspFaultSeverity::recoverable;
    fault.action = goden_audio::MasterDspFaultAction::moduleBypassed;
    fault.message = std::move(text);
    publishFault(std::move(fault));
}

void MasterDspController::publishFault(
    goden_audio::MasterDspFaultV2 fault) noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (fault.rackRevision == 0)
            fault.rackRevision = revision_.load(std::memory_order_relaxed);
        state_.faultText = fault.message;
        faults_.push_back(std::move(fault));
        if (faults_.size() > 64U) faults_.erase(faults_.begin());
        state_.status = goden_audio::MasterDspStatus::partialFault;
    }
    publishUi();
}

void MasterDspController::changedLocked(
    bool persist, bool recordHistory, bool bumpRackRevision)
{
    if (recordHistory && historyReady_) {
        undo_.push_back(committedState_);
        if (undo_.size() > 64) undo_.erase(undo_.begin());
        redo_.clear();
    }
    const auto& rack = selectedRackLocked();
    const auto count = std::count_if(rack.modules.begin(), rack.modules.end(),
        [](const auto& module) { return !module.bypassed; });
    enabled_.store(state_.enabled, std::memory_order_release);
    bypassed_.store(state_.bypassed, std::memory_order_release);
    activeModuleCount_.store(static_cast<std::size_t>(count),
        std::memory_order_release);
    moduleCount_.store(rack.modules.size(), std::memory_order_release);
    state_.status = !state_.enabled ? goden_audio::MasterDspStatus::off
        : state_.bypassed ? goden_audio::MasterDspStatus::bypassed
        : count == 0 ? goden_audio::MasterDspStatus::dry
        : goden_audio::MasterDspStatus::active;
    state_.faultText.clear();
    if (bumpRackRevision) {
        state_.revision = revision_.fetch_add(1, std::memory_order_acq_rel) + 1;
    } else {
        state_.revision = revision_.load(std::memory_order_acquire);
    }
    std::erase_if(faults_, [&](const auto& fault) {
        return fault.rackRevision != 0 && fault.rackRevision < state_.revision;
    });
    goden_audio::notifyMasterDspCompilerWorkers();
    if (persist) persistLocked();
    // A parameter drag is a preview transaction. Its intermediate states are
    // audible and repaint normally, but the committed history point advances
    // only when endParameterGesture() runs on mouse release/capture loss.
    if (!parameterGestureActive_) committedState_ = state_;
    historyReady_ = true;
}

void MasterDspController::loadLocked()
{
    undo_.clear();
    redo_.clear();
    historyReady_ = false;
    state_ = {};
    state_.enabled = g_masterDspEnabled.get();
    state_.bypassed = g_masterDspBypass.get();
    std::vector<std::uint8_t> bytes;
    const auto stored = g_masterDspBlob.get();
    if (stored.is_valid() && stored->size() != 0 &&
            stored->size() <= kMaxRackBlobBytes) {
        const auto* begin = static_cast<const std::uint8_t*>(stored->get_ptr());
        bytes.assign(begin, begin + stored->size());
    } else {
        bytes = hexDecode(g_masterDspLegacyBlob.c_str());
    }
    bool repairedIdentities = false;
    if (bytes.size() >= sizeof(std::uint32_t) &&
            bytes.size() <= kMaxRackBlobBytes) {
        const auto* cursor = bytes.data();
        const auto* end = cursor + bytes.size();
        std::uint32_t magic = 0;
        std::uint8_t selected = 0;
        bool v2 = false;
        bool validHeader = readPod(cursor, end, magic);
        if (validHeader && magic == kBlobMagicV2) {
            std::uint32_t payloadSize = 0;
            std::uint32_t expectedCrc = 0;
            validHeader = readPod(cursor, end, payloadSize) &&
                readPod(cursor, end, expectedCrc) &&
                payloadSize <= kMaxRackBlobBytes &&
                static_cast<std::size_t>(end - cursor) == payloadSize &&
                crc32(std::span(cursor, payloadSize)) == expectedCrc;
            if (validHeader) {
                end = cursor + payloadSize;
                v2 = true;
            }
        } else if (magic != kBlobMagic) {
            validHeader = false;
        }
        if (validHeader && readPod(cursor, end, selected) &&
                readString(cursor, end, state_.presetName) &&
                readRack(cursor, end, state_.rackA) &&
                readRack(cursor, end, state_.rackB)) {
            state_.selectedB = selected != 0;
            std::uint8_t initialized = 0;
            if (readPod(cursor, end, initialized)) {
                state_.rackBInitialized = initialized != 0;
            } else {
                state_.rackBInitialized = !state_.rackB.modules.empty();
            }
            if (v2 && cursor != end) validHeader = false;
            if (!validHeader) {
                state_.rackA = {};
                state_.rackB = {};
                state_.selectedB = false;
                state_.presetName = "Null";
            }
        } else {
            state_.rackA = {};
            state_.rackB = {};
            state_.selectedB = false;
            state_.presetName = "Null";
        }
    }
    auto repairAssets = [](goden_audio::MasterDspRackV1& rack) {
        bool repaired = false;
        for (auto& module : rack.modules) {
            if (module.irContentKey.empty() ||
                validAssetKey(module.irContentKey)) {
                continue;
            }
            module.irContentKey.clear();
            module.bypassed = true;
            repaired = true;
        }
        return repaired;
    };
    repairedIdentities = repairAssets(state_.rackA) |
        repairAssets(state_.rackB) | repairedIdentities;
    std::unordered_set<std::string> identities;
    repairedIdentities = normalizeModuleIdentities(state_.rackA, identities) |
        normalizeModuleIdentities(state_.rackB, identities);
    repairedIdentities = normalizeTerminalStages(state_.rackA) |
        normalizeTerminalStages(state_.rackB) | repairedIdentities;
    repairedIdentities = consolidateLegacyAutoEqPreamp(
        state_.rackA, state_.presetName) |
        consolidateLegacyAutoEqPreamp(state_.rackB, state_.presetName) |
        repairedIdentities;
    changedLocked(false, false);
    if (repairedIdentities) persistLocked();
}

void MasterDspController::persistLocked() const
{
    std::vector<std::uint8_t> payload;
    payload.reserve(1024);
    appendPod(payload, static_cast<std::uint8_t>(state_.selectedB ? 1U : 0U));
    appendString(payload, state_.presetName);
    appendRack(payload, state_.rackA);
    appendRack(payload, state_.rackB);
    appendPod(payload, static_cast<std::uint8_t>(
        state_.rackBInitialized ? 1U : 0U));
    if (payload.size() > kMaxRackBlobBytes)
        throw std::length_error("DSP rack state exceeds 8 MiB");
    std::vector<std::uint8_t> bytes;
    bytes.reserve(payload.size() + 12U);
    if (rolloutFlagEnabled(RolloutFlag::masterDspStateV2)) {
        appendPod(bytes, kBlobMagicV2);
        appendPod(bytes, static_cast<std::uint32_t>(payload.size()));
        appendPod(bytes, crc32(payload));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    } else {
        appendPod(bytes, kBlobMagic);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    }
    g_masterDspBlob.set(bytes.data(), bytes.size());
    g_masterDspLegacyBlob = "";
}

void MasterDspController::publishUi() noexcept
{
    UiInvalidationBus::instance().publish(
        UiInvalidationSurface::nativePanels, UiDirty::semantic);
    UiInvalidationBus::instance().publish(
        UiInvalidationSurface::masterDsp, UiDirty::semantic);
}

} // namespace nekorox::component
