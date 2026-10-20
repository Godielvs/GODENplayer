"""Measure verbatim lyrics admission, with an SDK metadata/worker-queue seam.

The pending worker is deliberately not run: the measured span is caller-side
capture, publication and std::function construction/replacement. This is not a
native first-sound benchmark. Allocated bytes are counted on the caller thread.
"""
from pathlib import Path
import hashlib,json,sys
repo=Path(__file__).resolve().parents[1];out=Path(sys.argv[1]);source=Path(sys.argv[2])
out.mkdir(parents=True,exist_ok=False);text=source.read_text(encoding='utf-8-sig')
def block(s,signature):
 start=s.index(signature);end=s.index('{',start)+1;depth=1
 while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]
helpers='\n'.join(block(text,s) for s in ['std::uint64_t hashAppend(','std::uint64_t hashField(','std::string hex64(','std::string metadataValue('])
methods='\n'.join(block(text,s) for s in ['LyricsRequestContext LyricsService::capture(','void LyricsService::beginRequest('])
cpp=r'''
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>
#include "lyrics_provider_types.h"
thread_local bool countAllocations=false;
thread_local std::uint64_t allocations=0,allocatedBytes=0;
void* operator new(std::size_t n){if(auto* p=std::malloc(n?n:1)){if(countAllocations){++allocations;allocatedBytes+=n;}return p;}throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
using t_size=std::size_t;
template<class T>struct Ptr{
 std::shared_ptr<T> value;Ptr()=default;Ptr(std::shared_ptr<T> p):value(std::move(p)){}bool is_valid()const{return bool(value);}T* operator->()const{return value.get();}
};
struct Info{
 std::map<std::string,std::vector<std::string>> fields;
 t_size meta_get_count_by_name(const char* n)const{auto i=fields.find(n);return i==fields.end()?0:i->second.size();}
 const char* meta_get(const char* n,t_size i)const{return fields.at(n)[i].c_str();}
};
struct metadb_info_container{using ptr=Ptr<metadb_info_container>;Info data;const Info& info()const{return data;}};
struct Track{
 metadb_info_container::ptr container;
 auto get_info_ref()const{return container;}double get_length()const{return 180.;}
 unsigned get_subsong_index()const{return 0;}const char* get_path()const{return "E:\\synthetic\\lyrics.flac";}
};
using metadb_handle_ptr=Ptr<Track>;
namespace nekorox::component {
__HELPERS__
std::string formatted(const metadb_handle_ptr& p,const char* expression){if(!p.is_valid())return {};return std::string(expression).find("title")!=std::string::npos?"Synthetic title":std::string(expression).find("artist")!=std::string::npos?"Synthetic artist":"Synthetic album";}
std::filesystem::path nativePath(const metadb_handle_ptr& p){return p.is_valid()?L"E:/synthetic/lyrics.flac":L"";}
std::filesystem::path profilePath(){return L"E:/synthetic/profile";}
struct Worker {
 std::optional<std::function<void(std::stop_token)>> pending;
 void cancelPending(){pending.reset();}
 bool submit(std::function<void(std::stop_token)> f){pending=std::move(f);return true;}
};
class LyricsService {
public:
 std::atomic<bool> started_{true};std::atomic<std::uint64_t> generation_{0},cacheSatisfiedGeneration_{0};Worker worker_;
 std::uint64_t admitted=0;RequestToken token;
 LyricsRequestContext capture(const metadb_handle_ptr&,std::uint64_t)const;
 void beginRequest(const metadb_handle_ptr&,LyricsRequestReason);
 bool onlineEnabled()const{return false;}bool offlineMode()const{return true;}
 void publish(LyricsSnapshot s){token=s.request;}
 void publishAdmission(const LyricsRequestContext& r,LyricsState){++admitted;token=r.request;}
 void publishState(const LyricsRequestContext&,LyricsState){}
 void resolveMediaRevision(LyricsRequestContext,std::stop_token){}
 void resolveMediaRevision(LyricsRequestContext,metadb_info_container::ptr,std::stop_token){}
};
__METHODS__
}
int main(){using namespace nekorox::component;
 for(std::size_t bytes:{0U,4096U,524288U,8388608U}){
  auto info=std::make_shared<metadb_info_container>();if(bytes){info->data.fields["SYNCEDLYRICS"]={std::string(bytes,'s')};info->data.fields["UNSYNCEDLYRICS"]={std::string(bytes,'p')};}
  auto object=std::make_shared<Track>();object->container=info;metadb_handle_ptr track=object;LyricsService service;
  service.beginRequest(track,LyricsRequestReason::newTrack);
  constexpr unsigned repeats=32;allocations=allocatedBytes=0;countAllocations=true;
  auto start=std::chrono::steady_clock::now();
  for(unsigned i=0;i<repeats;++i)service.beginRequest(track,LyricsRequestReason::newTrack);
  auto end=std::chrono::steady_clock::now();countAllocations=false;
  if(service.admitted!=repeats+1||!service.worker_.pending)return 1;
  std::cout<<"{\"embeddedBytesEach\":"<<bytes<<",\"admissions\":"<<repeats<<",\"meanCallerUs\":"<<std::chrono::duration<double,std::micro>(end-start).count()/repeats<<",\"allocationsPerAdmission\":"<<double(allocations)/repeats<<",\"allocatedBytesPerAdmission\":"<<double(allocatedBytes)/repeats<<"}\n";
 }
}
'''
cpp=cpp.replace('__HELPERS__',helpers).replace('__METHODS__',methods)
(out/'probe.cpp').write_text(cpp)
(out/'source.json').write_text(json.dumps(dict(source=str(source),sha256=hashlib.sha256(source.read_bytes()).hexdigest(),methods=['capture','beginRequest','metadataValue','hashAppend','hashField','hex64'],seams=['SDK metadata/titleformat/profile paths','LatestWorker queue stored without execution','publishAdmission records token without UI invalidations'],scope='Caller allocations and time, not worker/end-to-end/native latency'),indent=2)+'\n')
