"""Execute the real revision worker branch + snapshot publication with real SQLite.

Only SDK declarations, UI notifications and main-thread dispatch are stubbed.
The surrounding worker loop is replaced by a single event pump.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument('--expect-baseline', action='store_true')
    a = p.parse_args()
    repo = Path(__file__).resolve().parents[1]
    source = (a.source_root / 'src/component/database_service.cpp').read_text(encoding='utf-8-sig')
    header = (repo / 'src/component/database_service.h').read_text(encoding='utf-8-sig')
    start = source.index('        if (pending.revisionRebind) {')
    end = source.index('        if (pending.identities) {', start)
    branch = source[start:end]
    if not a.expect_baseline and 'database.bindKnownRevisions(bindings, error)' not in branch:
        raise RuntimeError('Candidate lacks atomic worker binding')
    def method(name):
        match = re.search(r'^(?:void|bool|std::shared_ptr<const TrackUserStateSnapshot>)\s+'
                          + re.escape(name) + r'\([\s\S]*?^}\n', source, re.M)
        if not match:
            raise RuntimeError('Production method missing: ' + name)
        return match.group()
    bodies = [method(n) for n in ('identifyLocalTrack',
              'DatabaseService::publishUserState', 'DatabaseService::userStateSnapshot')]
    if header.count('private:') != 1 or header.count('#include "sdk_compat.h"') != 1:
        raise RuntimeError('Header boundary changed')
    header = header.replace('#include "sdk_compat.h"',
        'class metadb_handle_ptr; namespace play_control { enum t_stop_reason : int; }')
    header = header.replace('private:', 'public: // Test access only.')
    header = header.replace('    DatabaseService() = default;',
        '    DatabaseService() = default;\n'
        '    void runBindingEvent(NekoDatabase& database, PendingEvent& pending, '
        'bool hasCurrentTrack, const PersistedTrack& currentTrack);')
    code = PREFIX + '\nnamespace nekorox::component {\n' + '\n'.join(bodies) + '''
void DatabaseService::runBindingEvent(NekoDatabase& database, PendingEvent& pending,
    bool hasCurrentTrack, const PersistedTrack& currentTrack) {
    for(int once=0;once<1;++once) {
''' + branch + '\n}}\n}\n' + TESTS
    a.output.mkdir(parents=True, exist_ok=True)
    (a.output / 'database_service_test.h').write_text(header, encoding='utf-8')
    (a.output / 'oracle.cpp').write_text(
        '#define EXPECT_BASELINE ' + str(int(a.expect_baseline)) + '\n' + code, encoding='utf-8')
    (a.output / 'source-manifest.json').write_text(json.dumps({
        'source': str(a.source_root.resolve()),
        'branchSha256': hashlib.sha256(branch.encode()).hexdigest(),
        'methodsSha256': [hashlib.sha256(b.encode()).hexdigest() for b in bodies],
        'limits': __doc__, 'expectBaseline': a.expect_baseline}, indent=2), encoding='utf-8')


PREFIX = r'''
#include "database_service_test.h"
#include "nekorox/track_identity.h"
#include "sqlite3.h"
#include <windows.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <fstream>
namespace qa { std::vector<std::function<void()>> callbacks; int notifications=0; }
namespace fb2k { template<class C> void inMainThread(C c) { qa::callbacks.emplace_back(std::move(c)); } }
namespace nekorox::component {
struct AlbumSessionService {static AlbumSessionService& instance(){static AlbumSessionService a;return a;}
 void refreshPlayingIdentity(){} }; // Independently exercised by album_live_identity_oracle.py.
struct Runtime { static Runtime& instance(){static Runtime r;return r;}
 void refreshPresentationState(){++qa::notifications;} };
void notifyWorkspaceModelChanged(){} void postListenRefresh(){}
std::int64_t utcNowMs(){return std::chrono::duration_cast<std::chrono::milliseconds>(
 std::chrono::system_clock::now().time_since_epoch()).count();}
std::uint64_t fileTimeTicks(const std::filesystem::file_time_type& t){return t.time_since_epoch().count();}
std::string wideToUtf8(const std::wstring& text) {
 int size=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
 std::string result(size,'\0');
 WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),size,nullptr,nullptr);
 return result;
}
}
'''

TESTS = r'''
using namespace nekorox;
using namespace nekorox::component;
int checks=0;
void require(bool okay,const char* why){++checks;if(!okay)throw std::runtime_error(why);}
int main(int argc,char** argv) {try{
 require(argc==2,"Fresh test directory required");
 const std::filesystem::path root(argv[1]);
 require(!std::filesystem::exists(root),"Refusing existing fixture directory");
 std::filesystem::create_directories(root);
 NekoDatabase db;std::string error;require(db.open(root/"state.db",error),error.c_str());
 PersistedTrack old;old.trackId="old";old.path="c:/old.flac";
 auto edited=old;edited.trackId="edited";
 require(db.setFavorite(old,true,10,error),error.c_str());
 require(db.setNote(old,"old note",10,error),error.c_str());
 require(db.setFavorite(edited,false,20,error),error.c_str());
 require(db.setNote(edited,"new note",20,error),error.c_str());
 DatabaseService service;
 service.admittedPlayback_=std::make_shared<DatabaseService::PlaybackContext>();
 service.admittedPlayback_->track=edited;
 auto run=[&](std::vector<TrackRevisionRebind> requests,bool expectedSuccess,
              const PersistedTrack& playing) {
  DatabaseService::PendingEvent event;
  event.revisionRebind=std::make_unique<DatabaseService::RevisionRebindTask>();
  event.revisionRebind->revisions=std::move(requests);
  int called=0;bool success=false;std::string failure;
  event.revisionRebind->callback=[&](bool okay,std::string why){++called;success=okay;failure=why;};
  service.runBindingEvent(db,event,true,playing);
  require(called==0&&!qa::callbacks.empty(),"Completion was not dispatched to main");
  // A valid state publication may also notify Album. Count the operation's
  // completion itself, independently of other main-thread observers.
  while(!qa::callbacks.empty()){
   auto callbacks=std::move(qa::callbacks);qa::callbacks.clear();for(auto& callback:callbacks)callback();
  }
  require(called==1&&success==expectedSuccess,"Worker completion reported wrong result");
  require(expectedSuccess?failure.empty():!failure.empty(),"Completion error detail mismatch");
 };
 run({{old.trackId,L"c:/unavailable.flac",0,edited}},true,edited);
 auto snapshot=service.userStateSnapshot();
 require(snapshot->available&&snapshot->revisionTrackId==edited.trackId,"Playing snapshot not refreshed");
 require(EXPECT_BASELINE ? snapshot->favorite&&snapshot->note=="old note" :
    !snapshot->favorite&&snapshot->note=="new note","Snapshot did not reflect expected state merge");
 // Playback can move back to an existing canonical/alias before the bind runs.
 // The merge changes that entity too, even though its playing revision is old.
 auto another=old;another.trackId="another";
 require(db.setNote(another,"alias latest",25,error),error.c_str());
 service.admittedPlayback_->track=old;
 const auto beforeAlias=service.userStateSnapshot()->generation;
 run({{old.trackId,L"missing",0,another}},true,old);
 require(EXPECT_BASELINE ? service.userStateSnapshot()->generation==beforeAlias :
    service.userStateSnapshot()->generation>beforeAlias&&
    service.userStateSnapshot()->revisionTrackId==old.trackId&&
    service.userStateSnapshot()->note=="alias latest",
    "State merge left the playing target alias snapshot stale");
 auto first=old;first.trackId="batch-first";
 auto second=old;second.trackId="batch-second";
 require(db.setNote(first,"pending first",40,error),error.c_str());
 service.admittedPlayback_->track=first;
 const auto before=service.userStateSnapshot()->generation;
 auto invalid=second;invalid.subsong=7;
 run({{old.trackId,L"missing",0,first},{old.trackId,L"missing",0,invalid}},false,first);
 TrackIdentityResolution id;require(db.resolveTrackIdentity(first.trackId,id,error),error.c_str());
 require(EXPECT_BASELINE?id.entityId=="entity:v1:old":id.entityId=="entity:v1:batch-first",
    "Later invalid frozen revision left wrong batch state");
 require(EXPECT_BASELINE?service.userStateSnapshot()->generation>before:
    service.userStateSnapshot()->generation==before,"Failed batch incorrectly published new snapshot");
 // Fail after the first actual SQL update, not just during request preparation.
 sqlite3* raw=nullptr;require(sqlite3_open16((root/L"state.db").c_str(),&raw)==SQLITE_OK,"Raw DB open failed");
 auto sql=[&](const char* text){require(sqlite3_exec(raw,text,nullptr,nullptr,nullptr)==SQLITE_OK,"Fixture SQL failed");};
 sql("CREATE TRIGGER later_failure BEFORE UPDATE ON track_revision_bindings "
     "WHEN NEW.revision_track_id='batch-second' BEGIN SELECT RAISE(ABORT,'later failure'); END;");
 auto third=old;third.trackId="batch-third";
 service.admittedPlayback_->track=third;
 const auto beforeSql=service.userStateSnapshot()->generation;
 run({{old.trackId,L"missing",0,third},{old.trackId,L"missing",0,second}},false,third);
 require(db.resolveTrackIdentity(third.trackId,id,error),error.c_str());
 require(EXPECT_BASELINE?id.found:!id.found,"SQL failure did not roll back earlier inserted revision");
 require(EXPECT_BASELINE?service.userStateSnapshot()->generation>beforeSql:
    service.userStateSnapshot()->generation==beforeSql,"SQL rollback published current state");
 sql("DROP TRIGGER later_failure;");sqlite3_close_v2(raw);
 run({{old.trackId,L"missing",0,third},{old.trackId,L"missing",0,second}},true,third);
 require(db.resolveTrackIdentity(second.trackId,id,error)&&id.entityId=="entity:v1:old",
    "Independent batch did not bind both revisions");
 // A read/identity failure for a later live path must also leave the first untouched.
 auto live=old;live.trackId="live-first";
 run({{old.trackId,L"missing",0,live},{old.trackId,root/L"absent.flac",0,{}}},false,third);
 require(db.resolveTrackIdentity(live.trackId,id,error),error.c_str());
 require(EXPECT_BASELINE?id.found:!id.found,"Live path failure committed earlier frozen revision");
 // Unknown old identity has no state to transfer; it remains a successful no-op.
 run({{"unknown",root/L"absent.flac",0,{}}},true,third);
 // Old playback publication must never replace a newer admitted track.
 auto obsolete=service.admittedPlayback_;
 service.admittedPlayback_=std::make_shared<DatabaseService::PlaybackContext>();
 service.admittedPlayback_->track=old;
 const auto stable=service.userStateSnapshot()->generation;
 PersistedTrackUserState stale;stale.found=true;stale.note="must not publish";
 service.publishUserState(stale,{},0,obsolete);
 require(service.userStateSnapshot()->generation==stable,"Stale playback overwrote snapshot");
 run({{old.trackId,L"missing",0,third}},true,third);
 require(service.userStateSnapshot()->generation==stable,"Unrelated playback was refreshed");
 db.close();
 run({{old.trackId,L"missing",0,third}},false,third);
 std::cout<<"PASS "<<checks<<" real database worker checks; baseline="<<EXPECT_BASELINE<<'\n';
 return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
'''

if __name__ == '__main__':
    main()
