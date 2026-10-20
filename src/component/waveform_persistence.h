#pragma once

#include "waveform_cache_service.h"

#include <memory>

namespace nekorox::component {

std::shared_ptr<WaveformPersistenceHook>
createWaveformPersistenceHook();

} // namespace nekorox::component
