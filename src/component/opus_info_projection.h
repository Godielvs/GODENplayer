#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace nekorox::component::opus_info {

// A read-only, in-memory view for the host metadata reader. Only picture
// comments are omitted. The source file and the audio pages are never changed.
struct Projection {
    std::vector<std::uint8_t> prefix;
    std::uint64_t sourceAudioOffset = 0;
};
inline constexpr std::size_t maxHeaderBytes = 48U * 1024U * 1024U;
inline constexpr std::size_t maxTextBytes = 1024U * 1024U;

inline std::uint32_t le32(const std::uint8_t* p) noexcept
{ return p[0] | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24); }
inline void put32(std::uint8_t* p, std::uint32_t v) noexcept
{ for (unsigned i=0;i<4;++i) p[i]=static_cast<std::uint8_t>(v>>(8*i)); }

inline std::uint32_t crc(std::span<const std::uint8_t> bytes) noexcept
{
    static const auto table = [] {
        std::array<std::array<std::uint32_t,256>,8> t{};
        for (unsigned i=0;i<256;++i) {
            auto c=std::uint32_t(i)<<24;
            for (unsigned b=0;b<8;++b) c=(c<<1)^((c&0x80000000U)?0x04c11db7U:0);
            t[0][i]=c;
        }
        for (unsigned n=1;n<8;++n) for (unsigned i=0;i<256;++i) {
            auto c=t[n-1][i]; t[n][i]=(c<<8)^t[0][c>>24];
        }
        return t;
    }();
    std::uint32_t c=0; std::size_t i=0;
    for (;i+8<=bytes.size();i+=8) {
        c^=(std::uint32_t(bytes[i])<<24)|(std::uint32_t(bytes[i+1])<<16)|
            (std::uint32_t(bytes[i+2])<<8)|bytes[i+3];
        c=table[7][c>>24]^table[6][(c>>16)&255]^table[5][(c>>8)&255]^table[4][c&255]^
            table[3][bytes[i+4]]^table[2][bytes[i+5]]^table[1][bytes[i+6]]^table[0][bytes[i+7]];
    }
    for (;i<bytes.size();++i) c=(c<<8)^table[0][(c>>24)^bytes[i]];
    return c;
}
inline bool pictureName(std::span<const std::uint8_t> field) noexcept
{
    constexpr std::string_view name="METADATA_BLOCK_PICTURE=";
    if (field.size()<name.size()) return false;
    for (std::size_t i=0;i<name.size();++i) {
        auto c=field[i]; if(c>='a'&&c<='z') c-=32;
        if(c!=static_cast<unsigned char>(name[i])) return false;
    }
    return true;
}

struct TagPage {
    std::uint64_t source = 0;
    std::size_t packetStart = 0, bodyBytes = 0;
    std::vector<std::uint8_t> header;
};

// Metadata-only cursor: image values are opaque and skipped by length. CRCs are
// checked on every page supplying metadata; image-only pages are not read.
// The original decoder and artwork extractor still read the original file and
// validate the picture/audio when actually used. Never used for decoding.
template<class ReadAt> class TagCursor {
public:
    TagCursor(const std::vector<TagPage>& pages, ReadAt& readAt)
        : pages_(pages), readAt_(readAt) {}
    bool read(std::size_t at, void* output, std::size_t bytes) {
        auto* out=static_cast<std::uint8_t*>(output);
        while(bytes>0) {
            auto it=std::upper_bound(pages_.begin(),pages_.end(),at,
                [](std::size_t p,const TagPage& page){return p<page.packetStart;});
            if(it==pages_.begin())return false;--it;
            const auto index=static_cast<std::size_t>(it-pages_.begin());
            if(at-it->packetStart>=it->bodyBytes)return false;
            if(cached_!=index) {
                buffer_=it->header;buffer_.resize(buffer_.size()+it->bodyBytes);
                readAt_(it->source+it->header.size(),buffer_.data()+it->header.size(),it->bodyBytes);
                const auto expected=le32(buffer_.data()+22);put32(buffer_.data()+22,0);
                if(crc(buffer_)!=expected)return false;
                cached_=index;
            }
            const auto n=std::min(bytes,it->bodyBytes-(at-it->packetStart));
            std::memcpy(out,buffer_.data()+it->header.size()+at-it->packetStart,n);
            at+=n;out+=n;bytes-=n;
        }
        return true;
    }
private:
    const std::vector<TagPage>& pages_;ReadAt& readAt_;
    std::vector<std::uint8_t> buffer_;std::size_t cached_=static_cast<std::size_t>(-1);
};

