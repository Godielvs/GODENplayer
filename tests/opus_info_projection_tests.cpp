#include "../src/component/opus_info_projection.h"
#include "../src/component/opus_info_activation.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <filesystem>

using namespace nekorox::component::opus_info;
namespace {
void check(bool ok,const char* why) {if(!ok) throw std::runtime_error(why);}
std::uint32_t slowCrc(std::span<const std::uint8_t> data) {
    std::uint32_t c=0; for(auto b:data){c^=std::uint32_t(b)<<24;
        for(unsigned i=0;i<8;++i)c=(c<<1)^((c&0x80000000)?0x04c11db7:0);}
    return c;
}
std::vector<std::uint8_t> load(const std::filesystem::path& p) {
    std::ifstream f(p,std::ios::binary);check(bool(f),"open fixture");
    return {std::istreambuf_iterator<char>(f),{}};
}
bool project(const std::vector<std::uint8_t>& bytes,Projection& view,std::size_t* readBytes=nullptr) {
    if(readBytes)*readBytes=0;
    return makeProjection(bytes.size(),[&](std::uint64_t at,void* out,std::size_t n){
        check(at<=bytes.size() && n<=bytes.size()-at,"out-of-bounds read");
        if(readBytes)*readBytes+=n;
        std::memcpy(out,bytes.data()+at,n);
    },view);
}
void append32(std::vector<std::uint8_t>& b,std::uint32_t value){auto at=b.size();b.resize(at+4);put32(b.data()+at,value);}
std::vector<std::uint8_t> page(std::span<const std::uint8_t> packet,std::uint32_t sequence,unsigned flags) {
    const auto segments=packet.size()/255+1;check(segments<=255,"test page too big");
    std::vector<std::uint8_t> p(27+segments+packet.size(),0);std::memcpy(p.data(),"OggS",4);p[5]=static_cast<std::uint8_t>(flags);
    put32(p.data()+14,1234);put32(p.data()+18,sequence);p[26]=static_cast<std::uint8_t>(segments);
    std::fill(p.begin()+27,p.begin()+27+segments,255);p[27+segments-1]=static_cast<std::uint8_t>(packet.size()%255);
    std::memcpy(p.data()+27+segments,packet.data(),packet.size());put32(p.data()+22,slowCrc(p));return p;
}
void syntheticTests() {
    std::vector<std::uint8_t> head(19,0);std::memcpy(head.data(),"OpusHead",8);head[8]=1;head[9]=2;
    for(unsigned text=0;text<1000;text+=53) {
        std::vector<std::uint8_t> tags{'O','p','u','s','T','a','g','s'};append32(tags,4);tags.insert(tags.end(),{'t','e','s','t'});append32(tags,4);
        const std::array fields{std::string("ARTIST=Artist"),std::string("ARTIST=Other"),std::string("R128_TRACK_GAIN=-512"),std::string("METADATA_BLOCK_PICTURE=")+std::string(text,'A')};
        for(const auto& field:fields){append32(tags,static_cast<std::uint32_t>(field.size()));tags.insert(tags.end(),field.begin(),field.end());}
        auto bytes=page(head,0,2);auto tp=page(tags,1,0);bytes.insert(bytes.end(),tp.begin(),tp.end());
        const std::array<std::uint8_t,4> audio{0,2,3,4};auto ap=page(audio,2,4);bytes.insert(bytes.end(),ap.begin(),ap.end());
        Projection view;check(project(bytes,view),"synthetic tags rejected");
        std::string contents(view.prefix.begin(),view.prefix.end());
        for(unsigned i=0;i<3;++i)check(contents.find(fields[i])!=std::string::npos,"metadata field changed");
        check(contents.find("METADATA_BLOCK_PICTURE")==std::string::npos,"picture leaked into metadata view");
        check(std::equal(ap.begin(),ap.end(),bytes.begin()+view.sourceAudioOffset),"audio suffix changed");
        const auto tagsStart=page(head,0,2).size();auto corrupt=bytes;corrupt[tagsStart+22]^=1;
        check(!project(corrupt,view),"comment CRC ignored");
        for(std::size_t n=0;n<bytes.size();n+=13){auto truncated=bytes;truncated.resize(n);if(n<=tagsStart+tp.size())check(!project(truncated,view),"truncation accepted");}
    }
    std::cout<<"Synthetic metadata, multivalue, gain, audio preservation and truncation: PASS\n";
}
}
int main(int argc,char** argv) {
    try {
        check(argc==1 || argc==3,"usage: projection-tests [fixture-root output-root]");
        syntheticTests();
        ReaderId first{},own{},last{};first[0]=1;own[0]=2;last[0]=3;
        std::array ids{first,last};std::vector<std::uint8_t> order,updated;
        check(priorityOrder({},ids,own,order),"initial priority order");
        check(le32(order.data())==3 && order[4]==2 && order[20]==1 && order[36]==3,"priority default sequence");
        check(priorityOrder(order,ids,own,updated) && order==updated,"priority idempotence");
        auto invalid=order;invalid[0]=4;check(!priorityOrder(invalid,ids,own,updated),"invalid priority count");
        invalid=order;std::memcpy(invalid.data()+20,invalid.data()+4,16);check(!priorityOrder(invalid,ids,own,updated),"duplicate priority GUID");
        std::array<std::uint8_t,700> random{}; std::uint32_t state=0x5678;
        for(auto& b:random){state=state*1664525+1013904223;b=static_cast<std::uint8_t>(state>>24);}
        for(std::size_t n=0;n<random.size();++n)check(crc(std::span(random).first(n))==slowCrc(std::span(random).first(n)),"CRC slicing mismatch");
        if(argc==1){std::cout<<"CRC and priority migration: PASS\n";return 0;}
        for(auto name:{"none","embedded-720","embedded-5000","external-5000"}) {
            auto path=std::filesystem::path(argv[1])/name/"music"/"QA Artist 00"/"QA Album 000"/"01.opus";
            auto bytes=load(path); Projection view;std::size_t readBytes=0;
            const bool embedded=std::string_view(name).starts_with("embedded");
            check(project(bytes,view,&readBytes)==embedded,"projection eligibility");
            if(!embedded)continue;
            if(std::string_view(name)=="embedded-5000")check(readBytes<256*1024,"opaque image payload unnecessarily read");
            check(view.prefix.size()<1024*1024 && view.sourceAudioOffset<bytes.size(),"bounded prefix");
            std::vector<std::uint8_t> projected=view.prefix;
            projected.insert(projected.end(),bytes.begin()+view.sourceAudioOffset,bytes.end());
            // Independently check every page CRC and contiguous page sequence.
            std::size_t at=0;std::uint32_t sequence=0;
            while(at<projected.size()) {
                check(projected.size()-at>=27,"truncated projection");auto* p=projected.data()+at;
                check(std::memcmp(p,"OggS",4)==0,"capture");std::size_t n=27+p[26];
                for(unsigned j=0;j<p[26];++j)n+=p[27+j];check(n<=projected.size()-at,"page size");
                check(le32(p+18)==sequence++,"sequence changed");auto expected=le32(p+22);put32(p+22,0);
                check(slowCrc(std::span(p,n))==expected,"projected CRC");put32(p+22,expected);at+=n;
            }
            std::ofstream out(std::filesystem::path(argv[2])/(std::string(name)+".opus"),std::ios::binary);
            out.write(reinterpret_cast<const char*>(projected.data()),projected.size());
            Projection rejected; auto corrupt=bytes;corrupt[22]^=1;
            check(!project(corrupt,rejected),"bad CRC accepted");
            auto truncated=bytes;truncated.resize(view.sourceAudioOffset-1);
            check(!project(truncated,rejected),"truncated packet accepted");
            check(!makeProjection(bytes.size(),[&](std::uint64_t at,void* data,std::size_t n){
                std::memcpy(data,bytes.data()+at,n);if(at==0)static_cast<std::uint8_t*>(data)[4]=1;
            },rejected),"unsupported Ogg version accepted");
            unsigned reads=0;bool aborted=false;
            try{makeProjection(bytes.size(),[&](std::uint64_t at,void* data,std::size_t n){
                if(++reads==5)throw std::runtime_error("abort");std::memcpy(data,bytes.data()+at,n);
            },rejected);}catch(const std::runtime_error&){aborted=true;}
            check(aborted,"abort swallowed");
            std::cout<<name<<" physical="<<bytes.size()<<" metadataRead="<<readBytes<<" prefix="<<view.prefix.size()<<" PASS\n";
        }
        std::cout<<"CRC, bounds, source suffix, rejection and cancellation: PASS\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
