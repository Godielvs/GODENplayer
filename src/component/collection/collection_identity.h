#pragma once
#include <array>
#include <string>
#include <string_view>

namespace nekorox::component::collection {
// Frozen pre-curation ordinals. Never derive this table from the new registry:
// an old foobar profile has only the ordinal, including the two appended Nebulas.
inline constexpr std::array<std::string_view,24> legacyPresetIds{
    "avtqpukv","csln1b0s","cxbf4gfw","di76dy6c","fspiu4nn","gt8966nk",
    "gtl4w04i","lnbg0175","n65on4u7","nj47a5wg","paisgvhv","pnxxz6xp",
    "rq3v1xn2","rw3dfaou","rxhy4i5t","sxbkbfpc","tcsfryp6","u4uhc9b9",
    "wwi3etmp","3114","wXdfzj","3107","nj47a5wgclassic","nj47a5wgexpressive"};
inline constexpr std::string_view fallbackPresetId="nj47a5wgexpressive";
inline constexpr std::string_view canonicalPresetId(std::string_view id) noexcept {
    if(id=="lnbg0175"||id=="paisgvhv"||id=="rq3v1xn2")return "sculpted_ribbons";
    if(id=="nj47a5wg"||id=="nj47a5wgclassic"||id=="n65on4u7"||
       id=="sxbkbfpc"||id=="wwi3etmp"||id=="3107"||id=="gt8966nk")return fallbackPresetId;
    return id;
}
inline constexpr std::string_view migratePresetIndex(unsigned index) noexcept {
    return index<legacyPresetIds.size()?canonicalPresetId(legacyPresetIds[index]):fallbackPresetId;
}
inline std::string migratePresetFavorites(std::string_view previous) {
    std::string result;
    for(std::size_t at=0;at<previous.size();){
        const auto begin=previous.find('|',at);if(begin==previous.npos)break;
        const auto end=previous.find('|',begin+1);if(end==previous.npos)break;
        auto id=previous.substr(begin+1,end-begin-1);at=end+1;
        if(id.empty()||id.size()>64||id.find_first_not_of(
           "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=id.npos)continue;
        if(id=="sxbkbfpc"||id=="wwi3etmp"||id=="3107"||id=="gt8966nk")continue;
        id=id=="alloy_v2"?"alloy":canonicalPresetId(id);const auto token="|"+std::string(id)+"|";
        if(result.find(token)==result.npos&&result.size()+token.size()<=4096)result+=token;
    }
    return result;
}
} // namespace