// ReadAt must return exactly the requested bytes or throw; it also checks abort.
// Serial, sequence, continuation and all lengths are checked before accepting
// the projection. Only the metadata pages need their payload/checksum read.
// Unknown layouts fall back to the normal host reader.
template<class ReadAt>
bool makeProjection(std::uint64_t fileBytes, ReadAt&& readAt, Projection& result)
{
    result={}; std::uint64_t offset=0; std::uint32_t serial=0, sequence=0;
    std::vector<std::uint8_t> head; std::vector<TagPage> tagPages;
    std::size_t pages=0,packetBytes=0; bool packetComplete=false;
    for (unsigned n=0;n<4097;++n) {
        if (offset>fileBytes || fileBytes-offset<27 || offset>maxHeaderBytes) return false;
        std::array<std::uint8_t,282> h{}; readAt(offset,h.data(),27);
        if(std::memcmp(h.data(),"OggS",4)!=0 || h[4]!=0 || h[26]==0) return false;
        if(fileBytes-offset<27U+h[26] || maxHeaderBytes-offset<27U+h[26]) return false;
        readAt(offset+27,h.data()+27,h[26]);
        std::size_t body=0; for(unsigned j=0;j<h[26];++j) {
            body+=h[27+j]; if(j+1<h[26] && h[27+j]!=255) return false;
        }
        const auto size=27U+h[26]+body;
        if(size>fileBytes-offset || size>maxHeaderBytes-offset) return false;
        if(n==0) {
            std::vector<std::uint8_t> page(size); readAt(offset,page.data(),size);
            const auto expected=le32(page.data()+22);put32(page.data()+22,0);
            if(crc(page)!=expected)return false;put32(page.data()+22,expected);
            if(h[5]!=2 || h[26]!=1 || body<19 || body>276 || h[27]==255 ||
                std::memcmp(page.data()+28,"OpusHead",8)!=0) return false;
            head=page; serial=le32(h.data()+14); sequence=le32(h.data()+18);
        } else {
            if(le32(h.data()+14)!=serial || le32(h.data()+18)!=++sequence ||
                h[5]!=(n==1?0:1)) return false;
            bool zeroGranule=true,unknownGranule=true;
            for(unsigned j=6;j<14;++j) {zeroGranule &= h[j]==0; unknownGranule &= h[j]==255;}
            const bool completes=h[27+h[26]-1]<255;
            if(!zeroGranule && !(unknownGranule && !completes)) return false;
            if(body>maxHeaderBytes-packetBytes)return false;
            TagPage page;page.source=offset;page.packetStart=packetBytes;page.bodyBytes=body;
            page.header.assign(h.begin(),h.begin()+27+h[26]);tagPages.push_back(std::move(page));
            packetBytes+=body;++pages;
            if(h[27+h[26]-1]<255) { offset+=size; packetComplete=true; break; }
        }
        offset+=size;
    }
    if(!packetComplete || pages==0 || pages>4096 || offset>=fileBytes || packetBytes<16)return false;
    TagCursor cursorReader(tagPages,readAt);std::array<std::uint8_t,8> marker{};
    if(!cursorReader.read(0,marker.data(),8) || std::memcmp(marker.data(),"OpusTags",8)!=0)return false;
    std::size_t at=8; const auto take32=[&](std::uint32_t& out) {
        std::array<std::uint8_t,4> b{};
        if(at>packetBytes || packetBytes-at<4 || !cursorReader.read(at,b.data(),4)) return false;
        out=le32(b.data()); at+=4; return true;
    };
    std::uint32_t vendor=0,count=0;
    if(!take32(vendor) || vendor>maxTextBytes || vendor>packetBytes-at) return false;
    at+=vendor;
    std::vector<std::uint8_t> kept(at);
    if(!cursorReader.read(0,kept.data(),kept.size()))return false;
    if(!take32(count) || count>65536) return false;
    const auto countAt=kept.size(); kept.resize(kept.size()+4);
    if(kept.size()>maxTextBytes) return false;
    std::uint32_t retained=0; bool removed=false;
    for(std::uint32_t i=0;i<count;++i) {
        std::uint32_t length=0;
        if(!take32(length) || length>packetBytes-at) return false;
        std::array<std::uint8_t,23> name{};const auto nameBytes=std::min<std::size_t>(length,name.size());
        if(!cursorReader.read(at,name.data(),nameBytes))return false;
        const auto field=std::span<const std::uint8_t>(name).first(nameBytes);
        if(pictureName(field)) removed=true;
        else {
            if(length>maxTextBytes || kept.size()+4+length>maxTextBytes) return false;
            auto start=kept.size(); kept.resize(start+4); put32(kept.data()+start,length);
            start=kept.size();kept.resize(start+length);
            if(!cursorReader.read(at,kept.data()+start,length))return false;++retained;
        }
        at+=length;
    }
    if(!removed) return false;
    put32(kept.data()+countAt,retained);
    // OpusTags permits padding, and bit 0 marks preserved binary suffix data.
    std::uint8_t suffix=0;
    if(at<packetBytes && !cursorReader.read(at,&suffix,1))return false;
    if(at<packetBytes && (suffix&1)) {
        if(packetBytes-at>maxTextBytes-kept.size()) return false;
        const auto start=kept.size();kept.resize(start+packetBytes-at);
        if(!cursorReader.read(at,kept.data()+start,packetBytes-at))return false;
    }
    kept.resize(std::max(kept.size(),(pages-1)*255),0);
    auto prefix=std::move(head); std::size_t cursor=0;
    for(std::size_t i=0;i<pages;++i) {
        const auto remaining=kept.size()-cursor;
        const auto amount=i+1==pages?remaining:
            std::min<std::size_t>(65025,(remaining-(pages-i-2)*255)/255*255);
        if(amount>65025 || (i+1==pages && amount==65025)) return false;
        const auto segments=amount/255+((i+1==pages)?1:0);
        if(segments==0 || segments>255) return false;
        const auto start=prefix.size(); prefix.resize(start+27+segments+amount,0);
        auto* p=prefix.data()+start; std::memcpy(p,"OggS",4);
        p[5]=i==0?0:1;
        if(i+1<pages) std::fill(p+6,p+14,std::uint8_t{255});
        put32(p+14,serial); put32(p+18,le32(prefix.data()+18)+1+static_cast<std::uint32_t>(i));
        p[26]=static_cast<std::uint8_t>(segments);
        std::fill(p+27,p+27+segments,std::uint8_t{255});
        if(i+1==pages) p[27+segments-1]=static_cast<std::uint8_t>(amount%255);
        std::memcpy(p+27+segments,kept.data()+cursor,amount); cursor+=amount;
        put32(p+22,crc(std::span<const std::uint8_t>(p,27+segments+amount)));
    }
    result.prefix=std::move(prefix); result.sourceAudioOffset=offset; return true;
}
} // namespace nekorox::component::opus_info
