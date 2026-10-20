// Complete production provider + relay, fake WinHTTP only. The build driver
// substitutes host integration headers, not provider method bodies. No player,
// personal profile, real network or original artwork is used.
#define GODEN_RELAY_FIXTURE_NO_MAIN
#define InternetHandle RelayFixtureInternetHandle
#include "discord_relay_lifecycle_tests.cpp"
#undef InternetHandle
#include <filesystem>
#include <fstream>
#include <functional>
#include <dbghelp.h>

LONG WINAPI reportCrash(EXCEPTION_POINTERS* exception) {
    std::fprintf(stderr,"Fixture exception: 0x%08lX\n",exception->ExceptionRecord->ExceptionCode);
    SymInitialize(GetCurrentProcess(),nullptr,TRUE);
    CONTEXT context=*exception->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset=context.Rip;frame.AddrPC.Mode=AddrModeFlat;
    frame.AddrStack.Offset=context.Rsp;frame.AddrStack.Mode=AddrModeFlat;
    frame.AddrFrame.Offset=context.Rbp;frame.AddrFrame.Mode=AddrModeFlat;
    for(unsigned i=0;i<24;++i) {
        if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64,GetCurrentProcess(),GetCurrentThread(),&frame,&context,
                nullptr,SymFunctionTableAccess64,SymGetModuleBase64,nullptr)) break;
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO)+256]{};
        auto* symbol=reinterpret_cast<SYMBOL_INFO*>(buffer);symbol->SizeOfStruct=sizeof(SYMBOL_INFO);symbol->MaxNameLen=255;
        DWORD64 displacement=0;
        if(SymFromAddr(GetCurrentProcess(),frame.AddrPC.Offset,&displacement,symbol))
            std::fprintf(stderr,"  %s + %llu\n",symbol->Name,static_cast<unsigned long long>(displacement));
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

namespace host_fixture {
std::string profile;
std::atomic<unsigned> artworkCalls{0};
std::uint64_t artHash=23;
}
struct metadb_handle_ptr {
    bool valid=true;
    bool is_valid() const noexcept {return valid;}
};
struct abort_callback {
    std::atomic<bool> aborted{false};
};
struct abort_callback_impl : abort_callback {
    void abort() noexcept {aborted=true;}
};
namespace core_api {
const char* get_profile_path() {return host_fixture::profile.c_str();}
}
namespace filesystem {
std::string g_get_native_path(const char* value) {return value;}
}
namespace nekorox::component {
struct ArtPixels {
    std::uint32_t width=16,height=16;
    std::uint64_t contentHash=0;
    std::vector<std::uint8_t> bgra=std::vector<std::uint8_t>(16*16*4,128);
};
std::shared_ptr<ArtPixels> loadResolvedArtworkBlocking(
    const metadb_handle_ptr&,std::uint32_t edge,abort_callback& abort) {
    ++host_fixture::artworkCalls;
    fixture::check(edge==1024,"provider exceeded artwork request bound");
    if(abort.aborted) return {};
    auto result=std::make_shared<ArtPixels>();result->contentHash=host_fixture::artHash;
    return result;
}
std::wstring utf8ToWide(std::string_view value) {
    return std::wstring(value.begin(),value.end());
}
enum class RolloutFlag {presenceV3,presenceEphemeralRelayV1};
bool rolloutFlagEnabled(RolloutFlag) {return true;}
}
// Generated copies preserve all production method bodies. Only the host header
// includes are substituted; private access is exposed in this fixture alone.
#include "discord_cover_service.fixture.cpp"

