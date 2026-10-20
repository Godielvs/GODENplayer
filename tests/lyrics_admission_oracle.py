"""Extract the production admission pipeline; run it with real LatestWorker,
local provider and lyrics parser. SDK metadata and main dispatch are controlled
seams; persistence/cache record their inputs instead of accessing SQLite.
Usage: python tests/lyrics_admission_oracle.py OUTPUT SOURCE baseline|candidate
"""
from pathlib import Path
import hashlib,json,sys
out=Path(sys.argv[1]);source=Path(sys.argv[2]);out.mkdir(parents=True,exist_ok=False)
text=source.read_text(encoding='utf-8-sig')
def block(signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth: depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
helpers=['std::uint64_t hashAppend(','std::uint64_t hashField(','std::string hex64(',
         'std::string metadataValue(','std::int64_t utcNowMs(', 'std::string negativeKey(']
methods=['LyricsRequestContext LyricsService::capture(', 'void LyricsService::beginRequest(',
         'bool LyricsService::isCurrent(', 'bool LyricsService::promoteResolvedRequest(',
         'void LyricsService::resolveMediaRevision(', 'void LyricsService::publishAdmission(',
         'void LyricsService::publishState(', 'void LyricsService::runRequest(',
         'void LyricsService::onStop(', 'void LyricsService::stop()']
candidate='LyricsProviderResult LyricsService::lookupLocal(' in text
assert sys.argv[3] in ('baseline','candidate')
assert candidate==(sys.argv[3]=='candidate'), 'Requested oracle mode does not match production source'
if candidate:methods+=['LyricsProviderResult LyricsService::lookupLocal(']
cpp=r'''
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>
#include "latest_worker.h"
#include "lyrics_local_provider.h"
constexpr bool candidate=__CANDIDATE__;
const auto mainId=std::this_thread::get_id();
unsigned checks=0;
void require(bool b,const char* m){++checks;if(!b)throw std::runtime_error(m);}
namespace core_api {bool is_main_thread(){return std::this_thread::get_id()==mainId;}}
namespace fb2k {
std::mutex mutex;std::condition_variable cv;std::deque<std::function<void()>> queue;
void inMainThread(std::function<void()> f){std::lock_guard lock(mutex);queue.push_back(std::move(f));cv.notify_all();}
void wait(){std::unique_lock lock(mutex);if(!cv.wait_for(lock,std::chrono::seconds(5),[]{return !queue.empty();}))throw std::runtime_error("main callback timeout");}
bool pump(){std::function<void()> f;{std::lock_guard lock(mutex);if(queue.empty())return false;f=std::move(queue.front());queue.pop_front();}f();return true;}
void drain(){while(pump()){};}
}
using t_size=std::size_t;
template<class T>struct Ptr{
 std::shared_ptr<T> value;Ptr()=default;Ptr(std::shared_ptr<T> p):value(std::move(p)){}
 bool is_valid()const{return bool(value);}T* operator->()const{return value.get();}void release(){value.reset();}
};
struct Info{
 std::map<std::string,std::vector<std::string>> fields;
 mutable std::atomic<unsigned> mainReads{0},workerReads{0};bool throwOnRead=false;
 t_size meta_get_count_by_name(const char* n)const{
  (core_api::is_main_thread()?mainReads:workerReads).fetch_add(1);
  if(throwOnRead)throw std::runtime_error("metadata access failure");
  auto i=fields.find(n);return i==fields.end()?0:i->second.size();
 }
 const char* meta_get(const char* n,t_size i)const{return fields.at(n)[i].c_str();}
};
struct metadb_info_container{using ptr=Ptr<metadb_info_container>;Info data;const Info& info()const{return data;}};
struct Track{
 metadb_info_container::ptr container;std::string path,title="Synthetic title",artist="Synthetic artist",album="Synthetic album";
 bool throwInfo=false;unsigned subsong=0;
 auto get_info_ref()const{if(throwInfo)throw std::runtime_error("no info");return container;}
 double get_length()const{return 180.;}unsigned get_subsong_index()const{return subsong;}const char* get_path()const{return path.c_str();}
};
using metadb_handle_ptr=Ptr<Track>;
namespace nekorox::component {
__HELPERS__
std::string formatted(const metadb_handle_ptr& p,const char* e){if(!p.is_valid())return {};return std::string(e).find("title")!=std::string::npos?p->title:std::string(e).find("artist")!=std::string::npos?p->artist:p->album;}
std::filesystem::path nativePath(const metadb_handle_ptr& p){return p.is_valid()?p->path:std::string{};}
std::filesystem::path profilePath(){return std::filesystem::current_path()/"profile";}
// The real persistent worker executes all tasks. Only admission failure is
// injectable, avoiding a thread-creation/resource-exhaustion dependency.
struct Worker{
 LatestWorker real;bool failNext=false;std::atomic<unsigned> entries{0};
 bool submit(LatestWorker::Task f){if(failNext){failNext=false;return false;}return real.submit([this,f=std::move(f)](std::stop_token s){++entries;f(s);});}
 void cancelPending(){real.cancelPending();}void shutdownAndDrain(){real.shutdownAndDrain();}
};
struct Remote{LyricsProviderResult lookup(const LyricsRequestContext&,std::stop_token){throw std::runtime_error("unexpected online request");}};
class LyricsService{
public:
 struct NegativeEntry{LyricsState state;std::chrono::steady_clock::time_point expires;unsigned validMisses;};
 std::atomic<bool> started_{true};std::atomic<std::uint64_t> generation_{0},cacheSatisfiedGeneration_{0};
 std::atomic<std::shared_ptr<const LyricsSnapshot>> snapshot_{std::make_shared<LyricsSnapshot>()};
 Worker worker_;LyricsLocalProvider local_;Remote remote_;std::mutex negativeMutex_;std::map<std::string,NegativeEntry> negative_;
 std::atomic<unsigned> cacheCalls{0},persistCalls{0},bodyLeaks{0};std::vector<LyricsCandidateV2> stored;
 std::mutex storedMutex;
 ~LyricsService(){stop();}
 LyricsRequestContext capture(const metadb_handle_ptr&,std::uint64_t)const;
 void beginRequest(const metadb_handle_ptr&,LyricsRequestReason);
 bool isCurrent(const RequestToken&)const noexcept;
 bool promoteResolvedRequest(const LyricsRequestContext&);
 void resolveMediaRevision(LyricsRequestContext,__INFO__std::stop_token);
 void runRequest(LyricsRequestContext,__INFO__std::stop_token);
 LyricsProviderResult lookupLocal(const LyricsRequestContext&,metadb_info_container::ptr,std::stop_token);
 void publishAdmission(const LyricsRequestContext&,LyricsState);
 void publishState(const LyricsRequestContext&,LyricsState,std::shared_ptr<const LyricsDocumentV2> ={},bool=false,LyricsSourceRank=LyricsSourceRank::none,std::uint64_t=0);
 void onStop()noexcept;void stop()noexcept;
 bool onlineEnabled()const{return false;}bool offlineMode()const{return true;}
 auto snapshot()const{return snapshot_.load();}
 void publish(LyricsSnapshot s){require(core_api::is_main_thread(),"publication main affinity");if(s.request.generation==generation_)snapshot_.store(std::make_shared<LyricsSnapshot>(std::move(s)));}
 void inspect(const LyricsRequestContext& r){if(!r.embeddedPlain.empty()||!r.embeddedSynced.empty())++bodyLeaks;}
 void requestCached(const LyricsRequestContext& r){inspect(r);++cacheCalls;}
 std::string rememberedRejection(const LyricsRequestContext& r,std::string_view){inspect(r);return {};}
 void persist(const LyricsRequestContext& r,const LyricsCandidateV2& c,const LyricsDocumentV2&){inspect(r);std::lock_guard lock(storedMutex);stored.push_back(c);++persistCalls;}
};
__METHODS__
}
using namespace nekorox;using namespace nekorox::component;
metadb_handle_ptr track(std::string name,std::string synced,std::string plain){
 auto t=std::make_shared<Track>();t->path=(std::filesystem::current_path()/(name+".flac")).string();
 std::ofstream(t->path)<<"not audio: stat-only oracle fixture";
 auto info=std::make_shared<metadb_info_container>();
 if(!synced.empty())info->data.fields["SYNCEDLYRICS"]={synced};
 if(!plain.empty())info->data.fields["UNSYNCEDLYRICS"]={plain};
 t->container=info;return t;
}
void admit(LyricsService& s,const metadb_handle_ptr& t){s.beginRequest(t,LyricsRequestReason::newTrack);}
void until(const std::function<bool()>& done){auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(!done()){fb2k::pump();if(std::chrono::steady_clock::now()>end)throw std::runtime_error("state timeout");std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
void ready(LyricsService& s,std::string text){until([&]{return s.snapshot()->state==LyricsState::ready&&s.persistCalls>0;});require(s.snapshot()->document&&s.snapshot()->document->plainTextUtf8==text,"exact parsed text");}
void finish(LyricsService& s){s.stop();fb2k::drain();require(!candidate||s.bodyLeaks==0,"no embedded bodies in cache/persist/rejection callbacks");require(s.worker_.entries>0,"real worker ran");}
int main(){try{
 std::filesystem::create_directories("profile");
 // Admission uses the immutable old snapshot even if the host updates its
 // handle before revision resolution returns to the main thread.
 {LyricsService s;auto a=track("snapshot","[00:00.00]Old A\n[00:01.00]Second", "plain decoy");auto held=a->container;
  admit(s,a);fb2k::wait();require(held->data.mainReads==(candidate?0U:2U),"admission reads policy");
  auto b=track("unused","[00:00.00]New B\n[00:01.00]Second","");a->container=b->container;
  ready(s,"Old A\nSecond");require(!candidate||held->data.workerReads>0,"worker extracts snapshot");
  s.persistCalls=0;admit(s,a);ready(s,"New B\nSecond");finish(s);}
 // All aliases, empty values, synced-over-plain-over-sidecar priority and
 // static conversion of plain-tag LRC use the real local provider/parser.
 for(const char* alias:{"SYNCEDLYRICS","SYNCED LYRICS","SYNCED LYRICS UTF8","UNSYNCEDLYRICS","UNSYNCED LYRICS","LYRICS"}){
  LyricsService s;auto a=track(std::string("alias-")+alias,"","");
  a->container->data.fields[alias]={"","[00:00.00]Alias first\n[00:01.00]Alias second"};
  bool sync=std::string(alias).rfind("SYNCED",0)==0;
  if(sync)a->container->data.fields["LYRICS"]={"plain decoy"};
  auto sidecar=std::filesystem::path(a->path);sidecar.replace_extension(".lrc");std::ofstream(sidecar)<<"[00:00.00]Sidecar decoy";
  admit(s,a);ready(s,"Alias first\nAlias second");require(s.snapshot()->timingUsable==sync,"sync/plain timing policy");
  require(s.stored.front().source.recordId==(sync?"embedded-synced":"embedded-plain"),"embedded source priority");finish(s);
 }
 // Missing/throwing primary info retains sidecar fallback.
 for(unsigned mode=0;mode<3;++mode){LyricsService s;auto a=track("fallback"+std::to_string(mode),"","");
  if(mode==0)a->container.release();if(mode==1)a->throwInfo=true;if(mode==2)a->container->data.throwOnRead=true;
  auto sidecar=std::filesystem::path(a->path);sidecar.replace_extension(".lrc");std::ofstream(sidecar)<<"[00:00.00]Fallback\n[00:01.00]Second";
  admit(s,a);ready(s,"Fallback\nSecond");require(s.stored.front().sourceDisplayName=="Sidecar","fallback source");finish(s);}
 // A resolved callback queued before a newer request, stop or shutdown may
 // neither start local extraction nor publish. Real worker, delayed main queue.
 for(unsigned mode=0;mode<4;++mode){LyricsService s;auto a=track("staleA","[00:00.00]Stale A","");auto old=a->container;
  admit(s,a);fb2k::wait();auto b=track("currentB","[00:00.00]Current B","");
  if(mode==0)admit(s,b);
  if(mode==1){a->container=b->container;s.beginRequest(a,LyricsRequestReason::refresh);}
  if(mode==2)s.onStop();if(mode==3)s.stop();
  fb2k::pump();require(s.cacheCalls==0,"stale resolved callback suppressed");
  require(!candidate||old->data.workerReads==0,"stale snapshot not extracted");
  if(mode<2)ready(s,"Current B");else require(s.snapshot()->state!=LyricsState::ready,"stop/shutdown no stale ready");
  finish(s);
 }
 // Both submit failure points retain complete identity for failed publication.
 {LyricsService s;auto a=track("fail-admit","[00:00.00]A","");s.worker_.failNext=true;admit(s,a);
  require(s.snapshot()->state==LyricsState::failed,"first submit failure state");require(!s.snapshot()->request.recording.entityId.empty(),"first submit failure identity");s.stop();fb2k::drain();}
 {LyricsService s;auto a=track("fail-local","[00:00.00]A","");admit(s,a);fb2k::wait();s.worker_.failNext=true;fb2k::pump();
  require(s.snapshot()->state==LyricsState::failed,"second submit failure state");require(s.snapshot()->request.recording.mediaRevision!="pending-v1","failure resolved identity");finish(s);}
 // A cancelled or stale request cannot copy its metadata. Run helper with a
 // real separate thread and an explicitly cancelled token before entry.
 if constexpr(candidate){LyricsService s;auto a=track("cancel","[00:00.00]Cancel","");
  auto r=s.capture(a,1);s.generation_=1;s.publishAdmission(r,LyricsState::loadingLocal);
  std::stop_source cancelled;cancelled.request_stop();LyricsProviderResult result;
  std::thread worker([&]{result=s.lookupLocal(r,a->container,cancelled.get_token());});worker.join();
  require(result.outcome==LyricsProviderOutcome::cancelled&&a->container->data.workerReads==0,"cancel before metadata copy");
  s.onStop();std::thread stale([&]{result=s.lookupLocal(r,a->container,{});});stale.join();
  require(result.outcome==LyricsProviderOutcome::cancelled&&a->container->data.workerReads==0,"generation guard before copy");s.stop();}
 // Only production callback/worker ownership keeps the non-empty old snapshot
 // alive after the handle releases it; there is no strong observer reference.
 if constexpr(candidate){LyricsService s;auto a=track("owned-snapshot","[00:00.00]Owned A\n[00:01.00]Second","");
  std::weak_ptr<metadb_info_container> weak=a->container.value;admit(s,a);fb2k::wait();a->container.release();
  require(!weak.expired(),"queued callback owns admitted snapshot");ready(s,"Owned A\nSecond");
  until([&]{return weak.expired();});require(weak.expired(),"non-empty snapshot released after extraction");finish(s);}
 // Cancellation drops the last retained snapshot without extracting its text.
 if constexpr(candidate){LyricsService s;auto a=track("cancel-owner","[00:00.00]Cancelled","");
  std::weak_ptr<metadb_info_container> weak=a->container.value;admit(s,a);fb2k::wait();a->container.release();
  require(!weak.expired(),"queued cancellation owns snapshot");s.onStop();fb2k::pump();
  require(weak.expired()&&s.cacheCalls==0&&s.persistCalls==0,"cancel releases snapshot without publication");finish(s);}
 // Empty/missing lyrics releases the metadata snapshot before the existing
 // 800ms stabilization delay.
 if constexpr(candidate){LyricsService s;auto a=track("lifetime","","");std::weak_ptr<metadb_info_container> weak=a->container.value;
  admit(s,a);fb2k::wait();a->container.release();fb2k::pump();until([&]{return weak.expired();});
  require(s.snapshot()->state==LyricsState::loadingLocal,"metadata released before remote delay");s.onStop();finish(s);}
 std::cout<<"{\"mode\":\""<<(candidate?"candidate":"baseline")<<"\",\"checks\":"<<checks<<",\"result\":\"PASS\",\"realWorker\":true,\"realLocalProviderAndParser\":true}\n";
 }catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
'''
if not candidate:
    # A discarded non-template if still requires lookupLocal to link on v142.
    methods_text='\n'.join(block(s) for s in methods)+'\nLyricsProviderResult LyricsService::lookupLocal(const LyricsRequestContext& r,metadb_info_container::ptr,std::stop_token s){return local_.lookup(r,s);}'
else:methods_text='\n'.join(block(s) for s in methods)
cpp=cpp.replace('__CANDIDATE__',str(candidate).lower()).replace('__INFO__','metadb_info_container::ptr,' if candidate else '').replace('__HELPERS__','\n'.join(block(s) for s in helpers)).replace('__METHODS__',methods_text)
(out/'oracle.cpp').write_text(cpp)
(out/'source.json').write_text(json.dumps(dict(source=str(source),sha256=hashlib.sha256(source.read_bytes()).hexdigest(),methods=methods,helpers=helpers,real=['LatestWorker','LyricsLocalProvider','core lyrics selection/parser'],seams=['SDK immutable metadata container','controlled main-thread queue','cache/persist/rejection input recorder','worker submit failure injection','publish swaps snapshot without HWND invalidation']),indent=2)+'\n')
