"""Exercise production PacketCost admission/DB branches with real SQLite and IDs.

SDK types and callback dispatch are seams. File identity, fingerprints, packet
serialization, compressed SQLite layers and production admission/store decisions
are real. The test owns queue delivery so it can edit a file between both stages.
"""
from pathlib import Path
import hashlib,json,re,sys
repo=Path(__file__).resolve().parents[1]
out,source_root,mode=Path(sys.argv[1]),Path(sys.argv[2]),sys.argv[3]
assert mode in ('baseline','candidate')
out.mkdir(parents=True,exist_ok=False)
source=source_root/'src/component/database_service.cpp'
header_path=source_root/'src/component/database_service.h'
packet_header=source_root/'src/core/include/nekorox/packet_cost.h'
text=source.read_text(encoding='utf-8-sig')
header=header_path.read_text(encoding='utf-8-sig')
def block(s,signature):
 start=s.index(signature);end=s.index('{',start)+1;depth=1
 while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]
assert ('expectedPacketCostTrackId' in text)==(mode=='candidate')
definitions='\n'.join(block(text,s) for s in (
 'bool DatabaseService::enqueue(', 'void DatabaseService::loadPacketCost(',
 'void DatabaseService::storePacketCost('))
helpers='\n'.join(block(text,s) for s in (
 'std::int64_t utcNowMs()', 'std::int64_t secondsToMs(',
 'std::uint64_t fileTimeTicks(', 'bool identifyLocalTrack('))
