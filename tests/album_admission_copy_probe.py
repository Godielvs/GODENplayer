"""Compare verbatim baseline/candidate Album onNewTrack and snapshot types.
SDK handles, shell mode, playlist/enter/publish/identity services are seams.
No native latency claim: measures the discarded snapshot copy at album changes.
"""
from pathlib import Path
import json,hashlib,sys
root=Path(__file__).resolve().parents[1];out=Path(sys.argv[1]);old=Path(sys.argv[2]);new=root/'src/component/album_session_service.cpp';header=root/'src/component/album_session_service.h'
out.mkdir(parents=True,exist_ok=False)
def block(text,sig):
 a=text.index(sig);b=text.index('{',a)+1;depth=1
 while depth:depth+=(text[b]=='{')-(text[b]=='}');b+=1
 return text[a:b]
ht=header.read_text(encoding='utf-8-sig')
structs='\n'.join(block(ht,s)+';' for s in ['struct AlbumSessionTrack {','enum class AlbumSessionState :','enum class AlbumSessionSourceKind :','struct AlbumSessionSnapshot {'])
method='void AlbumSessionService::onNewTrack('
baseline=block(old.read_text(encoding='utf-8-sig'),method).replace('AlbumSessionService::onNewTrack','Baseline::onNewTrack')
candidate=block(new.read_text(encoding='utf-8-sig'),method).replace('AlbumSessionService::onNewTrack','Candidate::onNewTrack')
cpp=r'''
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "nekorox/album_session.h"
thread_local bool countAllocations=false;
thread_local std::uint64_t allocations=0,allocatedBytes=0;
void* operator new(std::size_t n){if(auto* p=std::malloc(n?n:1)){if(countAllocations){++allocations;allocatedBytes+=n;}return p;}throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
unsigned checks=0;void require(bool b,const char* m){++checks;if(!b)throw std::runtime_error(m);}
struct metadb_handle_ptr{std::shared_ptr<int> value;bool is_valid()const{return bool(value);}bool operator==(const metadb_handle_ptr&)const=default;};
metadb_handle_ptr handle(int n){return {std::make_shared<int>(n)};}
namespace nekorox::component {
__STRUCTS__
enum class PlayerShellMode{library,album};PlayerShellMode mode=PlayerShellMode::album;
PlayerShellMode playerShellMode(){return mode;}
constexpr auto pfc_infinite=static_cast<std::size_t>(-1);
struct playlist_manager{unsigned count=0;static auto get(){static playlist_manager p;return &p;}unsigned queue_get_count()const{return count;}};
struct Service{
 std::shared_ptr<const AlbumSessionSnapshot> current;metadb_handle_ptr entered;
 unsigned enters=0,publishes=0,refreshes=0,cancellations=0;std::size_t playlist=pfc_infinite;
 auto snapshot(){return current;}void cancelContinuation(){++cancellations;}
 bool enterForTrack(const metadb_handle_ptr& t){entered=t;++enters;return true;}
 void publish(std::shared_ptr<AlbumSessionSnapshot> s){current=std::move(s);++publishes;}
 std::size_t ownedPlaylist(){return playlist;}void refreshPlayingIdentity(){++refreshes;}
};
struct Baseline:Service{void onNewTrack(const metadb_handle_ptr&);};
struct Candidate:Service{void onNewTrack(const metadb_handle_ptr&);};
__BASELINE__
__CANDIDATE__
}
using namespace nekorox::component;
std::shared_ptr<AlbumSessionSnapshot> session(unsigned n){
 auto s=std::make_shared<AlbumSessionSnapshot>();s->sessionId=123;s->revision=17;s->state=AlbumSessionState::ready;s->sourceKind=AlbumSessionSourceKind::library;
 s->album=std::wstring(96,L'a');s->albumArtist=std::wstring(64,L'b');s->albumKey=std::wstring(128,L'k');s->stableArtworkAnchor=std::wstring(128,L'x');s->year=L"2000";
 s->policy={nekorox::AlbumRepeatMode::album,true,1234};s->compositionRevision=66;s->sourceRevision=77;s->sourceLayoutRevision=88;
 s->tracks.reserve(n);s->permutation.reserve(n);
 for(unsigned i=0;i<n;++i){s->tracks.push_back({handle(i),std::wstring(96,L't')+std::to_wstring(i),std::wstring(64,L'a'),std::string(67,'r')+std::to_string(i),std::string(74,'e')+std::to_string(i),1,static_cast<int>(i),i%2});s->permutation.push_back(n-i-1);}
 if(n){s->anchor=s->tracks[0].handle;s->actualPlayingIndex=0;s->currentInSession=0;}
 return s;
}
std::wstring describe(const AlbumSessionSnapshot& s){
 std::wostringstream o;o<<s.requestId<<','<<s.sessionId<<','<<s.revision<<','<<s.sourceRevision<<','<<s.sourceLayoutRevision<<','<<int(s.state)<<','<<int(s.sourceKind)<<','<<s.anchor.value.get()<<','<<s.albumKey<<','<<s.stableArtworkAnchor<<','<<s.error<<','<<s.album<<','<<s.albumArtist<<','<<s.year<<','<<s.currentInSession<<','<<s.actualPlayingIndex.value_or(-1)<<','<<s.outsideSession<<','<<s.queueExternal<<','<<s.playlistLeaseValid<<','<<s.ownedPlaylistValid<<','<<s.transportOrderLease<<','<<s.compositionExternallyModified<<','<<s.compositionRevision<<','<<int(s.policy.repeat)<<','<<s.policy.shuffle<<','<<s.policy.seed;
 for(auto& t:s.tracks)o<<'|'<<t.handle.value.get()<<','<<t.title<<','<<t.artist<<','<<t.revisionTrackId.c_str()<<','<<t.entityId.c_str()<<','<<t.disc<<','<<t.track<<','<<t.subsong;
 for(auto p:s.permutation)o<<':'<<p;return o.str();
}
void equivalent(const std::shared_ptr<AlbumSessionSnapshot>& s,metadb_handle_ptr t,PlayerShellMode selected,unsigned queue,std::size_t playlist){
 mode=selected;playlist_manager::get()->count=queue;Baseline a;Candidate b;a.current=b.current=s;a.playlist=b.playlist=playlist;
 auto before=s?describe(*s):L"null";a.onNewTrack(t);b.onNewTrack(t);
 require(a.cancellations==b.cancellations&&a.cancellations==1,"continuation cancellation");
 require(a.enters==b.enters&&a.entered==b.entered,"album admission preserved");
 require(a.publishes==b.publishes&&a.refreshes==b.refreshes,"publication and identity refresh preserved");
 require(bool(a.current)==bool(b.current),"snapshot null state");
 if(a.current)require(describe(*a.current)==describe(*b.current),"full snapshot state preserved");
 require(!s||describe(*s)==before,"old snapshot immutable");
 if(s&&s->sessionId&&t.is_valid()&&a.enters==0){require(b.current!=s,"published selection gets new snapshot");require(b.current->revision==s->revision+1,"revision advanced");}
}
template<class T>void measure(const char* name,std::shared_ptr<AlbumSessionSnapshot> s,metadb_handle_ptr outside,unsigned phase){
 T service;service.current=s;mode=PlayerShellMode::album;service.onNewTrack(outside);
 allocations=allocatedBytes=0;countAllocations=true;auto start=std::chrono::steady_clock::now();
 constexpr unsigned repeat=128;for(unsigned i=0;i<repeat;++i)service.onNewTrack(outside);
 auto end=std::chrono::steady_clock::now();countAllocations=false;
 require(service.enters==repeat+1&&service.current==s&&service.publishes==0,"boundary goes directly to new album");
 if constexpr(std::is_same_v<T,Candidate>)require(allocations==0,"no discarded snapshot allocation");
 std::cout<<"{\"mode\":\""<<name<<"\",\"phase\":"<<phase<<",\"tracks\":"<<s->tracks.size()<<",\"repeats\":"<<repeat<<",\"meanUs\":"<<std::chrono::duration<double,std::micro>(end-start).count()/repeat<<",\"allocationsPerSwitch\":"<<double(allocations)/repeat<<",\"allocatedBytesPerSwitch\":"<<double(allocatedBytes)/repeat<<"}\n";
}
int main(){try{
 auto s=session(12);auto outside=handle(999);
 for(auto m:{PlayerShellMode::library,PlayerShellMode::album})for(auto q:{0U,2U})for(auto lease:{pfc_infinite,std::size_t{5}}){
  equivalent(s,s->tracks.front().handle,m,q,lease);equivalent(s,s->tracks.back().handle,m,q,lease);equivalent(s,outside,m,q,lease);equivalent(s,{},m,q,lease);
  equivalent({},outside,m,q,lease);auto empty=session(0);equivalent(empty,outside,m,q,lease);empty->sessionId=0;equivalent(empty,outside,m,q,lease);
  auto duplicate=session(8);duplicate->tracks[5].handle=duplicate->tracks[2].handle;equivalent(duplicate,duplicate->tracks[2].handle,m,q,lease);
 }
 for(unsigned n:{12U,120U,1200U,12000U}){auto large=session(n);measure<Baseline>("baseline",large,outside,0);measure<Candidate>("candidate",large,outside,1);measure<Candidate>("candidate",large,outside,2);measure<Baseline>("baseline",large,outside,3);}
 std::cout<<"{\"result\":\"PASS\",\"checks\":"<<checks<<"}\n";
 }catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
'''
(out/'probe.cpp').write_text(cpp.replace('__STRUCTS__',structs).replace('__BASELINE__',baseline).replace('__CANDIDATE__',candidate))
(out/'source.json').write_text(json.dumps(dict(baseline=dict(path=str(old),sha256=hashlib.sha256(old.read_bytes()).hexdigest()),candidate=dict(path=str(new),sha256=hashlib.sha256(new.read_bytes()).hexdigest()),headerSha256=hashlib.sha256(header.read_bytes()).hexdigest(),method='onNewTrack',scope='Album boundary discarded copy only; enterForTrack work excluded; SDK/service seams'),indent=2)+'\n')
