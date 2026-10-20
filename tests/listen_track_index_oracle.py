"""Compile actual Listen lookup with the actual Library index key and records.

Host handles/titleformat are bounded seams. Count comparisons and verify every
LibraryTrack field; timings cover lookup/copy only, not track switching or audio.
"""
from pathlib import Path
import hashlib,json,re,sys
repo=Path(__file__).resolve().parents[1]
out,source,mode=Path(sys.argv[1]),Path(sys.argv[2]),sys.argv[3]
assert mode in ('baseline','candidate')
out.mkdir(parents=True,exist_ok=False)
text=source.read_text(encoding='utf-8-sig')
header=(repo/'src/component/library_model.h').read_text(encoding='utf-8-sig')
model=(repo/'src/component/library_model.cpp').read_text(encoding='utf-8-sig')
def block(s,signature):
    start=s.index(signature);end=s.index('{',start)+1;depth=1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}');end+=1
    return s[start:end]
refresh=block(text,'void ListenWindow::refreshTrack(bool force)')
start=refresh.index('    if (track_.is_valid()) {')+len('    if (track_.is_valid()) {')
lookup=refresh[start:refresh.index('        hasTrackInfo_ = true;',start)]
assert ('sharedSnapshot()' in lookup)==(mode=='candidate')
assert ('std::find_if' in lookup)==(mode=='baseline')
track=block(header,'struct LibraryTrack')+';'
snapshot=block(header,'struct LibrarySnapshot')+';'
key=block(model,'std::string handleKey(')
fields=[]
for line in track.splitlines():
    if ';' not in line or line.strip()=='};':continue
    declaration=line.split(';')[0].split('=')[0].strip()
    name=declaration.split()[-1]
    assert re.fullmatch(r'[A-Za-z][A-Za-z_0-9]*',name),line
    fields.append(name)