text_util=repo/'src/component/text_util.cpp'
helpers=block(text_util.read_text(encoding='utf-8-sig'),'std::string wideToUtf8(')+'\n'+helpers
analysis=block(text,'        if (pending.analysis) {')
prefix=analysis[:analysis.index('            if (isPacketCost) {')]
packet=block(analysis,'            if (isPacketCost) {')
process='void DatabaseService::qaProcess(PendingEvent pending, NekoDatabase& database) {\n'+prefix+packet+'\n}\n}\n'
header=header.replace('#include "sdk_compat.h"','class metadb_handle_ptr;\nnamespace play_control { enum t_stop_reason : int; }')
header=header.replace('private:', 'public: // Test access only.')
needle='    DatabaseService() = default;'
assert needle in header
header=header.replace(needle,needle+'\n    void qaProcess(PendingEvent pending, NekoDatabase& database);')
(out/'database_service_test.h').write_text(header,encoding='utf-8')
(out/'include/nekorox').mkdir(parents=True)
(out/'include/nekorox/packet_cost.h').write_bytes(packet_header.read_bytes())
cpp=r'''
#include "database_service_test.h"
#include "nekorox/track_identity.h"
#include "text_util.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
constexpr bool candidate=__CANDIDATE__;
unsigned checks=0,loadCompletions=0;
void require(bool ok,const char* label){++checks;if(!ok)throw std::runtime_error(label);}
namespace nekorox::component {
std::vector<PacketCostCachePayload> published;
__HELPERS__
__DEFINITIONS__
void DatabaseService::publishPacketCostResult(PacketCostCacheCallback callback,PacketCostCachePayload payload){
 if(callback)callback(std::move(payload));else published.push_back(std::move(payload));
}
__PROCESS__
}
using namespace nekorox;using namespace nekorox::component;
using Service=DatabaseService;
using Kind=Service::AnalysisTask::Kind;
Service::PendingEvent pop(Service& s){require(!s.queue_.empty(),"Queued job exists");auto p=std::move(s.queue_.front());s.queue_.pop_front();return p;}
void drain(Service& s,NekoDatabase& db){while(!s.queue_.empty())s.qaProcess(pop(s),db);}
void store(Service& s,const std::filesystem::path& p,const std::string& id,std::shared_ptr<const PacketCostResult> result){__STORE_CALL__}
PacketCostCachePayload load(Service& s,NekoDatabase& db,const std::filesystem::path& p){
 PacketCostCachePayload result;bool done=false;
 s.loadPacketCost(p,[&](auto payload){result=std::move(payload);done=true;++loadCompletions;});
 drain(s,db);require(done,"Actual load completes");return result;
}
PersistedTrack identity(const std::filesystem::path& path){PersistedTrack result;std::string error;require(identifyLocalTrack(path,result,error),error.c_str());return result;}
std::filesystem::path file(unsigned number){auto p=std::filesystem::absolute("source-"+std::to_string(number)+".bin");std::ofstream f(p,std::ios::binary);std::vector<char> bytes(1024*1024,'A');f.write(bytes.data(),bytes.size());f.close();return p;}
void mutate(const std::filesystem::path& p,std::uint64_t at,bool preserve=true){
 const auto stamp=std::filesystem::last_write_time(p);const auto size=std::filesystem::file_size(p);
 {std::fstream f(p,std::ios::binary|std::ios::in|std::ios::out);f.seekp(at);f.put('B');require(bool(f),"Fixture byte changed");}
 if(preserve){std::filesystem::last_write_time(p,stamp);require(std::filesystem::file_size(p)==size&&std::filesystem::last_write_time(p)==stamp,"Replacement preserves exact size and mtime");}
}
std::shared_ptr<const PacketCostResult> cost(unsigned bytes){
 PacketParseResult parsed;parsed.codec=PacketCodec::flac;parsed.sampleRate=48000;parsed.durationSeconds=2;
 parsed.packets={{0,bytes,0,1},{bytes,bytes*2,1,1}};
 auto result=std::make_shared<PacketCostResult>(makePacketCost(parsed));require(result->supported,"Valid synthetic packet-cost result");return result;
}
std::vector<std::byte> encoded(const PacketCostResult& c){std::vector<std::byte> bytes;std::string error;require(serializePacketCost(c,bytes,error),error.c_str());return bytes;}
PersistedAnalysisBundle read(NekoDatabase& db,const std::string& id,std::string version=std::string(kPacketCostAnalyzerVersion)){
 PersistedAnalysisBundle result;std::string error;require(db.readAnalysisBundle(id,version,result,error),error.c_str());return result;
}
void seed(NekoDatabase& db,const PersistedTrack& track,const PacketCostResult& c,const std::string& version,bool corrupt=false){
 PersistedAnalysisBundle bundle;bundle.trackId=track.trackId;bundle.analyzerVersion=version;bundle.manifestJson="{\"test\":\"sentinel\"}";bundle.layers.push_back({"packet-cost",100,corrupt?std::vector<std::byte>{std::byte{1}}:encoded(c)});
 std::string error;require(db.storeAnalysisBundle(track,bundle,kAnalysisCacheLimitBytes,error),error.c_str());
}
void sameLayer(const PersistedAnalysisBundle& bundle,const PacketCostResult& expected){require(bundle.found&&bundle.layers.size()==1,"Exactly one persisted layer");require(bundle.layers[0].data==encoded(expected),"Exact serialized result preserved");}
int main(){try{
 static_assert(kPacketCostBinaryVersion==1,"Worker protocol must remain compatible");
 require(std::string(kPacketCostAnalyzerVersion)==(candidate?"packet-cost-2.2.0":"packet-cost-2.1.0"),"Expected cache namespace");
 Service s;s.started_=true;NekoDatabase db;std::string error;require(db.open("oracle.db",error),error.c_str());
 auto a=cost(10000),b=cost(40000);
 // Miss -> store -> hit exercises the actual API, identity producer and branch.
 auto stable=file(1);auto stableId=identity(stable);auto missing=load(s,db,stable);
 require(!missing.found&&missing.error.empty(),"Fresh cache miss");__CHECK_MISS_ID__
 store(s,stable,stableId.trackId,a);require(s.queue_.size()==1,"Stable store admitted");drain(s,db);
 auto hit=load(s,db,stable);require(hit.found&&hit.cost&&encoded(*hit.cost)==encoded(*a),"Stable cache hit retains all cost bytes");__CHECK_HIT_ID__
 // Preserve size/mtime but change each actual sampled fingerprint window.
 for(unsigned i=0;i<3;++i){
  auto p=file(10+i);auto before=identity(p);auto miss=load(s,db,p);__CHECK_CASE_ID__
  store(s,p,before.trackId,a);require(s.queue_.size()==1,"Old result waits in queue");
  const std::uint64_t offset=i==0?17:i==1?512*1024:1024*1024-17;mutate(p,offset);auto after=identity(p);
  require(before.fileSize==after.fileSize&&before.modifiedTicks==after.modifiedTicks&&before.trackId!=after.trackId,"Real partial fingerprint distinguishes replacement with identical stat");
  seed(db,after,*b,std::string(kPacketCostAnalyzerVersion));auto sentinel=read(db,after.trackId);drain(s,db);
  auto current=read(db,after.trackId);
  sameLayer(current,candidate?*b:*a);
  require((current.manifestJson==sentinel.manifestJson)==candidate,"Newer revision cache manifest cannot be overwritten by old result");
  require(!read(db,before.trackId).found,"Rejected old store does not fabricate a cache entry for old identity");
  require(published.back().error.empty()!=candidate,"Mismatch rejection produces diagnostic");
 }
 // An ordinary append must be rejected too (not only fingerprint-only edits).
 auto resized=file(20);auto old=identity(resized);store(s,resized,old.trackId,a);
 {std::ofstream f(resized,std::ios::app|std::ios::binary);f<<'C';}
 auto resizedId=identity(resized);drain(s,db);require(read(db,resizedId.trackId).found!=candidate,"Changed size does not acquire old analysis");
 // Empty identities cannot bypass public admission or the worker-side guard.
 auto empty=file(30);auto emptyId=identity(empty);store(s,empty,"",a);
 require(s.queue_.size()==(candidate?0:1),"No provenance means no queued cache write");drain(s,db);
 seed(db,emptyId,*b,std::string(kPacketCostAnalyzerVersion));
 Service::PendingEvent injected;injected.analysis=std::make_unique<Service::AnalysisTask>();injected.analysis->kind=Kind::storePacketCost;injected.analysis->trackPath=empty;injected.analysis->packetCost=a;
 s.qaProcess(std::move(injected),db);sameLayer(read(db,emptyId.trackId),candidate?*b:*a);
 store(s,empty,emptyId.trackId,{});require(s.queue_.empty(),"Null result rejected");
 s.started_=false;store(s,empty,emptyId.trackId,a);require(s.queue_.empty(),"Stopped public admission rejected");s.started_=true;
 // Unavailable source/DB returns no provenance and never writes a bundle.
 auto removed=file(40);auto removedId=identity(removed);store(s,removed,removedId.trackId,a);std::filesystem::remove(removed);drain(s,db);
 require(!read(db,removedId.trackId).found&&!published.back().error.empty(),"Missing source prevents store");auto absent=load(s,db,removed);require(!absent.found&&!absent.error.empty(),"Missing file load reports failure");__CHECK_ABSENT_ID__
 NekoDatabase closed;auto unavailable=load(s,closed,stable);require(!unavailable.found&&!unavailable.error.empty(),"Unavailable database load reports failure");__CHECK_UNAVAILABLE_ID__
 store(s,stable,stableId.trackId,a);drain(s,closed);require(!published.back().error.empty(),"Unavailable database rejects store");sameLayer(read(db,stableId.trackId),*a);
 // Corrupt cache removal must retain fresh source identity for recomputation.
 auto corrupt=file(50);auto corruptId=identity(corrupt);seed(db,corruptId,*a,std::string(kPacketCostAnalyzerVersion),true);
 auto failed=load(s,db,corrupt);require(!failed.found&&!failed.error.empty(),"Corrupt packet layer rejected");__CHECK_CORRUPT_ID__
 store(s,corrupt,corruptId.trackId,b);drain(s,db);sameLayer(read(db,corruptId.trackId),*b);
 // Prior namespace entries can be contaminated but remain in normal cache LRU.
 auto legacy=file(60);auto legacyId=identity(legacy);seed(db,legacyId,*a,"packet-cost-2.1.0");auto legacyHit=load(s,db,legacy);
 require(legacyHit.found!=candidate,"Candidate ignores entries without validated provenance");sameLayer(read(db,legacyId.trackId,"packet-cost-2.1.0"),*a);
 // IDs are partial fingerprints, not complete-file mutation detection. A byte
 // outside all three windows intentionally keeps the existing TrackID contract.
 auto unsampled=file(70);auto sampledId=identity(unsampled);mutate(unsampled,128*1024);auto unsampledId=identity(unsampled);
 require(sampledId.trackId==unsampledId.trackId,"Document unchanged partial-fingerprint detection boundary");
 db.close();
 std::cout<<"{\"status\":\"PASS\",\"mode\":\""<<(candidate?"candidate":"baseline")<<"\",\"checks\":"<<checks<<",\"loads\":"<<loadCompletions<<",\"sourceIdentity\":\"real makeTrackId partial fingerprint\",\"baselineContaminationExpected\":"<<(candidate?"false":"true")<<"}\n";
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
'''
substitutions=dict(CANDIDATE='true' if mode=='candidate' else 'false',HELPERS=helpers,DEFINITIONS=definitions,PROCESS=process,
 STORE_CALL='s.storePacketCost(p,id,std::move(result));' if mode=='candidate' else 's.storePacketCost(p,std::move(result));')
