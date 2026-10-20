// Private SDK-host probe. The real provider/LatestWorker and main-thread
// dispatch are linked below; only the external library/filesystem data is
// controlled. This component is never part of a player distribution.
#include "../src/component/library_roots_provider.h"
#include "../src/component/text_util.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
DECLARE_COMPONENT_VERSION("Goden private library roots QA", "0.1", "Not distributable");
VALIDATE_COMPONENT_FILENAME("foo_roots_qa.dll");
FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
using namespace nekorox::component;
using namespace std::chrono_literals;
namespace {
std::thread runner;
std::filesystem::path qaRoot;
metadb_handle_list fixture;
std::atomic<unsigned> stage{0}, built{0};
std::atomic<unsigned> captures{0}, relatives{0}, probes{0}, publications{0}, version{1};
std::atomic<bool> blockRelative{false}, blockOnline{false}, entered{false}, captureError{false};
HANDLE gate = nullptr;
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
template<class F> void onMain(F fn) {
    auto done = std::make_shared<std::promise<void>>(); auto future = done->get_future();
    fb2k::inMainThread([done,fn] { try { fn();done->set_value(); } catch (...) { done->set_exception(std::current_exception()); } });
    if(future.wait_for(15s) != std::future_status::ready) throw std::runtime_error("main-thread deadline stage="+std::to_string(stage.load())+" built="+std::to_string(built.load())); future.get();
}
template<class F> void until(F predicate) {
    const auto end = std::chrono::steady_clock::now() + 10s;
    while (!predicate()) { check(std::chrono::steady_clock::now() < end, "condition deadline"); std::this_thread::sleep_for(2ms); }
}
void observer(std::shared_ptr<const LibraryRootsSnapshot>) { check(core_api::is_main_thread(), "observer affinity"); ++publications; }
void block(std::atomic<bool>& flag) {
    if (flag.exchange(false)) { entered = true; check(WaitForSingleObject(gate,15000)==WAIT_OBJECT_0,"fixture gate deadline"); }
}
void prepareBlock(std::atomic<bool>& flag) { entered = false; ResetEvent(gate);flag=true; }
}
namespace nekorox::component {
struct LibraryRootsTestPeer {
    static bool registered(LibraryRootsProvider& p) { return p.registered_; }
    static bool idle(LibraryRootsProvider& p) { return !p.busy_ && !p.posted_; }
    static void added(LibraryRootsProvider& p) { p.on_items_added(fixture); }
    static void removed(LibraryRootsProvider& p) { p.on_items_removed(fixture); }
};
namespace testing {
void captureLibraryRoots(metadb_handle_list& items, LibraryRootsSnapshot& next) {
    check(core_api::is_main_thread(), "capture must stay main-thread"); ++captures;
    if(captureError.exchange(false))throw std::runtime_error("injected capture failure");
    items.add_items(fixture); next.libraryEnabled=true;
    next.configuredDestination=qaRoot/L"configured";
    next.configuredPattern="version-"+std::to_string(version.load());
}
bool relativeLibraryPath(const metadb_handle_ptr& item,pfc::string_base& relative) {
    check(!core_api::is_main_thread(), "relative path unexpectedly on main-thread");
    ++relatives; block(blockRelative);
    relative = "album\\track.opus"; return true;
}
bool libraryRootOnline(const std::filesystem::path& path) {
    check(!core_api::is_main_thread(), "filesystem probe unexpectedly on main-thread");
    ++probes; block(blockOnline); std::error_code error;
    return std::filesystem::is_directory(path,error) && !error;
}
bool libraryRootAddable(const char* path) {
    check(!core_api::is_main_thread(), "policy query unexpectedly on main-thread");
    return std::string_view(path).find("offline") == std::string_view::npos;
}
}
}
namespace {
void waitIdle() {
    until([] { auto idle=std::make_shared<bool>(false);onMain([idle] { *idle=LibraryRootsTestPeer::idle(LibraryRootsProvider::instance()); }); return *idle; });
}
void validateSnapshot(unsigned expectedVersion) {
    auto s=LibraryRootsProvider::instance().snapshot();
    check(s->error.empty() && s->libraryEnabled && s->roots.size()==2,"snapshot fields/unique roots");
    check(s->configuredDestination==qaRoot/L"configured" && s->configuredPattern=="version-"+std::to_string(expectedVersion),"config/latest version");
    for(const auto& root:s->roots) {
        const bool offline=root.path.filename()==L"offline";
        check(root.online==!offline && root.policyAddable==!offline,"availability/policy changed");
    }
}
void run() {
    std::ofstream log(qaRoot/L"result-v2.log");
    try {
        auto& p=LibraryRootsProvider::instance();
        onMain([&] {
            stage=1;auto db=metadb::get();
            for(unsigned i=0;i<10000;++i) {
                const auto folder=(i%2)?L"online":L"offline";
                auto path=qaRoot/folder/L"album"/(std::to_wstring(i)+L".opus");
                metadb_handle_ptr item;
                db->handle_create(item,make_playable_location(wideToUtf8(path.wstring()).c_str(),0));
                fixture.add_item(item); built=i+1;
            }
            stage=2;for(unsigned i=0;i<1000;++i) LibraryRootsTestPeer::added(p);
            check(captures==0 && !LibraryRootsTestPeer::registered(p),"hidden scans or subscription");
            stage=3;p.observe(observer);stage=4;for(unsigned i=0;i<1000;++i)p.refresh();stage=5;
            check(captures==0,"refresh enumerated synchronously");
        });
        waitIdle();check(captures==1 && relatives==10000 && publications==1,"initial coalescing");validateSnapshot(1);
        log<<"1000 hidden notifications: 0 captures; 1000 visible requests: 1 capture, 10000 worker paths; PASS\n"<<std::flush;
        prepareBlock(blockRelative);
        onMain([&]{p.refresh();}); until([]{return entered.load();});
        const auto before=captures.load(), published=publications.load(), paths=relatives.load();
        const auto begin=std::chrono::steady_clock::now();
        onMain([&]{version=2;for(unsigned i=0;i<1000;++i)LibraryRootsTestPeer::removed(p);check(captures==before,"copies stacked during scan");});
        const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        check(elapsed<1000,"refresh waited on blocked worker");
        SetEvent(gate);waitIdle();check(captures==before+1 && publications==published+1,"stale scan published or latest lost");
        check(relatives==paths+10000,"stale worker kept scanning");validateSnapshot(2);
        log<<"Blocked worker + 1000 notifications: "<<elapsed<<" ms including main-thread dispatch, one latest capture; PASS\n"<<std::flush;
        prepareBlock(blockOnline);onMain([&]{p.refresh();});until([]{return entered.load();});
        const auto oldPublished=publications.load(), oldCaptures=captures.load();
        onMain([&]{p.suspend();check(!LibraryRootsTestPeer::registered(p),"hidden registration");version=3;for(unsigned i=0;i<1000;++i)p.refresh();});
        SetEvent(gate);waitIdle();check(captures==oldCaptures && publications==oldPublished,"hidden completion/scan");
        onMain([&]{p.observe(observer);});waitIdle();validateSnapshot(3);
        log<<"Suspend during filesystem check, no hidden publication/work, reopen latest: PASS\n"<<std::flush;
        prepareBlock(blockRelative);onMain([&]{p.refresh();});until([]{return entered.load();});
        const auto beforeReopen=publications.load();
        onMain([&]{p.suspend();version=4;p.observe(observer);});SetEvent(gate);waitIdle();
        check(publications==beforeReopen+1,"reopen accepted stale completion");validateSnapshot(4);
        onMain([&]{captureError=true;p.refresh();});waitIdle();
        check(p.snapshot()->error=="injected capture failure","capture error not published");
        onMain([&]{p.refresh();});waitIdle();validateSnapshot(4);
        log<<"Reopen before old completion; capture error then recovery: PASS\n"<<std::flush;
        prepareBlock(blockRelative);onMain([&]{p.refresh();});until([]{return entered.load();});
        const auto beforeQuit=publications.load();
        std::thread release([]{std::this_thread::sleep_for(30ms);SetEvent(gate);});
        onMain([&]{p.shutdownAndDrain();p.shutdownAndDrain();p.observe(observer);p.refresh();fixture.remove_all();
            check(!LibraryRootsTestPeer::registered(p),"shutdown registration");});release.join();
        onMain([]{});check(publications==beforeQuit,"shutdown publication");
        log<<"Explicit shutdown joins worker, unregisters, no late publication/restart: PASS\nDONE\n";
    } catch(const std::exception& e) {
        SetEvent(gate);log<<"ERROR: "<<e.what()<<'\n';
        try{onMain([]{LibraryRootsProvider::instance().shutdownAndDrain();});}catch(...){}
    }
}
class Init : public initquit {
 void on_init() override {
    wchar_t hooks[8]{},profile[8]{},root[2048]{};
    GetEnvironmentVariableW(L"NEKOROX_QA_HOOKS",hooks,8);GetEnvironmentVariableW(L"NEKOROX_QA_PROFILE",profile,8);
    GetEnvironmentVariableW(L"NEKOROX_ROOTS_QA",root,2048);
    if(std::wstring_view(hooks)!=L"1"||std::wstring_view(profile)!=L"1"||!root[0])return;
    qaRoot=root;if(qaRoot.filename()!=L"continuous-20261013-library-roots")return;
    std::filesystem::create_directories(qaRoot/L"online");
    gate=CreateEventW(nullptr,TRUE,TRUE,nullptr);
    (void)LibraryRootsProvider::instance();
    runner=std::thread([] { std::this_thread::sleep_for(2s);run(); });
 }
 void on_quit() override {
    if(!gate)return;SetEvent(gate);
    LibraryRootsProvider::instance().shutdownAndDrain();
    if(runner.joinable())runner.join();CloseHandle(gate);gate=nullptr;
 }
};
initquit_factory_t<Init> init;
}