namespace cover_fixture {
using Service=nekorox::component::DiscordCoverService;
using Result=nekorox::component::DiscordCoverResult;
using Provider=nekorox::component::PresenceCoverProvider;
using Clock=std::chrono::system_clock;
using namespace std::chrono_literals;
using fixture::check;
void policy(Provider provider=Provider::catbox,bool accepted=true) {
    nekorox::component::PresenceCoverConfiguration value;
    value.provider=provider;value.disclosureAccepted=accepted;
    nekorox::component::publishPresenceCoverConfiguration(std::move(value));
}
void begin(const std::filesystem::path& root,const char* name) {
    fixture::reset();fixture::coverTransport=true;fixture::coverQaOverride=false;
    fixture::coverResponse="https://files.catbox.moe/abc123.png\n";
    host_fixture::profile=(root/name).string();
    std::filesystem::create_directories(host_fixture::profile);
    host_fixture::artworkCalls=0;policy();
}
Result resolve(Service& service,std::uint64_t key=1,
        std::optional<std::uint64_t> admission=std::nullopt) {
    return std::async(std::launch::async,[&service,key,admission] {
        return service.resolve(metadb_handle_ptr{},key,{},admission);
    }).get();
}
void disabledAndAdmission(const std::filesystem::path& root) {
    begin(root,"disabled");Service service;
    policy(Provider::unconfigured);
    check(!resolve(service).succeeded(),"disabled provider returned cover");
    policy(Provider::catbox,false);
    check(!resolve(service).succeeded(),"undisclosed provider returned cover");
    policy();const auto old=service.admissionTicket();service.cancel();
    check(!resolve(service,1,old).succeeded(),"stale admission accepted");
    check(fixture::opens==0 && host_fixture::artworkCalls==0,"denied request performed work");
    policy(Provider::privateRelay);
    check(!service.upload(1,{137U,80U}, {},service.admissionTicket()).succeeded() && fixture::opens==0,
        "replacement upload bypassed provider boundary");
    std::puts("PASS cover disabled/disclosure/stale admission performs no artwork/network work");
}
void persistenceAndEncoding(const std::filesystem::path& root) {
    begin(root,"persistent");
    {
        Service first;const auto result=resolve(first);
        check(result.succeeded() && result.source==Result::Source::upload,"first upload failed");
        check(result.url=="https://wsrv.nl/?url=https%3A%2F%2Ffiles.catbox.moe%2Fabc123.png&output=png&maxage=1y","unexpected compatibility delivery URL");
        check(fixture::coverUploads==1,"first upload count");
        std::string body;
        for(const auto& h:fixture::handles) if(!h->requestBody.empty()) body=h->requestBody;
        const auto png=body.find(std::string("\x89PNG\r\n\x1a\n",8));
        check(png!=std::string::npos,"POST lacks reencoded PNG");
        check(body.substr(png+16,8)==std::string("\0\0\2\0\0\0\2\0",8),"thumbnail not 512 square");
        check(body.find(host_fixture::profile)==std::string::npos,"profile path entered upload");
        check(body.find("tEXt")==std::string::npos && body.find("eXIf")==std::string::npos,"unexpected image metadata");
        check(resolve(first).source==Result::Source::cache,"same cover not cached");
    }
    const auto posts=fixture::coverUploads;
    Service restarted;check(resolve(restarted).source==Result::Source::cache,"disk cache not recovered");
    check(fixture::coverUploads==posts,"restart uploaded redundant cover");
    restarted.clear();check(resolve(restarted).source==Result::Source::cache,"disable/re-enable lost persistent origin");
    check(fixture::coverUploads==posts,"re-enable uploaded redundant cover");
    std::puts("PASS real WIC thumbnail, metadata/path boundary, cache restart and disable/re-enable without POST");
}
void cancelPendingUpload(const std::filesystem::path& root) {
    begin(root,"cancel-post");Service service;
    fixture::holdHttpSend=true;fixture::holdClose=true;
    auto future=std::async(std::launch::async,[&] {return service.resolve(metadb_handle_ptr{},1,{});});
    auto* request=fixture::waitPending(fixture::Pending::sendHeaders);
    const auto started=std::chrono::steady_clock::now();service.cancel();
    check(std::chrono::steady_clock::now()-started<200ms,"cancel blocked caller");
    fixture::wait([&]{return request->closing;},"cancel did not close pending POST");
    check(future.wait_for(10ms)==std::future_status::timeout,"provider returned before callback drain");
    check(request->capacity>0 && request->buffer!=nullptr,"pending POST body lost");
    check(std::memcmp(request->buffer,request->requestBody.data(),request->requestBody.size())==0,"POST body destroyed before drain");
    fixture::releaseClosing();check(future.get().error=="cancelled","canceled POST published result");
    std::puts("PASS pending POST cancellation, owned body lifetime and HANDLE_CLOSING drain");
}
void cancelAfterOrigin(const std::filesystem::path& root) {
    begin(root,"cancel-delivery");Service service;
    fixture::holdCoverGet=true;
    auto future=std::async(std::launch::async,[&] {return service.resolve(metadb_handle_ptr{},1,{});});
    fixture::waitPending(fixture::Pending::sendHeaders);
    service.cancel();check(future.get().error=="cancelled","canceled delivery published result");
    check(fixture::coverUploads==1 && std::filesystem::exists(service.cachePath_),"origin not saved before delivery");
    fixture::holdCoverGet=false;
    check(resolve(service).succeeded(),"retry of persisted origin failed");
    check(fixture::coverUploads==1,"delivery retry repeated POST");
    check(host_fixture::artworkCalls==1,"delivery retry decoded artwork again");
    std::puts("PASS origin saved before CDN failure; admitted retry uses GET only and retained PNG");
}
void missingOriginPolicy(const std::filesystem::path& root) {
    begin(root,"origin-policy");Service service;
    const auto hash=nekorox::component::encodedContentHash(host_fixture::artHash);
    const auto seed=[&] {
        service.cacheLoaded_=true;service.verifiedDelivery_.clear();
        service.cache_[hash]={"https://files.catbox.moe/abc123.png",Clock::time_point::max(),Clock::now()-11min};
    };
    seed();service.cache_[hash].lastUsed=Clock::now();
    fixture::coverReplies={{503,""},{404,""}};
    check(!resolve(service).succeeded(),"fresh unavailable delivery accepted");
    check(fixture::coverGets==1 && fixture::coverReplies.size()==1 && fixture::coverUploads==0 && service.cache_.contains(hash),
        "ten-minute grace period probed/deleted/reuploaded origin");
    const auto checkLastDestinations=[] {
        std::vector<fixture::Handle*> gets;
        for(const auto& h:fixture::handles) if(h->kind==fixture::Kind::request && h->get) gets.push_back(h.get());
        check(gets.size()>=2,"delivery and origin checks missing");
        check(gets.back()->host==L"files.catbox.moe" &&
            gets.back()->path==L"/abc123.png","missing origin decision did not check independent exact origin");
    };
    seed();fixture::coverReplies={{429,""}};
    check(!resolve(service).succeeded(),"rate-limit accepted as image");
    checkLastDestinations();
    check(fixture::coverUploads==0 && service.cache_.contains(hash),"rate-limit evicted origin/reuploaded");
    seed();fixture::coverReplies={{200,fixture::validCoverPng()},{404,""}};
    check(!resolve(service).succeeded(),"CDN failure accepted as image");
    check(fixture::coverUploads==0 && service.cache_.contains(hash),"CDN 404 evicted readable origin");
    seed();fixture::coverReplies={{200,"",false}};
    check(!resolve(service).succeeded(),"empty response without length accepted");
    check(fixture::coverUploads==0 && service.cache_.contains(hash),"unknown-length EOF evicted origin");
    seed();fixture::coverReplies={{200,"bad"}};
    check(!resolve(service).succeeded(),"truncated origin accepted");
    check(fixture::coverUploads==0 && service.cache_.contains(hash),"truncation evicted origin");
    seed();fixture::coverReplies={{404,""}};
    check(resolve(service).succeeded(),"confirmed missing origin not replaced");
    check(fixture::coverUploads==1,"confirmed missing origin replacement POST count");
    seed();fixture::coverReplies={{410,""}};
    check(resolve(service).succeeded() && fixture::coverUploads==2,"confirmed gone origin not replaced once");
    seed();service.saveCache();
    Service restarted;
    fixture::coverReplies={{200,""}};
    check(resolve(restarted).succeeded() && fixture::coverUploads==3,"persisted zero-byte origin not replaced once");
    check(resolve(restarted).source==Result::Source::cache && fixture::coverUploads==3,
        "recovered origin uploaded again");
    seed();service.cache_[hash].lastUsed=Clock::now();
    fixture::coverReplies={{200,""}};
    check(!resolve(service).succeeded() && fixture::coverUploads==3 && fixture::coverReplies.empty(),
        "fresh zero-byte suspicion bypassed grace period");
    fixture::coverReplies.clear();
    std::puts("PASS fresh-origin grace, CDN/rate-limit/truncation preservation and independent origin 404/410/empty recovery across restart");
}
std::vector<std::uint8_t> decodedPng(const std::vector<std::uint8_t>& png) {
    const auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    check(SUCCEEDED(initialized),"test COM init failed");
    std::vector<std::uint8_t> pixels(512U*512U*4U);
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()))),"test WIC factory failed");
        check(SUCCEEDED(factory->CreateStream(stream.ReleaseAndGetAddressOf())) &&
            SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()),static_cast<DWORD>(png.size()))) &&
            SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,
                decoder.ReleaseAndGetAddressOf())) &&
            SUCCEEDED(decoder->GetFrame(0,frame.ReleaseAndGetAddressOf())),"full PNG decode failed");
        UINT width=0,height=0;
        check(SUCCEEDED(frame->GetSize(&width,&height)) && width==512 && height==512,
            "repair changed thumbnail dimensions");
        check(SUCCEEDED(factory->CreateFormatConverter(converter.ReleaseAndGetAddressOf())) &&
            SUCCEEDED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,
                nullptr,0.0,WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(converter->CopyPixels(nullptr,512U*4U,static_cast<UINT>(pixels.size()),pixels.data())),
            "full PNG pixel read failed");
    }
    CoUninitialize();return pixels;
}
void deduplicatedOriginRepair(const std::filesystem::path& root) {
    begin(root,"dedup-repair");Service service;service.cacheLoaded_=true;
    const auto hash=nekorox::component::encodedContentHash(host_fixture::artHash);
    nekorox::component::ArtPixels pixels;
    const auto ordinary=nekorox::component::encodePng(pixels,512);
    check(!ordinary.empty(),"ordinary fixture encode failed");
    service.prepared_={1,hash,ordinary};
    service.cache_[hash]={"https://files.catbox.moe/bad123.png",Clock::time_point::max(),Clock::now()-11min};
    fixture::coverReplies={{200,""}};
    check(resolve(service).succeeded(),"retained PNG did not recover a broken dedup origin");
    check(fixture::coverUploads==1 && host_fixture::artworkCalls==1,"repair duplicated POST/artwork work");
    std::string body;for(const auto& h:fixture::handles)if(!h->requestBody.empty())body=h->requestBody;
    const auto start=body.find(std::string("\x89PNG\r\n\x1a\n",8));
    const auto end=body.rfind("\r\n--");
    check(start!=std::string::npos && end>start,"repair multipart is invalid");
    const std::vector<std::uint8_t> repaired(body.begin()+start,body.begin()+end);
    check(repaired!=ordinary,"repair repeated deduplicated PNG bytes");
    check(decodedPng(repaired)==decodedPng(ordinary),"lossless repair changed decoded pixels");
    check(body.find("tEXt")==std::string::npos && body.find("eXIf")==std::string::npos,
        "repair introduced metadata");
    check(resolve(service).source==Result::Source::cache && fixture::coverUploads==1,
        "repair cache repeated POST");
    begin(root,"dedup-changed-art");Service changed;changed.cacheLoaded_=true;
    changed.prepared_={1,hash,ordinary};
    changed.cache_[hash]={"https://files.catbox.moe/bad123.png",Clock::time_point::max(),Clock::now()-11min};
    const auto previousArt=host_fixture::artHash;++host_fixture::artHash;
    const auto newHash=nekorox::component::encodedContentHash(host_fixture::artHash);
    changed.cache_[newHash]={"https://files.catbox.moe/good12.png",Clock::time_point::max(),Clock::now()};
    changed.verifiedDelivery_.insert(newHash);
    fixture::coverReplies={{200,""}};
    const auto newArt=resolve(changed);
    check(newArt.source==Result::Source::cache && newArt.contentHash==newHash && fixture::coverUploads==0,
        "rejection of old artwork replaced a different valid image");
    check(changed.cache_.contains(hash) && changed.cache_.contains(newHash),"artwork handoff erased cache evidence");
    host_fixture::artHash=previousArt;
    std::puts("PASS retained broken-origin repair changes PNG bytes, preserves every decoded pixel and dimensions, no metadata or repeat POST");
}
void repeatedBrokenOriginRepair(const std::filesystem::path& root) {
    begin(root,"dedup-second-repair");Service first;first.cacheLoaded_=true;
    const auto hash=nekorox::component::encodedContentHash(host_fixture::artHash);
    first.cache_[hash]={"https://files.catbox.moe/bad123.png",Clock::time_point::max(),Clock::now()-11min,1};
    fixture::coverResponse="https://files.catbox.moe/next01.png";
    fixture::coverReplies={{200,""}};
    check(resolve(first).succeeded(),"first repair failed");
    const auto lastPng=[] {
        std::string body;for(const auto& h:fixture::handles)if(!h->requestBody.empty())body=h->requestBody;
        const auto start=body.find(std::string("\x89PNG\r\n\x1a\n",8));
        const auto end=body.rfind("\r\n--");
        check(start!=std::string::npos && end>start,"repair multipart invalid");
        return std::vector<std::uint8_t>(body.begin()+start,body.begin()+end);
    };
    const auto previous=lastPng();
    first.cache_[hash].lastUsed=Clock::now()-11min;first.saveCache();
    Service restarted;
    fixture::coverResponse="https://files.catbox.moe/next02.png";
    fixture::coverReplies={{200,""}};
    check(resolve(restarted).succeeded(),"second confirmed broken origin did not recover after restart");
    const auto replacement=lastPng();
    check(replacement!=previous,"second repair repeated the previous rejected PNG bytes");
    check(decodedPng(replacement)==decodedPng(previous),"second repair changed decoded pixels");
    check(fixture::coverUploads==2,"second repair posted more than once");
    std::vector<std::vector<std::uint8_t>> rejected{previous,replacement};
    for(unsigned attempt=0;attempt<5;++attempt) {
        restarted.cache_[hash].lastUsed=Clock::now()-11min;
        restarted.verifiedDelivery_.clear();restarted.saveCache();
        const auto posts=fixture::coverUploads;
        fixture::coverReplies={{200,""}};
        const auto result=resolve(restarted);
        if(!result.succeeded()) {
            check(result.error=="cover storage recovery limit reached" && fixture::coverUploads==posts,
                "exhausted recovery posted or reported the wrong failure");
            Service exhausted;
            fixture::coverReplies={{200,""}};
            check(resolve(exhausted).error==result.error && fixture::coverUploads==posts,
                "restart reset exhausted storage recovery");
            fixture::coverReplies.clear();
            check(resolve(exhausted).source==Result::Source::cache && fixture::coverUploads==posts,
                "exhaustion blocked a now-readable cached image");
            break;
        }
        const auto candidate=lastPng();
        check(std::find(rejected.begin(),rejected.end(),candidate)==rejected.end(),
            "storage recovery cycled through rejected representations");
        check(decodedPng(candidate)==decodedPng(previous),"bounded recovery changed pixels");
        rejected.push_back(candidate);
        check(attempt<4,"storage recovery did not stop");
    }
    check(rejected.size()<=4,"storage recovery exceeded lossless encoding bound");
    std::puts("PASS repeated broken storage uses different lossless bytes after restart");
}
void emptyAcknowledgementAndLegacyRetry(const std::filesystem::path& root) {
    begin(root,"empty-ack-legacy");Service service;service.cacheLoaded_=true;
    const auto hash=nekorox::component::encodedContentHash(host_fixture::artHash);
    service.cache_[host_fixture::artHash]={"https://files.catbox.moe/bad123.png",Clock::time_point::max(),Clock::now()-11min};
    fixture::coverReplies={{200,""}};fixture::coverResponse="";
    check(resolve(service).error=="cover host returned an empty upload response",
        "empty acknowledgement accepted as upload success");
    check(fixture::coverUploads==1 && !service.cache_.contains(hash),"empty acknowledgement cached a URL");
    const auto encoding=service.prepared_.pngEncoding;
    check(encoding>=3 && !service.prepared_.png.empty(),"repair bytes not retained for retry");
    fixture::coverResponse="https://files.catbox.moe/good12.png";
    const auto loads=host_fixture::artworkCalls.load();
    check(resolve(service).source==Result::Source::upload && fixture::coverUploads==2 &&
            host_fixture::artworkCalls==loads,"ack retry repeated artwork work or upload");
    check(service.cache_[hash].pngEncoding==encoding,"legacy retry forgot its lossless encoding");
    Service restarted;restarted.loadCache();
    check(restarted.cache_[hash].pngEncoding==encoding,"encoding not recovered after retry/restart");
    std::puts("PASS empty acknowledgement preserves retained bytes and encoding across legacy migration/restart");
}
void encodingCacheRecords(const std::filesystem::path& root) {
    begin(root,"encoding-records");Service first;first.cacheLoaded_=true;
    const auto now=Clock::now();
    for(std::uint64_t key=1;key<=512;++key) {
        first.cache_[key]={"https://files.catbox.moe/abc123.png",Clock::time_point::max(),now,
            1U+static_cast<unsigned>(key%5)};
    }
    first.saveCache();Service loaded;loaded.loadCache();
    check(loaded.cache_.size()==512,"comment records truncated bounded cache");
    for(const auto& [hash,entry]:loaded.cache_)
        check(entry.pngEncoding==1U+hash%5,"typed encoding cache roundtrip failed");
    begin(root,"malformed-encoding-records");Service malformed;
    std::filesystem::create_directories(malformed.cachePath_.parent_path());
    {
        std::ofstream output(malformed.cachePath_);
        output<<"goden.discord-cover-cache.v2\n"
              <<"#png-encoding-v1\t17\t4\n" // Unbound annotations are ignored.
              <<"17\t0\t0\thttps://files.catbox.moe/abc123.png\n"
              <<"#png-encoding-v1\t17\t0\n#png-encoding-v1\t17\t6\n"
              <<"#png-encoding-v1\t17\t-1\n#png-encoding-v1\t17\t4junk\n"
              <<"#png-encoding-v1\t17junk\t3\n#png-encoding-v1\t17\t99999999999999999999999999\n";
    }
    malformed.loadCache();
    check(malformed.cache_.size()==1 && malformed.cache_[23].pngEncoding==0,
        "malformed encoding annotation changed cache policy");
    std::puts("PASS optional encoding records preserve v2 URL rows, bounds and legacy/untrusted parsing");
}
void boundedCacheAndLegacy(const std::filesystem::path& root) {
    begin(root,"bounded");Service service;service.cacheLoaded_=true;
    const auto now=Clock::now();
    for(std::uint64_t key=1;key<=700;++key) {
        service.cache_[key]={"https://files.catbox.moe/abc123.png",Clock::time_point::max(),now+std::chrono::seconds(key)};
        service.verifiedDelivery_.insert(key);
    }
    service.pruneCache();
    check(service.cache_.size()==512 && service.verifiedDelivery_.size()==512,"verification identities escaped LRU bound");
    for(const auto key:service.verifiedDelivery_) check(service.cache_.contains(key),"orphaned verification identity");
    service.cache_.begin()->second.expires=now-1s;service.pruneCache();
    check(service.cache_.size()==511 && service.verifiedDelivery_.size()==511,"expiry retained verification identity");
    service.clear();service.cacheLoaded_=true;
    const auto legacy=host_fixture::artHash;
    service.cache_[legacy]={"https://files.catbox.moe/abc123.png",Clock::time_point::max(),now};
    check(resolve(service).source==Result::Source::cache,"legacy origin migration failed");
    const auto reads=fixture::coverGets;
    check(resolve(service).source==Result::Source::cache && fixture::coverGets==reads,"legacy verification repeated GET");
    check(service.verifiedDelivery_.contains(legacy),"verification not keyed by stored legacy identity");
    service.cache_[legacy].expires=now-1s;service.pruneCache();
    check(service.verifiedDelivery_.empty(),"legacy eviction left verification identity");
    std::puts("PASS LRU/expiry bounds include verification state; legacy cache keeps fast path");
}
void automaticProxyReads(const std::filesystem::path& root) {
    begin(root,"proxy-reads");fixture::requireCoverGetProxy=true;
    {
        Service first;const auto result=resolve(first);
        check(result.succeeded() && first.preferAutomaticProxy_,"post-upload GET did not recover through proxy");
        check(fixture::coverUploads==1 && fixture::coverGets==3,"proxy fallback repeated upload or looped");
    }
    const auto reads=fixture::coverGets;
    Service restarted;
    check(resolve(restarted).source==Result::Source::cache && restarted.preferAutomaticProxy_,
        "cached first GET after restart did not recover through proxy");
    check(fixture::coverUploads==1 && fixture::coverGets==reads+3,"cached proxy recovery repeated upload or looped");
    const auto verifiedReads=fixture::coverGets;
    check(resolve(restarted).source==Result::Source::cache && fixture::coverGets==verifiedReads,
        "verified proxy cover repeated a GET");
    fixture::requireCoverGetProxy=false;fixture::requireCoverReadProxy=true;
    Service readFailure;
    const auto previous=fixture::coverGets;
    check(resolve(readFailure).source==Result::Source::cache && readFailure.preferAutomaticProxy_ &&
        fixture::coverGets==previous+3 && fixture::coverUploads==1,
        "HTTP 200 followed by body timeout did not recover cached GET through proxy");
    std::puts("PASS direct GET failure -> one system-proxy GET, same origin, no redundant POST, restart cache");
}
void completeStoredPng(const std::filesystem::path& root) {
    begin(root,"complete-stored-png");Service service;
    const auto good=fixture::validCoverPng();
    const auto truncated=good.substr(0,good.size()-15U); // incomplete IDAT CRC, no IEND
    auto badCrc=good;badCrc[good.size()-1U]^=1;
    const auto probe=[&](fixture::CoverReply reply,bool expectedValid,bool expectedBroken) {
        fixture::coverReplies.clear();fixture::coverReplies.push_back(std::move(reply));
        unsigned status=0;bool broken=false;
        const auto valid=std::async(std::launch::async,[&] {
            return service.verifyRemotePng("https://files.catbox.moe/probe1.png",true,{},
                service.admissionTicket(),&status,&broken);
        }).get();
        check(valid==expectedValid && broken==expectedBroken,"complete PNG classification mismatch");
    };
    fixture::readChunk=3;
    probe({200,good},true,false);
    probe({200,truncated},false,true);
    probe({200,badCrc},false,true);
    probe({200,good.substr(0,good.size()-12U)},false,true);
    probe({200,truncated,false},false,false);
    probe({200,truncated,true,static_cast<DWORD>(good.size())},false,false);
    probe({200,"<html>temporary upstream error</html>"},false,false);
    probe({200,good,true,1024U*1024U+1U},false,false);
    fixture::readChunk=8;
    probe({200,good,true,std::nullopt,8U},false,false);
    check(fixture::coverUploads==0,"PNG verification uploaded artwork");

    const auto hash=nekorox::component::encodedContentHash(host_fixture::artHash);
    const auto seed=[&] {
        service.cacheLoaded_=true;service.verifiedDelivery_.clear();service.prepared_={};
        service.cache_[hash]={"https://files.catbox.moe/stored.png",Clock::time_point::max(),Clock::now()-11min,1};
    };
    seed();fixture::coverReplies={{200,truncated,false}};
    check(!resolve(service).succeeded() && fixture::coverUploads==0 && service.cache_.contains(hash),
        "unknown-length PNG damage authorized a POST");
    seed();fixture::coverReplies={{200,truncated,true,static_cast<DWORD>(good.size())}};
    check(!resolve(service).succeeded() && fixture::coverUploads==0 && service.cache_.contains(hash),
        "short network read authorized a POST");
    seed();fixture::coverReplies={{200,truncated}};
    check(resolve(service).succeeded() && fixture::coverUploads==1,
        "exact-length stored truncated PNG did not recover once");
    check(service.cache_.at(hash).pngEncoding>1,"stored corrupt PNG repair repeated initial encoding");
    check(resolve(service).source==Result::Source::cache && fixture::coverUploads==1,
        "stored corrupt PNG recovery repeated POST");
    std::puts("PASS full bounded PNG chunks/CRCs/EOF; stored truncation recovers once; unknown length, short reads, timeout, HTML and oversized responses cannot authorize POST");
}
void proxyCannotHideOriginDamage(const std::filesystem::path& root) {
    begin(root,"proxy-masked-corruption");Service cached;cached.cacheLoaded_=true;
    const auto hash=nekorox::component::encodedContentHash(host_fixture::artHash);
    const auto truncated=fixture::validCoverPng().substr(0,50);
    fixture::CoverReply damaged{200,truncated};damaged.host=L"files.catbox.moe";
    cached.cache_[hash]={"https://files.catbox.moe/source.png",Clock::time_point::max(),Clock::now(),1};
    fixture::coverReplies={damaged}; // only the origin is damaged; proxy stays valid
    check(!resolve(cached).succeeded() && fixture::coverUploads==0 && fixture::coverGets==1 &&
        !cached.verifiedDelivery_.contains(hash),"healthy proxy concealed fresh damaged origin");
    check(fixture::handles.back()->host==L"files.catbox.moe","origin was not validated first");
    cached.cache_[hash].lastUsed=Clock::now()-11min;
    fixture::coverReplies={damaged};
    check(resolve(cached).succeeded() && fixture::coverUploads==1 && cached.verifiedDelivery_.contains(hash),
        "healthy proxy blocked aged damaged-origin recovery");

    begin(root,"new-upload-broken-origin");Service uploaded;
    for(unsigned i=0;i<32;++i) fixture::coverReplies.push_back(damaged);
    const auto result=resolve(uploaded);
    check(result.error=="cover origin was not complete after upload" && fixture::coverUploads==1 &&
        uploaded.cache_.contains(hash) && !uploaded.verifiedDelivery_.contains(hash),
        "new incomplete origin was published or lost its acknowledged URL");
    for(const auto& h:fixture::handles) if(h->kind==fixture::Kind::request && h->get)
        check(h->host==L"files.catbox.moe","CDN was trusted before complete origin");
    fixture::coverReplies.clear();fixture::requireCoverReadProxy=true;
    check(resolve(uploaded).source==Result::Source::cache && fixture::coverUploads==1,
        "transient-origin retry repeated POST");
    const auto gets=fixture::coverGets;
    check(resolve(uploaded).source==Result::Source::cache && fixture::coverGets==gets,
        "verified origin/delivery pair lost network-free fast path");
    Service restarted;
    check(resolve(restarted).source==Result::Source::cache && fixture::coverUploads==1,
        "restart repeated acknowledged upload");
    std::puts("PASS healthy CDN cannot conceal damaged origin; fresh upload remains cached/unverified; GET-only retry and restart recover without duplicate POST");
}
}
int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    std::setvbuf(stderr,nullptr,_IONBF,0);
    SetUnhandledExceptionFilter(reportCrash);
    fixture::check(argc==2,"provide a unique disposable fixture root");
    const auto root=std::filesystem::absolute(argv[1]);
    fixture::check(!std::filesystem::exists(root),"fixture root already exists");
    std::filesystem::create_directories(root);
    cover_fixture::disabledAndAdmission(root);
    cover_fixture::persistenceAndEncoding(root);
    cover_fixture::cancelPendingUpload(root);
    cover_fixture::cancelAfterOrigin(root);
    cover_fixture::missingOriginPolicy(root);
    cover_fixture::deduplicatedOriginRepair(root);
    cover_fixture::repeatedBrokenOriginRepair(root);
    cover_fixture::emptyAcknowledgementAndLegacyRetry(root);
    cover_fixture::encodingCacheRecords(root);
    cover_fixture::boundedCacheAndLegacy(root);
    cover_fixture::automaticProxyReads(root);
    cover_fixture::completeStoredPng(root);
    cover_fixture::proxyCannotHideOriginDamage(root);
    fixture::reset();
    std::puts("All cover provider lifecycle tests PASS (production methods/WIC, fake transport).");
}
