#include "media_binding_recovery.h"
#include "nekorox/track_identity.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace nekorox;
using namespace nekorox::component;
namespace {
int checks = 0;
void require(bool okay, const char* message) { ++checks; if (!okay) throw std::runtime_error(message); }
std::string read(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}};
}
void write(const std::filesystem::path& p, std::string_view bytes) {
    std::ofstream out(p, std::ios::binary); out << bytes; require(bool(out), "Fixture write");
}
PersistedTrack identity(const std::filesystem::path& path, unsigned sub = 0) {
    PersistedTrack t; t.subsong = sub; t.fileSize = std::filesystem::file_size(path);
    t.modifiedTicks = static_cast<std::uint64_t>(std::filesystem::last_write_time(path).time_since_epoch().count());
    auto id = makeTrackId({path, sub, t.fileSize, t.modifiedTicks});
    t.trackId = id.value; t.partialFingerprint = id.partialFingerprint; t.path = id.normalizedPathUtf8; return t;
}
struct Fixture {
    std::filesystem::path root, file, journal; MediaCopyExecutionOptions options; PersistedTrack old;
    explicit Fixture(std::filesystem::path p) : root(std::move(p)), file(root/L"media"/L"file.flac"),
        journal(root/L"journals"/L"item.json") {
        std::filesystem::create_directories(file.parent_path()); write(file, std::string(180000, 'a'));
        old = identity(file); options.journalPath = journal; options.backupRoot = root/L"backups";
        options.captureSourceRevisions = [](const auto& p) {
            auto t=identity(p); return std::vector<MediaRevisionBinding>{{t.trackId,t}};
        };
        options.mutateStaging = [](const auto& p, auto&) { std::ofstream out(p,std::ios::binary|std::ios::app);out<<"NEW";return bool(out); };
    }
    MediaCopyResult execute() {
        auto plan=buildMediaPlan({},{{file,{}}},MediaOrganizationMode::inPlace,MediaCollisionPolicy::replace);
        auto results=executeMediaPlan(plan,{},options);require(results.size()==1,"One result");return results[0];
    }
    MediaBindingRecoveryScan scan() {return scanMediaBindingRecovery(journal.parent_path(),{});}
};
}
int wmain(int argc,wchar_t**argv){try{
    require(argc==2,"Fresh fixture root required");const std::filesystem::path root(argv[1]);
    require(root.is_absolute()&&root.wstring().find(L"continuous-20261014-media-recovery")!=std::wstring::npos,
        "Only round fixtures");require(!std::filesystem::exists(root),"No overwriting evidence");
    {
        Fixture f(root/L"before-intent");f.options.allowStep=[](auto step){return step!=L"before-verified-journal";};
        const auto result=f.execute();require(result.outcome==MediaCopyOutcome::failed,"Intent refusal blocks commit");
        require(identity(f.file).trackId==f.old.trackId,"Intent failure preserves source");
        require(f.scan().pending.empty(),"No false recovery before durable intent");
    }
    {
        Fixture f(root/L"before-rename");bool durable=false;
        f.options.allowStep=[&](auto step){if(step==L"before-final-rename"){
            durable=read(f.journal).find(f.old.trackId)!=std::string::npos;return false;}return true;};
        auto result=f.execute();require(durable,"Previous identity durable before rename");
        require(result.outcome==MediaCopyOutcome::conflict&&identity(f.file).trackId==f.old.trackId,"Rename refused safely");
        require(f.scan().pending.empty(),"Known failed rename is not pending");
    }
    {
        Fixture f(root/L"after-rename-failure");
        f.options.allowStep=[&](auto step){if(step==L"after-final-rename")write(f.file,"Changed before final verification");return true;};
        const auto result=f.execute();
        require(result.outcome==MediaCopyOutcome::failed&&result.physicalCommitNeedsBinding,"Post-rename failure retains binding admission flag");
        require(f.scan().blockedPaths.size()==1,"Post-rename mismatch leaves durable recovery attention");
    }
    {
        Fixture f(root/L"external");auto result=f.execute();require(result.outcome==MediaCopyOutcome::copied,"Commit");
        write(f.file,"Externally changed after edit");auto scan=f.scan();
        require(scan.pending.empty()&&scan.blockedPaths.size()==1,"External edit blocks replay");
        require(read(f.file)=="Externally changed after edit","Recovery never alters external bytes");
    }
    {
        Fixture f(root/L"missing");f.execute();std::filesystem::remove(f.file);auto scan=f.scan();
        require(scan.pending.empty()&&scan.blockedPaths.size()==1,"Missing media blocks replay");
    }
    {
        Fixture f(root/L"ambiguous");f.execute();
        std::filesystem::copy_file(f.journal,f.journal.parent_path()/L"second.json");auto scan=f.scan();
        require(scan.pending.empty()&&scan.blockedPaths.size()==2,"Ambiguous chain does not pick newest");
    }
    {
        Fixture f(root/L"receipt");f.execute();auto scan=f.scan();require(scan.pending.size()==1,"Pending exists");
        auto item=scan.pending[0];write(f.journal,read(f.journal)+" \n");
        require(!acknowledgeMediaBindingRecovery(item,{}),"Stale callback cannot confirm new journal bytes");
        scan=f.scan();require(scan.pending.size()==1,"Changed journal still pending");
        // Receipt format/idempotence only; process probe separately commits real DB before ack.
        require(acknowledgeMediaBindingRecovery(scan.pending[0],{}),"Receipt persisted");
        require(acknowledgeMediaBindingRecovery(scan.pending[0],{}),"Receipt idempotent");
        require(f.scan().pending.empty(),"Confirmed record excluded");
        write(f.journal,read(f.journal)+" \n");require(f.scan().pending.size()==1,"Receipt cannot hide altered record");
    }
    {
        Fixture f(root/L"malformed");f.execute();write(f.journal,"{\"schema\":\"bad\",\"schema\":\"duplicate\"}");
        auto scan=f.scan();require(scan.limited&&scan.pending.empty(),"Strict malformed JSON fails closed");
    }
    {
        Fixture f(root/L"oversize");f.execute();write(f.journal,std::string(16*1024*1024+1,' '));
        auto scan=f.scan();require(scan.limited&&scan.pending.empty(),"Oversized journal bounded");
    }
    {
        Fixture f(root/L"subsongs");f.options.captureSourceRevisions=[](const auto& p){
            std::vector<MediaRevisionBinding> result;for(unsigned s:{7,19,65535}){auto t=identity(p,s);result.push_back({t.trackId,t});}return result;};
        f.execute();auto scan=f.scan();require(scan.pending.size()==1&&scan.pending[0].revisions.size()==3,"All nonordinal subsongs survive journal");
        for(const auto& binding:scan.pending[0].revisions)
            require(binding.current.trackId==identity(f.file,binding.current.subsong).trackId,"Frozen subsong equals real identity");
    }
    {
        Fixture f(root/L"frozen");std::string error;NekoDatabase db;
        require(db.open(f.root/L"state.db",error),"DB open");require(db.setFavorite(f.old,true,1,error),"Seed favorite");
        f.execute();auto scan=f.scan();require(scan.pending.size()==1,"Recovery frozen");const auto binding=scan.pending[0].revisions[0];
        write(f.file,"A later external revision C");auto newer=identity(f.file);
        TrackIdentityResolution previous;require(db.resolveTrackIdentity(binding.previousRevisionTrackId,previous,error)&&previous.found,"Old entity found");
        require(db.bindKnownRevision(binding.current,previous.entityId,error),"Bind snapshot");
        PersistedTrackUserState b,c;require(db.readTrackUserState(binding.current.trackId,b,error)&&b.favorite,"Frozen B keeps favorite");
        require(db.readTrackUserState(newer.trackId,c,error)&&!c.favorite,"Later C not relabelled");
        require(db.bindKnownRevision(binding.current,previous.entityId,error),"Crash after DB commit replays idempotently");
    }
    {
        Fixture f(root/L"undo");auto original=read(f.file);auto result=f.execute();auto scan=f.scan();
        require(acknowledgeMediaBindingRecovery(scan.pending[0],{}),"Forward receipt fixture");
        MediaRestoreExecutionOptions options;options.journalPath=f.journal.parent_path()/L"restore.json";
        auto restored=restoreInPlaceBackups({result},{},options);require(restored.size()==1&&restored[0].outcome==MediaCopyOutcome::moved,"Undo restored");
        require(read(f.file)==original,"Undo byte exact");scan=f.scan();
        require(scan.pending.size()==1,"Undo owns separate durable intent");
        require(scan.pending[0].revisions[0].previousRevisionTrackId==f.old.trackId,"Undo stays on original entity");
        require(scan.pending[0].revisions[0].current.trackId==identity(f.file).trackId,"Undo captures restored revision");
    }
    std::cout<<"PASS: "<<checks<<" recovery/failure assertions\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
