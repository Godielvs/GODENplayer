"""Run identity admission, the real SQLite worker branch and the Album callback.

The production header/methods/branch/callback are extracted verbatim. The host
SDK, main-thread dispatcher and Album snapshot owner are replaced by test seams;
this does not qualify the foobar host, its lifecycle or rendered UI.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path,
                        default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expect-baseline', action='store_true')
    args = parser.parse_args()
    component = args.source_root / 'src/component'
    header = (component / 'database_service.h').read_text(encoding='utf-8-sig')
    source = (component / 'database_service.cpp').read_text(encoding='utf-8-sig')
    album = (component / 'album_session_service.cpp').read_text(encoding='utf-8-sig')
    methods = []
    for name in ('enqueue', 'resolveTrackIdentities'):
        match = re.search(r'^(?:void|bool) DatabaseService::' + name +
                          r'\([\s\S]*?^}\n', source, re.M)
        if not match:
            raise RuntimeError('Production method missing: ' + name)
        methods.append(match.group())
    start = source.index('        if (pending.identities) {')
    branch = source[start:source.index('        if (pending.analysis) {', start)]
    start = album.index('[sessionId =', album.index('.resolveTrackIdentities('))
    end = album.index('\n        });', start) + len('\n        }')
    callback = album[start:end]
    payload = 'struct TrackIdentityBatchPayload' in header
    if payload == args.expect_baseline:
        raise RuntimeError('Unexpected baseline/candidate identity contract')
    if header.count('private:') != 1 or header.count('#include "sdk_compat.h"') != 1:
        raise RuntimeError('Header test seam changed')
    header = header.replace('#include "sdk_compat.h"',
        'class metadb_handle_ptr; namespace play_control { enum t_stop_reason : int; }')
    header = header.replace('private:', 'public: // Test access only.')
    header = header.replace('    DatabaseService() = default;',
        '    DatabaseService() = default;\n'
        '    void runIdentityEvent(NekoDatabase&, PendingEvent&);')
    code = '#define EXPECT_BASELINE ' + str(int(args.expect_baseline)) + '\n' + PREFIX
    code += '\nnamespace nekorox::component {\n' + '\n'.join(methods)
    code += '\nvoid DatabaseService::runIdentityEvent(NekoDatabase& database, PendingEvent& pending) {\n'
    code += 'for (int once=0;once<1;++once) {\n' + branch + '\n}}\n}\n'
    code += ALBUM_SEAM + '\nreturn ' + callback + ';\n}\n}\n' + TESTS
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'database_service_test.h').write_text(header, encoding='utf-8')
    (args.output / 'oracle.cpp').write_text(code, encoding='utf-8')
    (args.output / 'source-manifest.json').write_text(json.dumps({
        'sourceRoot': str(args.source_root.resolve()), 'baseline': args.expect_baseline,
        'methodSha256': [hashlib.sha256(x.encode()).hexdigest() for x in methods],
        'branchSha256': hashlib.sha256(branch.encode()).hexdigest(),
        'albumCallbackSha256': hashlib.sha256(callback.encode()).hexdigest(),
        'limits': __doc__}, indent=2), encoding='utf-8')


PREFIX = r'''
#include "database_service_test.h"
#include "sqlite3.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
namespace qa { std::vector<std::function<void()>> callbacks; }
namespace fb2k { template<class C> void inMainThread(C c) { qa::callbacks.emplace_back(std::move(c)); } }
'''

ALBUM_SEAM = r'''
namespace nekorox::component {
struct AlbumSessionTrack { std::string revisionTrackId; std::string entityId; };
struct AlbumSessionSnapshot {
 std::uint64_t sessionId=1, requestId=2, compositionRevision=3, revision=4;
 std::vector<AlbumSessionTrack> tracks;
};
struct AlbumSessionService {
 std::shared_ptr<AlbumSessionSnapshot> state; unsigned publications=0;
 static AlbumSessionService& instance(){static AlbumSessionService s;return s;}
 std::shared_ptr<const AlbumSessionSnapshot> snapshot(){return state;}
 void publish(std::shared_ptr<AlbumSessionSnapshot> next){state=std::move(next);++publications;}
};
TrackIdentityBatchCallback albumCompletion(const std::shared_ptr<AlbumSessionSnapshot>& identitySource) {
 const auto requestId=identitySource->requestId;
 const std::shared_ptr<const AlbumSessionSnapshot> identityBaseline=std::make_shared<AlbumSessionSnapshot>(*identitySource);
'''

TESTS = r'''
using namespace nekorox;
using namespace nekorox::component;
int checks=0;
void require(bool okay,const char* why){++checks;if(!okay)throw std::runtime_error(why);}
void drain(){auto pending=std::move(qa::callbacks);qa::callbacks.clear();for(auto& fn:pending)fn();}
struct Result {std::vector<TrackIdentityResolution> ids;std::string error;int calls=0;};
#if EXPECT_BASELINE
void unpack(Result& r,std::vector<TrackIdentityResolution> p){++r.calls;r.ids=std::move(p);}
void deliver(TrackIdentityBatchCallback& fn,Result r){fn(std::move(r.ids));}
#else
void unpack(Result& r,TrackIdentityBatchPayload p){++r.calls;r.ids=std::move(p.identities);r.error=std::move(p.error);}
void deliver(TrackIdentityBatchCallback& fn,Result r){fn({std::move(r.ids),std::move(r.error)});}
#endif
void admission() {
 for(int mode=0;mode<3;++mode){
  DatabaseService service;service.started_=mode!=0;service.stopping_=mode==1;
  Result result;
  service.resolveTrackIdentities({"known"},[&](auto p){unpack(result,std::move(p));});
  require(result.calls==0,"Admission invoked callback inline");
  if(mode<2){
   require(service.queue_.empty(),"Rejected identity request queued");drain();
   require(result.calls==(EXPECT_BASELINE?0:1),"Rejected request did not complete exactly once");
   require(EXPECT_BASELINE||(!result.error.empty()&&result.ids.empty()),"Rejection lost error");
  }else{
   require(service.queue_.size()==1&&qa::callbacks.empty(),"Accepted request lost ownership");
   require(service.queue_.front().identities->revisionTrackIds==std::vector<std::string>{"known"},"Accepted payload changed");
  }
 }
 DatabaseService service;service.started_=true;
 service.resolveTrackIdentities({"known"},{});
 require(service.queue_.empty()&&qa::callbacks.empty(),"Null callback created work");
}
Result run(DatabaseService& service,NekoDatabase& db,std::vector<std::string> ids) {
 Result result;service.resolveTrackIdentities(std::move(ids),[&](auto p){unpack(result,std::move(p));});
 require(service.queue_.size()==1,"Expected single admitted batch");
 auto pending=std::move(service.queue_.front());service.queue_.pop_front();
 std::thread worker([&]{service.runIdentityEvent(db,pending);});worker.join();
 require(result.calls==0&&qa::callbacks.size()==1,"Worker did not post one completion");drain();
 require(result.calls==1,"Worker completion count wrong");return result;
}
void album(const Result& success,const Result& failure) {
 auto& owner=AlbumSessionService::instance();
 auto reset=[&]{owner.state=std::make_shared<AlbumSessionSnapshot>();owner.publications=0;
  for(const auto& id:success.ids)owner.state->tracks.push_back({id.revisionTrackId,"retained"});
  return albumCompletion(owner.state);};
 auto callback=reset();auto original=owner.state;
 // Error carries a deliberately full-size fallback too: consumer must test error.
 auto failed=failure;failed.ids=success.ids;for(auto& id:failed.ids)id.entityId.clear();
 deliver(callback,failed);
 require(EXPECT_BASELINE?owner.publications==1:owner.state==original&&owner.publications==0,
     "Failed DB result replaced Album identities");
 for(int mutation=0;mutation<6;++mutation){
  callback=reset();
  if(mutation==0)++owner.state->sessionId;
  if(mutation==1)++owner.state->requestId;
  if(mutation==2)++owner.state->compositionRevision;
  if(mutation==3)owner.state->tracks.pop_back();
  if(mutation==4)owner.state->tracks.back().revisionTrackId="different";
  if(mutation==5)owner.state.reset();
  original=owner.state;deliver(callback,success);
  require(owner.state==original&&owner.publications==0,"Stale/closed/mismatched Album callback published");
 }
 callback=reset();deliver(callback,success);
 require(owner.publications==1,"Successful changed identity batch did not publish");
 for(std::size_t i=0;i<success.ids.size();++i)
  require(owner.state->tracks[i].entityId==success.ids[i].entityId,"Album association/order incorrect");
 const auto stable=owner.state;deliver(callback,success);
 require(EXPECT_BASELINE?owner.publications==2:owner.publications==1&&owner.state==stable,
     "Identical identity batch caused redundant Album publication");
 callback=reset();for(auto& track:owner.state->tracks)track.entityId.clear();
 auto missing=success;for(auto& id:missing.ids){id.found=false;id.entityId.clear();}
 deliver(callback,missing);
 require(owner.publications==(EXPECT_BASELINE?1:0),"All-missing album republished unchanged state");
}
int main(int argc,char** argv){try{
 require(argc==2,"Fresh fixture directory required");const std::filesystem::path root(argv[1]);
 require(!std::filesystem::exists(root),"Refusing existing fixture");std::filesystem::create_directories(root);
 admission();NekoDatabase db;std::string error;require(db.open(root/L"state.db",error),error.c_str());
 PersistedTrack known;known.trackId="known";known.path="c:/known.flac";
 require(db.setNote(known,"keep this",10,error),error.c_str());
 TrackIdentityResolution identity;require(db.resolveTrackIdentity(known.trackId,identity,error),error.c_str());
 auto alias=known;alias.trackId="alias";require(db.bindKnownRevision(alias,identity.entityId,error),error.c_str());
 auto broken=known;broken.trackId="broken";require(db.setFavorite(broken,true,11,error),error.c_str());
 DatabaseService service;service.started_=true;
 const auto success=run(service,db,{"known","","missing","alias"});
 require(success.error.empty()&&success.ids.size()==4,"Mixed success batch failed");
 require(success.ids[0].found&&success.ids[0].entityId==identity.entityId,"Known identity wrong");
 require(!success.ids[1].found&&success.ids[1].revisionTrackId.empty(),"Empty placeholder lost position");
 require(!success.ids[2].found&&success.ids[2].revisionTrackId=="missing","Missing identity became error");
 require(success.ids[3].found&&success.ids[3].entityId==identity.entityId,"Alias association wrong");
 const auto empty=run(service,db,{});require(empty.ids.empty()&&empty.error.empty(),"Empty batch failed");
 sqlite3* raw=nullptr;require(sqlite3_open16((root/L"state.db").c_str(),&raw)==SQLITE_OK,"Raw DB failed");
 auto sql=[&](const char* text){char* why=nullptr;auto rc=sqlite3_exec(raw,text,nullptr,nullptr,&why);
  std::string message=why?why:"Fixture SQL failed";sqlite3_free(why);require(rc==SQLITE_OK,message.c_str());};
 sql("ALTER TABLE track_revision_bindings RENAME TO identity_fixture;");
 const auto sqlFailure=run(service,db,{"known"});
 require(EXPECT_BASELINE?sqlFailure.error.empty()&&sqlFailure.ids.size()==1:
    !sqlFailure.error.empty()&&sqlFailure.ids.empty(),"SQL failure masqueraded as missing");
 sql("CREATE VIEW track_revision_bindings AS SELECT revision_track_id, "
     "CASE WHEN revision_track_id='broken' THEN abs(-9223372036854775808) ELSE entity_id END AS entity_id "
     "FROM identity_fixture;");
 require(db.resolveTrackIdentity("known",identity,error)&&identity.found,"Partial-error fixture first read failed");
 require(!db.resolveTrackIdentity("broken",identity,error)&&!error.empty(),"Partial-error fixture did not fail");
 const auto partial=run(service,db,{"known","broken","alias"});
 require(EXPECT_BASELINE?partial.error.empty()&&partial.ids.size()==3&&partial.ids.front().found:
    !partial.error.empty()&&partial.ids.empty(),"Partial failed batch published successful prefix");
 sql("DROP VIEW track_revision_bindings;");sql("ALTER TABLE identity_fixture RENAME TO track_revision_bindings;");
 sqlite3_close_v2(raw);
 const auto restored=run(service,db,{"known","alias"});require(restored.error.empty()&&restored.ids.size()==2,"Read did not recover after SQL repair");
 PersistedTrackUserState state;require(db.readTrackUserState("known",state,error)&&state.note=="keep this","Read path changed user data");
 db.close();const auto unavailable=run(service,db,{"known"});
 require(EXPECT_BASELINE?unavailable.error.empty()&&unavailable.ids.size()==1:
    !unavailable.error.empty()&&unavailable.ids.empty(),"Closed DB masqueraded as missing");
 // A batch taken by the worker before stop still completes once.
 service.resolveTrackIdentities({"known"},[](auto){});
 auto admitted=std::move(service.queue_.front());service.queue_.pop_front();
 service.started_=false;service.stopping_=true;service.runIdentityEvent(db,admitted);
 require(qa::callbacks.size()==1,"Already admitted completion lost at stop");drain();
 album(success,unavailable);
 std::cout<<"PASS "<<checks<<" identity batch checks; baseline="<<EXPECT_BASELINE<<'\n';
 if(EXPECT_BASELINE)std::cout<<"CONFIRMED GAPS: rejected completion omitted; DB errors delivered as missing; partial SQL batch published; unchanged Album republished.\n";
 return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
'''


if __name__ == '__main__':
    main()
