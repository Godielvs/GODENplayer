"""Extract real incoming-open/shutdown callbacks; bound SDK/playlist effects.

Usage OUTPUT SOURCE baseline|candidate. No SDK parser or time-to-sound claim.
The playlist-effect seam records accepted handles after the actual production
playIncomingItems guard. Native tests cover actual host playback separately.
"""
from pathlib import Path
import hashlib,json,sys
out=Path(sys.argv[1]);src=Path(sys.argv[2]);mode=sys.argv[3]
assert mode in ('baseline','candidate');out.mkdir(parents=True,exist_ok=False)
s=src.read_text(encoding='utf-8-sig')
def method(name):
 a=s.index('void '+name+'(');b=s.index('\n}',a)+2;return s[a:b]
opening=method('openIncomingUrls');shutdown=method('shutdownFileOpenService')
play=method('playIncomingItems');guard=play[:play.index('    observeFileOpen(')]
observe=method('observeFileOpen') if 'void observeFileOpen(' in s else ''
cpp=r'''
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
using HWND=void*;
using Handle=std::pair<int,int>;
std::size_t copied=0;int checks=0,dispatched=0,dismissed=0;
struct metadb_handle_list {
 std::vector<Handle> values;
 std::size_t get_count()const{return values.size();}
 void add_items(const metadb_handle_list& x){copied+=x.values.size();values.insert(values.end(),x.values.begin(),x.values.end());}
};
using metadb_handle_list_cref=const metadb_handle_list&;
namespace pfc {template<class T>struct list_t{std::vector<T> values;void add_item(T v){values.push_back(v);}};}
namespace nekorox {constexpr unsigned kFileOpenCallbackBitV1=1,kFileOpenRequestStartedBitV1=2;}
std::atomic<std::uint64_t> g_openRequest{0};std::atomic<std::uint32_t> g_fileOpenQaState{0};
std::vector<std::function<void()>> mainJobs;
namespace fb2k {template<class F>void inMainThread(F f){++dispatched;mainJobs.emplace_back(std::move(f));}}
struct process_locations_notify {
 std::function<void(metadb_handle_list_cref)> fn;
 template<class F>static std::shared_ptr<process_locations_notify> create(F f){return std::make_shared<process_locations_notify>(process_locations_notify{std::move(f)});}
};
struct Request {std::vector<std::string> urls;HWND owner;unsigned flags;std::shared_ptr<process_locations_notify> notify;};
std::vector<Request> requests;
struct playlist_incoming_item_filter_v2 {
 enum {op_flag_delay_ui=1,op_flag_no_filter=2};
 static playlist_incoming_item_filter_v2* get(){static playlist_incoming_item_filter_v2 x;return &x;}
 void process_locations_async(const pfc::list_t<const char*>& input,unsigned flags,void*,void*,HWND owner,std::shared_ptr<process_locations_notify> n){Request r{{},owner,flags,std::move(n)};for(auto u:input.values)r.urls.emplace_back(u);requests.emplace_back(std::move(r));}
};
std::vector<std::vector<Handle>> accepted;
void dismissTemporaryAlbum(bool){++dismissed;}
__OBSERVE__
__PLAYGUARD__
    accepted.emplace_back(items.values);
}
__OPEN__
__SHUTDOWN__
constexpr bool candidate=__CANDIDATE__;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
void drain(){auto jobs=std::move(mainJobs);mainJobs.clear();for(auto& f:jobs)f();}
void reset(){requests.clear();mainJobs.clear();accepted.clear();copied=0;dispatched=dismissed=0;g_fileOpenQaState=0;g_openRequest=0;}
void deliver(std::size_t i,const metadb_handle_list& items){requests.at(i).notify->fn(items);}
int main(){try{
 metadb_handle_list large;for(int i=0;i<100000;++i)large.values.emplace_back(i/2,i%2);
 const metadb_handle_list small{{{7,0},{7,1},{7,0},{3,4}}};
 reset();openIncomingUrls({"",""},nullptr);require(requests.empty()&&g_openRequest==0,"empty does not supersede");
 openIncomingUrls({"A","","B","A"},reinterpret_cast<HWND>(17));
 require(requests[0].urls==std::vector<std::string>({"A","B","A"}),"URL order and duplicates");
 require(requests[0].owner==reinterpret_cast<HWND>(17)&&requests[0].flags==3,"SDK arguments retained");
 deliver(0,small);require(copied==4&&dispatched==1,"live resolution retained");drain();require(accepted==std::vector<std::vector<Handle>>{small.values},"order duplicates subsongs retained");
 for(int scenario=0;scenario<2;++scenario){
  reset();openIncomingUrls({"old"},nullptr);
  if(scenario==0||scenario==2)openIncomingUrls({"new"},nullptr);else shutdownFileOpenService();
  deliver(0,large);
  require(copied==(candidate?0u:100000u),"stale callback copy admission");
  require(dispatched==(candidate?0:1),"stale callback queue admission");
  require((g_fileOpenQaState&nekorox::kFileOpenCallbackBitV1)!=0,"callback observed even stale");
  if(scenario==0||scenario==2){deliver(1,small);require(copied==(candidate?4u:100004u),"new callback copied once");}
  drain();require(accepted.size()==((scenario==0||scenario==2)?1u:0u),"only current request plays");
  if(!accepted.empty())require(accepted[0]==small.values,"latest payload exact");
 }
 reset();openIncomingUrls({"old"},nullptr);openIncomingUrls({"new"},nullptr);
 deliver(1,small);drain();require(accepted.size()==1&&accepted[0]==small.values,"new result completes first");
 deliver(0,large);drain();require(copied==(candidate?4u:100004u),"late old result skipped before copy");
 require(accepted.size()==1&&accepted[0]==small.values,"late old result cannot replace playing selection");
 for(bool shutdown:{false,true}){
  reset();openIncomingUrls({"old"},nullptr);deliver(0,large);
  require(copied==100000&&dispatched==1,"live callback snapshots before race");
  if(shutdown)shutdownFileOpenService();else openIncomingUrls({"new"},nullptr);
  drain();require(accepted.empty(),"dispatch rechecks generation after race");
 }
 reset();openIncomingUrls({"live"},nullptr);deliver(0,{});drain();require(accepted.empty(),"empty resolution never plays");
 reset();for(int i=0;i<64;++i)openIncomingUrls({std::to_string(i)},nullptr);
 for(int i=0;i<63;++i)deliver(i,large);
 require(copied==(candidate?0u:6300000u),"burst stale handles eliminated");
 deliver(63,small);drain();require(accepted.size()==1&&accepted[0]==small.values,"burst newest wins");
 std::cout<<"{\"result\":\"PASS\",\"checks\":"<<checks<<",\"candidate\":"<<(candidate?"true":"false")<<",\"burstCopied\":"<<copied<<",\"burstDispatched\":"<<dispatched<<"}\n";
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
'''
cpp=cpp.replace('__OBSERVE__',observe).replace('__PLAYGUARD__',guard).replace('__OPEN__',opening).replace('__SHUTDOWN__',shutdown).replace('__CANDIDATE__',str(mode=='candidate').lower())
(out/'probe.cpp').write_text(cpp)
(out/'source.json').write_text(json.dumps({'source':str(src),'sha256':hashlib.sha256(src.read_bytes()).hexdigest(),'mode':mode,'seams':'SDK resolver delivery, main dispatch queue, playlist effect after verbatim production guard. No timing claim.'},indent=2)+'\n')
print('Extracted',mode)
