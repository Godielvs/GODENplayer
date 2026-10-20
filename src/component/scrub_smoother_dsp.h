#pragma once

namespace nekorox::component {

// Installs the terminal DSP only for the lifetime of this GODENplayer run.
// The previous core chain is leased and restored on clean shutdown. A stale
// lease from a crash is recovered before any new mutation is considered.
void ensureScrubSmootherDspEnabled(bool allowMutation = true);
void restoreScrubSmootherDspChain() noexcept;

} // namespace nekorox::component
