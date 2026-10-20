"""Compile extracted production identity/invalidation code with bounded host seams.

This checks key boundaries, cancellation and the playback-edit fan-out order.
Native CUE/Listen pixel qualification is separate; no decoder or renderer is
simulated here, and these checks do not claim CPU or frame-rate improvements.
"""
from pathlib import Path
import hashlib, json, sys

repo=Path(__file__).resolve().parents[1]
out=Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=False)
art=(repo/'src/component/art_cache.cpp').read_text(encoding='utf-8-sig')
runtime=(repo/'src/component/runtime.cpp').read_text(encoding='utf-8-sig')
def method(source,signature):
    start=source.index(signature);end=source.index('{',start);depth=1;end+=1
    while depth:
        if source[end]=='{':depth+=1
        elif source[end]=='}':depth-=1
        end+=1
    return source[start:end]
names=['std::wstring artworkTrackIdentity(', 'std::uint32_t brokerEdge(',
       'std::wstring trackBrokerKey(', 'std::wstring artworkVariantKey(',
       '    void invalidatePrefix(std::wstring_view prefix)', 'void ArtCache::invalidateTrack(']
bodies={n:method(art,n) for n in names}
edited=method(runtime,'    void on_playback_edited(metadb_handle_ptr track) override').replace(' override','')
cpp=r'''
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <Windows.h>
struct Track {std::string path;unsigned subsong;const char* get_path()const{return path.c_str();}unsigned get_subsong_index()const{return subsong;}};
struct metadb_handle_ptr:std::shared_ptr<Track>{using std::shared_ptr<Track>::shared_ptr;bool is_valid()const{return bool(*this);}};
std::wstring utf8ToWide(const char* s){int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,nullptr,0);std::wstring r(n? n-1:0,L'\0');if(n)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,r.data(),n);return r;}
constexpr unsigned kArtworkMasterEdge=720;
enum class ArtAnalysisFlags:unsigned char{legacy,alloyV2};
int checks=0;void check(bool b,const char* label){++checks;if(!b){std::cerr<<label<<'\n';exit(1);}}
struct Cancellation{bool aborted=false;void abort(){aborted=true;}};
struct Entry{std::shared_ptr<Cancellation> cancellation;};
class ArtworkBroker {public:
 static ArtworkBroker& instance(){static ArtworkBroker value;return value;}
 std::mutex mutex_;std::map<std::wstring,Entry> entries_;int released=0;
 void releaseContentLocked(Entry&){++released;}
__PREFIX__
};
struct ArtCache{static void invalidateTrack(const metadb_handle_ptr&);};
__HELPERS__
__INVALIDATE__
std::vector<std::string> events;
void observer(const char* name){check(ArtworkBroker::instance().entries_.empty(),"Purge must precede every edit observer");events.push_back(name);}
struct ListenWindow{static ListenWindow& instance(){static ListenWindow x;return x;}void notifyArtworkChanged(){observer("listen");}};
void notifyWorkspaceArtworkChanged(){observer("workspace");}
void notifyNativePanelsPlaybackChanged(bool art){check(art,"Native edit notification");observer("native");}
struct Runtime{static Runtime& instance(){static Runtime x;return x;}void notifyPlaybackMetadataChanged(metadb_handle_ptr){observer("metadata");}};
struct LyricsService{static LyricsService& instance(){static LyricsService x;return x;}void onMetadataChanged(metadb_handle_ptr){observer("lyrics");}};
struct PlaybackCallback{__EDITED__};
metadb_handle_ptr track(const char* path,unsigned sub){return metadb_handle_ptr(new Track{path,sub});}
int main(){
 auto a=track("file://C:/qa/disc.cue",1),b=track("file://C:/qa/disc.cue",2);
 auto hashNeighbor=track("file://C:/qa/disc.cue#other",1),longNeighbor=track("file://C:/qa/disc.cue-other",1);
 const auto ka=artworkTrackIdentity(a),kb=artworkTrackIdentity(b);
 check(ka!=kb,"Subsong identity independent");check(ka==artworkTrackIdentity(a),"Stable identity");
 check(artworkTrackIdentity({}).empty(),"Null identity");
 check(trackBrokerKey(ka,96,ArtAnalysisFlags::legacy)==trackBrokerKey(ka,512,ArtAnalysisFlags::legacy),"Main and row same-profile master coalesces");
 check(trackBrokerKey(ka,720,ArtAnalysisFlags::legacy)!=trackBrokerKey(kb,720,ArtAnalysisFlags::legacy),"Separate subsong masters");
 check(trackBrokerKey(ka,720,ArtAnalysisFlags::legacy)!=trackBrokerKey(ka,720,ArtAnalysisFlags::alloyV2),"Analysis profiles remain separate");
 auto& broker=ArtworkBroker::instance();std::vector<std::shared_ptr<Cancellation>> cancelled,retained;
 for(auto identity:{ka,kb})for(auto profile:{ArtAnalysisFlags::legacy,ArtAnalysisFlags::alloyV2}){
  auto master=trackBrokerKey(identity,720,profile);
  for(auto key:{master,artworkVariantKey(master,123,64)}){auto token=std::make_shared<Cancellation>();cancelled.push_back(token);broker.entries_[key]={token};}
 }
 for(auto handle:{hashNeighbor,longNeighbor}){auto token=std::make_shared<Cancellation>();retained.push_back(token);broker.entries_[trackBrokerKey(artworkTrackIdentity(handle),720,ArtAnalysisFlags::legacy)]={token};}
 ArtCache::invalidateTrack({});check(broker.entries_.size()==10,"Null invalidation inert");
 ArtCache::invalidateTrack(a);check(broker.entries_.size()==2,"Invalidate all subsong/profile/variant entries only");
 check(broker.released==8,"Resident ownership released once per removed entry");
 for(auto token:cancelled)check(token->aborted,"In-flight decode aborted");
 for(auto token:retained)check(!token->aborted,"Neighbor decode preserved including hash in filename");
 ArtCache::invalidateTrack(b);check(broker.released==8,"Repeated physical invalidation idempotent");
 broker.entries_.clear();auto token=std::make_shared<Cancellation>();broker.entries_[trackBrokerKey(kb,720,ArtAnalysisFlags::legacy)]={token};
 PlaybackCallback{}.on_playback_edited(a);check(token->aborted,"Runtime invalidates physical siblings before fan-out");
 check(events==std::vector<std::string>{"listen","workspace","native","metadata","lyrics"},"Existing edit observers preserved");
 std::cout<<"PASS "<<checks<<" artwork identity/invalidation checks\n";
}
'''
cpp=cpp.replace('__PREFIX__',bodies[names[4]]).replace('__HELPERS__','\n'.join(bodies[n] for n in names[:4])).replace('__INVALIDATE__',bodies[names[5]]).replace('__EDITED__',edited)
(out/'oracle.cpp').write_text(cpp,encoding='utf-8')
(out/'source-manifest.json').write_text(json.dumps({n:hashlib.sha256(v.encode()).hexdigest() for n,v in (bodies|{'playback edited':edited}).items()},indent=2))
print(out/'oracle.cpp')
