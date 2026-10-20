"""Exercise literal production Album/DB publication code with real SQLite.

Only SDK handles/playback, main-thread dispatch and the Album snapshot owner are
seams. This proves event ordering and associations, not native rendered UI.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


def method(source, name):
    m = re.search(r'^(?:void|bool|std::shared_ptr<const TrackUserStateSnapshot>)\s+'
                  + re.escape(name) + r'\([\s\S]*?^}\n', source, re.M)
    if not m:
        raise RuntimeError('Missing production method: ' + name)
    return m.group()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--expect-baseline', action='store_true')
    a = p.parse_args()
    component = a.source_root/'src/component'
    source = (component/'database_service.cpp').read_text(encoding='utf-8-sig')
    header = (component/'database_service.h').read_text(encoding='utf-8-sig')
    album = (component/'album_session_service.cpp').read_text(encoding='utf-8-sig')
    db = [method(source, 'DatabaseService::'+n) for n in
          ('enqueue','resolveTrackIdentities','publishUserState','publishUserStateLoading','userStateSnapshot')]
    start = source.index('        if (pending.identities) {')
    branch = source[start:source.index('        if (pending.analysis) {', start)]
    on_track = method(album, 'AlbumSessionService::onNewTrack')
    if a.expect_baseline:
        marker = 'fb2k::inMainThread([this, identitySource, requestId, ids = std::move(ids)]() mutable {'
        start = album.index(marker) + len(marker)
        end = album.index('\n        });\n    });', start)
        adopt = ADOPT_SIGNATURE+' {\n'+album[start:end]+'\n}\n'
        refresh = 'void AlbumSessionService::refreshPlayingIdentity() {}\n'
    else:
        adopt = method(album, 'AlbumSessionService::adoptRevisionIds')
        refresh = method(album, 'AlbumSessionService::refreshPlayingIdentity')
    header = header.replace('#include "sdk_compat.h"',
        'class metadb_handle_ptr; namespace play_control { enum t_stop_reason : int; }')
    assert header.count('private:') == 1
    header = header.replace('private:', 'public: // Oracle access only.')
    header = header.replace('    DatabaseService() = default;',
        '    DatabaseService() = default;\n    void runIdentityEvent(NekoDatabase&, PendingEvent&);')
    code = '#define EXPECT_BASELINE '+str(int(a.expect_baseline))+'\n'+PREFIX
    code += '\nnamespace nekorox::component {\n'+SEAM+'\n'+'\n'.join(db)
    code += '\nvoid DatabaseService::runIdentityEvent(NekoDatabase& database, PendingEvent& pending) {\nfor(int once=0;once<1;++once){\n'+branch+'\n}}\n'
    code += adopt+refresh+on_track+'\n}\n'+TESTS
    a.output.mkdir(parents=True, exist_ok=True)
    (a.output/'database_service_test.h').write_text(header)
    (a.output/'oracle.cpp').write_text(code)
    bodies = db+[branch,adopt,refresh,on_track]
    (a.output/'source-manifest.json').write_text(json.dumps(dict(sourceRoot=str(a.source_root.resolve()),
        baseline=a.expect_baseline, bodiesSha256=[hashlib.sha256(s.encode()).hexdigest() for s in bodies], limits=__doc__), indent=2))


ADOPT_SIGNATURE = '''void AlbumSessionService::adoptRevisionIds(
 const std::shared_ptr<const AlbumSessionSnapshot>& identitySource,
 std::uint64_t requestId, std::vector<std::string> ids)'''

PREFIX = r'''
#include "database_service_test.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
namespace qa { std::vector<std::function<void()>> callbacks; }
namespace fb2k {template<class C> void inMainThread(C c){qa::callbacks.emplace_back(std::move(c));}}
class metadb_handle_ptr {
public: int value=0;
 metadb_handle_ptr()=default; metadb_handle_ptr(int v):value(v){}
 bool is_valid() const{return value!=0;}
 bool operator==(const metadb_handle_ptr&) const=default;
};
struct playback_control {
 metadb_handle_ptr playing;
 static playback_control* get(){static playback_control p;return &p;}
 bool get_now_playing(metadb_handle_ptr& p){p=playing;return p.is_valid();}
};
struct playlist_manager {static playlist_manager* get(){static playlist_manager p;return &p;}
 unsigned queue_get_count(){return 0;}};
constexpr auto pfc_infinite=static_cast<std::size_t>(-1);
'''

SEAM = r'''
struct Runtime {static Runtime& instance(){static Runtime r;return r;}void refreshPresentationState(){}};
void notifyWorkspaceModelChanged(){} void postListenRefresh(){}
enum class PlayerShellMode {normal,album};
PlayerShellMode playerShellMode(){return PlayerShellMode::normal;}
enum class AlbumSessionState {empty,loading,ready,error};
struct AlbumSessionTrack {metadb_handle_ptr handle;std::string revisionTrackId,entityId;};
struct AlbumSessionSnapshot {
 std::uint64_t sessionId=1,requestId=2,compositionRevision=3,revision=4;
 AlbumSessionState state=AlbumSessionState::ready;
 std::vector<AlbumSessionTrack> tracks;
 std::optional<std::size_t> actualPlayingIndex=1;
 std::size_t currentInSession=1;
 bool outsideSession=false,queueExternal=false,playlistLeaseValid=false,ownedPlaylistValid=false;
};
struct AlbumSessionService {
 std::shared_ptr<AlbumSessionSnapshot> state;
 std::atomic<std::uint64_t> requestId_{2};bool identityStopped_=false;
 unsigned publications=0;
 static AlbumSessionService& instance(){static AlbumSessionService a;return a;}
 std::shared_ptr<const AlbumSessionSnapshot> snapshot(){return state;}
 void publish(std::shared_ptr<AlbumSessionSnapshot> next){state=std::move(next);++publications;}
 void cancelContinuation(){} bool enterForTrack(const metadb_handle_ptr&){return false;}
 std::size_t ownedPlaylist() const{return 0;}
 void adoptRevisionIds(const std::shared_ptr<const AlbumSessionSnapshot>&,std::uint64_t,std::vector<std::string>);
 void refreshPlayingIdentity();void onNewTrack(const metadb_handle_ptr&);
};
DatabaseService& DatabaseService::instance(){static DatabaseService d;return d;}
'''

TESTS = r'''
using namespace nekorox;using namespace nekorox::component;
int checks=0;
void require(bool okay,const char* why){++checks;if(!okay)throw std::runtime_error(why);}
void drain(){while(!qa::callbacks.empty()){auto batch=std::move(qa::callbacks);qa::callbacks.clear();for(auto& f:batch)f();}}
void worker(NekoDatabase& db){auto& d=DatabaseService::instance();require(d.queue_.size()==1,"Expected one identity query");
 auto pending=std::move(d.queue_.front());d.queue_.pop_front();std::thread t([&]{d.runIdentityEvent(db,pending);});t.join();}
void reset(){auto& d=DatabaseService::instance();auto& a=AlbumSessionService::instance();
 qa::callbacks.clear();d.queue_.clear();d.started_=true;d.stopping_=false;
 d.admittedPlayback_=std::make_shared<DatabaseService::PlaybackContext>();
 d.userStateSnapshot_=std::make_shared<TrackUserStateSnapshot>();
 a.state=std::make_shared<AlbumSessionSnapshot>();a.requestId_=2;a.identityStopped_=false;a.publications=0;
 a.state->tracks={{1,"A",""},{2,"B",""},{2,"B",""},{3,"C",""}};
 playback_control::get()->playing=2;
}
PersistedTrackUserState stateFor(NekoDatabase& db,const char* id){PersistedTrackUserState state;std::string error;
 require(db.readTrackUserState(id,state,error)&&state.found,error.c_str());return state;}
void emit(PersistedTrackUserState state,std::string error={}){auto& d=DatabaseService::instance();auto context=d.admittedPlayback_;
 std::thread t([&]{d.publishUserState(std::move(state),std::move(error),0,context);});t.join();}
void assertB(const std::string& entity){auto& a=AlbumSessionService::instance();
 require(a.state->tracks[1].entityId==entity&&a.state->tracks[2].entityId==entity,"Playing revision duplicates disagree");
 require(a.state->tracks[3].entityId.empty(),"Other subsong/row was associated");}
int main(int argc,char** argv){try{
 require(argc==2,"Fresh fixture required");std::filesystem::path root(argv[1]);require(!std::filesystem::exists(root),"Refusing existing fixture");
 std::filesystem::create_directories(root);NekoDatabase db;std::string error;require(db.open(root/L"state.db",error),error.c_str());
 PersistedTrack a;a.trackId="A";a.path="c:/album/shared.opus";a.subsong=0;
 require(db.setNote(a,"A note",1,error),error.c_str());
 auto b=a;b.trackId="B";b.subsong=1;
 reset();auto& d=DatabaseService::instance();auto& album=AlbumSessionService::instance();
 // The original failure: lookup returned missing; first playback creates B later.
 album.adoptRevisionIds(album.snapshot(),2,{"A","B","B","C"});worker(db);drain();assertB("");
 require(db.setFavorite(b,true,2,error),error.c_str());auto bState=stateFor(db,"B");
 auto before=album.state;emit(bState);require(album.state==before,"DB worker touched Album directly");drain();
 assertB(EXPECT_BASELINE?"":bState.entityId);
 auto stable=album.state;auto publications=album.publications;emit(bState);drain();
 require(album.state==stable&&album.publications==publications,"Repeated state caused no-op publication");
 // DB state can arrive before the revision IDs, with no generation change later.
 reset();for(auto& row:album.state->tracks)row.revisionTrackId.clear();emit(bState);drain();assertB("");
 album.adoptRevisionIds(album.snapshot(),2,{"A","B","B","C"});
 assertB(EXPECT_BASELINE?"":bState.entityId);worker(db);drain();assertB(bState.entityId);
 // Capture an earlier missing result; live publication B then playback A must survive it.
 reset();album.adoptRevisionIds(album.snapshot(),2,{"A","missing","missing","C"});worker(db);
 auto missing=std::move(qa::callbacks);qa::callbacks.clear();
 auto missingTrack=b;missingTrack.trackId="missing";
 require(db.setNote(missingTrack,"created after lookup",3,error),error.c_str());
 auto missingState=stateFor(db,"missing");
 emit(missingState);drain();playback_control::get()->playing=1;album.onNewTrack(1);
 for(auto& fn:missing)fn();
 require(album.state->tracks[1].entityId==(EXPECT_BASELINE?"":missingState.entityId),"Late missing erased live identity after playback changed");
 require(!album.state->tracks[0].entityId.empty(),"Late batch lost valid sibling A");
 // Same composition playback hook can reconcile an already available user state.
 reset();album.state->actualPlayingIndex=0;album.state->currentInSession=0;playback_control::get()->playing=1;
 emit(bState);drain();assertB("");playback_control::get()->playing=2;album.onNewTrack(2);assertB(EXPECT_BASELINE?"":bState.entityId);
 // Canonical A alias must associate exact B revision only, not C or A.
 require(db.bindKnownRevision(b,stateFor(db,"A").entityId,error),error.c_str());auto alias=stateFor(db,"B");
 require(alias.currentRevisionTrackId=="B"&&alias.canonicalTrackId=="A","Alias fixture invalid");
 reset();emit(alias);drain();assertB(EXPECT_BASELINE?"":alias.entityId);require(album.state->tracks[0].entityId.empty(),"Canonical alias leaked into nonplaying row");
 // Publication contexts/closed sessions/errors are barriers, not missing identities.
 for(int mode=0;mode<14;++mode){reset();emit(alias);
  if(mode==0)d.admittedPlayback_=std::make_shared<DatabaseService::PlaybackContext>();
  if(mode==1)d.started_=false;
  if(mode==2)d.stopping_=true;
  if(mode==3)d.publishUserStateLoading();
  if(mode==4)album.state.reset();
  if(mode==5)album.state->state=AlbumSessionState::loading;
  if(mode==6)album.state->outsideSession=true;
  if(mode==7)album.state->actualPlayingIndex.reset();
  if(mode==8)album.state->actualPlayingIndex=999;
  if(mode==9)playback_control::get()->playing=0;
  if(mode==10)playback_control::get()->playing=3;
  if(mode==11)album.state->tracks[1].revisionTrackId="other";
  if(mode==12)album.identityStopped_=true;
  if(mode==13)emit(alias,"read failure");
  auto held=album.state;auto count=album.publications;drain();
  require(album.state==held&&album.publications==count,"Invalid/stale live notification mutated Album");
 }
 // A newer note snapshot supersedes the old callback; latest valid callback works.
 reset();emit(alias);auto old=std::move(qa::callbacks);qa::callbacks.clear();emit(alias);
 auto held=album.state;for(auto& fn:old)fn();require(album.state==held,"Superseded user snapshot callback ran");drain();assertB(EXPECT_BASELINE?"":alias.entityId);
 // IDs arriving for replaced composition/handle or request are refused before DB work.
 for(int mode=0;mode<5;++mode){reset();auto source=album.snapshot();album.state=std::make_shared<AlbumSessionSnapshot>(*album.state);
  if(mode==0)++album.state->sessionId;
  if(mode==1)++album.state->compositionRevision;
  if(mode==2)++album.requestId_;
  if(mode==3)album.state->tracks[3].handle=5;
  if(mode==4)album.state->tracks.pop_back();
  held=album.state;album.adoptRevisionIds(source,2,{"A","B","B","C"});
  require(album.state==held&&d.queue_.empty(),"Stale revision IDs published or queried DB");
 }
 db.close();std::cout<<"PASS "<<checks<<" Album live identity checks; baseline="<<EXPECT_BASELINE<<'\n';
 if(EXPECT_BASELINE)std::cout<<"CONFIRMED: newly persisted playing revision remains unresolved until a later batch lookup; no live publication hook.\n";
 return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
'''


if __name__ == '__main__':
    main()
