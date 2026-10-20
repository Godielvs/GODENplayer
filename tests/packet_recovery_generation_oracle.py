"""Exercise actual PacketCost reader completion and controller transitions.

The main-thread dispatcher is manually delayed. Worker IPC/deserialization and
filesystem revisions are controlled seams; production ownership, cancellation,
generation checks and onTrackChanged transitions are compiled verbatim.
"""
from pathlib import Path
import hashlib, json, re, sys

repo = Path(__file__).resolve().parents[1]
out, source, mode = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
assert mode in ('baseline', 'candidate')
out.mkdir(parents=True, exist_ok=False)
text = source.read_text(encoding='utf-8-sig')
header_path = repo / 'src/component/packet_cost_controller.h'
header = header_path.read_text(encoding='utf-8-sig')

def block(s, signature):
    begin = s.index(signature)
    end = s.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[begin:end]

methods = ['onTrackChanged', 'shutdownAndDrain', 'readyResultForTrack',
           'cancelWorker', 'reapCompleted', 'readerMain']
signatures = {
    'readyResultForTrack': 'std::shared_ptr<const PacketCostResult> PacketCostController::readyResultForTrack(',
}
definitions = '\n\n'.join(block(text, signatures.get(n, 'void PacketCostController::'+n+'(')) for n in methods)
reader = block(text, 'void PacketCostController::readerMain(')
assert ('const auto generation = run->generation;' in reader) == (mode == 'candidate')
assert ('generation != generation_.load(std::memory_order_relaxed)' in reader[reader.index('fb2k::inMainThread'):]) == (mode == 'candidate')
header = re.sub(r'^#(?:include|pragma).*\n', '', header, flags=re.M).replace('private:', 'public:')
worker = block(text, 'struct PacketCostController::WorkerRun') + ';'
cpp = r'''
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
constexpr bool candidate = __CANDIDATE__;
std::uint64_t checks=0,refreshes=0,loads=0,stores=0,retirements=0,cancellations=0;
void check(bool ok,const char* label){++checks;if(!ok){std::cerr<<label<<'\n';std::exit(1);}}
std::deque<std::function<void()>> mainQueue;
namespace fb2k {void inMainThread(std::function<void()> fn){mainQueue.push_back(std::move(fn));}}
namespace nekorox::component {
struct PacketCostResult { bool supported=true; std::string error; unsigned sentinel=0; };
struct AnalysisWorkerHandles {int output=1,process=1;};
enum class AnalysisMessageKind {complete,error};
struct AnalysisFrameHeaderV1 {AnalysisMessageKind kind=AnalysisMessageKind::complete;};
std::unordered_map<std::wstring,std::string> revisions;
std::string sourceRevision(const std::filesystem::path& path){auto it=revisions.find(path.wstring());return it==revisions.end()?std::string{}:it->second;}
void postPacketCostRefresh(){++refreshes;}
std::wstring utf8ToWide(const std::string& value){return std::wstring(value.begin(),value.end());}
void WaitForSingleObject(int,int){}
bool parseSupported=true,parseValid=true;
bool readAnalysisWorkerFrame(int,std::uint64_t,AnalysisFrameHeaderV1& h,std::vector<std::byte>&,std::string&){h.kind=AnalysisMessageKind::complete;return true;}
bool deserializePacketCost(const std::vector<std::byte>&,PacketCostResult& out,std::string& err){out.supported=parseSupported;out.sentinel=99;if(!parseSupported)out.error="unsupported";if(!parseValid)err="bad packet";return parseValid;}
struct DatabaseService {
 static DatabaseService& instance(){static DatabaseService db;return db;}
 void storePacketCost(const std::filesystem::path&,std::shared_ptr<PacketCostResult>){++stores;}
 void storePacketCost(const std::filesystem::path&,const std::string&,std::shared_ptr<PacketCostResult>){++stores;}
};
}
__HEADER__
namespace nekorox::component {
__WORKER__
// Cleanup is synchronous in this seam; no actual child process is launched.
// The real cancelWorker/reapCompleted decide precisely which run may retire.
void PacketCostController::finishRun(std::unique_ptr<WorkerRun> run,bool) noexcept {if(run){++retirements;if(run->cancelled.load())++cancellations;}}
void PacketCostController::loadCache(std::uint64_t,const std::filesystem::path&,std::string){++loads;}
PacketCostController::~PacketCostController(){shutdownAndDrain();}
__DEFINITIONS__
}
using namespace nekorox::component;
void flush(){check(mainQueue.size()==1,"Exactly one completion queued");auto fn=std::move(mainQueue.front());mainQueue.pop_front();fn();check(mainQueue.empty(),"Dispatch does not enqueue an unbounded retry");}
void installRun(PacketCostController& c,const std::filesystem::path& path,std::uint64_t id){
 c.state_.trackPath=path;c.state_.sourceRevision=sourceRevision(path);c.state_.status=PacketCostStatus::running;c.state_.requestId=id;
 c.active_=std::make_unique<PacketCostController::WorkerRun>();c.active_->owner=&c;c.active_->requestId=id;c.active_->generation=c.generation_.load();c.active_->trackPath=path;c.active_->sourceRevision=c.state_.sourceRevision;
}
void queueChanged(PacketCostController& c,const std::filesystem::path& path){
 c.generation_.store(7);revisions[path.wstring()]="r1";installRun(c,path,10);revisions[path.wstring()]="r2";
 auto stored=stores;c.readerMain(c.active_.get());check(mainQueue.size()==1,"Reader posts completion instead of invoking UI inline");check(stores==stored,"Changed revision is not persisted");check(!c.state_.result,"Changed revision is not published");
}
int main(){
 auto a=std::filesystem::absolute("packet-a.fixture"),b=std::filesystem::absolute("packet-b.fixture");
 {std::ofstream(a)<<"a";std::ofstream(b)<<"b";}revisions[b.wstring()]="b1";
 // Same request still owns A: changed file must actually be retried.
 {PacketCostController c;queueChanged(c,a);auto before=loads,retired=retirements;flush();
  check(c.state_.trackPath==a&&c.state_.sourceRevision=="r2","Current request adopts updated revision");
  check(c.generation_.load()==8&&loads==before+1,"Current request retries exactly once");
  check(!c.active_&&retirements==retired+1,"Completed original worker reaped once");}
 // Ordinary playback B is selected while A's callback waits in the UI queue.
 {PacketCostController c;queueChanged(c,a);c.onTrackChanged(b);installRun(c,b,20);auto before=loads,retired=retirements,gen=c.generation_.load();flush();
  check(c.state_.trackPath==(candidate?b:a),"Discriminates old callback overwriting next track");
  check(bool(c.active_)==candidate,"Candidate retains newer worker, baseline cancels it");
  check(loads==before+(candidate?0:1)&&retirements==retired+(candidate?0:1),"Stale callback cannot create replacement work");
  check(c.generation_.load()==gen+(candidate?0:1),"No generation churn from old completion");}
 // Ready B result must also survive, even if there is no newer worker to guard.
 {PacketCostController c;queueChanged(c,a);c.onTrackChanged(b);auto result=std::make_shared<PacketCostResult>();result->sentinel=123;c.state_.status=PacketCostStatus::ready;c.state_.result=result;flush();
  check((c.readyResultForTrack(b)==result)==candidate,"Stale A completion cannot erase ready B result");}
 // A -> B -> A: path equality alone cannot distinguish generations.
 {PacketCostController c;queueChanged(c,a);c.onTrackChanged(b);c.onTrackChanged(a);installRun(c,a,30);auto gen=c.generation_.load();revisions[a.wstring()]="r3";auto before=loads;flush();
  check(c.state_.sourceRevision==(candidate?"r2":"r3"),"ABA callback belongs to old A generation");
  check(bool(c.active_)==candidate&&c.generation_.load()==gen+(candidate?0:1),"New A worker survives old A completion");
  check(loads==before+(candidate?0:1),"ABA does not cause duplicate cache load");}
 // Playback stop invalidates A even though the controller is still accepting.
 {PacketCostController c;queueChanged(c,a);c.onTrackChanged({});auto before=loads;flush();
  check(c.state_.trackPath.empty()==candidate,"Stopping playback cannot resurrect old analysis");
  check(loads==before+(candidate?0:1),"Stop suppresses stale reload");}
 // Shutdown must not resurrect work; both old and fixed code already stop it.
 {PacketCostController c;queueChanged(c,a);c.shutdownAndDrain();auto before=loads;flush();
  check(!c.accepting_.load()&&c.state_.trackPath.empty()&&!c.active_,"Shutdown remains drained");check(loads==before,"No reload after shutdown");}
 // Normal supported/unsupported and malformed result completions retain their
 // existing publication/cleanup paths, which are not source-change recovery.
 for(unsigned variant=0;variant<3;++variant){PacketCostController c;revisions[a.wstring()]="stable";installRun(c,a,40+variant);parseSupported=variant!=1;parseValid=variant!=2;
  auto before=loads,stored=stores;c.readerMain(c.active_.get());flush();
  check(!c.active_&&loads==before,"Normal completion reaps without recovery");
  check(stores==stored+(variant==2?0:1),"Only valid stable revision results persisted");
  check(c.state_.status==(variant==0?PacketCostStatus::ready:variant==1?PacketCostStatus::unsupported:PacketCostStatus::error),"Normal final status preserved");}
 parseSupported=parseValid=true;
 // Cancellation before reader completion does not enqueue another callback.
 {PacketCostController c;revisions[a.wstring()]="stable";installRun(c,a,50);c.active_->cancelled.store(true);c.readerMain(c.active_.get());check(mainQueue.empty(),"Cancelled run never posts delayed recovery");}
 std::cout<<"{\"status\":\"PASS\",\"mode\":\""<<(candidate?"candidate":"baseline")<<"\",\"checks\":"<<checks<<",\"baselineDefectsExpected\":"<<(candidate?"false":"true")<<",\"loads\":"<<loads<<",\"retirements\":"<<retirements<<",\"cancellations\":"<<cancellations<<"}\n";
}
'''
for key, value in dict(CANDIDATE='true' if mode=='candidate' else 'false',
                       HEADER=header, WORKER=worker, DEFINITIONS=definitions).items():
    cpp = cpp.replace('__'+key+'__', value)
(out/'oracle.cpp').write_text(cpp, encoding='utf-8')
(out/'source.json').write_text(json.dumps(dict(mode=mode,
    sourceSha256=hashlib.sha256(source.read_bytes()).hexdigest(),
    headerSha256=hashlib.sha256(header_path.read_bytes()).hexdigest(),
    methods=methods, definitionsSha256=hashlib.sha256(definitions.encode()).hexdigest(),
    seams=['Main dispatcher delayed manually; transitions and reader callback verbatim',
           'Worker IPC/deserialization, revision lookup and process wait controlled; no real child or files changed during analysis',
           'Cleanup counts ownership decisions; actual onTrackChanged, cancelWorker, reapCompleted, shutdownAndDrain compiled',
           'Baseline intentionally reproduces stale next-track/ready-result/ABA/stop defects; not an accepted production outcome']), indent=2)+'\n')
print(out/'oracle.cpp')
