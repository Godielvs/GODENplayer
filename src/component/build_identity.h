#pragma once

namespace nekorox::component::build_identity {

// Single checked-in identity for local release-candidate builds. Packaging
// verifies this value against component-manifest.json before an artifact is
// accepted; source/dirtiness and binary hashes remain generated per build.
inline constexpr char kVersion[] = "5.1.0-beta.18-public.12";
inline constexpr wchar_t kVersionWide[] = L"5.1.0-beta.18-public.12";

} // namespace nekorox::component::build_identity
