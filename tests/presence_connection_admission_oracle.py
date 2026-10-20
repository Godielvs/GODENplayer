"""Compile the production worker's connection-admission prefix with controlled
pipe opening and policy changes. Does not replace/native-test the full worker.
Usage: OUTPUT SOURCE baseline|candidate
"""
from pathlib import Path
import hashlib,json,sys
repo=Path(__file__).resolve().parents[1];out=Path(sys.argv[1]);src=Path(sys.argv[2]);mode=sys.argv[3]
assert mode in ('baseline','candidate');out.mkdir(parents=True,exist_ok=False)
s=src.read_text(encoding='utf-8-sig');worker=s.index('void DiscordPresenceService::workerMain(')
begin=s.index('    const auto connectionAllowed =',worker) if mode=='candidate' else s.index('    while (!stopToken.stop_requested()) {',worker)
end=s.index('        const auto appId = configuredApplicationId();',begin)
prefix=s[begin:end]
assert ('connectionAllowed' in prefix)==(mode=='candidate')
cpp=r'''
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <stop_token>
using namespace std::chrono_literals;
constexpr bool candidate=__CANDIDATE__;
enum class DiscordPresenceStatus{connecting,discordUnavailable};
using HANDLE=int;constexpr HANDLE INVALID_HANDLE_VALUE=-1;
bool g_presenceEnabled=true;
unsigned checks=0;void require(bool b,const char* m){++checks;if(!b)throw std::runtime_error(m);}
struct Service{
 bool started_=true,drainPending_=false;std::mutex mutex_;std::atomic<bool> rpcReady_=true;
 std::stop_source source;unsigned scans=0,opens=0,closes=0,handshakes=0,waits=0;int effect=0;bool openFails=false;
 struct Changed{Service* s;template<class... A>void wait_for(A&&...){++s->waits;s->source.request_stop();}}changed_{this};
 void refreshExternalPublisherConflict(){++scans;}
 void setStatus(DiscordPresenceStatus){}
 void CloseHandle(HANDLE h){require(h!=INVALID_HANDLE_VALUE,"invalid handle closed");++closes;}
 HANDLE connectDiscordPipe(){++opens;if(effect==1)g_presenceEnabled=false;if(effect==2)drainPending_=true;if(effect==3)started_=false;if(effect==4)source.request_stop();return openFails?INVALID_HANDLE_VALUE:1;}
 void run(){auto stopToken=source.get_token();auto retryDelay=1s;auto nextExternalPublisherScan=std::chrono::steady_clock::time_point::min();
__PREFIX__
  // Stops immediately before the real handshake; only admission is tested here.
  ++handshakes;break;
 }
 }
};
int main(){try{
 for(unsigned mask=0;mask<16;++mask){
  Service s;s.started_=bool(mask&1);g_presenceEnabled=bool(mask&2);s.drainPending_=bool(mask&4);if(mask&8)s.source.request_stop();
  const unsigned expected=!(mask&8)&&(!candidate||((mask&1)&&(mask&2)&&!(mask&4)));
  s.run();require(s.opens==expected,"initial policy admission");require(s.scans==expected,"no disabled process scan");require(s.handshakes==expected,"initial handshake admission");require(s.waits==0&&s.closes==0,"initial gate has no cleanup/retry");
 }
 for(int effect=1;effect<=4;++effect)for(bool failure:{false,true}){
  Service s;g_presenceEnabled=true;s.effect=effect;s.openFails=failure;s.run();
  require(s.opens==1&&s.scans==1,"single admitted opening");
  require(s.handshakes==unsigned(!candidate&&!failure),"policy changed while opening prevents handshake");
  require(s.closes==unsigned(candidate&&!failure),"valid unhandshaken connection released");
  require(s.waits==unsigned(!candidate&&failure),"disabled failed opening avoids retry delay");
 }
 Service failed;g_presenceEnabled=true;failed.openFails=true;failed.run();require(failed.opens==1&&failed.waits==1&&failed.handshakes==0,"enabled connection failure keeps bounded retry");
 Service enabled;g_presenceEnabled=true;enabled.run();require(enabled.handshakes==1&&enabled.opens==1,"enabled admission preserved");
 std::cout<<"{\"mode\":\""<<(candidate?"candidate":"baseline")<<"\",\"result\":\"PASS\",\"checks\":"<<checks<<"}\n";
 }catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
'''.replace('__CANDIDATE__',str(mode=='candidate').lower()).replace('__PREFIX__',prefix)
(out/'oracle.cpp').write_text(cpp)
(out/'source.json').write_text(json.dumps(dict(mode=mode,source=str(src),sha256=hashlib.sha256(src.read_bytes()).hexdigest(),extracted='verbatim worker connection admission through just before handshake',prefixSha256=hashlib.sha256(prefix.encode()).hexdigest(),seams=['pipe opening/closing','policy mutation during opening','retry wait requests stop','status/scan counters'],fullWorker=False,noNetwork=True),indent=2)+'\n')
