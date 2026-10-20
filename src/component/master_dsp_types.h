#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace nekorox::goden_audio {

enum class MasterDspStatus : std::uint8_t {
    off = 0,
    dry,
    active,
    bypassed,
    preparing,
    partialFault,
};

enum class MasterDspProcessDisposition : std::uint8_t {
    untouched = 0,
    delayedDry,
    processed,
};

struct MasterDspProcessResultV2 {
    MasterDspProcessDisposition disposition =
        MasterDspProcessDisposition::untouched;
    std::uint32_t effectiveLatencySamples = 0;
    std::uint64_t rackRevision = 0;
    std::uint32_t faultCount = 0;

    [[nodiscard]] bool changed() const noexcept
    {
        return disposition != MasterDspProcessDisposition::untouched;
    }
};

enum class MasterDspPrepareState : std::uint8_t {
    pending = 0,
    ready,
    fault,
};

struct MasterDspPrepareTicket {
    std::uint8_t domain = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint64_t rackRevision = 0;
    std::uint64_t generation = 0;
    MasterDspPrepareState state = MasterDspPrepareState::pending;
    std::uint32_t faultCode = 0;
};

enum class MasterDspParameterKind : std::uint8_t {
    continuous = 0,
    integer,
    boolean,
    enumeration,
    asset,
};

enum class MasterDspParameterScale : std::uint8_t {
    linear = 0,
    logarithmic,
    decibels,
};

enum class MasterDspParameterUpdate : std::uint8_t {
    realtime = 0,
    structural,
};

struct MasterDspParameterDescriptorV2 {
    std::string key;
    std::string displayName;
    std::string unit;
    std::string group;
    MasterDspParameterKind kind = MasterDspParameterKind::continuous;
    MasterDspParameterScale scale = MasterDspParameterScale::linear;
    MasterDspParameterUpdate update = MasterDspParameterUpdate::structural;
    double minimum = 0.0;
    double maximum = 1.0;
    // The slider stays inside the ergonomic range above. Direct entry,
    // imports and presets may use this wider, audited envelope. Keeping the
    // two ranges separate prevents an extreme-capable control from becoming
    // impossible to tune with a mouse.
    double hardMinimum = 0.0;
    double hardMaximum = 1.0;
    double defaultValue = 0.0;
    double step = 0.01;
    std::vector<std::string> enumLabels;
    std::vector<double> enumValues;
    bool visible = true;

    [[nodiscard]] bool accepts(double value) const noexcept
    {
        return std::isfinite(value) && value >= hardMinimum &&
            value <= hardMaximum;
    }

    [[nodiscard]] bool isInsideComfortRange(double value) const noexcept
    {
        return std::isfinite(value) && value >= minimum && value <= maximum;
    }
};

enum class MasterDspTerminalClass : std::uint8_t {
    none = 0,
    sampleRateConversion,
    outputTrim,
    truePeakLimiter,
    dither,
};

enum class MasterDspMultichannelPolicy : std::uint8_t {
    allChannels = 0,
    frontStereoDryRemainder,
    exactChannelCount,
};

enum class MasterDspQualification : std::uint8_t {
    hidden = 0,
    core,
    qualified,
};

struct MasterDspProcessorDescriptorV2 {
    std::string type;
    std::string displayName;
    std::string category;
    std::uint32_t minimumChannels = 1;
    std::uint32_t maximumChannels = 8;
    std::uint32_t minimumSampleRate = 8000;
    std::uint32_t maximumSampleRate = 384000;
    std::uint32_t declaredLatencySamples = 0;
    std::uint32_t declaredTailSamples = 0;
    MasterDspTerminalClass terminalClass = MasterDspTerminalClass::none;
    MasterDspMultichannelPolicy multichannelPolicy =
        MasterDspMultichannelPolicy::allChannels;
    MasterDspQualification qualification = MasterDspQualification::hidden;
    bool requiresAsset = false;
    std::vector<MasterDspParameterDescriptorV2> parameters;
};

enum class ProcessorVisualizationClass : std::uint8_t {
    none = 0,
    measured,
    analytical,
    telemetry,
    illustrative,
};

// Describes what a module HUD is allowed to claim. A provider with `none`
// leaves every parameter editable but must not invent a response curve.
struct ProcessorVisualizationProviderV1 {
    ProcessorVisualizationClass classification =
        ProcessorVisualizationClass::none;
    std::string label;
};

enum class MasterDspFaultSeverity : std::uint8_t {
    information = 0,
    warning,
    recoverable,
    fatal,
};

enum class MasterDspFaultAction : std::uint8_t {
    none = 0,
    moduleBypassed,
    rackKeptDry,
    importRejected,
};

struct MasterDspFaultV2 {
    std::uint32_t code = 0;
    std::string moduleUuid;
    std::uint64_t rackRevision = 0;
    MasterDspFaultSeverity severity = MasterDspFaultSeverity::warning;
    MasterDspFaultAction action = MasterDspFaultAction::none;
    std::string message;
};

struct AutoEqBandV2 {
    enum class Type : std::uint8_t { peak = 0, lowShelf, highShelf };
    Type type = Type::peak;
    double frequencyHz = 1000.0;
    double gainDb = 0.0;
    double q = 1.0;
    bool enabled = true;
};

struct AutoEqPayloadV2 {
    double preampDb = 0.0;
    std::vector<AutoEqBandV2> bands;
};

struct MasterDspModuleV1 {
    std::string uuid;
    std::string type;
    std::string parametersJson{"{}"};
    std::string irContentKey;
    std::string opaqueFuturePayload;
    std::uint32_t version = 1;
    bool bypassed = false;
    bool terminal = false;
};

struct MasterDspRackV1 {
    std::uint32_t schemaVersion = 1;
    std::vector<MasterDspModuleV1> modules;
};

struct MasterDspTelemetryV1 {
    std::uint64_t revision = 0;
    float inputPeak[2]{};
    float outputPeak[2]{};
    float rms[2]{};
    float truePeakDbtp = -120.0f;
    float integratedLufs = -120.0f;
    float gainReductionDb = 0.0f;
    float correlation = 1.0f;
    float callbackCpuPercent = 0.0f;
    std::uint32_t latencySamples = 0;
    std::uint32_t tailSamples = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t faultCount = 0;
};

struct MasterDspStateV1 {
    MasterDspRackV1 rackA;
    MasterDspRackV1 rackB;
    MasterDspTelemetryV1 telemetry;
    MasterDspStatus status = MasterDspStatus::off;
    std::uint64_t revision = 0;
    std::string presetName{"Null"};
    std::string faultText;
    bool enabled = false;
    bool bypassed = false;
    bool selectedB = false;
    bool rackBInitialized = false;
};

struct MasterDspStateV2 {
    static constexpr std::uint32_t schemaVersion = 2;
    MasterDspStateV1 compatible;
    std::vector<MasterDspFaultV2> faults;
    std::uint32_t crc32 = 0;
};

struct MasterDspCatalogEntryV1 {
    std::string type;
    std::string displayName;
    std::string category;
    std::vector<std::string> parameterNames;
    bool terminal = false;
    bool requiresAsset = false;
};

} // namespace nekorox::goden_audio
