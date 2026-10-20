#include "a4_preferences.h"
#include "curated/curated_preferences.h"
#include "collection/collection_preferences.h"
#include "sdk_compat.h"
#include <atomic>

namespace nekorox::component::a4 {
namespace {
const GUID kStyle{0xeea02ed1,0x794b,0x4df7,{0x8b,0xab,0xf6,0x82,0xe5,0x17,0x36,0x01}};
const GUID kGrain{0xeea02ed2,0x794b,0x4df7,{0x8b,0xab,0xf6,0x82,0xe5,0x17,0x36,0x01}};
const GUID kFades{0xeea02ed3,0x794b,0x4df7,{0x8b,0xab,0xf6,0x82,0xe5,0x17,0x36,0x01}};
const GUID kQuality{0xeea02ed4,0x794b,0x4df7,{0x8b,0xab,0xf6,0x82,0xe5,0x17,0x36,0x01}};
// Default promotion is compile-time and explicit; existing persisted choice wins.
#ifndef GODEN_A4_RELEASE_DEFAULT
#define GODEN_A4_RELEASE_DEFAULT 4 // Visual collection; saved choices still win.
#endif
const GUID kMaterialStyle{0xa7b31c73,0xba5e,0x432e,{0x91,0x44,0x6d,0x72,0x99,0x02,0x81,0x31}};
const GUID kV2MainBands{0x810386d2,0x61bc,0x4d24,{0xb6,0x9a,0x27,0xde,0x96,0x42,0xee,0x01}};
cfg_uint selectedV2MainBands(kV2MainBands,0); // Explicit artistic A/B, never an automatic fallback.
const GUID kC12Restored{0x810386d3,0x61bc,0x4d24,{0xb6,0x9a,0x27,0xde,0x96,0x42,0xee,0x01}};
cfg_bool c12Restored(kC12Restored,false);
cfg_uint selectedMaterialStyle(kMaterialStyle,2); // Refined C12; legacy stays selectable.
cfg_uint selectedStyle(kStyle,GODEN_A4_RELEASE_DEFAULT);
cfg_uint selectedGrain(kGrain,2);
cfg_uint selectedQuality(kQuality,0); // Full is the user-facing default.
cfg_bool fadesApplied(kFades,false);
std::atomic<std::uint64_t> revision{1};
class NativeFadeDefaults : public initquit {
public:
 void on_init() override {
   if(!c12Restored){
    if(unsigned(selectedMaterialStyle)==1U)selectedMaterialStyle=2U;
    selectedV2MainBands=0U;c12Restored=true;
    revision.fetch_add(1,std::memory_order_relaxed);
   }
   if(static_cast<unsigned>(selectedStyle)>static_cast<unsigned>(Style::selectedCollection)){
    selectedStyle=static_cast<unsigned>(Style::flow31);
    revision.fetch_add(1,std::memory_order_relaxed);
    console::print("GODEN retired visual preference migrated to Flow A3.1");
   }
   if(!fadesApplied){
    // One migration attempt. A user's later choice is never rewritten.
    fadesApplied=true;
    const auto enabled=applyNativeFades();
    console::print(enabled?"GODEN native output fades enabled":"GODEN native output fades unavailable; output configuration preserved");
   }
  }
 void on_quit() override {}
};
FB2K_SERVICE_FACTORY(NativeFadeDefaults);
}
Style style() noexcept {const auto v=static_cast<unsigned>(selectedStyle);return v<=4U?static_cast<Style>(v):Style::flow31;}
// Historical comparisons retire into the requested refined C12 material.
V2MainBands v2MainBands() noexcept { return V2MainBands::refined; }
bool setV2MainBands(V2MainBands value) noexcept { (void)value;return false; }
MaterialStyle materialStyle() noexcept {
 const auto value=std::min(static_cast<unsigned>(selectedMaterialStyle),2U);
 return static_cast<MaterialStyle>(value);
}
bool setMaterialStyle(MaterialStyle value) noexcept {
 const unsigned next=std::min(static_cast<unsigned>(value),2U);
 if(unsigned(selectedMaterialStyle)==next)return false;
 selectedMaterialStyle=next;revision.fetch_add(1,std::memory_order_relaxed);return true;
}
Grain grain() noexcept {unsigned v=selectedGrain;return v==0?Grain::off:v==1?Grain::fine:Grain::strong;}
QualityMode qualityMode() noexcept {
 const auto value=std::min(static_cast<unsigned>(selectedQuality),3U);
 // Value 1 was the retired automatic GPU budget. Migrate it to Full without
 // rewriting the user's other visual choices.
 return value==static_cast<unsigned>(QualityMode::automatic)?
    QualityMode::full:static_cast<QualityMode>(value);
}
bool setStyle(Style value) noexcept {
 const auto raw=static_cast<unsigned>(value);const auto next=raw<=4U?raw:0U;
 if(static_cast<unsigned>(selectedStyle)==next)return false;
 selectedStyle=next;revision.fetch_add(1,std::memory_order_relaxed);return true;
}
bool setGrain(Grain value) noexcept {
 const auto raw=static_cast<unsigned>(value);const auto next=std::min(raw,2U);
 if(static_cast<unsigned>(selectedGrain)==next)return false;
 selectedGrain=next;revision.fetch_add(1,std::memory_order_relaxed);return true;
}
bool setQualityMode(QualityMode value) noexcept {
 auto next=std::min(static_cast<unsigned>(value),3U);
 if(next==static_cast<unsigned>(QualityMode::automatic))
    next=static_cast<unsigned>(QualityMode::full);
 if(static_cast<unsigned>(selectedQuality)==next)return false;
 selectedQuality=next;revision.fetch_add(1,std::memory_order_relaxed);return true;
}
float grainAmplitude() noexcept {return grain()==Grain::strong?.070f:grain()==Grain::fine?.024f:0.f;}
std::uint64_t preferencesRevision() noexcept{return revision.load(std::memory_order_relaxed);}
bool applyNativeFades() noexcept {
 if(!core_api::is_main_thread())return false;
 try {
  auto manager=output_manager_v2::get();auto before=manager->getCoreConfig();auto next=before;
  next.m_flags |= outputCoreConfig_t::flagUseFades;
  if(next.m_flags!=before.m_flags)manager->setCoreConfig(&next,sizeof(next),true);
  auto after=manager->getCoreConfig();
  const bool unchanged=InlineIsEqualGUID(before.m_output,after.m_output)&&InlineIsEqualGUID(before.m_device,after.m_device)&&
      before.m_buffer_length==after.m_buffer_length&&before.m_bitDepth==after.m_bitDepth&&
       (before.m_flags&~outputCoreConfig_t::flagUseFades)==(after.m_flags&~outputCoreConfig_t::flagUseFades);
  return unchanged&&(after.m_flags&outputCoreConfig_t::flagUseFades)!=0;
 }catch(...){return false;}
}
bool showMenu(HWND owner,POINT point) noexcept {
 HMENU menu=CreatePopupMenu();if(!menu)return false;
 const auto check=[](bool yes){return MF_STRING|(yes?MF_CHECKED:0);};
 AppendMenuW(menu,check(style()==Style::flow31),41001,L"Visual: Flow A3.1 (preserved)");
 AppendMenuW(menu,check(style()==Style::spectralAlloy),41002,L"Visual: Clovis · C12 refinado");
 AppendMenuW(menu,check(style()==Style::curatedCaustics),41003,L"Curated: Caustics — David A. Roberts");
 AppendMenuW(menu,check(style()==Style::curatedWaves),41004,L"Curated: Wave Field — michael0884");
 curated::appendMenu(menu,style()==Style::curatedCaustics||style()==Style::curatedWaves);
 collection::appendMenu(menu);
 AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
 AppendMenuW(menu,check(materialStyle()==MaterialStyle::legacy),41200,L"A4 material: previous Spectral Alloy");
 AppendMenuW(menu,check(materialStyle()==MaterialStyle::clovis),41201,L"A4 material: Clovis Light / curved glass");
 AppendMenuW(menu,check(materialStyle()==MaterialStyle::spectralAlloyV2),41202,L"A4 material: C12 · base refinada");
 AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
 AppendMenuW(menu,check(grain()==Grain::off),41010,L"A4 film grain: Off");
 AppendMenuW(menu,check(grain()==Grain::fine),41011,L"A4 film grain: Fine");
 AppendMenuW(menu,check(grain()==Grain::strong),41012,L"A4 film grain: Strong");
 AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
 AppendMenuW(menu,check(qualityMode()==QualityMode::full),41030,L"A4 quality: Full (unbounded)");
 AppendMenuW(menu,check(qualityMode()==QualityMode::light),41032,L"A4 quality: Light");
 AppendMenuW(menu,check(qualityMode()==QualityMode::fieldOnly),41033,L"A4 quality: Field only");
 AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
 AppendMenuW(menu,MF_STRING|(style()==Style::spectralAlloy?MF_ENABLED:MF_GRAYED),41021,L"Retry A4 now");
 AppendMenuW(menu,MF_STRING,41020,L"Enable native playback fades");
 const UINT choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,point.x,point.y,0,owner,nullptr);
 DestroyMenu(menu);bool changed=false;
 if(choice>=41001&&choice<=41004)changed=setStyle(static_cast<Style>(choice-41001));
 if(choice>=41200&&choice<=41202)changed=setMaterialStyle(static_cast<MaterialStyle>(choice-41200))||changed;
 if(collection::handleMenu(choice)){changed=true;revision.fetch_add(1,std::memory_order_relaxed);}
 if(curated::handleMenu(choice)){changed=true;revision.fetch_add(1,std::memory_order_relaxed);}
 if(choice>=41010&&choice<=41012)changed=setGrain(static_cast<Grain>(choice-41010))||changed;
 if(choice==41030||choice==41032||choice==41033)
    changed=setQualityMode(static_cast<QualityMode>(choice-41030))||changed;
 if(choice==41021&&style()==Style::spectralAlloy){changed=true;revision.fetch_add(1,std::memory_order_relaxed);}
 if(choice==41020&&!applyNativeFades())MessageBoxW(owner,L"The selected output does not expose the native fading setting. Output device, volume and buffer were not replaced.",L"Native fading",MB_OK|MB_ICONINFORMATION);
 return changed;
}
} // namespace nekorox::component::a4
