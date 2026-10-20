#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
namespace goden::c11 {
struct Rect {
    float l=0,t=0,r=0,b=0;
    float width()const noexcept{return (std::max)(0.f,r-l);}
    float height()const noexcept{return (std::max)(0.f,b-t);}
    bool empty()const noexcept{return width()==0||height()==0;}
    bool contains(float x,float y)const noexcept{return !empty()&&x>=l&&x<r&&y>=t&&y<b;}
};
enum class ArtworkAlignment : std::uint32_t { left=0,center=1,right=2 };
struct Visibility {
    bool tracks=true,lyrics=true,artwork=true;
    ArtworkAlignment artworkAlignment=ArtworkAlignment::left;
};
struct AlbumLayout {
    Rect title,tracksToggle,lyricsToggle,panelsMenu,preset,close,art,tracks,lyrics;
    bool compactControls=false,tabbed=false;
    float headerBottom=0;
};
inline Rect box(float x,float y,float w,float h)noexcept{
    return {x,y,x+(std::max)(0.f,w),y+(std::max)(0.f,h)};
}
inline Rect squareIn(Rect area,float cap)noexcept{
    const float s=(std::max)(0.f,(std::min)({area.width(),area.height(),cap}));
    return box((area.l+area.r-s)*.5f,(area.t+area.b-s)*.5f,s,s);
}
inline AlbumLayout layoutAlbum(float width,float height,Visibility v,
                              bool focused,bool preferLyrics=true)noexcept{
    AlbumLayout o{};
    const float w=(std::max)(0.f,width),h=(std::max)(0.f,height);
    const float m=w>=900?24.f:12.f,g=w>=900?24.f:12.f;
    if(w<96||h<72)return o;
    const float buttonH=28.f;
    // Header never consumes transport/volume real estate.
    o.title=box(m,10,(std::max)(0.f,w-2*m),24);
    const float y=38,closeW=focused?52.f:0.f;
    float right=w-m;
    if(focused){o.close=box(right-closeW,y,closeW,buttonH);right-=closeW+8;}
    const float presetW=(std::min)(156.f,(std::max)(72.f,(w-2*m-closeW)*.40f));
    o.preset=box(right-presetW,y,presetW,buttonH);right-=presetW+8;
    o.compactControls=(right-m)<240;
    if(o.compactControls){
        o.panelsMenu=box(m,y,(std::max)(0.f,(std::min)(96.f,right-m)),buttonH);
    }else{
        o.lyricsToggle=box(right-72,y,72,buttonH);right-=80;
        o.tracksToggle=box(right-72,y,72,buttonH);right-=80;
        o.panelsMenu=box(right-72,y,72,buttonH);
    }
    o.headerBottom=(std::min)(h,y+buttonH+12.f);
    Rect content=box(m,o.headerBottom,(std::max)(0.f,w-2*m),(std::max)(0.f,h-o.headerBottom-m));
    if(content.empty())return o;
    const int panes=int(v.tracks)+int(v.lyrics);
    const auto alignArt=[&](float size,bool verticallyCentered){
        const float available=content.width()-size;
        const float factor=v.artworkAlignment==ArtworkAlignment::left?0.f:
            v.artworkAlignment==ArtworkAlignment::right?1.f:.5f;
        return box(content.l+available*factor,
            content.t+(verticallyCentered?(content.height()-size)*.5f:0.f),size,size);
    };
    if(!v.artwork){
        if(panes==2&&w>=700){
            const float tracksW=(content.width()-g)*.38f;
            o.tracks=box(content.l,content.t,tracksW,content.height());
            o.lyrics=box(o.tracks.r+g,content.t,content.width()-tracksW-g,content.height());
        }else if(panes==2){
            o.tabbed=true;
            if(preferLyrics)o.lyrics=content;else o.tracks=content;
        }else if(v.tracks)o.tracks=content;else if(v.lyrics)o.lyrics=content;
        return o;
    }
    if(!panes){
        const float artW=(std::min)({content.width(),content.height(),focused?860.f:760.f});
        o.art=alignArt(artW,focused);
        return o;
    }
    if(v.artworkAlignment==ArtworkAlignment::center&&w>=980){
        const float artW=(std::min)({content.width()*(focused?.42f:.36f),content.height(),620.f});
        const float side=(content.width()-artW-2*g)*.5f;
        o.art=box(content.l+side+g,content.t+(content.height()-artW)*.5f,artW,artW);
        if(v.tracks)o.tracks=box(content.l,content.t,side,content.height());
        if(v.lyrics)o.lyrics=box(o.art.r+g,content.t,side,content.height());
        return o;
    }
    if(w>=1200&&panes==2){
        const float artW=(std::min)({content.width()*.32f,content.height(),560.f});
        const float rem=content.width()-artW-2*g;
        const float tracksW=rem*.35f;
        const bool rightArt=v.artworkAlignment==ArtworkAlignment::right;
        o.art=alignArt(artW,focused);
        o.tracks=box(rightArt?content.l:o.art.r+g,content.t,tracksW,content.height());
        o.lyrics=box(o.tracks.r+g,content.t,rem-tracksW,content.height());
        return o;
    }
    Rect panel{};
    if(w>=700&&v.artworkAlignment!=ArtworkAlignment::center){
        const float artW=(std::min)({content.width()*(focused?.52f:.40f),content.height(),620.f});
        o.art=alignArt(artW,focused);
        panel=box(v.artworkAlignment==ArtworkAlignment::right?content.l:o.art.r+g,
            content.t,(std::max)(0.f,content.width()-artW-g),content.height());
    }else{
        const float artH=(std::min)({content.width(),content.height()*.45f,focused?420.f:280.f});
        o.art=alignArt(artH,false);
        panel=box(content.l,o.art.b+g,content.width(),(std::max)(0.f,content.b-o.art.b-g));
    }
    if(panes==2){
        o.tabbed=true; // Existing local compact tab controller selects the visible panel.
        if(preferLyrics)o.lyrics=panel;else o.tracks=panel;
    }else if(v.tracks)o.tracks=panel;else o.lyrics=panel;
    return o;
}
} // namespace goden::c11
