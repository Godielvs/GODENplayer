"""Compile actual KeepPlaying methods with bounded host/state seams.
Usage: OUTPUT KEEP_CPP KEEP_HEADER baseline|candidate.
Native tests exercise owned temporary GUID/location and real transport.
"""
from pathlib import Path
import hashlib,json,sys
repo=Path(__file__).resolve().parents[1];out=Path(sys.argv[1]);src=Path(sys.argv[2]);hdr=Path(sys.argv[3]);mode=sys.argv[4]
assert mode in ('baseline','candidate');out.mkdir(parents=True,exist_ok=False)
s=src.read_text(encoding='utf-8-sig');h=hdr.read_text(encoding='utf-8-sig')
names=['clearContext','albumContains','onPlaybackStarting','onNewTrack','onPlaybackStop','onQueueChanged','handleNextCommand','continuePlayback','resolveNativeNextBoundary']
methods=[]
for name in names:
 p=s.index('KeepPlayingService::'+name+'(');a=s.rfind('\n',0,p)+1;b=s.index('\n}',p)+2;methods.append(s[a:b])
declaration=h[h.index('enum class KeepPlayingMode'):h.index('} // namespace')]
declaration=declaration.replace('private:','public:')
assert ('temporaryAlbumPlayingItem().has_value()' in s)==(mode=='candidate')
cpp=r'''
#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>
using t_size=std::size_t;
struct metadb_handle{};
struct metadb_handle_ptr{int id=0;bool is_valid()const{return id!=0;}void release(){id=0;}const metadb_handle* get_ptr()const{return reinterpret_cast<const metadb_handle*>(std::uintptr_t(id));}bool operator==(const metadb_handle_ptr&)const=default;};
namespace play_control {enum t_track_command{track_command_next,track_command_play,track_command_prev};enum t_stop_reason{stop_reason_eof,stop_reason_user,stop_reason_shutting_down,stop_reason_starting_another};}
struct playlist_manager{int count=0;static playlist_manager* get(){static playlist_manager p;return &p;}int queue_get_count()const{return count;}};
struct playback_control{bool playing=true,paused=false,valid=true;int stops=0;metadb_handle_ptr current{3};static playback_control* get(){static playback_control c;return &c;}bool get_now_playing(metadb_handle_ptr& out){out=current;return valid;}bool is_playing()const{return playing;}bool is_paused()const{return paused;}void stop(){++stops;playing=false;}};
std::vector<std::function<void()>> callbacks;
namespace fb2k{template<class F>void inMainThread(F f){callbacks.emplace_back(f);}}
bool temporary=false,enabledFlag=true,canPlay=true;int bootstraps=0,nextCalls=0;
std::optional<std::size_t> temporaryAlbumPlayingItem(){return temporary?std::optional<std::size_t>(3):std::nullopt;}
__DECLARATION__
KeepPlayingService::KeepPlayingService()=default;
bool KeepPlayingService::enabled()const noexcept{return enabledFlag;}
void KeepPlayingService::ensureFullLibraryContext(bool){++bootstraps;albums_={{{1},{2},{3}}};albumByTrack_.clear();for(const auto& t:albums_[0])albumByTrack_[t.get_ptr()]=0;}
bool KeepPlayingService::playNextAlbum(){++nextCalls;return canPlay;}
__METHODS__
constexpr bool candidate=__CANDIDATE__;
int checks=0;
void require(bool v,const char* m){++checks;if(!v)throw std::runtime_error(m);}
void reset(){temporary=false;enabledFlag=true;canPlay=true;bootstraps=nextCalls=0;callbacks.clear();*playlist_manager::get()={};*playback_control::get()={};}
void seed(KeepPlayingService& k){k.ensureFullLibraryContext();bootstraps=0;k.currentAlbum_=0;}
int main(){try{
 for(bool context:{false,true})for(bool temp:{false,true})for(int track:{2,3})for(bool enabled:{false,true})for(int queue:{0,1}){
  reset();KeepPlayingService k;if(context)seed(k);temporary=temp;enabledFlag=enabled;playback_control::get()->current={track};playlist_manager::get()->count=queue;
  const bool expected=queue==0&&track==3&&(!candidate||!temp);const bool consumed=k.handleNextCommand();
  require(consumed==expected,"button next respects temporary order");require(nextCalls==int(expected&&enabled),"album continuation only at allowed boundary");require(playback_control::get()->stops==int(expected&&!enabled),"disabled policy remains for ordinary album");
  if(candidate&&temp)require(bootstraps==0,"temporary next must not rebuild full Library");
 }
 for(bool temp:{false,true})for(int queue:{0,1})for(int track:{2,3})for(auto command:{play_control::track_command_next,play_control::track_command_play,play_control::track_command_prev}){
  reset();KeepPlayingService k;seed(k);temporary=temp;playlist_manager::get()->count=queue;playback_control::get()->current={track};
  k.nativeNextBoundaryPending_=true;k.nativeNextExpectedTrack_={77};auto generation=k.generation_;k.onPlaybackStarting(command);
  bool pending=queue==0&&track==3&&command==play_control::track_command_next&&(!candidate||!temp);
  require(k.generation_==generation+1,"native start invalidates old intent");require(k.nativeNextBoundaryPending_==pending,"native start temporary guard");
  require(k.nativeNextExpectedTrack_.id==(pending?1:0),"native expected wrap identity");k.onNewTrack({1});require(callbacks.size()==std::size_t(pending),"only proven ordinary wrap schedules callback");
  if(pending){auto jobs=std::move(callbacks);for(auto& job:jobs)job();require(nextCalls==1,"ordinary native deferred boundary retained");}
 }
 for(bool temp:{false,true})for(bool stale:{false,true})for(bool paused:{false,true})for(int queue:{0,1})for(bool enabled:{false,true}){
  reset();KeepPlayingService k;temporary=temp;enabledFlag=enabled;playlist_manager::get()->count=queue;playback_control::get()->paused=paused;k.generation_=7;
  const bool expected=!stale&&!paused&&!queue&&(!candidate||!temp);require(k.resolveNativeNextBoundary(stale?6:7)==expected,"deferred boundary revalidates destination");require(nextCalls==int(expected&&enabled),"deferred continuation count");require(playback_control::get()->stops==int(expected&&!enabled),"deferred stop count");
 }
 for(bool temp:{false,true})for(bool enabled:{false,true})for(bool playing:{false,true})for(int queue:{0,1}){
  reset();KeepPlayingService k;temporary=temp;enabledFlag=enabled;playback_control::get()->playing=playing;playlist_manager::get()->count=queue;k.generation_=9;
  require(k.continuePlayback(9)==(enabled&&!playing&&!queue),"natural EOF policy unchanged for temporary and regular");
 }
 reset();KeepPlayingService k;seed(k);k.randomBag_={1,2};k.nativeNextBoundaryPending_=true;k.nativeNextExpectedTrack_={1};k.expectedTrack_={2};k.continuationInFlight_=true;k.fullLibraryContext_=true;
 auto generation=k.generation_;k.clearContext();require(k.albums_.empty()&&k.albumByTrack_.empty()&&k.randomBag_.empty(),"temporary admission clears old lists");require(!k.expectedTrack_.is_valid()&&!k.nativeNextExpectedTrack_.is_valid()&&!k.nativeNextBoundaryPending_&&!k.continuationInFlight_&&!k.fullLibraryContext_&&k.generation_==generation+1,"temporary admission revokes pending continuation");
 for(auto reason:{play_control::stop_reason_user,play_control::stop_reason_shutting_down,play_control::stop_reason_starting_another,play_control::stop_reason_eof}){
  reset();KeepPlayingService p;temporary=true;playback_control::get()->playing=false;p.onPlaybackStop(reason);require(callbacks.size()==std::size_t(reason==play_control::stop_reason_eof),"only natural EOF schedules continuation");auto jobs=std::move(callbacks);for(auto& job:jobs)job();require(nextCalls==int(reason==play_control::stop_reason_eof),"temporary EOF continuation retained");
 }
 for(bool eof:{false,true}){
  reset();KeepPlayingService p;seed(p);
  if(eof){playback_control::get()->playing=false;p.onPlaybackStop(play_control::stop_reason_eof);}else{p.onPlaybackStarting(play_control::track_command_next);p.onNewTrack({1});}
  require(callbacks.size()==1,"old continuation was actually queued");
  p.clearContext();temporary=true;auto jobs=std::move(callbacks);for(auto& job:jobs)job();
  require(nextCalls==0&&playback_control::get()->stops==0,"admission cancels previously queued native and EOF closures");
 }
 std::cout<<"{\"mode\":\""<<(candidate?"candidate":"baseline")<<"\",\"result\":\"PASS\",\"checks\":"<<checks<<"}\n";
 }catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
'''.replace('__DECLARATION__',declaration).replace('__METHODS__','\n\n'.join(methods)).replace('__CANDIDATE__',str(mode=='candidate').lower())
(out/'oracle.cpp').write_text(cpp)
(out/'source.json').write_text(json.dumps(dict(source=str(src),sha256=hashlib.sha256(src.read_bytes()).hexdigest(),headerSha256=hashlib.sha256(hdr.read_bytes()).hexdigest(),realMethods=names,seams=['SDK handle/playback/queue','owned temporary lookup','full library expansion/playNextAlbum','deferred callback scheduler'],notCovered='Owned GUID/location helper and file-open admission are exercised natively. No timing or CPU claim.'),indent=2)+'\n')
(out/'build.cmd').write_text('@echo off\ncall "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\nif errorlevel 1 exit /b 1\ncd /d "'+str(out.resolve())+'"\ncl /nologo /std:c++20 /EHsc /MT /O2 /W4 oracle.cpp /Fe:oracle.exe\nexit /b %errorlevel%\n')
