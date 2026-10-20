#include "collection_preferences.h"
#include "collection_identity.h"
#include "a4/a4_preferences.h"
#include "sdk_compat.h"
#include <atomic>
#include <string>
#include <string_view>
namespace nekorox::component::collection {
namespace {
const GUID kPreset{0xc4139241,0x3a2f,0x4b29,{0xa1,0xf4,0x45,0x30,0x11,0x94,0x64,0x01}};
const GUID kMode{0xc4139241,0x3a2f,0x4b29,{0xa1,0xf4,0x45,0x30,0x11,0x94,0x64,0x02}};
const GUID kStrength{0xc4139241,0x3a2f,0x4b29,{0xa1,0xf4,0x45,0x30,0x11,0x94,0x64,0x03}};
const GUID kColors{0xc5139241,0x3a2f,0x4b29,{0xa1,0xf4,0x45,0x30,0x11,0x94,0x64,0x04}};
const GUID kFavorites{0xc5139241,0x3a2f,0x4b29,{0xa1,0xf4,0x45,0x30,0x11,0x94,0x64,0x05}};
const GUID kPresetId{0xc6139241,0x3a2f,0x4b29,{0xa1,0xf4,0x45,0x30,0x11,0x94,0x64,0x06}};
cfg_string presetId(kPresetId,"");
cfg_string favorites(kFavorites,"");
cfg_uint albumColors(kColors,1);
// Keep the legacy ordinal until migration runs so older stored indices remain
// authoritative. A fresh profile starts at frozen ordinal 23: Nebula Expressiva.
cfg_uint preset(kPreset,23),music(kMode,1),strength(kStrength,100);
std::atomic<std::uint64_t> restart{1};
void migrate() {
 static bool done=false;if(done)return;
 if(std::string_view(presetId.get_ptr()).empty()){
  const auto id=migratePresetIndex(unsigned(preset));
  presetId=std::string(id).c_str();
 }
 const auto updated=migratePresetFavorites(favorites.get_ptr());
 favorites=updated.c_str();
 done=true;
}
unsigned resolvedPreset() noexcept {
 try{migrate();const auto id=canonicalPresetId(presetId.get_ptr());
  const auto all=presets();unsigned fallback=0;
  for(unsigned i=0;i<all.size();++i){if(all[i].id==id)return i;if(all[i].id==fallbackPresetId)fallback=i;}
  return fallback;
 }catch(...){return 0;}
}
}
UINT qaMessage()noexcept{static const UINT id=RegisterWindowMessageW(L"Goden.CollectionQa.V1");return id;}
bool selected()noexcept{return static_cast<unsigned>(a4::style())==4U;}
Selection selection()noexcept{return {resolvedPreset(),unsigned(music)!=0,std::clamp(float(unsigned(strength))*.01f,.25f,2.f),restart.load(std::memory_order_relaxed),unsigned(albumColors)!=0};}
const wchar_t* title()noexcept{return presets()[selection().preset].title;}
void appendMenu(HMENU parent)noexcept{
 HMENU menu=CreatePopupMenu();if(!menu)return;const auto s=selection();const auto& all=presets();
 const auto appendPreset=[&](unsigned i){const auto flag=MF_STRING|((selected()&&s.preset==i)?MF_CHECKED:0);AppendMenuW(menu,flag,47000+i,all[i].title);};
 for(unsigned i=0;i<all.size();++i){
  appendPreset(i);
 }
 AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
 AppendMenuW(menu,MF_STRING|(!s.music?MF_CHECKED:0),48000,L"Response: Original equations / reference profile");
 AppendMenuW(menu,MF_STRING|(s.music?MF_CHECKED:0),48001,L"Response: Music — expressive mapping");
 AppendMenuW(menu,MF_STRING|(s.intensity==1.25f?MF_CHECKED:0),48002,L"Music intensity: Expressive (125%)");
 AppendMenuW(menu,MF_STRING|(s.intensity==1.75f?MF_CHECKED:0),48003,L"Music intensity: Strong (175%)");
 AppendMenuW(menu,MF_STRING|(s.albumColors?MF_CHECKED:0),48006,L"Colors: Album material");
 AppendMenuW(menu,MF_STRING|(!s.albumColors?MF_CHECKED:0),48007,L"Colors: Author palette");
 AppendMenuW(menu,MF_STRING,48008,L"Music intensity: Balanced (100%)");
 AppendMenuW(menu,MF_STRING,48009,L"Music intensity: Subtle (75%)");
 AppendMenuW(menu,MF_STRING,48004,L"Restart selected effect");
 AppendMenuW(parent,MF_POPUP,reinterpret_cast<UINT_PTR>(menu),L"Visual collection C5");
}
bool handleMenu(UINT id)noexcept{
 if(id>=47000&&id<47000+presets().size()){try{migrate();presetId=presets()[id-47000].id;}catch(...){return false;}(void)a4::setStyle(static_cast<a4::Style>(4U));return true;}
 if(id==48000||id==48001){music=id-48000;return true;}
 if(id==48002||id==48003){strength=id==48002?125:175;return true;}
 if(id==48006||id==48007){albumColors=id==48006;return true;}
 if(id==48008||id==48009){strength=id==48008?100:75;return true;}
 if(id==48004){restart.fetch_add(1,std::memory_order_relaxed);return true;}
 return false;
}
bool favorite(std::string_view id)noexcept{
 try{migrate();const std::string all=favorites.get_ptr();const std::string token="|"+std::string(id)+"|";return all.find(token)!=std::string::npos;}catch(...){return false;}
}
void toggleFavorite(std::string_view id){
 migrate();
 if(id.empty()||id.size()>64||id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string_view::npos)return;
 std::string all=favorites.get_ptr();const std::string token="|"+std::string(id)+"|";
 const auto at=all.find(token);if(at==std::string::npos){if(all.size()<4096)all+=token;}else all.erase(at,token.size());favorites=all.c_str();
}
} // namespace
