"""Compile the verbatim completion method and cache identity guard without a host.

This exercises production result admission, not worker decoding or live UI.
"""
import argparse
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
component = Path(__file__).resolve().parents[1] / 'src/component'
source = (component / 'analysis_controller.cpp').read_text(encoding='utf-8-sig')
header = (component / 'analysis_controller.h').read_text(encoding='utf-8-sig')
method = re.search(r'^void AnalysisController::applyFrame\([\s\S]*?^}\n', source, re.M)
reader = re.search(r'^void AnalysisController::readerMain\([\s\S]*?^}\n', source, re.M)
run = re.search(r'^struct AnalysisController::WorkerRun \{[\s\S]*?^};', source, re.M)
db = (component / 'database_service.cpp').read_text(encoding='utf-8-sig')
guard = re.search(r'                if \(payload.error.empty\(\) && task->kind == AnalysisTask::Kind::storeDeep &&[\s\S]*?^                }', db, re.M)
if not method or not reader or not run or not guard or header.count('private:') != 1:
    raise RuntimeError('Analysis revision extraction boundary changed')
args.output.mkdir(parents=True, exist_ok=True)
(args.output / 'analysis_controller_test.h').write_text(header.replace('private:', 'public:'), encoding='utf-8')
(args.output / 'worker_failure.h').write_bytes((component / 'worker_failure.h').read_bytes())
code = r'''
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include "analysis_controller_test.h"
#include "worker_failure.h"
#include "nekorox/analysis_source_revision.h"
namespace nekorox::component {
struct AnalysisWorkerHandles {HANDLE output=nullptr,process=nullptr,startThread=nullptr;};
struct PacketCostController {
    static PacketCostController& instance(){static PacketCostController c;return c;}
    void endDeepAnalysis(std::uint64_t){}
};
namespace fb2k {template<class F> void inMainThread(F&&) {}}
struct DatabaseService {
    unsigned stores=0; std::string identity;
    static DatabaseService& instance() {static DatabaseService d; return d;}
    void storeAnalysis(std::filesystem::path,std::string id,AnalysisMediaInfoV1,
        std::shared_ptr<const std::vector<AnalysisWaveformBinV1>>,std::string) {++stores;identity=std::move(id);}
};
static std::uint64_t analysisQpcNow(){return 100;}
static std::uint64_t analysisQpcFrequency(){return 1000;}
static void postListenAnalysisRefresh() {}
static std::wstring utf8ToWide(const std::string& s){return {s.begin(),s.end()};}
''' + run.group() + '\n' + method.group() + r'''
static AnalysisController::WorkerRun* reading=nullptr;
static std::string expectedIdentity;
static bool resumed=false, editRefused=false;
static DWORD testResumeThread(HANDLE) {
    if(!reading || reading->sourceTrackId!=expectedIdentity || expectedIdentity.empty())
        throw std::runtime_error("worker resumed before source fingerprint");
    resumed=true;return 1;
}
static bool readAnalysisWorkerFrame(HANDLE,std::uint64_t id,AnalysisFrameHeaderV1& frame,
    std::vector<std::byte>& payload,std::string&) {
    if(!resumed)throw std::runtime_error("decode began before source admission");
    const auto writer=CreateFileW(reading->sourcePath.c_str(),GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    editRefused=writer==INVALID_HANDLE_VALUE && GetLastError()==ERROR_SHARING_VIOLATION;
    if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
    frame.kind=AnalysisMessageKind::complete;frame.requestId=id;
    payload={std::byte{'{'},std::byte{'}'}};return true;
}
#define ResumeThread testResumeThread
#define WaitForSingleObject(...) WAIT_OBJECT_0
#define TerminateProcess(...) TRUE
''' + reader.group() + r'''
#undef ResumeThread
#undef WaitForSingleObject
#undef TerminateProcess
void AnalysisController::reapCompleted(std::uint64_t){}
AnalysisController::~AnalysisController() = default;
struct AnalysisTask { enum class Kind {storeDeep,loadDeep};Kind kind=Kind::storeDeep; std::string expectedDeepAnalysisTrackId;};
bool writerAccepts(std::string expected,std::string current) {
    AnalysisTask taskValue;auto* task=&taskValue;task->expectedDeepAnalysisTrackId=std::move(expected);
    struct {std::string trackId;} analysisTrack{std::move(current)};
    struct {std::string error;} payload;
''' + guard.group() + r'''
    return payload.error.empty();
}
}
static void requireRevision(bool value,const char* why){if(!value)throw std::runtime_error(why);}
int main(){
  try {
    using namespace nekorox;using namespace nekorox::component;
    const auto path=std::filesystem::temp_directory_path()/
        (L"goden-analysis-oracle-"+std::to_wstring(GetCurrentProcessId())+L".tmp");
    {std::ofstream file(path);file<<"before";}
    AnalysisController c;
    auto admit=[&]{
      c.active_=std::make_unique<AnalysisController::WorkerRun>();
      c.active_->requestId=42;c.active_->sourcePath=path;
      c.active_->sourceTrackId=analysisSourceIdentity(path);
      c.state_={};c.state_.status=DeepAnalysisStatus::running;c.state_.trackPath=path;
    };
    admit();
    AnalysisFrameHeaderV1 frame;frame.kind=AnalysisMessageKind::complete;frame.requestId=42;
    std::vector<std::byte> result{std::byte{'{'},std::byte{'}'}};
    {std::ofstream file(path);file<<"replaced with a different recording";}
    c.active_->sourceValidated=analysisSourceIdentity(path)==c.active_->sourceTrackId;
    c.applyFrame(frame,result);
    requireRevision(c.state_.status==DeepAnalysisStatus::error && !c.state_.resultJson &&
      DatabaseService::instance().stores==0,"changed source published or cached old analysis");
    admit();c.active_->sourceValidated=true;const auto admitted=c.active_->sourceTrackId;
    c.applyFrame(frame,result);
    requireRevision(c.state_.status==DeepAnalysisStatus::complete && c.state_.resultJson &&
      DatabaseService::instance().stores==1 && DatabaseService::instance().identity==admitted,
      "unchanged completion did not preserve its observed identity");
    requireRevision(writerAccepts("before","before") && !writerAccepts("before","after") &&
      !writerAccepts("","after"),"writer accepted a stale or unidentified queued result");
    {std::ofstream file(path);file<<"aaaaaa";}
    const auto timestamp=std::filesystem::last_write_time(path);
    const auto revision=analysisSourceRevision(path);const auto original=analysisSourceIdentity(path);
    {std::ofstream file(path);file<<"bbbbbb";}
    std::filesystem::last_write_time(path,timestamp);
    const auto replacement=analysisSourceIdentity(path);
    requireRevision(analysisSourceRevision(path)==revision && replacement!=original &&
        !writerAccepts(original,replacement),"same-size/mtime edit escaped real fingerprint");
    // The child has not executed yet: it must receive the newly captured ID,
    // and decoding may not mutate/replace that file until the run has finished.
    admit();reading=c.active_.get();expectedIdentity=replacement;
    c.readerMain(reading);
    requireRevision(resumed && editRefused && reading->sourceValidated &&
      c.state_.status==DeepAnalysisStatus::complete &&
      DatabaseService::instance().identity==replacement,"reader source freeze/admission failed");
    {std::ofstream file(path);file<<"cccccc";}
    requireRevision(analysisSourceIdentity(path)!=replacement,"source lock survived completed reader");
    std::filesystem::remove(path);
    std::cout<<"Analysis production reader/freeze/completion + real identity guard PASS\n";return 0;
  }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}
}
'''
(args.output / 'oracle.cpp').write_text(code, encoding='utf-8')
