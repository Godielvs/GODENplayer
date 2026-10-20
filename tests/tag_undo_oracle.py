"""Extract TAG Undo/state methods verbatim; real files, SHA256, copy and rename.

SDK playback/metadata callbacks and journal persistence are controlled stubs.
The native host suite separately verifies the SDK and durable JSON journals.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    component = args.source_root / 'src/component'
    source = (component / 'tag_edit_service.cpp').read_text(encoding='utf-8-sig')
    header = (component / 'tag_edit_service.h').read_text(encoding='utf-8-sig')
    def take(pattern):
        found = re.search(pattern, source, re.M)
        if not found:
            raise RuntimeError('Production boundary changed: ' + pattern)
        return found.group()
    definitions = [take(r'^struct Revision \{[\s\S]*?^};'),
                   take(r'^struct TagEditService::Operation \{[\s\S]*?^};')]
    helpers = [take(r'^' + signature + r'[\s\S]*?^}\n') for signature in [
        r'bool revisionOf\(', r'bool sameRevision\(', r'std::string hex\(',
        r'bool sha256File\(', r'bool copyFileCancellable\(', r'bool undoable\(']]
    preserve_audit = 'readRecoveryJournal(' in source
    if preserve_audit:
        helpers.extend([take(r'^std::optional<TagRecoveryJournal> readRecoveryJournal\([\s\S]*?^}\n'),
                        take(r'^bool persistRecoveryJournal\([\s\S]*?^}\n')])
    methods = [take(r'^(?:bool|void) TagEditService::' + name + r'\([\s\S]*?^}\n')
               for name in ['resumeDeferred', 'startNext', 'undoLast', 'prepareUndo', 'finish']]
    candidate = 'TagEditService::playbackProtects(' in source
    if candidate:
        methods.insert(0, take(r'^bool TagEditService::playbackProtects\([\s\S]*?^}\n'))
    assert header.count('#include "sdk_compat.h"') == 1 and header.count('private:') == 1
    header = header.replace('#include "sdk_compat.h"', SDK).replace('private:', 'public: // Test only')
    helpers = '\n'.join(helpers).replace('bool sha256File(', 'bool realSha256File(').replace(
        'bool copyFileCancellable(', 'bool realCopyFileCancellable(')
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'tag_service_test.h').write_text(header, encoding='utf-8')
    (args.output / 'oracle.cpp').write_text(PREFIX + '\n#define PRESERVE_AUDIT ' + str(int(preserve_audit)) +
        '\n#define CANDIDATE ' + str(int(candidate)) +
        '\nnamespace nekorox::component {\n' + '\n'.join(definitions) + '\n' + helpers +
        '\n' + STUBS + '\n' + '\n'.join(methods) + '\n}\n' + TESTS, encoding='utf-8')
    (args.output / 'source-manifest.json').write_text(json.dumps({
        'candidate': candidate, 'sourceRoot': str(args.source_root.resolve()),
        'methodSha256': hashlib.sha256('\n'.join(methods).encode()).hexdigest(),
        'extractedSha256': hashlib.sha256(('\n'.join(definitions) + helpers + '\n'.join(methods)).encode()).hexdigest(),
        'limits': 'SDK and journal persistence stubbed; actual SHA256, copies and MoveFileEx on disposable files',
    }, indent=2), encoding='utf-8')


SDK = r'''
#define NOMINMAX
#include <windows.h>
#include <vector>
#include <filesystem>
#include <functional>
struct metadb_handle_ptr { std::filesystem::path path; };
struct metadb_handle_list {
    std::vector<int> handles;
    void add_items(const metadb_handle_list& list){handles.insert(handles.end(),list.handles.begin(),list.handles.end());}
    size_t get_count() const {return handles.size();}
};
using metadb_handle_list_cref = const metadb_handle_list&;
'''

PREFIX = r'''
#include "tag_service_test.h"
#include "tag_recovery_journal.h"
#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
namespace qa {
int checks=0, updates=0, loads=0, copies=0, completions=0, failSourceJournal=0, uiJournals=0;
const auto uiThread=std::this_thread::get_id();
std::filesystem::path root, playing;
std::vector<std::function<void()>> callbacks;
std::vector<std::function<void(unsigned)>> reloads;
std::map<std::filesystem::path,nekorox::component::TagApplySummary> journals;
std::function<void()> afterCopy;
std::function<void(const std::filesystem::path&)> beforeHash;
std::function<void(const std::filesystem::path&)> afterHash;
void require(bool condition,const char* label){++checks;if(!condition)throw std::runtime_error(label);}
void pump(){auto pending=std::move(callbacks);callbacks.clear();for(auto& call:pending)call();}
void notify(){auto pending=std::move(reloads);reloads.clear();for(auto& call:pending)call(0);}
void reset(){playing.clear();updates=loads=copies=completions=failSourceJournal=uiJournals=0;callbacks.clear();reloads.clear();journals.clear();afterCopy={};afterHash={};beforeHash={};}
}
struct playback_control {
    static playback_control* get(){static playback_control value;return &value;}
    bool get_now_playing(metadb_handle_ptr& out){out.path=qa::playing;return !out.path.empty();}
};
namespace fb2k {
template<class F>void inMainThread(F f){qa::callbacks.emplace_back(std::move(f));}
template<class F>std::function<void(unsigned)> makeCompletionNotify(F f){return f;}
}
struct file_info {};
using t_filestats=int;
struct file_info_filter {template<class F>static int create(F){return 0;}};
struct metadb_io {enum{load_info_force=1};};
struct metadb_io_v2 {
    enum{op_flag_background=1,op_flag_delay_ui=2,op_flag_no_errors=4};
    static metadb_io_v2* get(){static metadb_io_v2 value;return &value;}
    void load_info_async(const metadb_handle_list&,int,HWND,int,std::function<void(unsigned)> f){++qa::loads;qa::reloads.push_back(f);}
    void update_info_async(const metadb_handle_list&,int,HWND,int,std::function<void(unsigned)>){++qa::updates;}
};
'''

STUBS = r'''
std::string wideToUtf8(std::wstring_view input){
    const auto size=WideCharToMultiByte(CP_UTF8,0,input.data(),int(input.size()),nullptr,0,nullptr,nullptr);
    std::string out(size,'\0');WideCharToMultiByte(CP_UTF8,0,input.data(),int(input.size()),out.data(),size,nullptr,nullptr);return out;
}
struct TagEditService::RecoveryState {};
TagEditService::~TagEditService(){if(worker_.joinable())worker_.join();}
void TagEditService::updateCompleted(std::shared_ptr<Operation>,size_t,unsigned){}
void TagEditService::bindCompleted(std::shared_ptr<Operation>,bool,std::string){}
std::filesystem::path nativePath(const metadb_handle_ptr& p){return p.path;}
std::filesystem::path backupRoot(){return qa::root;}
std::wstring operationId(){static int seq=0;return L"attempt-"+std::to_wstring(++seq);}
std::optional<unsigned long> qaUnsigned(const wchar_t*){return {};}
struct TrackRevisionRebind{std::string id;std::filesystem::path path;uint32_t subsong;};
struct DatabaseService {
    static DatabaseService& instance(){static DatabaseService s;return s;}
    template<class F>void rebindTagRevisions(std::vector<TrackRevisionRebind>,F){}
};
bool persistJournal(const TagApplySummary& summary){
    if(std::this_thread::get_id()==qa::uiThread)++qa::uiJournals;
    if(summary.operationId==L"apply"&&qa::failSourceJournal){--qa::failSourceJournal;return false;}
    qa::journals[summary.journalPath]=summary;return true;
}
bool sha256File(const std::filesystem::path& p,std::string& hash,std::stop_token stop={}){
    if(qa::beforeHash)qa::beforeHash(p);
    const auto okay=realSha256File(p,hash,stop);
    if(qa::afterHash)qa::afterHash(p);
    return okay;
}
bool copyFileCancellable(const std::filesystem::path& a,const std::filesystem::path& b,std::stop_token stop){
    ++qa::copies;const auto okay=realCopyFileCancellable(a,b,stop);
    if(qa::afterCopy)qa::afterCopy();return okay;
}
'''

TESTS = r'''
using namespace nekorox::component;
using qa::require;
using Service=TagEditService;
using Outcome=TagFileOutcome;
void write(const std::filesystem::path& p,const std::string& data){std::ofstream f(p,std::ios::binary);f<<data;}
std::string hash(const std::filesystem::path& p){std::string out;require(realSha256File(p,out),"Hash actual fixture");return out;}
auto setup(Service& s,int count=1){
    qa::reset();auto source=std::make_shared<Service::Operation>();source->generation=1;
    source->summary.operationId=L"apply";source->summary.operationSeq=77;source->summary.journalPath=qa::root/L"apply.json";
    for(int i=0;i<count;++i){
        Service::Operation::PhysicalFile f;f.path=qa::root/(L"file-"+std::to_wstring(i)+L".flac");
        f.resultIndex=i;f.itemResultIndices={size_t(i*2),size_t(i*2+1)};f.handles.handles={i};
        TagFileResult r;r.path=f.path;r.backupPath=qa::root/(L"backup-"+std::to_wstring(i));
        write(r.backupPath,"ORIGINAL-"+std::to_string(i));write(r.path,"EDITED-"+std::to_string(i));
        r.sourceHash=hash(r.backupPath);r.postWriteHash=hash(r.path);r.outcome=Outcome::committed;
        revisionOf(f.path,f.expected);source->physical.push_back(f);source->summary.files.push_back(r);
        for(int n=0;n<2;++n){TagItemResult item;item.outcome=Outcome::committed;item.afterRevision="after";source->summary.items.push_back(item);}
    }
    s.lastCompleted_=source;s.generation_=1;persistJournal(source->summary);return source;
}
auto undo(Service& s){
    require(s.undoLast(nullptr,[](TagApplySummary){++qa::completions;}),"Undo starts");
    auto op=s.active_;s.worker_.join();return op;
}
void settle(){qa::pump();qa::notify();}
void noStaging(){for(auto& f:std::filesystem::directory_iterator(qa::root))require(f.path().wstring().find(L"goden-restore-")==std::wstring::npos,"No staging leaked");}
void stopBeforeReload(){
    Service s;auto original=setup(s);qa::playing=original->physical[0].path;
    auto op=undo(s);qa::pump();qa::playing.clear();s.resumeDeferred();
    require(qa::updates==(CANDIDATE?0:1),"Stop during Undo can never enter Apply (baseline reproduces it)");
    if(CANDIDATE){
        require(!s.active_&&s.lastCompleted_==original&&qa::loads==0,"Protected Undo finishes without useless reload and retains retry");
        auto retry=undo(s);settle();require(retry->summary.files[0].outcome==Outcome::restored&&!s.lastCompleted_,"Manual retry restores and consumes");
    }else{qa::notify();}
}
void partialAndRestart(){
    Service s;auto original=setup(s,2);qa::playing=original->physical[0].path;
    auto op=undo(s);qa::pump();require(qa::loads==1,"Only changed files trigger metadata reload");
    qa::notify();require(s.lastCompleted_!=nullptr,"Partial Undo remains available");
    const auto journal=qa::journals.at(original->summary.journalPath);
    require(journal.files[0].outcome==Outcome::committed&&journal.files[1].outcome==(CANDIDATE?Outcome::restored:Outcome::committed),"Source journal consumes only restored file");
    if(CANDIDATE){
        require(original->summary.items[2].outcome==Outcome::restored&&original->summary.items[3].afterRevision.empty(),"All restored subsongs consumed");
        qa::playing.clear();s.resumeDeferred();auto retry=undo(s);settle();
        require(retry->physical.size()==1&&retry->physical[0].path==original->physical[0].path,"Partial retry excludes already restored file");
        require(!s.lastCompleted_&&qa::journals.at(original->summary.journalPath).files[0].outcome==Outcome::restored,"Completed source journal has no recoverable entries");
    }
}
void lateGuard(bool cancel){
    Service s;auto original=setup(s);qa::afterCopy=[&]{if(cancel){++s.generation_;}else{
#if CANDIDATE
        s.playbackPath_=original->physical[0].path;
#endif
    }};
    auto op=undo(s);settle();
    require(op->summary.files[0].outcome==(cancel?Outcome::cancelledBeforeWrite:Outcome::deferredUntilReleased),"Late playback/cancellation prevents commit");
    require(hash(original->physical[0].path)==original->summary.files[0].postWriteHash,"Late guard leaves edited file unchanged");noStaging();
}
void concurrentPlayback(int change){
    Service s;auto original=setup(s);std::mutex gate;std::condition_variable cv;bool copied=false,released=false;
    qa::afterCopy=[&]{std::unique_lock lock(gate);copied=true;cv.notify_one();cv.wait(lock,[&]{return released;});};
    require(s.undoLast(nullptr,[](TagApplySummary){++qa::completions;}),"Concurrent Undo starts");
    auto op=s.active_;
    {std::unique_lock lock(gate);require(cv.wait_for(lock,std::chrono::seconds(5),[&]{return copied;}),"Worker reached copy barrier");}
    // Real main-thread entry point while the worker is parked after staging.
    qa::playing=change==0?original->physical[0].path:change==1?qa::root/L"another.flac":std::filesystem::path{};
    s.resumeDeferred();require(qa::updates==0,"Playback notification never starts Apply during Undo worker");
    {std::lock_guard lock(gate);released=true;}cv.notify_one();s.worker_.join();settle();
    require(op->summary.files[0].outcome==(change==0?Outcome::deferredUntilReleased:Outcome::restored),"Late actual notification determines final protection");
    noStaging();
}
void cancellationAfterCommit(){
    Service s;auto original=setup(s);const auto file=original->summary.files[0];
    std::mutex gate;std::condition_variable cv;bool committed=false,released=false;
    qa::beforeHash=[&](const std::filesystem::path& p){
        if(p!=file.path)return;std::string current;
        if(!realSha256File(p,current)||current!=file.sourceHash)return;
        std::unique_lock lock(gate);committed=true;cv.notify_one();cv.wait(lock,[&]{return released;});
    };
    require(s.undoLast(nullptr,[](TagApplySummary){++qa::completions;}),"Post-commit cancellation Undo starts");auto op=s.active_;
    {std::unique_lock lock(gate);require(cv.wait_for(lock,std::chrono::seconds(5),[&]{return committed;}),"Worker paused after actual replacement");}
    s.worker_.request_stop();++s.generation_;
    {std::lock_guard lock(gate);released=true;}cv.notify_one();s.worker_.join();settle();
    require(op->summary.files[0].outcome==Outcome::restored&&!s.lastCompleted_,"Cancellation after physical commit still verifies and consumes restored bytes");
    require(hash(file.path)==file.sourceHash,"Post-commit cancelled file contains original bytes");
}
void rejected(bool backup){
    Service s;auto original=setup(s);const auto file=original->summary.files[0];
    write(backup?file.backupPath:file.path,"EXTERNAL");const auto before=hash(file.path);
    auto op=undo(s);settle();require(hash(file.path)==before,"Invalid source or backup never overwritten");
    require(op->summary.files[0].outcome==(backup?Outcome::failedBeforeWrite:Outcome::conflictExternalChange),"Hash conflict is explicit");
    require(s.lastCompleted_!=nullptr&&qa::loads==0,"Rejected Undo retained without reload");noStaging();
}
void journalRetry(){
    Service s;auto original=setup(s);const auto disk=original->summary;qa::failSourceJournal=2;
    auto op=undo(s);settle();require(op->summary.files[0].outcome==Outcome::restored&&!s.lastCompleted_,"Journal error cannot undo physical success");
    require(op->summary.files[0].detail.find(L"journal update failed")!=std::wstring::npos,"Journal failure visible in attempt");
    require(qa::journals.at(disk.journalPath).files[0].outcome==Outcome::committed,"Injected source journal writes failed");
    // Model restart discovery reading the unchanged original journal.
    original->summary=disk;s.lastCompleted_=original;const auto copies=qa::copies;
    auto retry=undo(s);settle();require(qa::copies==copies,"Already restored hash is consumed without another copy");
    require(!s.lastCompleted_&&qa::journals.at(disk.journalPath).files[0].outcome==Outcome::restored,"Retry repairs stale recovery journal");
}
void transientJournal(){
    Service s;auto original=setup(s);qa::failSourceJournal=1;auto op=undo(s);settle();
    require(op->summary.files[0].detail.find(L"journal update failed")==std::wstring::npos,"Successful final journal retry clears transient warning");
    require(qa::journals.at(original->summary.journalPath).files[0].outcome==Outcome::restored,"Transient failure still consumes durable source");
}
void uiJournalWrite(){
    Service s;setup(s);qa::uiJournals=0;auto op=undo(s);settle();
    require(qa::uiJournals==(PRESERVE_AUDIT?0:1),"Undo completion no longer rewrites journal on UI");
}
#if PRESERVE_AUDIT
std::string read(const std::filesystem::path& path){std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
void recoveredAudit(bool changedJournal){
    Service s;auto original=setup(s);original->recovered=true;original->physical[0].recoveredFileIndex=1;
    const auto file=original->summary.files[0];
    const auto quoted=[](const std::filesystem::path& p){return "\""+sonare::util::json::escape_string(wideToUtf8(p.wstring()))+"\"";};
    const std::string target="\"outcome\":\"Committed\",\"detail\":\"restore-me\"";
    std::string raw="{\n\"schema\":\"goden.tag-operation.v2\",\"operationId\":\"apply\",\"operationSeq\":77,\n"
        "\"future\":{\"exactInteger\":18446744073709551615,\"empty\":[],\"escaped\":\"a\\\"{}[]\"},\n"
        "\"mutations\":[{\"field\":\"TITLE\",\"mode\":\"set\",\"values\":[\"old audit\"]}],\n"
        "\"items\":[{\"itemId\":\"opaque\",\"subsong\":0,\"outcome\":\"Committed\",\"draftRevision\":9007199254740993}],\n"
        "\"files\":[{\"outcome\":\"Restored\",\"detail\":\"keep this row\",\"extension\":true},{\"path\":"+quoted(file.path)+
        ",\"backupPath\":"+quoted(file.backupPath)+",\"sourceSha256\":\""+file.sourceHash+"\",\"postWriteSha256\":\""+
        (changedJournal?std::string(64,'e'):file.postWriteHash)+"\","+target+",\"subsongs\":[0]}]}\n";
    write(original->summary.journalPath,raw);qa::uiJournals=0;auto op=undo(s);settle();
    require(hash(file.path)==file.sourceHash,"Recovered Undo restores real bytes");
    auto expected=raw;
    if(!changedJournal)expected.replace(expected.find(target),target.size(),
        "\"outcome\":\"Restored\",\"detail\":\"Original bytes restored and SHA-256 verified\"");
    require(read(original->summary.journalPath)==expected,"Recovery updates only selected string spans; all other bytes and audit survive");
    require((op->summary.files[0].detail.find(L"journal update failed")!=std::wstring::npos)==changedJournal,"Unexpected journal change reported without destructive fallback");
    require(qa::uiJournals==0,"Recovered Undo journal I/O stays off UI");
}
#endif
void staleCallback(){
    Service s;setup(s);auto op=undo(s);s.active_.reset();const auto journals=qa::journals.size();settle();
    require(qa::loads==0&&qa::completions==0&&qa::journals.size()==journals,"Drained callback cannot reload or publish completion");
    s.finish(op);require(qa::completions==0,"Stale finish ignored");
}
int wmain(int argc,wchar_t**argv){try{
    require(argc==2,"Disposable directory argument");qa::root=argv[1];std::filesystem::create_directories(qa::root);
    stopBeforeReload();partialAndRestart();
    if(CANDIDATE){lateGuard(false);lateGuard(true);for(int i=0;i<3;++i)concurrentPlayback(i);cancellationAfterCommit();rejected(false);rejected(true);journalRetry();transientJournal();staleCallback();uiJournalWrite();}
#if PRESERVE_AUDIT
    recoveredAudit(false);recoveredAudit(true);
#endif
    std::cout<<"PASS checks="<<qa::checks<<" candidate="<<CANDIDATE<<"\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
'''

if __name__ == '__main__':
    main()
