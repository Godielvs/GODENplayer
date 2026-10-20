"""Compile extracted production shell event/identity methods against host seams.

Exercises the missing integration (session notification -> cache-hit palette
adoption), not color extraction or GPU rendering. Pass an older shell source to
demonstrate regression detection. Generated C++/MSBuild files stay under out.
"""
from pathlib import Path
import argparse, hashlib, json

p=argparse.ArgumentParser()
p.add_argument('--shell',type=Path)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
repo=Path(__file__).resolve().parents[1]
path=a.shell or repo/'src/component/player_shell_panel.cpp'
source=path.read_text(encoding='utf-8-sig')
def block(sig):
    start=source.index(sig); end=source.index('{',start)+1; depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]

identity=block('    std::wstring albumArtworkIdentity(')
sync=block('    void syncAlbumThemeIntent(bool resolveArtwork)')
activation=block('    void setAlbumArtworkActive(bool active)')
dispatch=block('        if (message == albumArtworkReadyMessage() ||')
visit=block('BOOL CALLBACK visitPlayerShell(HWND window, LPARAM value)')
code=r'''
#include <algorithm>
#include <cwchar>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>
using UINT=unsigned; using HWND=int; using LPARAM=int; using WPARAM=unsigned; using BOOL=int;
#define CALLBACK
constexpr bool FALSE=false,TRUE=true;
UINT albumArtworkReadyMessage(){return 10;}
UINT albumContentReadyMessage(){return 11;}
UINT playerShellModeMessage(){return 12;}
std::vector<UINT> events;
int GetClassNameW(HWND,wchar_t* p,int){std::wcscpy(p,L"GodenPlayerShellV2");return 18;}
void PostMessageW(HWND,UINT m,WPARAM,LPARAM){events.push_back(m);}
void SendMessageW(HWND,UINT,WPARAM,LPARAM){}
void InvalidateRect(HWND,void*,bool){}
enum class PlayerShellMode {normal,album};
enum class AlbumSessionState {loading,ready};
enum class ArtCacheStatus {missing,loading,ready};
struct Track {std::wstring handle;};
struct AlbumSessionSnapshot {
    AlbumSessionState state=AlbumSessionState::loading;
    std::wstring anchor;
    std::vector<Track> tracks;
    size_t currentInSession=0;
};
std::wstring artworkTrackIdentity(const std::wstring& v){return v;}
struct AlbumSessionService {
    std::shared_ptr<AlbumSessionSnapshot> current;
    static AlbumSessionService& instance(){static AlbumSessionService s;return s;}
    auto snapshot(){return current;}
};
struct ArtPixels {std::string color;};
struct LocalTrackContent {};
struct ThemeService {
    std::wstring identity;
    std::string color="gray";
    static ThemeService& instance(){static ThemeService s;return s;}
    void resetForTrack(const std::wstring& id){identity=id;if(id.empty())color="gray";}
    bool updateFromArtwork(const std::wstring& id,const ArtPixels& a) {
        if(id!=identity)return false; color=a.color;return true;
    }
    void resolveWithoutArtwork(const std::wstring& id){if(id==identity)color="gray";}
};
struct Frame {void requestImmediate(){}};
struct Shell {
    HWND window_=1;
    struct CacheOwner {void attach(HWND){} void detach(){}} artCache_;
    unsigned artworkPrefetchSessionId_=0,artworkPrefetchEdge_=0;
    size_t artworkPrefetchCenter_=0;
    bool albumArtworkActive_=true;
    PlayerShellMode mode_=PlayerShellMode::album;
    std::wstring albumThemeIdentity_;
    Frame frameClock_;
    std::map<std::wstring,std::shared_ptr<ArtPixels>> cache;
    int requests=0;
    ArtCacheStatus absent=ArtCacheStatus::loading;
    void invalidatePresentation(){}
    std::shared_ptr<LocalTrackContent> albumContent(const std::shared_ptr<const AlbumSessionSnapshot>&){return {};}
    auto albumArtwork(const std::shared_ptr<const AlbumSessionSnapshot>& s,
        const std::shared_ptr<LocalTrackContent>&,const std::wstring& id,ArtCacheStatus* status) {
        ++requests;
        auto found=cache.find(id);
        if(found!=cache.end()){*status=ArtCacheStatus::ready;return found->second;}
        *status=s->tracks.empty()?ArtCacheStatus::missing:absent;
        return std::shared_ptr<ArtPixels>{};
    }
__IDENTITY__
__SYNC__
__ACTIVATION__
    int onMessage(UINT message){
__DISPATCH__
        return 0;
    }
};
__VISIT__
int failures=0,checks=0;
void check(bool ok,const char* message){
    ++checks; if(!ok){++failures;std::cerr<<"FAIL: "<<message<<"\n";}
}
void drain(Shell& s){auto batch=events;events.clear();for(auto m:batch)s.onMessage(m);}
void publish(Shell& s,const wchar_t* id,bool ready) {
    auto snapshot=std::make_shared<AlbumSessionSnapshot>();
    snapshot->anchor=id;snapshot->state=ready?AlbumSessionState::ready:AlbumSessionState::loading;
    if(ready)snapshot->tracks.push_back({id});
    AlbumSessionService::instance().current=snapshot;
    visitPlayerShell(1,2);drain(s);
}
int main(){
    Shell s;auto& theme=ThemeService::instance();
    s.cache[L"A"]=std::make_shared<ArtPixels>(ArtPixels{"amber"});
    s.cache[L"B"]=std::make_shared<ArtPixels>(ArtPixels{"blue"});
    publish(s,L"A",true);s.syncAlbumThemeIntent(true);
    check(theme.color=="amber","initial ready cover");
    publish(s,L"B",false);s.syncAlbumThemeIntent(true);
    check(theme.identity==L"B","loading owns valid anchor identity");
    check(theme.color=="amber","loading holds displayed color, no gray reset");
    publish(s,L"B",true);
    check(theme.color=="blue","session publication adopts cache hit without completion event");
    publish(s,L"A",true);
    check(theme.color=="amber","cached reverse transition resolves");
    s.syncAlbumThemeIntent(true);
    check(theme.color=="amber","same identity reentry retains palette");
    publish(s,L"B",true);
    check(!theme.updateFromArtwork(L"A",*s.cache[L"A"]),"late old completion rejected by current intent");
    check(theme.color=="blue","late old completion cannot overwrite selected color");
    const auto before=s.requests;
    s.setAlbumArtworkActive(false);
    publish(s,L"A",true);
    check(s.requests==before,"hidden event does not request artwork");
    s.setAlbumArtworkActive(true);
    check(theme.color=="amber","visible reentry resolves resident artwork");
    publish(s,L"C",false);
    check(theme.color=="amber","loading uncached cover holds old color");
    s.absent=ArtCacheStatus::missing;publish(s,L"C",true);
    check(theme.color=="gray","confirmed missing cover uses fallback");
    s.mode_=PlayerShellMode::normal;
    const auto normalBefore=s.requests;publish(s,L"B",true);
    check(s.requests==normalBefore,"Library notification does not activate Album artwork");
    std::cout<<"checks="<<checks<<" failures="<<failures<<"\n";
    return failures?1:0;
}
'''
for name,value in [('IDENTITY',identity),('SYNC',sync),('ACTIVATION',activation),('DISPATCH',dispatch),('VISIT',visit)]:
    code=code.replace('__'+name+'__',value)