equality=' && '.join(('a.handle.get()==b.handle.get()' if f=='handle' else f'a.{f}==b.{f}') for f in fields)
cpp=r'''
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
constexpr bool candidate=__CANDIDATE__;
std::uint64_t comparisons=0,fallbackCalls=0,checks=0;
void check(bool value,const char* message){++checks;if(!value){std::cerr<<message<<'\n';std::exit(1);}}
struct Track {std::string path;unsigned subsong=0;std::function<void()> onGetPath;const char* get_path()const{if(onGetPath)onGetPath();return path.c_str();}unsigned get_subsong_index()const{return subsong;}};
struct metadb_handle_ptr:std::shared_ptr<Track>{
 using std::shared_ptr<Track>::shared_ptr;bool is_valid()const{return bool(*this);}
 friend bool operator==(const metadb_handle_ptr& a,const metadb_handle_ptr& b){++comparisons;return a.get()==b.get();}
};
struct metadb_info_container{using ptr=std::shared_ptr<int>;};
__TRACK__
__SNAPSHOT__
__KEY__
bool equal(const LibraryTrack& a,const LibraryTrack& b){return __EQUALITY__;}
struct LibraryModel {
 std::shared_ptr<const LibrarySnapshot> snapshot;
 std::function<void()> afterSnapshot;
 static LibraryModel& instance(){static LibraryModel x;return x;}
 const std::vector<LibraryTrack>& tracks(){static const std::vector<LibraryTrack> empty;return snapshot&&snapshot->tracks?*snapshot->tracks:empty;}
 auto sharedSnapshot(){auto held=snapshot;if(afterSnapshot){auto hook=std::move(afterSnapshot);afterSnapshot={};hook();}return held;}
};
LibraryTrack makeFallbackTrack(const metadb_handle_ptr& h){++fallbackCalls;LibraryTrack result;result.handle=h;result.title=L"external fallback";result.durationSeconds=33.25;return result;}
__declspec(noinline) LibraryTrack lookup(metadb_handle_ptr track_){LibraryTrack trackInfo_;__LOOKUP__ return trackInfo_;}
LibraryTrack record(unsigned i){
 LibraryTrack t;const auto number=std::to_wstring(i);
 t.handle=metadb_handle_ptr(new Track{"file://Q:/qa/disc-"+std::to_string(i/4)+".cue",i%4});
 t.genre=L"g"+number;t.artist=L"a"+number;t.albumArtist=L"aa"+number;
 t.albumArtists={L"aa"+number,L"ab"+number};t.album=L"album"+number;t.title=L"title"+number;
 t.year=L"2001";t.codec=L"FLAC";t.releaseId=L"rid"+number;t.path=L"path"+number;t.searchFolded=L"fold"+number;
 t.disc=1+(i%4);t.track=i%32;t.rating=i%6;t.playCount=i*3;t.sampleRate=48000;t.bitsPerSample=24;t.durationSeconds=90.25+i;
 t.descriptionInfo=std::make_shared<int>(i);t.descriptionBrowse=std::make_shared<int>(i+1);return t;
}
int main(){
 constexpr unsigned count=100000;
 auto tracks=std::make_shared<std::vector<LibraryTrack>>();tracks->reserve(count);
 auto state=std::make_shared<LibrarySnapshot>();state->generation=7;state->tracks=tracks;state->handleIndex.reserve(count);
 for(unsigned i=0;i<count;++i){tracks->push_back(record(i));state->handleIndex.emplace(handleKey(tracks->back().handle),i);}
 auto& library=LibraryModel::instance();library.snapshot=state;
 struct Result{const char* name;std::uint64_t comparisons,fallbacks;};std::vector<Result> results;
 for(auto i:std::vector<unsigned>{0,1,2,3,count/2,count-1}){
  comparisons=0;fallbackCalls=0;auto result=lookup((*tracks)[i].handle);
  check(equal(result,(*tracks)[i]),"All cached metadata fields preserved including subsong");
  check(fallbackCalls==0,"Indexed/in-library hit avoids fallback formatting");
  check(comparisons==(candidate?1:i+1),"Discriminating real handle comparisons");
  results.push_back({i==0?"first":i==count/2?"middle":i==count-1?"last":"subsong",comparisons,fallbackCalls});
 }
 auto external=metadb_handle_ptr(new Track{"file://Q:/outside.flac",0});
 auto fallback=makeFallbackTrack(external);comparisons=0;fallbackCalls=0;
 check(equal(lookup(external),fallback),"External fallback metadata unchanged");
 check(fallbackCalls==1 && comparisons==(candidate?0:count),"External selection has no library scan in candidate");
 results.push_back({"external",comparisons,fallbackCalls});
 auto absentSubsong=metadb_handle_ptr(new Track{(*tracks)[0].handle->path,999});
 comparisons=0;fallbackCalls=0;check(lookup(absentSubsong).title==L"external fallback","Missing subsong does not alias a sibling");
 check(fallbackCalls==1,"Missing subsong formats its own metadata");
 library.snapshot={};comparisons=0;fallbackCalls=0;
 check(equal(lookup(external),fallback)&&fallbackCalls==1&&comparisons==0,"Unavailable snapshot preserves fallback");
 library.snapshot=std::make_shared<LibrarySnapshot>();fallbackCalls=0;
 check(equal(lookup(external),fallback)&&fallbackCalls==1,"Snapshot without tracks preserves fallback");
 auto empty=std::make_shared<LibrarySnapshot>();empty->tracks=std::make_shared<std::vector<LibraryTrack>>();library.snapshot=empty;fallbackCalls=0;
 check(equal(lookup(external),fallback)&&fallbackCalls==1,"Empty snapshot preserves fallback");
 auto duplicates=std::make_shared<LibrarySnapshot>();auto duplicateTracks=std::make_shared<std::vector<LibraryTrack>>();
 duplicateTracks->push_back((*tracks)[2]);duplicateTracks->push_back((*tracks)[2]);(*duplicateTracks)[1].title=L"later duplicate";
 duplicates->tracks=duplicateTracks;
 for(unsigned i=0;i<duplicateTracks->size();++i)duplicates->handleIndex.emplace(handleKey((*duplicateTracks)[i].handle),i);
 library.snapshot=duplicates;check(equal(lookup((*tracks)[2].handle),(*duplicateTracks)[0]),"First duplicate preserved exactly as worker emplace and old find_if");
 if(candidate){
  // Defensive paths must not read a stale/wrong slot or publish another song.
  auto broken=std::make_shared<LibrarySnapshot>(*state);library.snapshot=broken;
  auto handle=(*tracks)[2].handle;broken->handleIndex[handleKey(handle)]=count+2;fallbackCalls=0;
  check(lookup(handle).title==L"external fallback"&&fallbackCalls==1,"Out-of-bounds index rejected");
  broken->handleIndex[handleKey(handle)]=3;fallbackCalls=0;
  check(lookup(handle).title==L"external fallback"&&fallbackCalls==1,"Handle mismatch rejected");
  // A newly published immutable snapshot supplies updated metadata immediately.
  auto nextTracks=std::make_shared<std::vector<LibraryTrack>>(tracks->begin(),tracks->begin()+4);
  (*nextTracks)[2].rating=5;(*nextTracks)[2].playCount=777;(*nextTracks)[2].title=L"edited";
  auto next=std::make_shared<LibrarySnapshot>();next->tracks=nextTracks;
  for(unsigned i=0;i<nextTracks->size();++i)next->handleIndex.emplace(handleKey((*nextTracks)[i].handle),i);
  library.snapshot=next;check(equal(lookup(handle),(*nextTracks)[2]),"New publication metadata copied from matching immutable vector");
  auto transient=std::make_shared<LibrarySnapshot>();
  auto transientTracks=std::make_shared<std::vector<LibraryTrack>>(1,(*tracks)[2]);
  std::weak_ptr<const std::vector<LibraryTrack>> oldRecords=transientTracks;
  transient->tracks=std::move(transientTracks);transient->handleIndex.emplace(handleKey(handle),0);
  std::weak_ptr<const LibrarySnapshot> oldPublication=transient;
  library.snapshot=std::move(transient);
  library.afterSnapshot=[&]{library.snapshot=next;};
  handle->onGetPath=[&]{check(!oldPublication.expired()&&!oldRecords.expired(),"Caller retains exclusive publication and records after getter returns");};
  check(equal(lookup(handle),(*tracks)[2]),"Index and records stay from the same acquired publication");
  handle->onGetPath={};
  check(oldPublication.expired()&&oldRecords.expired(),"Old publication and records released after copying metadata");
 }
 library.snapshot=state;
 std::vector<metadb_handle_ptr> selection{(*tracks)[0].handle,(*tracks)[count/2].handle,(*tracks)[count-1].handle,external};
 constexpr unsigned iterations=256;std::uint64_t sink=0;comparisons=0;fallbackCalls=0;
 auto begin=std::chrono::steady_clock::now();
 for(unsigned i=0;i<iterations;++i){auto selected=lookup(selection[i%selection.size()]);sink+=selected.title.size()+selected.playCount;}
 auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
 std::cout<<"{\"status\":\"PASS\",\"mode\":\""<<(candidate?"candidate":"baseline")<<"\",\"checks\":"<<checks<<",\"tracks\":"<<count<<",\"iterations\":"<<iterations<<",\"lookupCopyMs\":"<<elapsed<<",\"benchmarkComparisons\":"<<comparisons<<",\"benchmarkFallbacks\":"<<fallbackCalls<<",\"sink\":"<<sink<<",\"cases\":[";
 for(unsigned i=0;i<results.size();++i){if(i)std::cout<<',';auto& r=results[i];std::cout<<"{\"case\":\""<<r.name<<"\",\"comparisons\":"<<r.comparisons<<",\"fallbacks\":"<<r.fallbacks<<'}';}
 std::cout<<"]}\n";
}
'''
for k,v in dict(CANDIDATE='true' if mode=='candidate' else 'false',TRACK=track,SNAPSHOT=snapshot,KEY=key,EQUALITY=equality,LOOKUP=lookup).items():
    cpp=cpp.replace('__'+k+'__',v)
(out/'oracle.cpp').write_text(cpp,encoding='utf-8')
(out/'source.json').write_text(json.dumps(dict(mode=mode,sourceSha256=hashlib.sha256(source.read_bytes()).hexdigest(),
    lookupSha256=hashlib.sha256(lookup.encode()).hexdigest(),modelSha256=hashlib.sha256((repo/'src/component/library_model.cpp').read_bytes()).hexdigest(),
    headerSha256=hashlib.sha256((repo/'src/component/library_model.h').read_bytes()).hexdigest(),comparedFields=fields,
    seams=['SDK handles instrumented to count equality; actual LibraryTrack and LibrarySnapshot layouts used','Fallback formatter replaced by sentinel metadata; production makeFallbackTrack unchanged','Timers include per-comparison instrumentation and measure extracted selection and record copying only; not a host speedup estimate']),indent=2)+'\n')
print(out/'oracle.cpp')
