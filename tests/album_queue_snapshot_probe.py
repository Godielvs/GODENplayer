"""Real queue handler, snapshot types, queue observer and deferred EOF method.
Only host SDK, publication sink and callback scheduler are seams.
"""
from pathlib import Path
import json,hashlib,sys
repo=Path(__file__).resolve().parents[1];out=Path(sys.argv[1]);old=Path(sys.argv[2]);new=repo/'src/component/album_session_service.cpp';header=repo/'src/component/album_session_service.h'
out.mkdir(parents=True,exist_ok=False)
def block(text,sig):
 a=text.index(sig);b=text.index('{',a)+1;depth=1
 while depth:depth+=(text[b]=='{')-(text[b]=='}');b+=1
 return text[a:b]
ht=header.read_text(encoding='utf-8-sig');nt=new.read_text(encoding='utf-8-sig')
structs='\n'.join(block(ht,s)+';' for s in ['struct AlbumSessionTrack {','enum class AlbumSessionState :','enum class AlbumSessionSourceKind :','struct AlbumSessionSnapshot {'])
methods=[]
for label,source in [('Baseline',old.read_text(encoding='utf-8-sig')),('Candidate',nt)]:
 methods.append(block(source,'void AlbumSessionService::onQueueChanged(').replace('AlbumSessionService::',label+'::'))
 methods.append(block(source,'bool AlbumSessionService::onPlaybackStop(').replace('AlbumSessionService::',label+'::'))
