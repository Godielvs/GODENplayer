"""Verbatim Workspace playback-message dispatch with controlled dirty bus,
metadata refresh and reveal seams. Native tests qualify actual data and UI.
Usage: OUTPUT SOURCE baseline|candidate
"""
from pathlib import Path
import hashlib,json,sys
repo=Path(__file__).resolve().parents[1];out=Path(sys.argv[1]);src=Path(sys.argv[2]);mode=sys.argv[3]
assert mode in ('baseline','candidate');out.mkdir(parents=True,exist_ok=False)
s=src.read_text(encoding='utf-8-sig');a=s.index('        case kPlaybackChangedMessage:');b=s.index('        case kRevealNowPlayingMessage:',a)
dispatch=s[a:b];candidate='const auto settledRefresh' in dispatch;assert candidate==(mode=='candidate')
assert ('bool revealNowPlaying(bool refresh = true)' in s)==candidate
cpp=r'''
#include <iostream>
#include <stdexcept>
#include <utility>
#include "ui_invalidation_bus.h"
using namespace nekorox::component;
constexpr bool candidate=__CANDIDATE__;
UiDirty pendingDirty=UiDirty::none;
namespace nekorox::component{
 UiInvalidationBus::UiInvalidationBus():impl_(nullptr){}
 UiInvalidationBus::~UiInvalidationBus(){}
 UiInvalidationBus& UiInvalidationBus::instance(){static UiInvalidationBus bus;return bus;}
 UiDirty UiInvalidationBus::consume(UiInvalidationSurface,HWND)noexcept{return std::exchange(pendingDirty,UiDirty::none);}
}
unsigned checks=0;void require(bool v,const char* m){++checks;if(!v)throw std::runtime_error(m);}
struct Handle{int id=0;bool is_valid()const{return id!=0;}void release(){id=0;}bool operator==(const Handle&)const=default;};
struct Panel{
 bool temporaryAlbumView_=false;Handle pendingRandomAlbumReveal_,nowPlaying_,runtimeTrack{1};
 int refreshes=0,reveals=0,invalidations=0,randomAlbumRevealAttempts_=0,randomAlbumRevealSuccesses_=0;
 int metadata=0,observedMetadata=-1,hostOccurrence=0,playingOccurrence=-1;bool revealWorks=true;
 void refreshNowPlaying(){++refreshes;nowPlaying_=runtimeTrack;observedMetadata=metadata;playingOccurrence=hostOccurrence;}
 bool revealNowPlaying(bool refresh=true){if(refresh)refreshNowPlaying();++reveals;return revealWorks;}
 void invalidate(){++invalidations;}
 int handle(unsigned wParam,UiDirty bits=UiDirty::none){pendingDirty=bits;HWND window=nullptr;constexpr int kPlaybackChangedMessage=7;
  switch(kPlaybackChangedMessage){
__DISPATCH__
  }return -1;
 }
};
int main(){try{
 // Normal new-track notification and its later completion resolve one track.
 Panel ordinary;ordinary.handle(0,UiDirty::semantic);ordinary.handle(2);
 require(ordinary.refreshes==(candidate?1:2),"ordinary semantic then settled");require(ordinary.nowPlaying_.id==1&&ordinary.invalidations==2,"ordinary state and paints retained");
 // A completion can consume pending semantic bits ahead of a stale bus wake.
 Panel reordered;reordered.handle(2,UiDirty::semantic);reordered.handle(0);
 require(reordered.refreshes==(candidate?1:2),"settled consumes real dirty work once");
 for(unsigned msg:{0U,1U,2U})for(auto bits:{UiDirty::semantic,UiDirty::artwork,UiDirty::semantic|UiDirty::position}){
  Panel p;p.handle(msg,bits);require(p.refreshes==1&&p.observedMetadata==0,"real semantic/artwork always refreshed");
 }
 for(unsigned msg:{0U,1U,2U})for(auto bits:{UiDirty::position,UiDirty::volume,UiDirty::analysis,UiDirty::theme,UiDirty::input}){
  Panel p;p.handle(msg,bits);require(p.refreshes==0&&p.invalidations==1,"nonsemantic dirty retains paint without metadata clone");
 }
 Panel unchanged;unchanged.handle(0,UiDirty::semantic);unchanged.metadata=44;unchanged.handle(0,UiDirty::semantic);
 require(unchanged.refreshes==2&&unchanged.observedMetadata==44,"same handle metadata update retained");
 unchanged.runtimeTrack.release();unchanged.handle(0,UiDirty::semantic);unchanged.handle(2);
 require(!unchanged.nowPlaying_.is_valid(),"stop clears playing track");
 // Repeated identical handles in temporary playlist still advance occurrence
 // after host playlist state settles, independent of semantic track identity.
 Panel temporary;temporary.temporaryAlbumView_=true;temporary.handle(0,UiDirty::semantic);temporary.hostOccurrence=1;temporary.handle(2);
 require(temporary.refreshes==2&&temporary.playingOccurrence==1&&temporary.nowPlaying_.id==1,"same handle different temporary occurrence");
 temporary.hostOccurrence=3;temporary.handle(2,UiDirty::position);require(temporary.playingOccurrence==(candidate?3:1),"settled occurrence overrides unrelated coalesced position");
 // Random/default action completion resolves expected identity before reveal.
 for(bool matches:{false,true})for(bool succeeds:{false,true}){
  Panel p;p.pendingRandomAlbumReveal_={matches?2:9};p.handle(0,UiDirty::semantic);
  require(p.reveals==0&&p.pendingRandomAlbumReveal_.is_valid(),"reveal waits for settled");
  p.runtimeTrack={2};p.revealWorks=succeeds;p.handle(2);
  require(p.randomAlbumRevealAttempts_==1&&!p.pendingRandomAlbumReveal_.is_valid(),"pending completion consumed exactly once");
  require(p.reveals==(matches?1:0)&&p.randomAlbumRevealSuccesses_==int(matches&&succeeds),"reveal expected handle and success contract");
  require(p.refreshes==(candidate?2:(matches?3:2)),"reveal reuses refresh from its handler");
 }
 Panel async;async.pendingRandomAlbumReveal_={3};async.runtimeTrack={3};async.handle(2,UiDirty::position);
 require(async.reveals==int(candidate)&&async.nowPlaying_.id==(candidate?3:0),"pending reveal handles nonsemantic bus payload");
 std::cout<<"{\"mode\":\""<<(candidate?"candidate":"baseline")<<"\",\"result\":\"PASS\",\"checks\":"<<checks<<"}\n";
 }catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
'''.replace('__CANDIDATE__',str(candidate).lower()).replace('__DISPATCH__',dispatch)
(out/'oracle.cpp').write_text(cpp)
(out/'source.json').write_text(json.dumps(dict(source=str(src),sha256=hashlib.sha256(src.read_bytes()).hexdigest(),dispatchSha256=hashlib.sha256(dispatch.encode()).hexdigest(),mode=mode,real=['message handler verbatim','UiDirty and bus declaration'],seams=['dirty consume','track/occurrence metadata refresh','reveal result/paint counters'],notCovered='full refreshNowPlaying/reveal geometry, actual Win32 queue/bus concurrency; native tests cover integration'),indent=2)+'\n')
