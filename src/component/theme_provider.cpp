#include "nekorox_theme_provider.h"

#include "theme_service.h"

namespace nekorox::component {
namespace {

interop::SharedRgbaColorV1 sharedColor(const RgbaColorV1& value) noexcept
{
    return {value.red, value.green, value.blue, value.alpha};
}

class ThemeProvider : public interop::ThemeProviderV1 {
public:
    bool getSnapshot(interop::SharedThemeSnapshotV2& output) noexcept override
    {
        try {
            const auto source = ThemeService::instance().contractSnapshot();
            output = {};
            output.generation = source.generation;
            output.artworkIdentityHash = source.artworkIdentityHash;
            output.artworkGeneration = source.artworkGeneration;
            output.base = sharedColor(source.base);
            output.panel = sharedColor(source.panel);
            output.text = sharedColor(source.text);
            output.accent = sharedColor(source.accent);
            output.secondary = sharedColor(source.secondary);
            output.highlight = sharedColor(source.highlight);
            output.minimumContrast = source.minimumContrast;
            output.transitionProgress = source.transitionProgress;
            if ((source.flags & kThemeFlagArtworkDerived) != 0U) {
                output.flags |= interop::kSharedThemeFlagArtworkDerived;
            }
            return interop::validSharedThemeSnapshot(output);
        } catch (...) {
            return false;
        }
    }
};

service_factory_single_t<ThemeProvider> g_themeProviderFactory;

} // namespace
} // namespace nekorox::component
