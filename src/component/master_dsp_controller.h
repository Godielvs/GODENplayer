#pragma once

#include "master_dsp_types.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace nekorox::component {

class MasterDspController final {
public:
    static MasterDspController& instance();

    void start();
    void stop();

    [[nodiscard]] goden_audio::MasterDspStateV1 snapshot() const;
    [[nodiscard]] goden_audio::MasterDspTelemetryV1 telemetrySnapshot() const;
    [[nodiscard]] goden_audio::MasterDspStateV2 snapshotV2() const;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] bool enabledRt() const noexcept;
    [[nodiscard]] bool bypassedRt() const noexcept;
    [[nodiscard]] std::size_t activeModuleCountRt() const noexcept;
    [[nodiscard]] std::size_t moduleCountRt() const noexcept;
    void setTelemetryDemand(bool demanded) noexcept;
    [[nodiscard]] bool telemetryDemandRt() const noexcept;

    void setEnabled(bool enabled);
    void setBypassed(bool bypassed);
    void toggleEnabled();
    void toggleBypass();
    void selectRackB(bool selectedB);
    void copyAToB();
    void copyBToA();
    bool undo();
    bool redo();
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;

    bool addModule(const std::string& type, bool terminal = false);
    bool removeModule(const std::string& uuid);
    bool moveModule(const std::string& uuid, int delta);
    bool setModuleBypassed(const std::string& uuid, bool bypassed);
    bool setModuleParameters(const std::string& uuid, std::string json);
    void beginParameterGesture();
    void endParameterGesture();
    void clearRack();

    [[nodiscard]] std::vector<goden_audio::MasterDspCatalogEntryV1> catalog() const;
    [[nodiscard]] const std::vector<goden_audio::MasterDspProcessorDescriptorV2>&
        catalogV2() const;
    [[nodiscard]] std::optional<goden_audio::MasterDspParameterDescriptorV2>
        parameterDescriptor(const std::string& type,
            const std::string& key) const;
    bool loadFactoryPreset(const std::string& name);
    [[nodiscard]] std::vector<std::string> factoryPresetNames() const;

    bool importEqualizerApo(const std::wstring& path, std::string* error = nullptr);
    bool importImpulseResponse(const std::wstring& path, std::string* error = nullptr);
    bool exportPreset(const std::wstring& path, std::string* error = nullptr) const;
    bool importPreset(const std::wstring& path, std::string* error = nullptr);

    void publishPreparing(bool preparing) noexcept;
    void publishTelemetry(const goden_audio::MasterDspTelemetryV1& telemetry) noexcept;
    void publishFault(std::string text) noexcept;
    void publishFault(goden_audio::MasterDspFaultV2 fault) noexcept;

private:
    MasterDspController() = default;
    ~MasterDspController() = default;
    MasterDspController(const MasterDspController&) = delete;
    MasterDspController& operator=(const MasterDspController&) = delete;

    goden_audio::MasterDspRackV1& selectedRackLocked() noexcept;
    const goden_audio::MasterDspRackV1& selectedRackLocked() const noexcept;
    void changedLocked(bool persist = true, bool recordHistory = true,
        bool bumpRackRevision = true);
    void loadLocked();
    void persistLocked() const;
    void publishUi() noexcept;

    mutable std::mutex mutex_;
    goden_audio::MasterDspStateV1 state_;
    goden_audio::MasterDspStateV1 committedState_;
    goden_audio::MasterDspStateV1 parameterGestureInitialState_;
    std::vector<goden_audio::MasterDspFaultV2> faults_;
    std::vector<goden_audio::MasterDspStateV1> undo_;
    std::vector<goden_audio::MasterDspStateV1> redo_;
    std::atomic<std::uint64_t> revision_{1};
    std::atomic<bool> enabled_{false};
    std::atomic<bool> bypassed_{false};
    std::atomic<std::size_t> activeModuleCount_{0};
    std::atomic<std::size_t> moduleCount_{0};
    std::atomic<bool> telemetryDemand_{false};
    bool started_ = false;
    bool historyReady_ = false;
    bool parameterGestureActive_ = false;
    bool parameterGestureChanged_ = false;
};

} // namespace nekorox::component
