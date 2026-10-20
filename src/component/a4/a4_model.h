#pragma once
// Original implementation for GODENplayer. Linear RGB unless explicitly named srgb.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include "../c11/c11_art_profile.h"
#include "../c11/c11_alloy_gpu.h"

namespace nekorox::component::a4 {
struct Vec3 { float x{},y{},z{}; };
inline Vec3 operator+(Vec3 a,Vec3 b) {return {a.x+b.x,a.y+b.y,a.z+b.z};}
inline Vec3 operator-(Vec3 a,Vec3 b) {return {a.x-b.x,a.y-b.y,a.z-b.z};}
inline Vec3 operator*(Vec3 a,float b) {return {a.x*b,a.y*b,a.z*b};}
inline Vec3 mix(Vec3 a,Vec3 b,float t) {return a+(b-a)*t;}
inline float sat(float x) {return std::clamp(x,0.f,1.f);}
inline float smooth(float a,float b,float x) {float t=sat((x-a)/std::max(b-a,1e-6f));return t*t*(3-2*t);}
inline float linear(float x) {x=sat(x);return x<=.04045f?x/12.92f:std::pow((x+.055f)/1.055f,2.4f);}
inline float encoded(float x) {x=sat(x);return x<=.0031308f?12.92f*x:1.055f*std::pow(x,1/2.4f)-.055f;}
inline float luma(Vec3 x){return .2126f*x.x+.7152f*x.y+.0722f*x.z;}
inline Vec3 lab(Vec3 c) { // Oklab transform, coefficients: Björn Ottosson, MIT reference.
 float l=std::cbrt(.4122214708f*c.x+.5363325363f*c.y+.0514459929f*c.z);
 float m=std::cbrt(.2119034982f*c.x+.6806995451f*c.y+.1073969566f*c.z);
 float s=std::cbrt(.0883024619f*c.x+.2817188376f*c.y+.6299787005f*c.z);
 return {.2104542553f*l+.7936177850f*m-.0040720468f*s,1.9779984951f*l-2.4285922050f*m+.4505937099f*s,.0259040371f*l+.7827717662f*m-.8086757660f*s};
}
inline float dist2(Vec3 a,Vec3 b){a=a-b;return a.x*a.x+a.y*a.y+a.z*a.z;}
inline float chroma(Vec3 c){auto x=lab(c);return std::hypot(x.y,x.z);}
struct Material {
 std::uint32_t version=1;
 std::uint64_t hash{};
 std::array<Vec3,4> colors{{{.18f,.18f,.18f},{.34f,.34f,.34f},{.54f,.54f,.54f},{.024f,.024f,.024f}}};
 float roughness=.36f, anisotropy=.7f, dispersion=0, detailMean=0;
 float orientation=0; // structure-tensor angle, radians; never a hue category
 float meanLuma=0, contrast=0, chromaMean=0;
 bool neutral=true, valid=false;
 goden::c11::ArtProfile alloyV2{};
 std::uint64_t contentRevision=1;
 bool alloyV2Ready=false;
 std::array<std::uint8_t,64*64> detail{};
};
// Fixed 64x64 stratified grid: never a prefix of the image. No per-frame allocations.
inline Material analyze(std::span<const std::uint8_t> bgra,std::uint32_t w,std::uint32_t h,
                        std::uint64_t hash) noexcept {
 Material out;out.hash=hash;
 if(!w||!h||std::uint64_t(w)*h>bgra.size()/4) return out;
 std::array<Vec3,4096> colors{},labs{}; std::array<float,4096> lum{};
 std::array<bool,4096> valid{};std::size_t n=0;double totalY=0,totalY2=0,totalC=0;
 for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){
  auto sx=std::min(w-1,static_cast<unsigned>((std::uint64_t(2*x+1)*w)/128));
  auto sy=std::min(h-1,static_cast<unsigned>((std::uint64_t(2*y+1)*h)/128));
  auto p=(std::size_t(sy)*w+sx)*4;float a=bgra[p+3]/255.f;auto k=y*64+x;
  if(a<.5f)continue;
  Vec3 c{linear(bgra[p+2]/(255.f*a)),linear(bgra[p+1]/(255.f*a)),linear(bgra[p]/(255.f*a))};
  colors[k]=c;labs[k]=lab(c);lum[k]=luma(c);valid[k]=true;++n;
  totalY+=lum[k];totalY2+=lum[k]*lum[k];totalC+=std::hypot(labs[k].y,labs[k].z);
 }
 if(!n)return out;
 out.valid=true;out.meanLuma=float(totalY/n);out.chromaMean=float(totalC/n);
 out.contrast=float(std::sqrt(std::max(0.,totalY2/n-(totalY/n)*(totalY/n))));
 double detailSum=0,tensorXX=0,tensorYY=0,tensorXY=0;
 for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){auto k=y*64+x;
  float d=0;if(valid[k]){
   for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
    auto xx=std::clamp(int(x)+dx,0,63),yy=std::clamp(int(y)+dy,0,63);
    auto j=unsigned(yy*64+xx);if(valid[j])d=std::max(d,std::abs(lum[k]-lum[j]));
   }
  }
   if(x>0&&x<63&&y>0&&y<63&&valid[k]&&valid[k-1]&&valid[k+1]&&valid[k-64]&&valid[k+64]){
    const double gx=lum[k+1]-lum[k-1], gy=lum[k+64]-lum[k-64];
    tensorXX+=gx*gx;tensorYY+=gy*gy;tensorXY+=gx*gy;
   }
   d=smooth(.02f,.25f,d);out.detail[k]=std::uint8_t(std::lround(255*d));detailSum+=d;
 }
 out.detailMean=float(detailSum/4096);out.roughness=.26f+.22f*out.detailMean;
 out.anisotropy=.55f+.30f*sat(out.contrast*2);
 out.orientation=float(.5*std::atan2(2*tensorXY,tensorXX-tensorYY));
 // Six clusters; start with a high-chroma representative, then farthest-point seeds.
 std::array<Vec3,6> centers{},rgb{};std::array<unsigned,6> count{};
 std::size_t first=0;float best=-1;
 for(std::size_t k=0;k<4096;++k)if(valid[k]){float score=std::hypot(labs[k].y,labs[k].z);if(score>best){best=score;first=k;}}
 centers[0]=labs[first];
 for(unsigned c=1;c<6;++c){best=-1;std::size_t chosen=first;
  for(std::size_t k=0;k<4096;++k)if(valid[k]){float d=1e9f;for(unsigned j=0;j<c;++j)d=std::min(d,dist2(labs[k],centers[j]));if(d>best){best=d;chosen=k;}}
  centers[c]=labs[chosen];
 }
 for(int pass=0;pass<8;++pass){std::array<Vec3,6>sums{};rgb={};count={};
  for(std::size_t k=0;k<4096;++k)if(valid[k]){unsigned c=0;float d=1e9f;
   for(unsigned j=0;j<6;++j){float v=dist2(labs[k],centers[j]);if(v<d){d=v;c=j;}}
   sums[c]=sums[c]+labs[k];rgb[c]=rgb[c]+colors[k];++count[c];
  }
  for(unsigned c=0;c<6;++c)if(count[c]){centers[c]=sums[c]*(1.f/count[c]);rgb[c]=rgb[c]*(1.f/count[c]);}
 }
 auto eligible=[&](unsigned c){return count[c]>=std::max<std::size_t>(1,n/200);};
 unsigned primary=0;best=-1;
 for(unsigned c=0;c<6;++c)if(eligible(c)){
  float score=std::sqrt(float(count[c])/float(n))*(.018f+std::hypot(centers[c].y,centers[c].z)*2);
  if(score>best){best=score;primary=c;}
 }
 unsigned second=primary,third=primary;best=-1;
 for(unsigned c=0;c<6;++c)if(c!=primary&&eligible(c)){float score=dist2(centers[c],centers[primary])*std::sqrt(float(count[c])/n);if(score>best){best=score;second=c;}}
 best=-1;
 for(unsigned c=0;c<6;++c)if(c!=primary&&c!=second&&eligible(c)){
  float score=std::min(dist2(centers[c],centers[primary]),dist2(centers[c],centers[second]))*std::sqrt(float(count[c])/n);if(score>best){best=score;third=c;}
 }
 out.neutral=chroma(rgb[primary])<.027f&&out.chromaMean<.018f;
 auto normalizeTint=[](Vec3 c,float Y){float y=luma(c);if(y<.001f)return Vec3{Y,Y,Y};
  c=c*(Y/y);float m=std::max({c.x,c.y,c.z,1.f});return c*(1.f/m);};
 if(out.neutral){ // No cyan/orange fallback. Below white highlight in the UI.
  float tint=out.meanLuma>.8f?.045f:.09f;
  out.colors[0]=mix({.16f,.16f,.16f},normalizeTint(rgb[primary],.16f),tint);
  out.colors[1]=mix({.34f,.34f,.34f},normalizeTint(rgb[second],.34f),tint);
  out.colors[2]=mix({.52f,.52f,.52f},normalizeTint(rgb[third],.52f),tint);
 }else{
  out.colors[0]=normalizeTint(rgb[primary],.24f);
  out.colors[1]=normalizeTint(rgb[second],.33f);
  out.colors[2]=mix(normalizeTint(rgb[third],.40f),{.55f,.55f,.55f},.12f);
 }
 out.colors[3]=out.colors[0]*.060f;
 out.dispersion=out.neutral?0.f:.20f+.55f*smooth(.025f,.14f,out.chromaMean);
 return out;
}
struct Fit {unsigned width{},height{};};
inline Fit fit(unsigned w,unsigned h,bool low=false) noexcept {
 if(!w||!h)return{};double s=std::min({1.,double(low?480:640)/w,double(low?480:640)/h,
      std::sqrt(double(low?130000:240000)/(double(w)*h))});
 return {std::max(1u,unsigned(std::floor(w*s))),std::max(1u,unsigned(std::floor(h*s)))};
}
struct Rect {float l{},t{},r{},b{};};
struct TrackColumns {Rect number,title,rating,plays,duration;};
inline TrackColumns columns(Rect r) noexcept {
 TrackColumns c{};
 if(!std::isfinite(r.l)||!std::isfinite(r.r)||r.r<=r.l||r.b<=r.t)return c;
 const float w=r.r-r.l, pad=std::min(8.f,w*.04f);
 float right=r.r-pad;
 if(w>=150){const float size=w>=220?48.f:40.f;c.duration={right-size,r.t,right,r.b};right-=size+6;}
 if(w>=340){c.plays={right-34,r.t,right,r.b};right-=40;}
 if(w>=280){c.rating={right-70,r.t,right,r.b};right-=78;}
 const float left=r.l+pad;
 if(w>=90){c.number={left,r.t,left+22,r.b};c.title={left+28,r.t,std::max(left+28,right),r.b};}
 else c.title={left,r.t,std::max(left,right),r.b};
 return c;
}
struct alignas(16) Float4 {float x{},y{},z{},w{};};
struct alignas(16) Constants {
 Float4 viewport,output,levels,hits,phase;
 Float4 color0,color1,color2,color3;
 Float4 material,rect,input0,input1,pointer,tuning;
 std::array<Float4,2> lightLevel{},lightTexture{};
 Float4 lightControl{};
 goden::c11::AlloyGpu alloyV2{};
};
static_assert(offsetof(Constants,alloyV2)==320);
static_assert(sizeof(Constants)==2624);
static_assert(offsetof(Constants,lightLevel)==15*16);
static_assert(offsetof(Constants,lightTexture)==17*16);
static_assert(offsetof(Constants,lightControl)==19*16);
inline Float4 vec(Vec3 c,float w=1){return{c.x,c.y,c.z,w};}
// Stable failure backoff. Never ACK a failed production and never turn a timeout into credit.
struct Retry {
 unsigned failures=0;double notBefore=0;bool latched=false;
 void success()noexcept{failures=0;notBefore=0;latched=false;}
 void fail(double now)noexcept{static constexpr double waits[]{.10,.25,.50,1.,2.};
  notBefore=now+waits[std::min(failures,4u)];latched=++failures>=6;}
 bool due(double now)const noexcept{return !latched&&now>=notBefore;}
};
} // namespace nekorox::component::a4
