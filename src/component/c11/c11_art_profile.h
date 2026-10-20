#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
namespace goden::c11 {
struct Color3 {float x=0,y=0,z=0;};
inline Color3 add(Color3 a,Color3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
inline Color3 mul(Color3 a,float w){return {a.x*w,a.y*w,a.z*w};}
inline float dist2(Color3 a,Color3 b){return (a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)+(a.z-b.z)*(a.z-b.z);}
inline float luminance(Color3 a){return .2126f*a.x+.7152f*a.y+.0722f*a.z;}
inline float decode(float x){x=std::clamp(x,0.f,1.f);return x<=.04045f?x/12.92f:std::pow((x+.055f)/1.055f,2.4f);}
inline Color3 lab(Color3 c){
    const float l=std::cbrt(.4122214708f*c.x+.5363325363f*c.y+.0514459929f*c.z);
    const float m=std::cbrt(.2119034982f*c.x+.6806995451f*c.y+.1073969566f*c.z);
    const float s=std::cbrt(.0883024619f*c.x+.2817188376f*c.y+.6299787005f*c.z);
    return {.2104542553f*l+.7936177850f*m-.0040720468f*s,
            1.9779984951f*l-2.4285922050f*m+.4505937099f*s,
            .0259040371f*l+.7827717662f*m-.8086757660f*s};
}
struct Pigment {
    Color3 linear{};
    float area=0, broadWeight=0, accentWeight=0, salience=0;
};
struct ArtProfile {
    std::uint32_t version=1,count=0;
    std::array<Pigment,8> pigments{};
    float edgeDensity=0, contrast=0, orientation=0, anisotropy=0;
    float meanLuma=0, neutralFraction=1, roughness=.48f, hardness=.2f;
    bool valid=false;
};
inline ArtProfile analyzeArt(const std::uint8_t* bgra,std::size_t width,
                             std::size_t height,std::size_t stride,std::size_t bytes) {
    ArtProfile out;
    if(!bgra||!width||!height||width>100000||height>100000||stride<width*4)return out;
    const auto limit=(std::numeric_limits<std::size_t>::max)();
    if(height-1>(limit-width*4)/stride || (height-1)*stride+width*4>bytes)return out;
    constexpr std::size_t maxSamples=64*64;
    struct Sample {Color3 rgb{},perceptual{};float a=0,edge=0,weight=0;};
    std::array<Sample,maxSamples> samples{};
    const auto nx=std::min<std::size_t>(64,width),ny=std::min<std::size_t>(64,height);
    double alphaSum=0,mean=0,m2=0,jxx=0,jyy=0,jxy=0;
    std::size_t valid=0;
    for(std::size_t y=0;y<ny;++y)for(std::size_t x=0;x<nx;++x){
        const auto px=(std::min)(width-1,(2*x+1)*width/(2*nx));
        const auto py=(std::min)(height-1,(2*y+1)*height/(2*ny));
        const auto* p=bgra+py*stride+px*4;
        auto& s=samples[y*nx+x];s.a=float(p[3])/255.f;
        if(s.a<.02f)continue;
        // ArtPixels uses premultiplied BGRA in encoded sRGB.
        const float inv=1.f/(255.f*s.a);
        s.rgb={decode(float(p[2])*inv),decode(float(p[1])*inv),decode(float(p[0])*inv)};
        s.perceptual=lab(s.rgb);++valid;
        const double lum=luminance(s.rgb);
        alphaSum+=s.a;mean+=lum*s.a;m2+=lum*lum*s.a;
    }
    if(!valid||alphaSum<=0)return out;
    out.meanLuma=float(mean/alphaSum);
    out.contrast=float(std::sqrt((std::max)(0.,m2/alphaSum-(mean/alphaSum)*(mean/alphaSum))));
    double edges=0,neutral=0;
    for(std::size_t y=0;y<ny;++y)for(std::size_t x=0;x<nx;++x){
        auto& s=samples[y*nx+x];if(s.a<.02f)continue;
        const float l=luminance(s.rgb);
        const float dx=x+1<nx&&samples[y*nx+x+1].a>=.02f?luminance(samples[y*nx+x+1].rgb)-l:0;
        const float dy=y+1<ny&&samples[(y+1)*nx+x].a>=.02f?luminance(samples[(y+1)*nx+x].rgb)-l:0;
        s.edge=(std::min)(1.f,std::sqrt(dx*dx+dy*dy)*2.f);
        const float chroma=std::hypot(s.perceptual.y,s.perceptual.z);
        s.weight=s.a*(.65f+.75f*s.edge+.35f*(std::min)(1.f,chroma/.18f));
        edges+=s.edge*s.a;if(chroma<.025f)neutral+=s.a;
        jxx+=dx*dx;jyy+=dy*dy;jxy+=dx*dy;
    }
    out.edgeDensity=float(edges/alphaSum);out.neutralFraction=float(neutral/alphaSum);
    out.orientation=.5f*float(std::atan2(2*jxy,jxx-jyy));
    out.anisotropy=float(std::sqrt((jxx-jyy)*(jxx-jyy)+4*jxy*jxy)/(jxx+jyy+1e-8));
    std::array<Color3,8> centers{};
    std::size_t seed=0;
    for(std::size_t i=1;i<nx*ny;++i)if(samples[i].weight>samples[seed].weight)seed=i;
    centers[0]=samples[seed].perceptual;std::size_t k=1;
    for(;k<8;++k){
        float best=-1;std::size_t bi=0;
        for(std::size_t i=0;i<nx*ny;++i)if(samples[i].a>=.02f){
            float d=1e9f;for(std::size_t j=0;j<k;++j)d=(std::min)(d,dist2(samples[i].perceptual,centers[j]));
            const float score=d*samples[i].weight;
            if(score>best){best=score;bi=i;}
        }
        if(best<.00008f)break;
        centers[k]=samples[bi].perceptual;
    }
    for(int iteration=0;iteration<8;++iteration){
        std::array<Color3,8> sum{};std::array<float,8> weight{};
        for(std::size_t i=0;i<nx*ny;++i)if(samples[i].a>=.02f){
            std::size_t c=0;float d=dist2(samples[i].perceptual,centers[0]);
            for(std::size_t j=1;j<k;++j){const float nd=dist2(samples[i].perceptual,centers[j]);if(nd<d){d=nd;c=j;}}
            sum[c]=add(sum[c],mul(samples[i].perceptual,samples[i].weight));weight[c]+=samples[i].weight;
        }
        for(std::size_t j=0;j<k;++j)if(weight[j]>0)centers[j]=mul(sum[j],1.f/weight[j]);
    }
    std::array<Color3,8> rgbSum{};
    std::array<float,8> area{},salience{};
    for(std::size_t i=0;i<nx*ny;++i)if(samples[i].a>=.02f){
        std::size_t c=0;float d=dist2(samples[i].perceptual,centers[0]);
        for(std::size_t j=1;j<k;++j){const float nd=dist2(samples[i].perceptual,centers[j]);if(nd<d){d=nd;c=j;}}
        rgbSum[c]=add(rgbSum[c],mul(samples[i].rgb,samples[i].a));area[c]+=samples[i].a;
        salience[c]+=samples[i].a*(samples[i].edge+
           .6f*std::abs(luminance(samples[i].rgb)-out.meanLuma)+
           .8f*std::hypot(samples[i].perceptual.y,samples[i].perceptual.z));
    }
    float broadSum=0,accentSum=0;
    for(std::size_t j=0;j<k;++j){
        if(area[j]<1e-6f)continue;
        const float fraction=area[j]/float(alphaSum);
        // Retain small coherent accents; reject isolated sampling specks.
        if(fraction<.0025f)continue;
        auto& p=out.pigments[out.count++];p.linear=mul(rgbSum[j],1.f/area[j]);
        p.area=fraction;p.salience=salience[j]/area[j];
        p.broadWeight=std::pow(fraction,.85f);
        p.accentWeight=std::sqrt(fraction)*(.12f+1.8f*p.salience);
        broadSum+=p.broadWeight;accentSum+=p.accentWeight;
    }
    if(!out.count)return ArtProfile{};
    for(std::size_t j=0;j<out.count;++j){
        out.pigments[j].broadWeight/=(std::max)(1e-6f,broadSum);
        out.pigments[j].accentWeight/=(std::max)(1e-6f,accentSum);
    }
    // Stable order within an artwork; no per-frame random palette selection.
    std::stable_sort(out.pigments.begin(),out.pigments.begin()+out.count,
        [](const Pigment&a,const Pigment&b){return a.broadWeight>b.broadWeight;});
    out.hardness=std::clamp(.1f+1.5f*out.edgeDensity+.8f*out.contrast,0.f,1.f);
    out.roughness=std::clamp(.70f-.40f*out.hardness,.26f,.72f);
    out.valid=true;return out;
}
} // namespace goden::c11
