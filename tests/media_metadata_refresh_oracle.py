"""Generate a deterministic oracle from the real media service state machine.

The SDK readers/hints, filesystem mutation, journal and main-thread dispatcher
are stubs. Production pump, Undo, finish and shutdown methods are extracted
verbatim. Native qualification separately checks actual file bytes and UI cache.
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
    parser.add_argument('--expect-baseline', action='store_true')
    args = parser.parse_args()
    component = args.source_root / 'src/component'
    source = (component / 'media_file_operation_service.cpp').read_text(encoding='utf-8-sig')
    identity = 'captureMediaRevisions(' in source
    if not args.expect_baseline and not identity:
        raise RuntimeError('Candidate is missing MEDIA identity integration')
    header = (component / 'media_file_operation_service.h').read_text(encoding='utf-8-sig')
    names = ['sameNativePath', 'needsMetadataRefresh', 'collectMetadataHints', 'publishMetadataHints']
    names += ['MediaFileOperationService::' + name for name in
              ['busy', 'mutationAllowed', 'beginBindingRecovery', 'hasInPlaceUndo', 'undoOperationSequence', 'undoLastInPlace',
               'playbackProtects', 'pump', 'finishOperation', 'beginShutdown', 'shutdownAndDrain']]
    methods = []
    for name in names:
        match = re.search(r'^(?:bool|void|std::uint64_t) ' + re.escape(name) + r'\([\s\S]*?^}\n', source, re.M)
        if not match:
            raise RuntimeError('Production boundary changed: ' + name)
        methods.append(match.group())
    assert header.count('#include "sdk_compat.h"') == 1 and header.count('private:') == 1
    header = header.replace('#include "sdk_compat.h"', '#include "sdk_stub.h"')
    header = header.replace('private:', 'public: // Oracle only.')
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'media_service_test.h').write_text(header, encoding='utf-8')
    (args.output / 'sdk_stub.h').write_text(SDK, encoding='utf-8')
    (args.output / 'oracle.cpp').write_text(
        '#define MEDIA_IDENTITY ' + str(int(identity)) + '\n' + PREFIX +
        '\nnamespace nekorox::component {\n' + STUBS + '\n' +
        '\n'.join(methods) + '\n}\n' + TESTS, encoding='utf-8')
    (args.output / 'source-manifest.json').write_text(json.dumps({
        'sourceRoot': str(args.source_root.resolve()),
        'methods': {name: hashlib.sha256(code.encode()).hexdigest() for name, code in zip(names, methods)},
        'limits': 'SDK I/O, file mutations, journal, callbacks are stubs; no native performance claim',
    }, indent=2), encoding='utf-8')


SDK = r'''
#pragma once
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
namespace qa {
inline const auto mainThread=std::this_thread::get_id();
inline int checks=0, creates=0, reads=0, adds=0, publishes=0, executes=0, notifications=0, journals=0;
inline int failJournal=0, failRead=0, failAdd=0, failCreate=0;
inline bool failPublish=false;
inline bool failCapture=false, failBinding=false, holdBinding=false;
inline bool failReceipt=false;inline int receipts=0;
inline bool postCommitFailure=false;inline std::function<void()> afterExecute;
inline int captures=0, freezes=0, bindings=0;
inline std::vector<std::function<void(bool,std::string)>> bindingCallbacks;
inline std::function<void()> onPublish;
inline std::mutex dispatchMutex;
inline std::vector<std::function<void()>> callbacks;
inline void require(bool value,const char* label){++checks;if(!value)throw std::runtime_error(label);}
inline void reset(){creates=reads=adds=publishes=executes=notifications=journals=0;
    failJournal=failRead=failAdd=failCreate=0;failPublish=false;onPublish={};callbacks.clear();
    failCapture=failBinding=holdBinding=failReceipt=postCommitFailure=false;afterExecute={};receipts=0;captures=freezes=bindings=0;bindingCallbacks.clear();}
}
template<class T> struct fake_ptr {
    std::shared_ptr<T> p;
    fake_ptr()=default; explicit fake_ptr(std::shared_ptr<T> value):p(std::move(value)){}
    bool is_valid()const{return !!p;} void release(){p.reset();} T* operator->()const{return p.get();}
};
struct metadb_handle_ptr {};
struct abort_callback_impl {std::atomic<bool> aborted=false; void abort(){aborted=true;}
    void check(){if(aborted)throw std::runtime_error("Cancelled");}};
namespace pfc {using string8=std::string;}
struct filesystem {static void g_get_canonical_path(const char* path,pfc::string8& out){out=path;}};
struct input_info_reader {using ptr=fake_ptr<input_info_reader>;std::string path;};
struct input_entry {static void g_open_for_info_read(input_info_reader::ptr& out,void*,const char* path,abort_callback_impl& abort){
    qa::require(std::this_thread::get_id()!=qa::mainThread,"Metadata read must stay off UI");
    abort.check();if(++qa::reads==qa::failRead)throw std::runtime_error("Read failed");
    out=input_info_reader::ptr(std::make_shared<input_info_reader>());out->path=path;
}};
struct metadb_hint_list {
    using ptr=fake_ptr<metadb_hint_list>;
    std::vector<std::string> paths;
    static ptr create(){if(++qa::creates==qa::failCreate)throw std::runtime_error("Factory failed");return ptr(std::make_shared<metadb_hint_list>());}
    void add_hint_reader(const char* path,const input_info_reader::ptr& reader,abort_callback_impl& abort){
        qa::require(std::this_thread::get_id()!=qa::mainThread,"Hint reads must stay off UI");abort.check();
        qa::require(reader->path==path,"Reader corresponds to final path");
        if(++qa::adds==qa::failAdd)throw std::runtime_error("Hint failed");paths.emplace_back(path);
    }
    void on_done(){qa::require(std::this_thread::get_id()==qa::mainThread,"Publication must run on UI");
        ++qa::publishes;if(qa::onPublish)qa::onPublish();if(qa::failPublish)throw std::runtime_error("Publish failed");}
};
namespace fb2k {template<class C>void inMainThread(C callback){std::lock_guard lock(qa::dispatchMutex);qa::callbacks.emplace_back(std::move(callback));}}
struct library_file_move_scope {using ptr=fake_ptr<library_file_move_scope>;};
struct library_file_move_manager {static library_file_move_manager* get(){static library_file_move_manager value;return &value;}
    library_file_move_scope::ptr acquire_scope(){return library_file_move_scope::ptr(std::make_shared<library_file_move_scope>());}};
'''

PREFIX = r'''
#include "media_service_test.h"
#include "media_binding_recovery.h"
#include <iostream>
#include <fstream>
#include <chrono>
namespace qa {inline std::vector<nekorox::component::MediaCopyResult> restoreResults, restoreInputs;}
namespace qa {inline nekorox::component::MediaBindingRecoveryScan recoveryScan;}
struct console {static void print(const char*){}};
'''

STUBS = r'''
std::string wideToUtf8(std::wstring_view value){std::string out;for(wchar_t c:value)out.push_back(static_cast<char>(c));return out;}
std::wstring utf8ToWide(std::string_view value){return std::wstring(value.begin(),value.end());}
std::filesystem::path journalPath(){return L"fixture-journals/item.json";}
struct TagEditService {static TagEditService& instance(){static TagEditService value;return value;}bool busy(){return false;}};
MediaBindingRecoveryScan scanMediaBindingRecovery(const std::filesystem::path&,std::stop_token){return qa::recoveryScan;}
bool acknowledgeMediaBindingRecovery(const MediaBindingRecoveryItem&,std::stop_token stop){
    qa::require(std::this_thread::get_id()!=qa::mainThread,"Receipt I/O stays off UI");
    ++qa::receipts;return !stop.stop_requested()&&!qa::failReceipt;}
std::uint64_t nextOperationSequence(){static std::uint64_t seq=200;return ++seq;}
MediaFileOperationService::~MediaFileOperationService(){shutdownAndDrain();}
bool MediaFileOperationService::persistQueueLocked()const{return ++qa::journals!=qa::failJournal;}
std::filesystem::path MediaFileOperationService::itemJournalPath(const PendingWork&)const{return L"fake-journal";}
void notifyOperations(const std::vector<MediaCopyResult>&){++qa::notifications;}
#if MEDIA_IDENTITY
struct TrackRevisionRebind {std::string previousRevisionTrackId;std::filesystem::path currentPath;
    std::uint32_t subsong=0;std::optional<PersistedTrack> frozenRevision;};
struct DatabaseService {
    static DatabaseService& instance(){static DatabaseService value;return value;}
    void rebindTagRevisions(std::vector<TrackRevisionRebind> requests,std::function<void(bool,std::string)> callback){
        qa::require(std::this_thread::get_id()==qa::mainThread,"Rebind admission is on main");++qa::bindings;
        qa::require(!requests.empty(),"No empty binding request");
        for(const auto& r:requests){qa::require(r.frozenRevision.has_value()&&!r.frozenRevision->trackId.empty(),"Frozen postcommit identity submitted");
            qa::require(r.subsong==7||r.subsong==19,"Physical subsong IDs preserved, not enumeration indices");}
        if(qa::holdBinding)qa::bindingCallbacks.push_back(std::move(callback));
        else fb2k::inMainThread([callback=std::move(callback)]{callback(!qa::failBinding,qa::failBinding?"Injected DB failure":"");});
    }
};
std::vector<MediaRevisionBinding> captureMediaRevisions(const std::filesystem::path&,std::stop_token){
    qa::require(std::this_thread::get_id()!=qa::mainThread,"Old identity capture stays on worker");++qa::captures;
    if(qa::failCapture)throw std::runtime_error("Capture failed");
    std::vector<MediaRevisionBinding> out;for(auto id:{7u,19u}){MediaRevisionBinding b;b.previousRevisionTrackId="old-"+std::to_string(id);b.current.subsong=id;out.push_back(b);}return out;
}
void freezeMediaRevisions(MediaCopyResult& r,std::vector<MediaRevisionBinding> old){
    if(!((r.outcome==MediaCopyOutcome::copied||r.outcome==MediaCopyOutcome::moved)&&r.source==r.destination)||old.empty())return;
    qa::require(std::this_thread::get_id()!=qa::mainThread,"New identity capture stays on worker");++qa::freezes;
    r.revisionBindings=std::move(old);r.identityRefreshFailed=false;
    for(auto& b:r.revisionBindings){b.current.trackId="frozen-"+std::to_string(b.current.subsong);b.current.path="file.flac";}
}
#endif
std::vector<MediaCopyResult> executeMediaPlan(const MediaPlan& plan,std::stop_token,const MediaCopyExecutionOptions& options){
    ++qa::executes;std::vector<MediaCopyResult> out;
    for(const auto& item:plan.items){MediaCopyResult r;r.source=item.source;r.destination=item.destination;
        r.backup=L"verified.backup";r.outcome=MediaCopyOutcome::copied;
        if(options.captureSourceRevisions){try{auto old=options.captureSourceRevisions(item.source);
            freezeMediaRevisions(r,std::move(old));r.bindingJournalPath=L"fixture-journal";r.bindingJournalSha256="hash";
            r.physicalCommitNeedsBinding=true;}
            catch(...){r.outcome=MediaCopyOutcome::failed;}}
        std::wstring error;if(options.mutateStagingForSource&&!options.mutateStagingForSource(item.source,L"staging",error))r.outcome=MediaCopyOutcome::failed;
        if(qa::postCommitFailure){r.outcome=MediaCopyOutcome::failed;r.physicalCommitNeedsBinding=true;}
        out.push_back(r);}if(qa::afterExecute)qa::afterExecute();return out;
}
std::vector<MediaCopyResult> restoreInPlaceBackups(const std::vector<MediaCopyResult>& input,std::stop_token,const MediaRestoreExecutionOptions& options){
    qa::restoreInputs=input;auto out=qa::restoreResults;
    for(auto& r:out){if(options.allowRestore&&!options.allowRestore(r.destination))r.outcome=MediaCopyOutcome::skipped;
        if(r.outcome==MediaCopyOutcome::moved&&!r.revisionBindings.empty()){
            freezeMediaRevisions(r,r.revisionBindings);r.physicalCommitNeedsBinding=true;}}
    return out;
}
'''

TESTS = r'''
using namespace nekorox::component;
using qa::require;using Service=MediaFileOperationService;
MediaCopyResult result(MediaCopyOutcome outcome=MediaCopyOutcome::copied,const wchar_t* path=L"file.flac"){
    MediaCopyResult r;r.source=r.destination=path;r.backup=L"verified.backup";r.outcome=outcome;return r;
}
void setup(Service& s){s.active_=s.workerActive_=true;s.activeOperationSequence_=11;s.activePlan_.mode=MediaOrganizationMode::inPlace;}
void collect(std::vector<MediaCopyResult>& results,metadb_hint_list::ptr& hints,std::stop_token stop={}){
    std::jthread worker([&]{collectMetadataHints(results,hints,stop);});worker.join();
}
void drain(Service& s){
    while(true){if(s.worker_.joinable())s.worker_.join();std::vector<std::function<void()>> calls;
        {std::lock_guard lock(qa::dispatchMutex);calls.swap(qa::callbacks);}if(calls.empty())break;
        for(auto& call:calls)call();}
}
void batchAndAffinity(){
    qa::reset();Service s;setup(s);metadb_hint_list::ptr hints;
    std::vector<MediaCopyResult> a{result()},b{result(MediaCopyOutcome::copied,L"next.flac")};
    collect(a,hints);collect(b,hints);a.insert(a.end(),b.begin(),b.end());s.metadataHints_=hints;
    require(qa::creates==1&&qa::adds==2&&qa::publishes==0,"Serial workers share one uncommitted batch");
    bool completed=false;
    qa::onPublish=[&]{require(s.busy()&&s.workerActive_,"Admission remains closed through SDK reentrancy");
        require(!s.hasInPlaceUndo(),"Undo unavailable during publication");s.pump();require(qa::executes==0,"Reentrant pump cannot start a worker");};
    s.finishOperation(a,[&](auto out){completed=true;require(!s.busy(),"Completion opens admission");
        require(out.size()==2&&!out[0].metadataRefreshFailed,"Successful output intact");},11,false,hints);
    require(completed&&qa::publishes==1&&s.inPlaceUndo_.size()==2&&s.undoOperationSequence()==11,"Publish once with full Undo retained");
    require(!s.metadataHints_.is_valid(),"Operation releases retained hint list");
    s.finishOperation(a,[](auto){throw std::runtime_error("Duplicate completion");},11,false,hints);
    require(qa::publishes==1,"Duplicate callback suppressed");
}
void refreshFailures(){
    for(int failure=0;failure<5;++failure){qa::reset();Service s;setup(s);metadb_hint_list::ptr hints;
        auto r=result();r.sha256="verified-after";r.sourceSha256="verified-before";
        std::vector<MediaCopyResult> results{r};std::stop_source stop;
        if(failure==0)qa::failRead=1;if(failure==1)qa::failCreate=1;if(failure==2)qa::failAdd=1;
        if(failure==3)stop.request_stop();if(failure==4)qa::failPublish=true;
        collect(results,hints,stop.get_token());bool completed=false;
        s.finishOperation(results,[&](auto out){completed=true;require(out[0].metadataRefreshFailed,"Cache failure separately reported");
            require(out[0].outcome==MediaCopyOutcome::copied&&out[0].sha256==r.sha256&&out[0].sourceSha256==r.sourceSha256,"Cache failure cannot falsify physical result");},11,false,hints);
        require(completed&&s.inPlaceUndo_.size()==1&&s.hasInPlaceUndo(),"All refresh failures preserve verified Undo");
    }
    qa::reset();std::vector<MediaCopyResult> mixed{result(),result(MediaCopyOutcome::failed),result(MediaCopyOutcome::cancelled),result()};
    mixed.back().destination=L"other.flac";metadb_hint_list::ptr hints;collect(mixed,hints);
    require(qa::reads==1&&qa::adds==1,"Only successful in-place writes are read");
    require(!mixed[1].metadataRefreshFailed&&!mixed[2].metadataRefreshFailed&&!mixed[3].metadataRefreshFailed,"Unrelated outcomes stay unchanged");
}
void ownership(){
    for(int mode=0;mode<4;++mode){qa::reset();Service s;setup(s);metadb_hint_list::ptr hints;
        std::vector<MediaCopyResult> results{result()};collect(results,hints);
        if(mode==0)s.active_=false;if(mode==1)s.shuttingDown_=true;if(mode==2)s.activeOperationSequence_=99;
        if(mode==3)qa::onPublish=[&]{s.beginShutdown();};
        bool completed=false;s.finishOperation(results,[&](auto){completed=true;},11,false,hints);
        require(!completed&&s.inPlaceUndo_.empty(),"Stale/shutdown owner cannot publish completion or Undo");
        require(qa::publishes==(mode==3?1:0),"Stale owner checked before SDK publication");
    }
}
void queueBatch(int failure){
    qa::reset();Service s;setup(s);s.workerActive_=false;qa::failJournal=failure;
    for(int i=0;i<3;++i){Service::PendingWork work;work.sequence=i;work.item.source=work.item.destination=L"track-"+std::to_wstring(i)+L".flac";
        s.pending_.push_back(work);}
    int count=0;std::vector<MediaCopyResult> output;s.completion_=[&](auto out){++count;output=std::move(out);};
    s.pump();drain(s);
    require(count==1&&!s.busy()&&!s.workerActive_,"Queue completes once and reopens admission");
    if(failure==0){require(qa::executes==3&&qa::creates==1&&qa::publishes==1,"Whole queue batches a single SDK publication");
        require(output.size()==3&&s.inPlaceUndo_.size()==3,"Batch outputs and Undo cover every committed file");}
    if(failure==3){require(qa::executes==1&&qa::publishes==1,"Pre-write journal failure still publishes previous commit");
        require(output.size()==3&&output[0].outcome==MediaCopyOutcome::copied&&output[1].outcome==MediaCopyOutcome::failed&&output[2].outcome==MediaCopyOutcome::cancelled,"Remaining files fail or cancel before mutation");
        require(s.inPlaceUndo_.size()==1,"Pre-write journal failure retains previous verified Undo");}
    if(failure==2){require(qa::executes==1&&qa::publishes==1&&s.inPlaceUndo_.size()==1,"Post-write journal failure preserves committed file, hint and Undo");}
}
void undoCases(){
    for(int mode=0;mode<3;++mode){qa::reset();Service s;s.inPlaceUndo_={result(),result(MediaCopyOutcome::copied,L"next.flac")};s.inPlaceUndoSequence_=17;
        qa::restoreResults={result(MediaCopyOutcome::moved),result(mode==2?MediaCopyOutcome::failed:MediaCopyOutcome::moved,L"next.flac")};
        qa::failPublish=mode==1;int count=0;
        require(s.undoLastInPlace([&](auto out){++count;require(out[0].outcome==MediaCopyOutcome::moved,"Restored physical outcome remains moved");
            require(out[0].metadataRefreshFailed==(mode==1),"Undo refresh warning matches SDK outcome");}),"Undo accepted");
        drain(s);require(count==1&&qa::publishes==1&&!s.busy(),"Undo reloads and publishes once");
        require(s.inPlaceUndo_.empty()==(mode!=2),"Physical restoration controls Undo consumption, independent of cache failure");
        if(mode==2){require(s.inPlaceUndo_.size()==1&&s.inPlaceUndo_[0].destination==L"next.flac","Partial Undo retains only failed file");
            qa::restoreResults={result(MediaCopyOutcome::moved,L"next.flac")};
            require(s.undoLastInPlace({}),"Remaining failed file may be retried");drain(s);
            require(qa::restoreInputs.size()==1&&qa::restoreInputs[0].destination==L"next.flac"&&s.inPlaceUndo_.empty(),"Retry does not re-restore already completed files");}
    }
}
void shutdownBeforeCallback(){
    qa::reset();Service s;setup(s);s.workerActive_=false;Service::PendingWork work;work.item.source=work.item.destination=L"track.flac";s.pending_.push_back(work);
    int completions=0;s.completion_=[&](auto){++completions;};s.pump();if(s.worker_.joinable())s.worker_.join();s.shutdownAndDrain();drain(s);
    require(qa::executes==1&&qa::publishes==0&&qa::notifications==0&&completions==0,"Queued callbacks are inert after shutdown");
}
void partialRefreshAndNextOperation(){
    qa::reset();Service s;setup(s);metadb_hint_list::ptr hints;
    std::vector<MediaCopyResult> first{result()},second{result(MediaCopyOutcome::copied,L"second.flac")};
    collect(first,hints);qa::failRead=2;collect(second,hints);first.insert(first.end(),second.begin(),second.end());
    s.metadataHints_=hints;
    s.finishOperation(first,[&](auto out){require(!out[0].metadataRefreshFailed&&out[1].metadataRefreshFailed,"One read failure cannot taint unrelated successful metadata");},11,false,hints);
    require(qa::publishes==1&&s.inPlaceUndo_.size()==2,"Partial refresh publishes existing hints and retains all physical Undo");
    hints.release();qa::failRead=0;setup(s);s.activeOperationSequence_=12;
    std::vector<MediaCopyResult> next{result(MediaCopyOutcome::copied,L"new-operation.flac")};collect(next,hints);
    s.finishOperation(next,{},12,false,hints);
    require(qa::creates==2&&qa::publishes==2&&s.inPlaceUndo_.size()==1&&s.undoOperationSequence()==12,"Next operation owns fresh hints and fresh Undo");
    setup(s);s.activePlan_.mode=MediaOrganizationMode::copy;auto copied=result();copied.destination=L"copy.flac";
    s.finishOperation({copied},{},11,false,{});
    require(qa::publishes==2&&s.inPlaceUndo_.size()==1&&s.undoOperationSequence()==12,"Copy completion does not consume previous in-place Undo or publish hints");
}
void playbackProtectedUndo(){
    qa::reset();Service s;s.inPlaceUndo_={result(),result(MediaCopyOutcome::copied,L"other.flac")};s.inPlaceUndoSequence_=17;
    s.playbackPath_=L"FILE.flac";qa::restoreResults={result(MediaCopyOutcome::moved),result(MediaCopyOutcome::moved,L"other.flac")};
    std::vector<MediaCopyResult> output;
    require(s.undoLastInPlace([&](auto out){output=std::move(out);}),"Protected batch accepts safe per-file processing");drain(s);
    require(output.size()==2&&output[0].outcome==MediaCopyOutcome::skipped&&output[1].outcome==MediaCopyOutcome::moved,"Playing resource is protected case-insensitively, unrelated resource restores");
    require(s.inPlaceUndo_.size()==1&&s.inPlaceUndo_[0].destination==L"file.flac"&&s.undoOperationSequence()==17,"Playing file keeps original Undo ownership");
    require(qa::reads==1,"Protected target has no metadata reload");
    s.playbackPath_.clear();qa::restoreResults={result(MediaCopyOutcome::moved)};
    require(s.undoLastInPlace({}),"Playback release permits explicit retry");drain(s);
    require(qa::restoreInputs.size()==1&&s.inPlaceUndo_.empty()&&!s.hasInPlaceUndo(),"Retry after playback restores remaining target only");
}
#if MEDIA_IDENTITY
void identityBoundaries(){
    for(int mode=0;mode<5;++mode){qa::reset();Service s;setup(s);s.workerActive_=false;
        Service::PendingWork work;work.item.source=work.item.destination=L"file.flac";s.pending_.push_back(work);
        qa::holdBinding=true;qa::failCapture=mode==4;
        int count=0;std::vector<MediaCopyResult> output;s.completion_=[&](auto r){++count;output=std::move(r);};
        s.pump();drain(s);
        if(mode==4){require(count==1&&qa::bindings==0&&qa::publishes==0&&!s.hasInPlaceUndo(),"Capture failure prevents mutation, binding and Undo");continue;}
        require(qa::captures==1&&qa::freezes==1&&qa::bindings==1,"Mutation hook captures source then committed revision");
        require(s.busy()&&!s.hasInPlaceUndo()&&count==0&&qa::publishes==0,"Binding closes admission until callback");
        s.pump();require(qa::executes==1,"Reentrant pump cannot mutate while binding pending");
        s.finishOperation({result()},{},11,false,{});require(qa::bindings==1,"Duplicate finish cannot submit second binding");
        auto callback=qa::bindingCallbacks.front();qa::bindingCallbacks.clear();
        if(mode==2)s.shutdownAndDrain();if(mode==3)s.activeOperationSequence_=99;
        callback(mode!=1,mode==1?"DB failure":"");drain(s);
        if(mode>=2){require(count==0&&qa::publishes==0,"Shutdown/stale binding callback is inert");continue;}
        require(count==1&&!s.busy()&&qa::publishes==1&&s.hasInPlaceUndo(),"Binding terminal result publishes once and retains verified Undo");
        require(output[0].outcome==MediaCopyOutcome::copied&&output[0].identityRefreshFailed==(mode==1),"Binding failure separately reported without falsifying physical commit");
        callback(true,"");drain(s);require(count==1&&qa::publishes==1,"Duplicate DB callback cannot finish twice");
        if(mode==1){require(!s.mutationAllowed(L"file.flac")&&!s.undoLastInPlace({}),"Unbound physical commit protects later edits and Undo");continue;}
        qa::holdBinding=false;qa::restoreResults=output;qa::restoreResults[0].outcome=MediaCopyOutcome::moved;
        require(s.undoLastInPlace([&](auto r){require(!r[0].identityRefreshFailed,"Successful Undo clears old Apply identity warning");}),"Undo after binding outcome accepted");
        drain(s);require(qa::bindings==2&&!s.hasInPlaceUndo(),"Undo submits restored revision binding before completion");
    }
}
void recoveryBoundaries(){
    for(int mode=0;mode<5;++mode){qa::reset();Service s;qa::recoveryScan={};
        MediaBindingRecoveryItem item;item.journalPath=L"pending.json";item.journalSha256="hash";
        for(unsigned sub:{7,19}){MediaRevisionBinding b;b.previousRevisionTrackId="old";b.current.trackId="new";
            b.current.path="file.flac";b.current.subsong=sub;item.revisions.push_back(b);}
        qa::recoveryScan.pending={item};qa::holdBinding=true;
        s.beginBindingRecovery();require(s.busy()&&!s.mutationAllowed(L"file.flac"),"Startup closes mutation admission synchronously");
        drain(s);require(qa::bindings==1&&qa::receipts==0&&s.busy(),"No receipt before DB commit");
        auto callback=qa::bindingCallbacks.front();qa::bindingCallbacks.clear();
        if(mode==4){s.recoveryActive_=false;s.beginBindingRecovery();drain(s);
            callback(true,"");drain(s);require(s.busy()&&qa::receipts==0,"Old replay generation cannot acknowledge or release a newer replay");
            auto newer=qa::bindingCallbacks.front();qa::bindingCallbacks.clear();newer(true,"");drain(s);
            require(!s.busy()&&qa::receipts==1,"Current replay generation completes normally");continue;}
        if(mode==3)s.shutdownAndDrain();qa::failReceipt=mode==2;
        callback(mode!=1,"");drain(s);
        if(mode==3){require(qa::receipts==0,"Late DB callback after shutdown cannot acknowledge");continue;}
        require(!s.busy(),"Recovery terminal callback releases global gate");
        require(s.mutationAllowed(L"file.flac")== (mode==0),"Failed DB or receipt protects affected path");
        require(s.mutationAllowed(L"unrelated.flac"),"Failed item does not block unrelated edits");
        require(qa::receipts==(mode==1?0:1),"DB failure never writes receipt");
    }
    qa::reset();Service empty;qa::recoveryScan={};empty.beginBindingRecovery();drain(empty);
    require(!empty.busy()&&empty.mutationAllowed(L"file.flac")&&qa::bindings==0&&qa::receipts==0,
        "Empty startup recovery releases admission without DB/receipt work");
    qa::reset();Service limited;qa::recoveryScan={};qa::recoveryScan.limited=true;
    limited.beginBindingRecovery();drain(limited);
    require(!limited.busy()&&!limited.mutationAllowed(L"file.flac"),"Partial scan cannot authorize another mutation");
    qa::reset();Service s;setup(s);auto failed=result(MediaCopyOutcome::failed);
    failed.physicalCommitNeedsBinding=true;failed.bindingJournalPath=L"pending.json";
    s.finishOperation({failed},{},11,false,{});
    require(!s.mutationAllowed(L"file.flac"),"Post-rename verification failure protects path even with Failed outcome");
    qa::reset();Service mixed;setup(mixed);
    auto good=result();good.bindingJournalPath=L"good.json";good.physicalCommitNeedsBinding=true;
    auto bad=result(MediaCopyOutcome::copied,L"bad.flac");bad.bindingJournalPath=L"bad.json";
    bad.identityRefreshFailed=true;bad.physicalCommitNeedsBinding=true;
    MediaRevisionBinding b;b.previousRevisionTrackId="old";b.current.trackId="new";b.current.path="file.flac";b.current.subsong=7;
    good.revisionBindings={b};bad.revisionBindings={b};
    mixed.finishOperation({good,bad},{},11,false,{});drain(mixed);
    require(qa::receipts==1&&mixed.mutationAllowed(L"file.flac")&&!mixed.mutationAllowed(L"bad.flac"),
        "Mixed batch acknowledges only revisions actually submitted to DB");
    qa::reset();Service retry;setup(retry);retry.workerActive_=false;
    const auto path=std::filesystem::temp_directory_path()/(L"goden-media-pump-"+
        std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count())+L".bin");
    require(!std::filesystem::exists(path),"Fresh retry fixture");{std::ofstream file(path);file<<"fixture";}
    Service::PendingWork work;work.item.source=work.item.destination=path;retry.pending_={work};
    qa::postCommitFailure=true;int completions=0;
    qa::afterExecute=[&]{std::lock_guard lock(retry.mutex_);retry.playbackPath_=path;};
    retry.completion_=[&](auto results){++completions;require(results.size()==1&&results[0].physicalCommitNeedsBinding,
        "Postcommit result retained, never replaced by retry");};
    retry.pump();drain(retry);std::filesystem::remove(path);
    require(completions==1&&!retry.busy()&&retry.pending_.empty()&&!retry.mutationAllowed(path),
        "Playback protection after rename cannot silently retry an unbound commit");
}
#endif
int main(){try{batchAndAffinity();refreshFailures();ownership();queueBatch(0);queueBatch(3);queueBatch(2);undoCases();shutdownBeforeCallback();
    partialRefreshAndNextOperation();playbackProtectedUndo();
#if MEDIA_IDENTITY
    identityBoundaries();recoveryBoundaries();
#endif
    std::cout<<"PASS "<<qa::checks<<" checks (real state machine; SDK and disk I/O stubbed)\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
'''


if __name__ == '__main__':
    main()
