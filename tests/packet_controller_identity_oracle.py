"""Compile actual PacketCost controller with gated worker IPC and DB callbacks.

Validates identity transport, deferred launch/resume and empty/error identities.
Actual std::thread, controller methods and callback closures are used. No native
child is launched; OS process waits and database admission are bounded seams.
"""
from pathlib import Path
import hashlib,json,re,sys
repo=Path(__file__).resolve().parents[1];out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=False)
source=Path(sys.argv[2]) if len(sys.argv)>2 else repo/'src/component/packet_cost_controller.cpp'
mode=sys.argv[3] if len(sys.argv)>3 else 'identity'
assert mode in ('identity','baseline','candidate')
header_path=repo/'src/component/packet_cost_controller.h';db_header=repo/'src/component/database_service.h'
text=source.read_text(encoding='utf-8-sig');header=header_path.read_text(encoding='utf-8-sig')
def block(s,signature):
 start=s.index(signature);end=s.index('{',start)+1;depth=1
 while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]
assert 'run->trackPath, run->sourceTrackId, std::move(result)' in text
methods=['onTrackChanged','loadCache','onCacheLoaded','startWorker','readerMain','cancelWorker','reapCompleted','beginDeepAnalysis','endDeepAnalysis','resumePending','onRunRetired','shutdownAndDrain']
definitions='\n\n'.join(block(text,'void PacketCostController::'+m+'(') for m in methods)
header=re.sub(r'^#(?:include|pragma).*\n','',header,flags=re.M).replace('private:','public:')
worker=block(text,'struct PacketCostController::WorkerRun')+';'
payload=block(db_header.read_text(encoding='utf-8-sig'),'struct PacketCostCachePayload')+';'
cpp=r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
unsigned checks=0,launches=0;
void require(bool ok,const char* label){++checks;if(!ok)throw std::runtime_error(label);}
std::mutex dispatchMutex;
std::deque<std::function<void()>> mainQueue;
namespace fb2k {void inMainThread(std::function<void()> f){std::lock_guard lock(dispatchMutex);mainQueue.push_back(std::move(f));}}
void flush(){std::deque<std::function<void()>> work;{std::lock_guard lock(dispatchMutex);work.swap(mainQueue);}for(auto& f:work)f();}
namespace nekorox::component {
struct PacketCostResult{bool supported=true;std::string error;};
__PAYLOAD__
using PacketCostCacheCallback=std::function<void(PacketCostCachePayload)>;
struct AnalysisWorkerHandles{int output=1,process=1;};
enum class AnalysisMessageKind{complete,error};
struct AnalysisFrameHeaderV1{AnalysisMessageKind kind=AnalysisMessageKind::complete;};
std::unordered_map<std::wstring,std::string> revisions;
std::string sourceRevision(const std::filesystem::path& p){auto it=revisions.find(p.wstring());return it==revisions.end()?std::string{}:it->second;}
void postPacketCostRefresh(){}
std::wstring utf8ToWide(const std::string& s){return {s.begin(),s.end()};}
struct Logger{template<class T>Logger& operator<<(const T&){return *this;}};
Logger FB2K_console_formatter(){return {};}
constexpr std::size_t kWorkerMemoryLimit=512ULL*1024*1024;
std::mutex gateMutex;std::condition_variable gateCv;bool readerEntered=false,readerReleased=false;
bool launchAnalysisWorker(const wchar_t*,std::uint64_t,const std::filesystem::path&,std::size_t,AnalysisWorkerHandles&,std::wstring&){++launches;std::lock_guard lock(gateMutex);readerEntered=false;readerReleased=false;return true;}
bool readAnalysisWorkerFrame(int,std::uint64_t,AnalysisFrameHeaderV1& h,std::vector<std::byte>&,std::string&){
 std::unique_lock lock(gateMutex);readerEntered=true;gateCv.notify_all();gateCv.wait(lock,[]{return readerReleased;});h.kind=AnalysisMessageKind::complete;return true;
}
bool deserializePacketCost(const std::vector<std::byte>&,PacketCostResult& result,std::string&){result.supported=true;return true;}
void WaitForSingleObject(int,int){}
void releaseReader(){std::lock_guard lock(gateMutex);readerReleased=true;gateCv.notify_all();}
void awaitReader(){std::unique_lock lock(gateMutex);require(gateCv.wait_for(lock,std::chrono::seconds(3),[]{return readerEntered;}),"Actual reader thread entered gated IPC");}
struct DatabaseService {
 struct Load{std::filesystem::path path;PacketCostCacheCallback callback;};
 struct Store{std::filesystem::path path;std::string expected;};
 std::deque<Load> loads;std::vector<Store> stores;std::mutex mutex;
 static DatabaseService& instance(){static DatabaseService d;return d;}
 void loadPacketCost(std::filesystem::path p,PacketCostCacheCallback c){loads.push_back({std::move(p),std::move(c)});}
 void storePacketCost(std::filesystem::path p,std::string expected,std::shared_ptr<PacketCostResult>){std::lock_guard lock(mutex);stores.push_back({std::move(p),std::move(expected)});}
 void deliver(PacketCostCachePayload p){require(!loads.empty(),"Database callback has actual admitted request");auto request=std::move(loads.front());loads.pop_front();require(request.path==p.trackPath,"Cache reply corresponds to requested path");request.callback(std::move(p));}
};
}
__HEADER__
namespace nekorox::component {
__WORKER__
void PacketCostController::finishRun(std::unique_ptr<WorkerRun> run,bool) noexcept {if(run){releaseReader();if(run->reader.joinable())run->reader.join();}}
PacketCostController::~PacketCostController(){shutdownAndDrain();}
__DEFINITIONS__
}
using namespace nekorox::component;
PacketCostCachePayload reply(const std::filesystem::path& p,std::string id,bool hit=false,std::string error={}){PacketCostCachePayload r;r.trackPath=p;r.sourceTrackId=std::move(id);r.found=hit;r.error=std::move(error);if(hit)r.cost=std::make_shared<PacketCostResult>();return r;}
void admit(PacketCostController& c,const std::filesystem::path& p){revisions[p.wstring()]="stable";c.onTrackChanged(p);require(c.state_.sourceTrackId.empty(),"New track begins without inherited source identity");}
void finish(PacketCostController& c,const std::string& expected){
 auto& db=DatabaseService::instance();require(bool(c.active_),"Worker actually installed");awaitReader();require(c.active_->sourceTrackId==expected,"Worker owns the requested identity");
 // State may receive another identity later; already-running work must retain
 // its original own string rather than consulting mutable controller state.
 {std::lock_guard lock(c.mutex_);c.state_.sourceTrackId="later-controller-state";}
 const auto before=db.stores.size();releaseReader();c.active_->reader.join();
 require(db.stores.size()==before+1&&db.stores.back().expected==expected,"Completion carries original identity into DB store API");
 flush();require(!c.active_&&c.state_.status==PacketCostStatus::ready,"Completion reaps run and retains volatile result");
}
int main(){try{
 auto p=std::filesystem::absolute("controller.fixture");std::ofstream(p)<<"fixture";auto& db=DatabaseService::instance();
 // Cache hits adopt provenance without launching the worker.
 {PacketCostController c;admit(c,p);auto before=launches;db.deliver(reply(p,"hit-id",true));require(c.state_.sourceTrackId=="hit-id"&&c.state_.status==PacketCostStatus::ready,"Hit adopts exact identity");require(launches==before,"Hit needs no worker");}
 // Miss -> start -> completion, with a real but controlled reader thread.
 {PacketCostController c;admit(c,p);db.deliver(reply(p,"miss-id"));finish(c,"miss-id");}
 // Cache corruption leaves a valid file identity for recomputation.
 {PacketCostController c;admit(c,p);db.deliver(reply(p,"repair-id",false,"corrupt layer"));finish(c,"repair-id");}
 // Database/source identity unavailable must clear old state and stay volatile.
 {PacketCostController c;admit(c,p);c.state_.sourceTrackId="must-be-cleared";db.deliver(reply(p,"",false,"database unavailable"));require(c.state_.sourceTrackId.empty(),"Failed identity clears old provenance");finish(c,"");}
 // Deep analysis defers cost, then refreshes identity through a NEW cache read.
 {PacketCostController c;admit(c,p);c.beginDeepAnalysis(99);auto before=launches;db.deliver(reply(p,"before-deep"));
  require(!c.active_&&c.deferredForDeepAnalysis_&&c.state_.sourceTrackId=="before-deep","Miss stores identity while deep analysis defers launch");
  c.endDeepAnalysis(100);require(db.loads.empty()&&launches==before,"Wrong deep request cannot resume");
  c.endDeepAnalysis(99);require(db.loads.size()==1,"Matching deep end admits one fresh cache lookup");c.endDeepAnalysis(99);require(db.loads.size()==1,"Repeated deep end does not duplicate lookup");
  db.deliver(reply(p,"after-deep"));finish(c,"after-deep");}
 // A retiring child postpones launch; resume uses the latest validated identity.
 {PacketCostController c;admit(c,p);c.retiring_=1;auto before=launches;db.deliver(reply(p,"before-retirement"));
  require(c.pendingStart_&&!c.active_&&launches==before,"Retiring run postpones launch");
  c.onCacheLoaded(c.generation_.load(),"stable",reply(p,"after-retirement"));c.retiring_=0;c.resumePending();require(!c.pendingStart_,"Pending start consumed once");finish(c,"after-retirement");}
 // Late response cannot replace current source identity, even with same path.
 {PacketCostController c;admit(c,p);auto request=std::move(db.loads.front());db.loads.pop_front();auto generation=c.generation_.load();c.generation_=generation+1;c.state_.sourceTrackId="current";auto before=launches;request.callback(reply(p,"stale",true));require(c.state_.sourceTrackId=="current"&&launches==before,"Stale generation does not adopt payload identity");}
 // Different path with a matching generation must likewise be rejected.
 {PacketCostController c;admit(c,p);db.loads.clear();c.state_.sourceTrackId="current";auto before=launches;c.onCacheLoaded(c.generation_.load(),"stable",reply({},"wrong-path",true));require(c.state_.sourceTrackId=="current"&&launches==before,"Path mismatch cannot replace identity");}
 __DEEP_CASES__
 require(db.loads.empty()&&mainQueue.empty(),"All owned work is drained");
 std::cout<<"{\"status\":\"PASS\",\"mode\":\"__MODE__\",\"checks\":"<<checks<<",\"realReaderThreads\":"<<launches<<",\"storeCalls\":"<<db.stores.size()<<"}\n";
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
'''
deep_cases=r'''
 constexpr bool fixed=__FIXED__;
 // A miss waits for the prior process to retire, then Deep starts. Deliver
 // the reaper wake during Deep; matching end must become the sole new owner.
 {PacketCostController c;admit(c,p);c.retiring_=1;auto before=launches;db.deliver(reply(p,"before-deep"));c.beginDeepAnalysis(99);
  c.onRunRetired();require(c.retiring_==0,"Actual retirement callback decrements the final counter");flush();require(!c.active_&&c.pendingStart_,"Deep blocks retirement wake without consuming pending");
  c.endDeepAnalysis(100);require(db.loads.empty()&&c.pendingStart_,"Wrong end cannot consume pending");
  c.endDeepAnalysis(99);
  if(fixed){require(!c.pendingStart_&&db.loads.size()==1&&launches==before,"Matching end requests fresh identity exactly once");c.endDeepAnalysis(99);require(db.loads.size()==1,"Repeated end cannot duplicate fresh lookup");db.deliver(reply(p,"fresh-after-deep"));finish(c,"fresh-after-deep");}
  else{require(c.pendingStart_&&db.loads.empty()&&!c.active_,"Baseline reproduces permanently lost wake");}}
 // End before retirement. Either the fresh response arrives first, or the
 // reaper callback arrives first. Neither ordering may launch old identity.
 for(bool responseFirst:{false,true}){
  PacketCostController c;admit(c,p);c.retiring_=1;auto before=launches;db.deliver(reply(p,"old-pending"));c.beginDeepAnalysis(99);c.endDeepAnalysis(99);
  if(fixed){require(!c.pendingStart_&&db.loads.size()==1,"Deep end transfers pending ownership into fresh cache lookup");
   if(responseFirst){db.deliver(reply(p,"fresh-pending"));require(c.pendingStart_&&launches==before,"Fresh reply may wait for retirement");c.onRunRetired();flush();}
   else{c.onRunRetired();flush();require(launches==before&&db.loads.size()==1,"Early reaper cannot launch stale tuple before fresh reply");db.deliver(reply(p,"fresh-pending"));}
   finish(c,"fresh-pending");}
  else{require(c.pendingStart_&&db.loads.empty(),"Baseline end leaves stale tuple without refreshing identity");c.onRunRetired();flush();finish(c,"old-pending");}}
 // Retirement is already complete but its main-thread wake is delayed until
 // after end. Pending must be cleared before the asynchronous DB call.
 {PacketCostController c;admit(c,p);c.retiring_=1;auto before=launches;db.deliver(reply(p,"late-reaper-old"));c.beginDeepAnalysis(99);c.onRunRetired();require(!mainQueue.empty(),"Actual reaper wake is queued but not yet delivered");c.endDeepAnalysis(99);flush();
  if(fixed){require(launches==before&&db.loads.size()==1&&!c.active_,"Delayed reaper cannot race fresh identity lookup");db.deliver(reply(p,"late-reaper-fresh"));finish(c,"late-reaper-fresh");}
  else{require(db.loads.empty()&&launches==before+1,"Baseline delayed reaper launches pre-Deep identity");finish(c,"late-reaper-old");}}
 // Loading without pending has an existing DB callback. Deep ending before
 // it replies must not create a second cache load or worker.
 {PacketCostController c;admit(c,p);c.beginDeepAnalysis(99);c.endDeepAnalysis(99);require(db.loads.size()==1&&!c.pendingStart_,"Loading alone does not duplicate in-flight cache lookup");db.deliver(reply(p,"in-flight-only"));finish(c,"in-flight-only");}
 // Old tuples can survive onTrackChanged; final cached B must stay final.
 auto p2=std::filesystem::absolute("controller-second.fixture");std::ofstream(p2)<<"other";
 for(bool supported:{false,true}){
  PacketCostController c;admit(c,p);c.retiring_=1;db.deliver(reply(p,"previous-track"));admit(c,p2);auto cached=reply(p2,"final-B",true);auto result=std::make_shared<PacketCostResult>();result->supported=supported;cached.cost=std::move(result);db.deliver(std::move(cached));
  auto before=launches;c.beginDeepAnalysis(99);c.endDeepAnalysis(99);
  require(db.loads.empty()&&launches==before&&c.state_.sourceTrackId=="final-B","Final result is not recomputed at deep end");
  if(fixed)require(!c.pendingStart_&&c.pendingTrack_.empty()&&c.pendingSourceRevision_.empty()&&c.pendingGeneration_==0,"Even stale pending tuple is consumed atomically");
  c.retiring_=0;c.resumePending();require(launches==before&&c.state_.status==(supported?PacketCostStatus::ready:PacketCostStatus::unsupported),"Old reaper cannot replace final result");}
 // A pending tuple is eligible only if generation, path AND revision match.
 for(unsigned mismatch=0;mismatch<3;++mismatch){
  PacketCostController c;admit(c,p);c.pendingStart_=true;c.pendingGeneration_=c.generation_.load();c.pendingTrack_=p;c.pendingSourceRevision_="stable";
  if(mismatch==0)--c.pendingGeneration_;else if(mismatch==1)c.pendingTrack_=p2;else c.pendingSourceRevision_="old-revision";
  c.beginDeepAnalysis(99);c.endDeepAnalysis(99);require(db.loads.size()==1,"Stale tuple cannot create duplicate lookup");
  if(fixed)require(!c.pendingStart_,"Invalid tuple consumed without a new launch");
  db.deliver(reply(p,"valid-current"));finish(c,"valid-current");}
 // Latest Deep request owns wake-up; obsolete/repeated ends are inert.
 {PacketCostController c;admit(c,p);c.retiring_=1;db.deliver(reply(p,"pending"));c.beginDeepAnalysis(99);c.beginDeepAnalysis(100);c.endDeepAnalysis(99);
  require(c.pendingStart_&&db.loads.empty()&&c.deepAnalysisRequestId_==100,"Old Deep cannot release newest gate");c.endDeepAnalysis(100);
  if(fixed){require(db.loads.size()==1&&!c.pendingStart_,"Newest end transfers pending once");c.retiring_=0;db.deliver(reply(p,"newest"));finish(c,"newest");}
  else{require(c.pendingStart_&&db.loads.empty(),"Baseline newest end also loses pending wake");c.retiring_=0;}}
 // Shutdown clears the gate and tuple. A late matching end cannot resurrect it.
 {PacketCostController c;admit(c,p);c.retiring_=1;db.deliver(reply(p,"pending"));c.beginDeepAnalysis(99);auto before=launches;c.shutdownAndDrain();c.onRunRetired();require(mainQueue.empty(),"Retirement after shutdown does not queue a wake");c.endDeepAnalysis(99);c.resumePending();
  require(db.loads.empty()&&launches==before&&!c.active_&&!c.pendingStart_&&c.state_.trackPath.empty(),"Late end after shutdown does not restart analysis");}
'''
deep_cases=deep_cases.replace('__FIXED__','true' if mode=='candidate' else 'false') if mode!='identity' else ''
for k,v in dict(PAYLOAD=payload,HEADER=header,WORKER=worker,DEFINITIONS=definitions,DEEP_CASES=deep_cases,MODE=mode).items():cpp=cpp.replace('__'+k+'__',v)
(out/'oracle.cpp').write_text(cpp,encoding='utf-8')
(out/'source.json').write_text(json.dumps(dict(mode=mode,sourceSha256=hashlib.sha256(source.read_bytes()).hexdigest(),headerSha256=hashlib.sha256(header_path.read_bytes()).hexdigest(),dbHeaderSha256=hashlib.sha256(db_header.read_bytes()).hexdigest(),methods=methods,
 seams=['Real std::thread and all listed controller methods verbatim','Child-process launch/IPC/wait and DB callbacks are controlled seams','Cleanup releases IPC and joins actual thread, not a real native child','Store records transmitted identity; DB admission rejection separately covered by packet_cache_identity_oracle']),indent=2)+'\n')
print(out/'oracle.cpp')
