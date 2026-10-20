#pragma once
#include "a4_budget.h"
#include <windows.h>
#include <cstdint>
namespace nekorox::component::a4 {
enum class Style : unsigned { flow31=0,spectralAlloy=1,curatedCaustics=2,curatedWaves=3,selectedCollection=4 };
enum class MaterialStyle : unsigned { legacy=0,clovis=1,spectralAlloyV2=2 };
// Historical renderer snapshots share the current analysis feed so the visual
// A/B is controlled. Values 0/1 are preserved for existing user profiles.
enum class V2MainBands : unsigned {
 refined=0,
 v1=1,
 c11ShowLights=2,
 c11LargeMinibands=3
};
V2MainBands v2MainBands() noexcept;
bool setV2MainBands(V2MainBands value) noexcept;
MaterialStyle materialStyle() noexcept;
bool setMaterialStyle(MaterialStyle value) noexcept;
enum class Grain : unsigned { off=0,fine=1,strong=2 };
Style style() noexcept;
Grain grain() noexcept;
QualityMode qualityMode() noexcept;
bool setStyle(Style value) noexcept;
bool setGrain(Grain value) noexcept;
bool setQualityMode(QualityMode value) noexcept;
float grainAmplitude() noexcept;
std::uint64_t preferencesRevision() noexcept;
// Header context menu; other Album menus remain owned by the shell.
bool showMenu(HWND owner,POINT screenPoint) noexcept;
bool applyNativeFades() noexcept;
} // namespace nekorox::component::a4
