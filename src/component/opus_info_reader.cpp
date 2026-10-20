#include "sdk_compat.h"
#include "opus_info_projection.h"
#include "opus_info_activation.h"

namespace nekorox::component {
namespace {
constexpr GUID inputGuid={0x8aba6834,0xda71,0x481e,{0x9b,0xde,0x8b,0x65,0x72,0xdd,0x16,0x40}};

class MetadataFile : public file_readonly {
public:
    MetadataFile(file::ptr source, opus_info::Projection view, std::uint64_t size)
        : source_(std::move(source)), view_(std::move(view)), size_(size) {}
    t_size read(void* buffer,t_size bytes,abort_callback& abort) override {
        abort.check(); auto* out=static_cast<std::uint8_t*>(buffer);
        auto wanted=static_cast<t_size>(std::min<std::uint64_t>(bytes,get_size(abort)-position_));
        t_size done=0;
        if(position_<view_.prefix.size()) {
            done=std::min<t_size>(wanted,view_.prefix.size()-static_cast<t_size>(position_));
            std::memcpy(out,view_.prefix.data()+position_,done); position_+=done;
        }
        if(done<wanted) {
            source_->seek(view_.sourceAudioOffset+position_-view_.prefix.size(),abort);
            auto got=source_->read(out+done,wanted-done,abort); position_+=got; done+=got;
        }
        return done;
    }
    t_filesize get_size(abort_callback& abort) override { abort.check(); return size_-view_.sourceAudioOffset+view_.prefix.size(); }
    t_filesize get_position(abort_callback& abort) override {abort.check();return position_;}
    void seek(t_filesize p,abort_callback& abort) override {if(p>get_size(abort)) throw exception_io_seek_out_of_range(); position_=p;}
    bool can_seek() override {return true;}
    bool is_remote() override {return false;}
    bool get_content_type(pfc::string_base& out) override {return source_->get_content_type(out);}
    t_filetimestamp get_timestamp(abort_callback& abort) override {return source_->get_timestamp(abort);}
    void reopen(abort_callback& abort) override {abort.check();position_=0;}
private:
    file::ptr source_; opus_info::Projection view_; std::uint64_t size_=0,position_=0;
};

class MetadataReader : public input_info_reader_v2 {
public:
    MetadataReader(input_info_reader::ptr reader,file::ptr physical)
        :reader_(std::move(reader)),physical_(std::move(physical)){}
    t_uint32 get_subsong_count() override {return reader_->get_subsong_count();}
    t_uint32 get_subsong(t_uint32 n) override {return reader_->get_subsong(n);}
    void get_info(t_uint32 n,file_info& info,abort_callback& abort) override {reader_->get_info(n,info,abort);}
    t_filestats2 get_stats2(uint32_t flags,abort_callback& abort) override {return physical_->get_stats2_(flags,abort);}
private:
    input_info_reader::ptr reader_; file::ptr physical_;
};

// A metadata-only input: other requests fall through to the existing input.
// In particular it is not a decoder redirector, which the host deliberately
// excludes from some library reads. Native decoding/writing/artwork stay native.
class OpusMetadataInput : public input_entry_v3 {
public:
    bool is_our_content_type(const char* type) override {
        return _stricmp(type,"audio/ogg")==0 || _stricmp(type,"application/ogg")==0 ||
            _stricmp(type,"audio/opus")==0;
    }
    bool is_our_path(const char* path,const char* ext) override {
        return _stricmp(ext,"opus")==0 && (strncmp(path,"file://",7)==0 ||
            strncmp(path,"file-relative://",16)==0 ||
            (strlen(path)>2 && path[1]==':'));
    }
    unsigned get_flags() override {return 0;}
    GUID get_guid() override {return inputGuid;}
    const char* get_name() override {return "Goden Opus metadata reader";}
    GUID get_preferences_guid() override {return pfc::guid_null;}
    bool is_low_merit() override {return false;}
    void get_extended_data(file::ptr,const playable_location&,const GUID&,mem_block_container&,abort_callback&) override {}
    service_ptr open_v3(const GUID& what,file::ptr hint,const char* path,event_logger::ptr logger,abort_callback& abort) override {
        if(what!=input_info_reader::class_guid) {
            if(what==input_decoder::class_guid || what==input_info_writer::class_guid) throw exception_io_unsupported_format();
            throw pfc::exception_not_implemented();
        }
        static thread_local bool opening=false;
        if(opening) throw exception_io_unsupported_format();
        struct OpenScope {bool& active;explicit OpenScope(bool& flag):active(flag){active=true;}~OpenScope(){active=false;}} scope(opening);
        pfc::list_t<input_entry::ptr> enabled, targets;
        input_manager_v3::get()->get_enabled_inputs(enabled);
        const auto ext=pfc::string_extension(path);
        pfc::string8 contentType;
        if(hint.is_valid()) hint->get_content_type(contentType);
        for(t_size i=0;i<enabled.get_count();++i) {
            if(enabled[i]->get_guid_()!=inputGuid &&
                (enabled[i]->is_our_path(path,ext) ||
                    (!contentType.is_empty() && enabled[i]->is_our_content_type(contentType)))) targets.add_item(enabled[i]);
        }
        if(what==input_info_reader::class_guid) {
            file::ptr physical=hint;
            if(physical.is_empty()) filesystem::g_open_read(physical,path,abort);
            if(physical->can_seek() && !physical->is_remote()) {
                auto size=physical->get_size(abort); opus_info::Projection view;
                // Parsing is bounded and cancellable. No exception swallowing:
                // abort and file I/O failures retain their normal semantics.
                auto readAt=[&](std::uint64_t at,void* out,std::size_t bytes){
                    abort.check();physical->seek(at,abort);physical->read_object(out,bytes,abort);
                };
                if(size!=filesize_invalid && opus_info::makeProjection(size,readAt,view)) {
                    file::ptr projected=new service_impl_t<MetadataFile>(physical,std::move(view),size);
                    input_info_reader::ptr native;
                    native ^= input_entry::g_open_from_list(targets,what,projected,path,logger,abort);
                    return new service_impl_t<MetadataReader>(native,physical);
                }
                physical->seek(0,abort);
            }
            hint=physical;
        }
        return input_entry::g_open_from_list(targets,what,hint,path,logger,abort);
    }
};
service_factory_single_t<OpusMetadataInput> inputFactory;

void activateMetadataReader()
{
    // Before the host reads/caches its input order and starts library scanning.
    // This is a once-only migration for the qualified pinned host, using the
    // public configuration API. Never rewrite a later manual decoder choice.
    const auto version=core_version_info_v2::get()->get_version();
    if(version.m_major!=2 || version.m_minor1!=25 || version.m_minor2!=10) return;
    auto store=fb2k::configStore::get();
    constexpr const char* migrated="goden.opusMetadata.activation.v1";
    constexpr const char* orderKey="cfg_var.7D53C550-CDA5-4C83-A030-1024B40417BC";
    if(store->getConfigBool(migrated,false)) return;
    const auto previous=store->getConfigBlob(orderKey);
    std::vector<opus_info::ReaderId> defaults;
    for(auto entry:input_entry::enumerate()) {
        const auto guid=entry->get_guid_();if(guid==pfc::guid_null) continue;
        opus_info::ReaderId id;std::memcpy(id.data(),&guid,16);
        if(std::find(defaults.begin(),defaults.end(),id)==defaults.end()) defaults.push_back(id);
    }
    opus_info::ReaderId own;std::memcpy(own.data(),&inputGuid,16);
    std::vector<std::uint8_t> next;
    if(!opus_info::priorityOrder(std::span(static_cast<const std::uint8_t*>(previous->data()),previous->size()),defaults,own,next)) return;
    const auto transaction=store->acquireTransactionScope();
    store->setConfigBlob("goden.opusMetadata.previousInputOrder.v1",previous);
    store->setConfigBlob(orderKey,next.data(),next.size());
    store->setConfigBool(migrated,true);
}
FB2K_ON_INIT_STAGE(activateMetadataReader,init_stages::before_config_read);
}
} // namespace nekorox::component
