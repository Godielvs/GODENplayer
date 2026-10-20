#pragma once
#include "opus_info_projection.h"

namespace nekorox::component::opus_info {
using ReaderId=std::array<std::uint8_t,16>;

// The pinned 2.25.10 host stores the input order as uint32 + GUID[count].
// Refuse unknown/malformed wires. Only our entry moves; all other preferences
// keep their relative order, including entries unavailable in this session.
inline bool priorityOrder(std::span<const std::uint8_t> old,
    std::span<const ReaderId> defaults,const ReaderId& own,
    std::vector<std::uint8_t>& next)
{
    next.clear();std::vector<ReaderId> ids;
    if(!old.empty()) {
        if(old.size()<4)return false;auto count=le32(old.data());
        if(count>256 || old.size()!=4+16ULL*count)return false;
        for(unsigned i=0;i<count;++i){ReaderId id;std::memcpy(id.data(),old.data()+4+16*i,16);
            if(std::find(ids.begin(),ids.end(),id)!=ids.end())return false;ids.push_back(id);}
    } else ids.assign(defaults.begin(),defaults.end());
    std::erase(ids,own);ids.insert(ids.begin(),own);
    if(ids.size()>256)return false;
    next.resize(4+16*ids.size());put32(next.data(),static_cast<std::uint32_t>(ids.size()));
    for(std::size_t i=0;i<ids.size();++i)std::memcpy(next.data()+4+16*i,ids[i].data(),16);
    return true;
}
} // namespace nekorox::component::opus_info