observerSource=repo/'src/component/workspace_panel.cpp';observer=block(observerSource.read_text(encoding='utf-8-sig'),'class QueueObserver :')+';'
observer='template<class AlbumSessionService>\n'+observer
cancel=block(ht,'void cancelContinuation()')
fixture=(repo/'tests/album_admission_copy_probe.py').read_text()
fixture=fixture[fixture.index('std::shared_ptr<AlbumSessionSnapshot> session('):fixture.index('void equivalent(')]
cpp=r'''
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
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
namespace play_control{enum t_stop_reason{stop_reason_eof,stop_reason_user,stop_reason_starting_another};}
std::vector<std::function<void()>> jobs;
namespace fb2k{template<class F>void inMainThread(F f){jobs.emplace_back(f);}}
namespace nekorox::component {
__STRUCTS__
struct playlist_manager{unsigned count=0;static auto get(){static playlist_manager p;return &p;}unsigned queue_get_count()const{return count;}};
struct Service{
 std::shared_ptr<const AlbumSessionSnapshot> current;std::uint64_t transportIntent_=0;unsigned publishes=0,plays=0;
 auto snapshot(){return current;}
 __CANCEL__
 void publish(std::shared_ptr<AlbumSessionSnapshot> s){current=std::move(s);++publishes;}
 bool playTrack(std::size_t){++plays;return true;}
};
struct Baseline:Service{inline static Baseline* active=nullptr;static Baseline& instance(){return *active;}void onQueueChanged();bool onPlaybackStop(play_control::t_stop_reason);};
struct Candidate:Service{inline static Candidate* active=nullptr;static Candidate& instance(){return *active;}void onQueueChanged();bool onPlaybackStop(play_control::t_stop_reason);};
__METHODS__
struct KeepPlayingService{unsigned calls=0;static auto& instance(){static KeepPlayingService p;return p;}void onQueueChanged(){++calls;}};
struct Runtime{unsigned queueRevision=0;static auto& instance(){static Runtime p;return p;}void notifyQueueChanged(){++queueRevision;}};
unsigned workspaceNotifications=0,nativeNotifications=0;
void notifyWorkspaceQueueChanged(){++workspaceNotifications;}void notifyNativePanelsPlaybackChanged(){++nativeNotifications;}
struct playback_queue_callback{using t_change_origin=int;virtual void on_changed(t_change_origin)=0;virtual ~playback_queue_callback()=default;};
__OBSERVER__
}
using namespace nekorox::component;
__FIXTURE__
void compare(std::shared_ptr<AlbumSessionSnapshot> s,unsigned queue){
 Baseline a;Candidate b;Baseline::active=&a;Candidate::active=&b;a.current=b.current=s;playlist_manager::get()->count=queue;
 auto oldState=s?describe(*s):L"null";unsigned beforeNative=nativeNotifications,beforeWorkspace=workspaceNotifications,beforeRuntime=Runtime::instance().queueRevision,beforeKeep=KeepPlayingService::instance().calls;
 QueueObserver<Baseline>{}.on_changed(0);QueueObserver<Candidate>{}.on_changed(0);
 require(a.transportIntent_==1&&b.transportIntent_==1,"every queue event invalidates continuation, including no session/equal flag");
 require(nativeNotifications==beforeNative+2&&workspaceNotifications==beforeWorkspace+2&&Runtime::instance().queueRevision==beforeRuntime+2&&KeepPlayingService::instance().calls==beforeKeep+2,"real observer retains all other queue notifications");
 const bool valid=s&&s->sessionId;const bool changed=valid&&s->queueExternal!=(queue!=0);
 require(a.publishes==unsigned(valid)&&b.publishes==unsigned(changed),"only changed flag republishes candidate snapshot");
 if(s){
  require(describe(*s)==oldState,"prior snapshot remains immutable");
  require(b.current->revision==s->revision+unsigned(changed),"revision denotes actual change");
  require((b.current!=s)==changed,"retain snapshot identity on same-state mutation");
  auto projected=*a.current;projected.revision=b.current->revision;require(describe(projected)==describe(*b.current),"all snapshot fields except redundant revision match baseline");
 }else require(!a.current&&!b.current,"missing session retained");
}
template<class T>void cancellation(bool queueFlag,unsigned queue){
 auto s=session(3);s->queueExternal=queueFlag;s->transportOrderLease=true;
 T t;T::active=&t;t.current=s;playlist_manager::get()->count=0;
 require(t.onPlaybackStop(play_control::stop_reason_eof)&&jobs.size()==1,"real deferred EOF continuation is queued");
 playlist_manager::get()->count=queue;QueueObserver<T>{}.on_changed(0);playlist_manager::get()->count=0;
 auto pending=std::move(jobs);jobs.clear();for(auto& job:pending)job();require(t.plays==0,"queue edit cancels real pending EOF even after queue becomes empty");
}
template<class T>void measure(const char* name,std::shared_ptr<AlbumSessionSnapshot> s,unsigned phase){
 T t;t.current=s;playlist_manager::get()->count=1;t.onQueueChanged();
 allocations=allocatedBytes=0;countAllocations=true;auto start=std::chrono::steady_clock::now();
 constexpr unsigned repeat=128;for(unsigned i=0;i<repeat;++i){playlist_manager::get()->count=1+(i%4);t.onQueueChanged();}
 auto end=std::chrono::steady_clock::now();countAllocations=false;
 require(t.transportIntent_==repeat+1,"benchmark preserves cancellation on every edit");
 if constexpr(std::is_same_v<T,Candidate>)require(allocations==0&&t.current==s&&t.publishes==0,"same-flag edits allocate and publish nothing");
 std::cout<<"{\"mode\":\""<<name<<"\",\"phase\":"<<phase<<",\"tracks\":"<<s->tracks.size()<<",\"repeats\":"<<repeat<<",\"meanUs\":"<<std::chrono::duration<double,std::micro>(end-start).count()/repeat<<",\"allocationsPerEdit\":"<<double(allocations)/repeat<<",\"allocatedBytesPerEdit\":"<<double(allocatedBytes)/repeat<<"}\n";
}
int main(){try{
 for(auto state:{AlbumSessionState::empty,AlbumSessionState::loading,AlbumSessionState::ready,AlbumSessionState::error})for(bool flag:{false,true})for(unsigned queue:{0U,1U,2U})for(unsigned n:{0U,1U,12U}){
  auto s=session(n);s->state=state;s->queueExternal=flag;s->playlistLeaseValid=true;s->ownedPlaylistValid=true;s->transportOrderLease=true;s->outsideSession=n==0;compare(s,queue);s->sessionId=0;compare(s,queue);
 }
 compare({},0);compare({},2);
 for(bool flag:{false,true})for(unsigned queue:{0U,1U,2U}){cancellation<Baseline>(flag,queue);cancellation<Candidate>(flag,queue);}
 // Queue transitions interleaved with authoritative publications retain the
 // newest composition/playing identity; the guard is per immutable snapshot.
 Candidate c;Candidate::active=&c;c.current=session(3);
 for(unsigned queue:{0U,2U,1U,0U,0U,3U}){auto replacement=std::make_shared<AlbumSessionSnapshot>(*c.current);++replacement->compositionRevision;replacement->currentInSession=2;replacement->actualPlayingIndex=2;c.publish(replacement);playlist_manager::get()->count=queue;c.onQueueChanged();require(c.current->compositionRevision==replacement->compositionRevision&&c.current->actualPlayingIndex==2&&c.current->queueExternal==(queue!=0),"latest authoritative composition survives interleaved queue update");}
 for(unsigned n:{12U,120U,1200U,12000U}){auto s=session(n);s->queueExternal=true;measure<Baseline>("baseline",s,0);measure<Candidate>("candidate",s,1);measure<Candidate>("candidate",s,2);measure<Baseline>("baseline",s,3);}
 std::cout<<"{\"result\":\"PASS\",\"checks\":"<<checks<<"}\n";
 }catch(const std::exception& e){countAllocations=false;std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
'''
cpp=cpp.replace('__STRUCTS__',structs).replace('__CANCEL__',cancel).replace('__METHODS__','\n'.join(methods)).replace('__OBSERVER__',observer).replace('__FIXTURE__',fixture)
(out/'probe.cpp').write_text(cpp)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
(out/'source.json').write_text(json.dumps(dict(baseline=dict(path=str(old),sha256=sha(old)),candidate=dict(path=str(new),sha256=sha(new)),headerSha256=sha(header),observerSha256=sha(observerSource),corePolicySha256=sha(repo/'src/core/src/album_session.cpp'),real=['onQueueChanged','onPlaybackStop including queued closure','snapshot structures','cancelContinuation','QueueObserver','core policy'],seams=['SDK queue count','snapshot publication sink','host callback scheduling','playTrack sink'],scope='queue metadata clone allocations only; not whole process CPU or native UI latency'),indent=2)+'\n')
(out/'build.cmd').write_text('@echo off\ncall "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\nif errorlevel 1 exit /b 1\ncd /d "'+str(out.resolve())+'"\ncl /nologo /std:c++20 /EHsc /MT /O2 /W4 /I "'+str(repo/'src/core/include')+'" probe.cpp "'+str(repo/'src/core/src/album_session.cpp')+'" /Fe:probe.exe\nexit /b %errorlevel%\n')
