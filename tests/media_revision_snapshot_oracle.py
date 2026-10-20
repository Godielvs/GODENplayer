"""Extract real MEDIA revision capture/freeze helpers; stub only SDK subsong reader.

Uses real filesystem/stat/production makeTrackId and media_binding_recovery.
Link media_file_operations.cpp, media_binding_recovery.cpp, nekorox_core.lib
and bcrypt.lib. Does not establish SDK decoder or DB transaction behavior.
"""
import argparse
import re
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
a = p.parse_args()
source = (a.source_root / 'src/component/media_file_operation_service.cpp').read_text(encoding='utf-8-sig')
names = ['captureMediaRevisions']
methods = []
for name in names:
    m = re.search(r'^(?:bool|void|std::vector<MediaRevisionBinding>) ' + name + r'\([\s\S]*?^}\n', source, re.M)
    assert m, name
    methods.append(m.group())
prefix = r'''
#include "media_file_operations.h"
#include "media_binding_recovery.h"
#include "nekorox/track_identity.h"
#include <atomic>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>
using t_uint32=std::uint32_t;
namespace qa {inline std::vector<unsigned> ids{7,19,7};inline unsigned forcedCount=0;
inline int reads=0,checks=0;inline bool failRead=false;
void require(bool v,const char* s){++checks;if(!v)throw std::runtime_error(s);}}
struct abort_callback_impl {std::atomic<bool> cancelled=false;void abort(){cancelled=true;}
    void check(){if(cancelled)throw std::runtime_error("cancelled");}};
namespace pfc {using string8=std::string;}
struct filesystem {static void g_get_canonical_path(const char* p,pfc::string8& out){out=p;}};
struct input_info_reader {using ptr=std::shared_ptr<input_info_reader>;
    unsigned get_subsong_count(){return qa::forcedCount?qa::forcedCount:static_cast<unsigned>(qa::ids.size());}
    unsigned get_subsong(unsigned i){return qa::ids.at(i);}};
struct input_entry {static void g_open_for_info_read(input_info_reader::ptr& r,void*,const char*,abort_callback_impl& a){
    a.check();++qa::reads;if(qa::failRead)throw std::runtime_error("read failed");r=std::make_shared<input_info_reader>();}};
namespace nekorox::component {
std::string wideToUtf8(std::wstring_view s){return std::string(s.begin(),s.end());}
'''
tests = r'''
}
using namespace nekorox;using namespace nekorox::component;
int main(){try{
    const auto root=std::filesystem::temp_directory_path()/L"goden-media-revision-oracle";
    std::filesystem::create_directories(root);const auto path=root/L"fixture.bin";
    {std::ofstream out(path,std::ios::binary);out<<std::string(180000,'a');}
    auto old=captureMediaRevisions(path,{});
    qa::require(old.size()==2&&old[0].current.subsong==7&&old[1].current.subsong==19,"Actual physical subsong IDs deduplicated");
    auto size=std::filesystem::file_size(path);auto stamp=std::filesystem::last_write_time(path);
    for(const auto& b:old)qa::require(b.previousRevisionTrackId==makeTrackId({path,b.current.subsong,size,static_cast<std::uint64_t>(stamp.time_since_epoch().count())}).value,"Old revision equals production identity");
    {std::ofstream out(path,std::ios::binary);out<<std::string(200000,'b');}
    MediaCopyResult r;r.source=r.destination=path;r.outcome=MediaCopyOutcome::copied;
    r.revisionBindings=old;
    qa::require(freezeMediaBindingRevisions(path,r.revisionBindings,{}),"Postcommit freeze succeeds");
    qa::require(qa::reads==1,"Postcommit capture uses known physical subsongs without extra decoder open");
    size=std::filesystem::file_size(path);stamp=std::filesystem::last_write_time(path);
    for(const auto& b:r.revisionBindings){const auto expected=makeTrackId({path,b.current.subsong,size,static_cast<std::uint64_t>(stamp.time_since_epoch().count())});
        qa::require(b.current.trackId==expected.value&&b.current.trackId!=b.previousRevisionTrackId&&b.current.path==expected.normalizedPathUtf8&&b.current.partialFingerprint==expected.partialFingerprint,"Frozen postcommit revision is exact");
        qa::require(b.current.fileSize==size&&b.current.modifiedTicks==static_cast<std::uint64_t>(stamp.time_since_epoch().count()),"Frozen revision includes matching stat values");}
    auto failed=r;failed.source=failed.destination=root/L"missing.bin";
    qa::require(!freezeMediaBindingRevisions(failed.destination,failed.revisionBindings,{}),"Missing revision refuses capture");
    qa::require(failed.revisionBindings[0].previousRevisionTrackId==old[0].previousRevisionTrackId,"Original identity retained for later Undo");
    std::stop_source stop;stop.request_stop();bool refused=false;try{captureMediaRevisions(path,stop.get_token());}catch(...){refused=true;}
    qa::require(refused,"Cancelled pre-write capture refused");
    qa::ids.clear();refused=false;try{captureMediaRevisions(path,{});}catch(...){refused=true;}qa::require(refused,"Empty physical subsong set refused");
    qa::forcedCount=65537;refused=false;try{captureMediaRevisions(path,{});}catch(...){refused=true;}qa::require(refused,"Oversized physical subsong set refused");
    std::filesystem::remove(path);std::filesystem::remove(root);
    std::cout<<"PASS media revision snapshots checks="<<qa::checks<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
'''
a.output.mkdir(parents=True, exist_ok=True)
(a.output / 'oracle.cpp').write_text(prefix + '\n'.join(methods) + tests, encoding='utf-8')
