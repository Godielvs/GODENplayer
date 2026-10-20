"""Generate a C++20 oracle using verbatim TagEditService capturePostWrite code.

SDK handle declarations, hashing/identity I/O, journal and next-stage dispatch are
stubbed. The real capture method, Operation, Revision and public summary types
are extracted so the test exercises worker/publication ownership and affinity.
Actual hashes, journal durability and Undo are qualified separately in the host.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    component = args.source_root / 'src/component'
    header = (component / 'tag_edit_service.h').read_text(encoding='utf-8-sig')
    source = (component / 'tag_edit_service.cpp').read_text(encoding='utf-8-sig')
    def take(pattern):
        found = re.search(pattern, source, re.M)
        if not found:
            raise RuntimeError('Production boundary changed: ' + pattern)
        return found.group()
    revision = take(r'^struct Revision \{[\s\S]*?^};')
    operation = take(r'^struct TagEditService::Operation \{[\s\S]*?^};')
    method = take(r'^void TagEditService::capturePostWrite\([\s\S]*?^}\n')
    assert header.count('#include "sdk_compat.h"') == 1 and header.count('private:') == 1
    header = header.replace('#include "sdk_compat.h"', '#define NOMINMAX\n#include <windows.h>\n'
                            'struct metadb_handle_ptr {}; struct metadb_handle_list {};\n'
                            'using metadb_handle_list_cref = const metadb_handle_list&;')
    header = header.replace('private:', 'public: // Oracle only.')
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'tag_service_test.h').write_text(header, encoding='utf-8')
    (args.output / 'oracle.cpp').write_text(
        PREFIX + '\nnamespace nekorox::component {\n' + revision + '\n' + operation + '\n'
        + STUBS + '\n' + method + '\n}\n' + TESTS, encoding='utf-8')
    (args.output / 'source-manifest.json').write_text(json.dumps({
        'sourceRoot': str(args.source_root.resolve()),
        'methodSha256': hashlib.sha256(method.encode()).hexdigest(),
        'limits': 'Identity/hash I/O, SDK handles, journal and next dispatch are stubs',
    }, indent=2), encoding='utf-8')


PREFIX = r'''
#include "tag_service_test.h"
#include "nekorox/track_identity.h"
#include <algorithm>
#include <map>
#include <optional>
#include <iostream>
#include <stdexcept>
#include <condition_variable>

namespace qa {
const auto mainThread = std::this_thread::get_id();
std::mutex mutex;
std::vector<std::function<void()>> callbacks;
int checks=0, hashCalls=0, uiHashCalls=0, identityCalls=0, uiIdentityCalls=0;
int journalCalls=0, nextCalls=0, throwAt=-1, identityDelayMs=0;
bool failHash=false, failRevision=false;
std::vector<nekorox::TrackIdentityInput> inputs;
void require(bool value,const char* label) { ++checks; if(!value) throw std::runtime_error(label); }
void reset() {
    callbacks.clear(); hashCalls=uiHashCalls=identityCalls=uiIdentityCalls=0;
    journalCalls=nextCalls=0;throwAt=-1;identityDelayMs=0;failHash=failRevision=false;inputs.clear();
}
void pump() {
    std::vector<std::function<void()>> pending;
    {std::lock_guard lock(mutex);pending.swap(callbacks);}
    require(pending.size()==1,"Exactly one publication callback");
    for(auto& call:pending) call();
}
}
namespace fb2k {
template<class Callback> void inMainThread(Callback callback) {
    std::lock_guard lock(qa::mutex);qa::callbacks.emplace_back(std::move(callback));
}
}
namespace nekorox {
NekoTrackId makeTrackId(const TrackIdentityInput& input) {
    ++qa::identityCalls;
    if(std::this_thread::get_id()==qa::mainThread) ++qa::uiIdentityCalls;
    qa::inputs.push_back(input);
    if(qa::identityDelayMs) std::this_thread::sleep_for(std::chrono::milliseconds(qa::identityDelayMs));
    if(qa::identityCalls==qa::throwAt) throw std::runtime_error("Injected identity failure");
    return {"identity-"+std::to_string(input.subsong),{}, {}};
}
}
'''

STUBS = r'''
struct TagEditService::RecoveryState {};
TagEditService::~TagEditService() { if(worker_.joinable()) worker_.join(); }
void TagEditService::startNext(std::shared_ptr<Operation>) {
    qa::require(std::this_thread::get_id()==qa::mainThread,"Advance stays on main");++qa::nextCalls;
}
bool revisionOf(const std::filesystem::path&,Revision& out) noexcept {
    out.size=300000;out.writeTime=std::filesystem::file_time_type(std::filesystem::file_time_type::duration(42));
    return !qa::failRevision;
}
bool sha256File(const std::filesystem::path&,std::string& out,std::stop_token stop) {
    ++qa::hashCalls;if(std::this_thread::get_id()==qa::mainThread) ++qa::uiHashCalls;
    if(qa::failHash||stop.stop_requested())return false;
    out=std::string(64,'a');return true;
}
bool persistJournal(const TagApplySummary&) {
    qa::require(std::this_thread::get_id()==qa::mainThread,"Journal publication remains serialized on main");
    ++qa::journalCalls;return true;
}
'''

TESTS = r'''
using namespace nekorox::component;
using qa::require;
using Service=TagEditService;
bool baseline=false;
std::shared_ptr<Service::Operation> setup(Service& s) {
    auto op=std::make_shared<Service::Operation>();op->generation=11;op->activeIndex=0;
    Service::Operation::PhysicalFile file;file.path=L"synthetic.flac";file.resultIndex=0;
    file.itemResultIndices={2,0,1};op->physical.push_back(std::move(file));
    TagFileResult result;result.sourceHash=std::string(64,'b');result.backupPath=L"verified.backup";
    result.outcome=TagFileOutcome::mutationIntentDurable;op->summary.files.push_back(std::move(result));
    for(unsigned subsong:{1u,7u,42u}){TagItemResult item;item.subsong=subsong;op->summary.items.push_back(item);}
    s.active_=op;s.generation_=11;return op;
}
void startAndJoin(Service& s,const auto& op){s.capturePostWrite(op,0);s.worker_.join();}
bool emptyIDs(const auto& op) {return std::all_of(op->summary.items.begin(),op->summary.items.end(),[](const auto& i){return i.afterRevision.empty();});}
void success() {
    qa::reset();Service s;auto op=setup(s);qa::identityDelayMs=10;
    startAndJoin(s,op);
    require(emptyIDs(op)&&op->summary.files[0].postWriteHash.empty(),"Worker never mutates live summary");
    const auto begin=std::chrono::steady_clock::now();qa::pump();
    const auto uiMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    require(qa::identityCalls==3&&qa::uiIdentityCalls==(baseline?3:0)&&qa::uiHashCalls==0,"Identity and hash thread affinity");
    for(const auto& input:qa::inputs) require(input.sizeBytes==300000&&input.modifiedTicks==42&&input.path==L"synthetic.flac","Frozen revision input unchanged");
    for(const auto& item:op->summary.items)require(item.afterRevision=="identity-"+std::to_string(item.subsong),"Every subsong maps to its result index");
    require(op->summary.files[0].postWriteHash==std::string(64,'a')&&op->summary.files[0].outcome==TagFileOutcome::bindingPending,"Successful verification awaits DB binding");
    require(!op->activeIndex&&qa::nextCalls==1&&qa::journalCalls==1,"Publication advances exactly once");
    std::cout<<"identity_calls="<<qa::identityCalls<<" ui_identity_calls="<<qa::uiIdentityCalls<<" callback_ms_with_10ms_per_identity_injected="<<uiMs<<'\n';
}
void identityFailure() {
    qa::reset();Service s;auto op=setup(s);qa::throwAt=2;
    startAndJoin(s,op);bool threw=false;try{qa::pump();}catch(const std::exception&){threw=true;}
    require(threw==baseline,"Identity failure cannot escape publication/worker");
    if(baseline){require(op->activeIndex.has_value()&&qa::nextCalls==0&&!emptyIDs(op),"Baseline reproduces partial identity and stuck operation");}
    else{
        require(emptyIDs(op),"Failed identity publishes no partial IDs");
        require(op->summary.files[0].outcome==TagFileOutcome::recoverablePartial&&op->summary.files[0].postWriteHash==std::string(64,'a'),"Identity failure retains verified full hash for guarded Undo");
        require(!op->activeIndex&&qa::nextCalls==1&&qa::journalCalls==1,"Failure settles once");
    }
}
void rejected(bool revision) {
    qa::reset();Service s;auto op=setup(s);qa::failHash=!revision;qa::failRevision=revision;
    startAndJoin(s,op);qa::pump();
    require(qa::identityCalls==0&&emptyIDs(op),"Unavailable hash/revision never starts identities");
    require(!op->activeIndex&&op->summary.files[0].outcome==TagFileOutcome::recoverablePartial,"Unavailable file remains recoverable");
}
void cancelBeforeAdoption() {
    qa::reset();Service s;auto op=setup(s);startAndJoin(s,op);
    s.generation_.fetch_add(1);qa::pump();
    require(op->summary.files[0].outcome==(baseline?TagFileOutcome::bindingPending:TagFileOutcome::recoverablePartial),"Cancellation after hash is not falsely bound");
    require(qa::nextCalls==1&&!op->activeIndex,"Cancelled active write still settles");
    if(!baseline)require(emptyIDs(op)&&op->summary.files[0].postWriteHash.size()==64,"Cancelled completed hash retains guarded Undo but not success IDs");
}
void drainedOrReplaced(bool replaced) {
    qa::reset();Service s;auto op=setup(s);startAndJoin(s,op);
    s.active_=replaced?std::make_shared<Service::Operation>():nullptr;
    qa::pump();
    require(qa::journalCalls==(baseline?1:0)&&qa::nextCalls==(baseline?1:0),"Detached operation cannot publish journal or restart work");
    if(!baseline)require(emptyIDs(op)&&op->summary.files[0].postWriteHash.empty(),"Detached summary untouched");
}
void invalidAtAdoption() {
    qa::reset();Service s;auto op=setup(s);startAndJoin(s,op);op->summary.items.resize(1);qa::pump();
    if(!baseline){require(emptyIDs(op)&&op->summary.files[0].outcome==TagFileOutcome::recoverablePartial,"Result indices validated before any assignment");}
    require(!op->activeIndex&&qa::nextCalls==1,"Invalid result settles");
}
int main(int argc,char** argv){baseline=argc>1&&std::string(argv[1])=="baseline";
 try{success();identityFailure();rejected(false);rejected(true);cancelBeforeAdoption();drainedOrReplaced(false);drainedOrReplaced(true);invalidAtAdoption();
 std::cout<<"PASS checks="<<qa::checks<<" mode="<<(baseline?"baseline":"candidate")<<'\n';}
 catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
'''

if __name__ == '__main__':
    main()
