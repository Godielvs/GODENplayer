#pragma once
#include "performance_telemetry.h"
namespace nekorox::component {
class TelemetrySpan final {
public:
    TelemetrySpan(TelemetrySurface surface,std::uint64_t phase,std::uint64_t detail=0) noexcept
        :surface_(surface),phase_(phase),detail_(detail),started_(PerformanceTelemetry::instance().beginSpan(surface,phase,detail)){}
    ~TelemetrySpan(){PerformanceTelemetry::instance().endSpan(surface_,phase_,started_,detail_);}
    TelemetrySpan(const TelemetrySpan&)=delete;
    TelemetrySpan& operator=(const TelemetrySpan&)=delete;
private:
    TelemetrySurface surface_;std::uint64_t phase_,detail_,started_;
};
}
