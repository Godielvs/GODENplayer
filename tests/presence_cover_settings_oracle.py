"""Exercise production cover preference methods with real Windows DPAPI.

Only SDK persistence, relay validation and main-thread publication are stubbed.
No player, profile, network or real credential is accessed.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
from presence_cover_worker_oracle import extract_function

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''#include <windows.h>
#include <wincrypt.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "presence_cover_policy.h"
namespace nekorox::component {
struct CfgString : std::string {
    using std::string::operator=;
    const CfgString& get() const {return *this;}
};
CfgString g_relayEndpoint,g_relayCredential,g_coverImgBBKey;
unsigned g_coverProvider=0,g_coverAutomaticProviders=coverProviderBit(PresenceCoverProvider::catbox);
bool g_coverDisclosure=false,g_coverInstallChoiceConsumed=false;
bool g_presenceEnabled=true,g_presenceTemporaryCovers=true;
std::string trimAscii(std::string value) {
    while(!value.empty()&&static_cast<unsigned char>(value.front())<=32)value.erase(value.begin());
    while(!value.empty()&&static_cast<unsigned char>(value.back())<=32)value.pop_back();
    return value;
}
struct DiscordRelayClient {static bool validEndpoint(const std::string& endpoint) {return endpoint=="https://relay.example.test";}};
struct DiscordPresenceService {
    std::mutex mutex_;
    struct {unsigned cancels=0,disabled=0,verified=0;
        void cancel(){++cancels;}void disableRelay(){++disabled;}void requestVerification(){++verified;}
    } coverService_;
    struct {
        std::string coverUrl="old",coverToken="token";
        std::int64_t relayExpiresAtMilliseconds=123;
        struct {std::uint64_t coverEpoch=0,presenceRevision=0;bool urgentClear=false,allowCover=true;} update;
        struct {bool is_valid() const{return true;}}trackHandle;
    } pending_;
    bool started_=true;
    std::uint64_t coverEpoch_=0,pendingRevision_=0;
    struct {void notify_all(){}}changed_;
    void publishCurrent(bool){}
    void launchWorker(){}
    bool setCoverConfiguration(PresenceCoverProvider,std::string,std::string,bool,std::uint32_t,std::string);
    bool retryCurrentCover(bool);
};
'''
SUFFIX = r'''
}
int main() {
    using namespace nekorox::component;
    using P=PresenceCoverProvider;
    unsigned assertions=0;
    const auto check=[&](bool passed,const char* message) {++assertions;if(!passed)throw std::runtime_error(message);};
    try {
        DiscordPresenceService service;
        check(!service.setCoverConfiguration(P::imgbb,"","",true,coverProviderBit(P::catbox),""),"missing ImgBB key accepted");
        check(g_coverProvider==0 && service.coverService_.cancels==0,"invalid preference partially applied");
        const std::string key="generated-test-key-not-a-real-key-0123456789";
        check(service.setCoverConfiguration(P::imgbb,"","",true,coverProviderBit(P::catbox),key),"ImgBB preference save failed");
        check(g_coverImgBBKey!=key && g_coverImgBBKey.find(key)==std::string::npos,"API key persisted in plaintext");
        check(unprotectRelayCredential(g_coverImgBBKey)==key && presenceCoverConfiguration()->imgbbKey==key,"DPAPI key roundtrip failed");
        const auto protectedKey=g_coverImgBBKey;
        check(service.setCoverConfiguration(P::catbox,"","",true,coverProviderBit(P::catbox),""),"Catbox migration failed");
        check(g_coverImgBBKey==protectedKey && presenceCoverConfiguration()->imgbbKey.empty(),"provider switch lost key or needlessly decrypted it");
        check(service.setCoverConfiguration(P::imgbb,"","",true,coverProviderBit(P::catbox),""),"blank key did not retain saved value");
        check(!service.setCoverConfiguration(P::imgbb,"","",true,coverProviderBit(P::catbox),"invalid\r\nkey"),"multipart-injection key accepted");
        check(g_coverImgBBKey==protectedKey,"invalid key replaced protected setting");
        check(!service.setCoverConfiguration(P::automatic,"","",true,0,""),"empty automatic allowlist accepted");
        check(!service.setCoverConfiguration(P::automatic,"","",true,coverProviderBit(P::privateRelay),""),"relay admitted through automatic mask");
        check(service.setCoverConfiguration(P::automatic,"","",true,coverProviderBit(P::uguu),""),"automatic Uguu save failed");
        check(presenceCoverConfiguration()->allowsProvider(P::uguu) && !presenceCoverConfiguration()->allowsProvider(P::catbox),"allowlist changed after persistence");
        g_presenceEnabled=false;const auto cancels=service.coverService_.cancels;
        check(!service.retryCurrentCover(true) && service.coverService_.cancels==cancels,"manual test implicitly enabled Presence");
        g_presenceEnabled=true;
        check(service.retryCurrentCover(true) && service.coverService_.verified==1 && service.pending_.coverUrl.empty(),"manual test failed to invalidate only saved job");
        check(service.pending_.update.urgentClear && service.coverEpoch_>0,"manual retry did not publish privacy clear");
        const std::string credential="generated-relay-credential-01234567890123456789";
        check(service.setCoverConfiguration(P::privateRelay,"https://relay.example.test/",credential,true,coverProviderBit(P::catbox),""),"private relay preference failed");
        check(presenceCoverConfiguration()->relayCredential==credential && g_relayCredential!=credential,"private relay credential migration failed");
        check(service.setCoverConfiguration(P::catbox,"https://relay.example.test","",false,coverProviderBit(P::catbox),""),"opt-out save failed");
        check(!presenceCoverConfiguration()->allowsCovers()&&!service.retryCurrentCover(false),"manual retry bypassed permission");
        g_coverProvider=999;loadPresenceCoverConfiguration();
        check(!presenceCoverConfiguration()->allowsCovers(),"unknown persisted provider accepted");
        std::cout<<"PASS assertions="<<assertions<<" DPAPI/persistence/consent/manual recovery\n";
    } catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
'''

def main():
    if os.name != 'nt':
        raise RuntimeError('Real DPAPI validation requires Windows')
    compiler = shutil.which('cl')
    if not compiler:
        raise RuntimeError('Import the supported MSVC environment first')
    text=(ROOT/'src/component/discord_presence_service.cpp').read_text(encoding='utf8')
    functions='\n'.join(extract_function(text,s) for s in [
        'std::string protectRelayCredential(', 'std::string unprotectRelayCredential(',
        'void loadPresenceCoverConfiguration()', 'bool DiscordPresenceService::setCoverConfiguration(',
        'bool DiscordPresenceService::retryCurrentCover('])
    with tempfile.TemporaryDirectory(prefix='goden-cover-settings-') as tmp:
        folder=Path(tmp);cpp=folder/'settings.cpp';exe=folder/'settings.exe'
        cpp.write_text(PREFIX+'\n'+functions+'\n'+SUFFIX,encoding='utf8')
        cmd=[compiler,'/std:c++20','/EHsc','/utf-8','/nologo','/W3','/DWIN32_LEAN_AND_MEAN','/DNOMINMAX',
            '/I'+str(ROOT/'src/component'),'/Fe:'+str(exe),'/Fo:'+str(folder)+os.sep,str(cpp),'/link','/cgthreads:1','Crypt32.lib']
        result=subprocess.run(cmd,text=True,capture_output=True)
        if result.returncode:
            print(result.stdout+result.stderr);return result.returncode
        result=subprocess.run([str(exe)],text=True,capture_output=True)
        if result.returncode:
            print(result.stdout+result.stderr);return result.returncode
    print(json.dumps({'schema':'goden.presence-cover-settings.v1','result':'PASS',
        'methodsSha256':hashlib.sha256(functions.encode()).hexdigest(),'realDPAPI':True,
        'personalProfileAccessed':False,'realNetwork':False,'fullPlayerTested':False,
        'seams':'SDK cfg strings/integers in memory; relay endpoint validator, publishCurrent and launchWorker stubbed',
        'results':result.stdout.strip().splitlines()}))
    return 0

if __name__=='__main__':
    raise SystemExit(main())