a.output.mkdir(parents=True,exist_ok=True)
(a.output/'oracle.cpp').write_text(code,encoding='utf-8')
(a.output/'oracle.vcxproj').write_text('''<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
<ItemGroup Label="ProjectConfigurations"><ProjectConfiguration Include="Release|x64"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration></ItemGroup>
<PropertyGroup Label="Globals"><WindowsTargetPlatformVersion>10.0.19041.0</WindowsTargetPlatformVersion></PropertyGroup>
<Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />
<PropertyGroup Label="Configuration"><ConfigurationType>Application</ConfigurationType><PlatformToolset>v142</PlatformToolset></PropertyGroup>
<Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />
<PropertyGroup><OutDir>$(ProjectDir)</OutDir><IntDir>$(ProjectDir)obj\\</IntDir></PropertyGroup>
<ItemDefinitionGroup><ClCompile><LanguageStandard>stdcpp17</LanguageStandard><ExceptionHandling>Sync</ExceptionHandling><MultiProcessorCompilation>false</MultiProcessorCompilation></ClCompile></ItemDefinitionGroup>
<ItemGroup><ClCompile Include="oracle.cpp" /></ItemGroup>
<Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />
</Project>''',encoding='utf-8')
(a.output/'source.json').write_text(json.dumps(dict(shellSha256=hashlib.sha256(path.read_bytes()).hexdigest(),
    seams=['Artwork broker returns resident pixels synchronously without a completion notification',
           'Host handles, content lookup and theme ownership/color storage are deterministic seams',
           'Actual shell event handler, notification routing, identity and resolution methods extracted unchanged'],
    scope='Event integration regression, not visual or color extraction qualification'),indent=2))
print(a.output/'oracle.vcxproj')
