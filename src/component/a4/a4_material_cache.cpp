#include "a4_material_cache.h"
#include <list>
#include <mutex>
namespace nekorox::component::a4 {
namespace {struct Entry {std::uint64_t hash;bool alloyV2=false;std::shared_ptr<const Material> value;};
 std::mutex mutex;std::list<Entry> cache;constexpr unsigned capacity=128;}
std::shared_ptr<const Material> materialFor(std::span<const std::uint8_t> pixels,unsigned w,unsigned h,std::uint64_t hash,bool alloyV2){
 {std::lock_guard lock(mutex);for(auto it=cache.begin();it!=cache.end();++it)if(it->hash==hash&&(!alloyV2||it->alloyV2)){auto v=it->value;cache.splice(cache.begin(),cache,it);return v;}}
 // The lock never spans analysis. Duplicate in-flight computation is harmless
 // and bounded by the pre-existing artwork worker count.
 auto mutableResult=analyze(pixels,w,h,hash);
 if(alloyV2){
  mutableResult.alloyV2=goden::c11::analyzeArt(pixels.data(),w,h,std::size_t(w)*4,pixels.size());
  mutableResult.alloyV2Ready=mutableResult.alloyV2.valid;
  mutableResult.contentRevision=2;
 }
 auto result=std::make_shared<const Material>(std::move(mutableResult));
 if(!result->valid)return result;
 {std::lock_guard lock(mutex);for(auto& entry:cache)if(entry.hash==hash&&(!alloyV2||entry.alloyV2))return entry.value;
  cache.push_front({hash,alloyV2,result});while(cache.size()>capacity)cache.pop_back();}
 return result;
}
Vec3 quietAccent(const Material& material) noexcept {
 const Vec3 gray{.70f,.72f,.73f};
 auto c=material.colors[0];float y=luma(c);
 if(y<.0001f)return gray;
 c=c*(linear(.71f)/y);float maximum=std::max({c.x,c.y,c.z,1.f});c=c*(1.f/maximum);
 Vec3 srgb{encoded(c.x),encoded(c.y),encoded(c.z)};
 return mix(gray,srgb,material.neutral?.12f:.65f);
}
} // namespace nekorox::component::a4
