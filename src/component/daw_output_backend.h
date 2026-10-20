#pragma once

#include <cstdint>

#include <windows.h>
#include <mmsystem.h>

namespace nekorox::component {

// Thin WinMM boundary used by the DAW worker. Production always delegates to
// WinMM. A guarded QA profile can inject one operation failure or an artificial
// write delay without moving device ownership back to the UI thread.
class DawOutputBackend final {
public:
    static DawOutputBackend& instance() noexcept;

    MMRESULT open(
        HWAVEOUT* output,
        const WAVEFORMATEX* format,
        HANDLE completionEvent) noexcept;
    MMRESULT prepare(HWAVEOUT output, WAVEHDR* header) noexcept;
    MMRESULT write(HWAVEOUT output, WAVEHDR* header) noexcept;
    [[nodiscard]] bool completed(const WAVEHDR* header) const noexcept;
    MMRESULT reset(HWAVEOUT output) noexcept;
    MMRESULT unprepare(HWAVEOUT output, WAVEHDR* header) noexcept;
    MMRESULT close(HWAVEOUT output) noexcept;

private:
    enum class Fault : std::uint8_t {
        none,
        open,
        prepare,
        write,
        reset,
        unprepare,
        close,
        completionStall,
    };

    DawOutputBackend() = default;
    void refreshQaPolicy() noexcept;
    [[nodiscard]] bool fail(Fault operation) noexcept;

    Fault fault_ = Fault::none;
    std::uint32_t writeDelayMs_ = 0;
    std::uint32_t failAfterWrites_ = 0;
    std::uint32_t writes_ = 0;
};

} // namespace nekorox::component
