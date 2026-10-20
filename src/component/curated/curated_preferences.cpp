#include "curated_preferences.h"
#include "sdk_compat.h"
#include <atomic>
namespace nekorox::component::curated {
namespace {
const GUID kMode{0x8a6e7f31,0x9c25,0x4c2f,{0x96,0x42,0x61,0xe0,0xb5,0x7f,0xb1,0x10}};
const GUID kPalette{0x8a6e7f32,0x9c25,0x4c2f,{0x96,0x42,0x61,0xe0,0xb5,0x7f,0xb1,0x10}};
const GUID kResponse{0x922ca753,0xad3e,0x46ce,{0x9b,0x21,0x12,0x08,0x94,0x4b,0x6c,0x31}};
const GUID kSpeed{0x922ca754,0xad3e,0x46ce,{0x9b,0x21,0x12,0x08,0x94,0x4b,0x6c,0x31}};
cfg_uint selectedMode(kMode,1),selectedPalette(kPalette,0),selectedResponse(kResponse,0),selectedSpeed(kSpeed,2);
std::atomic<std::uint64_t> restartGeneration{1};
}
Response response()noexcept{return unsigned(selectedResponse)==1?Response::legacyC2:Response::expressive;}
float causticsSpeed()noexcept{const unsigned value=selectedSpeed;return value==0?1.f:value==1?1.5f:2.f;}
Mode mode()noexcept{return unsigned(selectedMode)==0?Mode::original:Mode::music;}
Palette palette()noexcept{return unsigned(selectedPalette)==1?Palette::artwork:Palette::original;}
std::uint64_t restartRevision()noexcept{return restartGeneration.load(std::memory_order_relaxed);}
bool setMode(Mode m)noexcept{const auto next=m==Mode::original?0U:1U;if(unsigned(selectedMode)==next)return false;selectedMode=next;return true;}
bool setPalette(Palette p)noexcept{const auto next=p==Palette::artwork?1U:0U;if(unsigned(selectedPalette)==next)return false;selectedPalette=next;return true;}
void restart()noexcept{restartGeneration.fetch_add(1,std::memory_order_relaxed);}
void appendMenu(HMENU menu,bool active)noexcept{
 const auto flags=[&](bool check){return MF_STRING|(active?MF_ENABLED:MF_GRAYED)|(check?MF_CHECKED:0);};
 AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
 AppendMenuW(menu,flags(mode()==Mode::original),41110,L"Curated response: Original (60 Hz reference)");
 AppendMenuW(menu,flags(mode()==Mode::music),41111,L"Curated response: Music");
 AppendMenuW(menu,flags(response()==Response::expressive),41112,L"Music mapping: C3 Expressive");
 AppendMenuW(menu,flags(response()==Response::legacyC2),41113,L"Music mapping: C2 Legacy (reference)");
 AppendMenuW(menu,flags(causticsSpeed()==1.f),41140,L"Caustics Music speed: 1x");
 AppendMenuW(menu,flags(causticsSpeed()==1.5f),41141,L"Caustics Music speed: 1.5x");
 AppendMenuW(menu,flags(causticsSpeed()==2.f),41142,L"Caustics Music speed: 2x");
 AppendMenuW(menu,flags(palette()==Palette::original),41120,L"Curated colors: Original");
 AppendMenuW(menu,flags(palette()==Palette::artwork),41121,L"Curated colors: Album (Music mode)");
 AppendMenuW(menu,flags(false),41130,L"Restart selected simulation");
 AppendMenuW(menu,flags(false),41131,L"Retry curated rendering / budget");
}
bool handleMenu(UINT command)noexcept{
 if(command==41110||command==41111)return setMode(Mode(command-41110));
 if(command==41112||command==41113){selectedResponse=command-41112;restart();return true;}
 if(command>=41140&&command<=41142){selectedSpeed=command-41140;return true;}
 if(command==41120||command==41121)return setPalette(Palette(command-41120));
 if(command==41130){restart();return true;}
 if(command==41131)return true;return false;
}
}