for key,expr in dict(MISS='missing.sourceTrackId==stableId.trackId',HIT='hit.sourceTrackId==stableId.trackId',CASE='miss.sourceTrackId==before.trackId',ABSENT='absent.sourceTrackId.empty()',UNAVAILABLE='unavailable.sourceTrackId.empty()',CORRUPT='failed.sourceTrackId==corruptId.trackId').items():
 substitutions['CHECK_'+key+'_ID']='require('+expr+',"'+key+' provenance preserved");' if mode=='candidate' else ''
for k,v in substitutions.items():cpp=cpp.replace('__'+k+'__',v)
assert not re.search(r'__[A-Z_]+__',cpp)
(out/'oracle.cpp').write_text(cpp,encoding='utf-8')
(out/'source.json').write_text(json.dumps(dict(mode=mode,sourceSha256=hashlib.sha256(source.read_bytes()).hexdigest(),headerSha256=hashlib.sha256(header_path.read_bytes()).hexdigest(),packetHeaderSha256=hashlib.sha256(packet_header.read_bytes()).hexdigest(),
 definitionsSha256=hashlib.sha256(definitions.encode()).hexdigest(),processSha256=hashlib.sha256(process.encode()).hexdigest(),textUtilSha256=hashlib.sha256(text_util.read_bytes()).hexdigest(),
 real=['identifyLocalTrack','makeTrackId','PacketCost serialization','NekoDatabase SQLite/compression','enqueue/load/store methods and packet worker branch'],
 seams=['SDK declarations removed; dispatcher calls callback immediately','Queue delivery controlled; synthetic result stands in for worker parsing','No claim that partial fingerprint detects changes outside sampled windows']),indent=2)+'\n')
print(out/'oracle.cpp')
