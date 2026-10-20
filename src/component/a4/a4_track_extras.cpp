#include "a4_track_extras.h"
#include "library_model.h"
#include "text_util.h"
#include <algorithm>
#include <cmath>
#include <cwchar>

namespace nekorox::component::a4 {
namespace {
std::string key(const metadb_handle_ptr& h) {
    if(!h.is_valid())return {};
    return std::string(h->get_path())+'\x1f'+std::to_string(h->get_subsong_index());
}
std::wstring timeText(double seconds) {
    if(!(seconds>=0)||!std::isfinite(seconds))return L"—";
    const auto t=static_cast<unsigned long long>(std::llround(seconds));
    wchar_t text[48]{};
    if(t>=3600)swprintf_s(text,L"%llu:%02llu:%02llu",t/3600,(t/60)%60,t%60);
    else swprintf_s(text,L"%llu:%02llu",t/60,t%60);
    return text;
}
bool sameRect(D2D1_RECT_F a,D2D1_RECT_F b) {
    return a.left==b.left&&a.top==b.top&&a.right==b.right&&a.bottom==b.bottom;
}
}
void TrackExtras::prepare(PanelD2D& render,const AlbumModeLayout& layout,
    const AlbumSessionSnapshot& session)
{
    auto* target=render.target();if(!target)return;
    float dx=96,dy=96;target->GetDpi(&dx,&dy);
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;target->GetFactory(factory.GetAddressOf());
    const auto factoryId=reinterpret_cast<std::uintptr_t>(factory.Get());
    if(factoryId!=factoryIdentity_){
        factoryIdentity_=factoryId;pixels_.bindFactory(factory.Get());generation_=~std::uint64_t{0};
    }
    const auto library=LibraryModel::instance().sharedSnapshot();
    const auto gen=library?library->generation:0;
    const auto w=std::max(1.f,layout.tracks.right-layout.tracks.left);
    if(session_==session.sessionId&&sessionRevision_==session.revision&&generation_==gen&&
        width_==w&&dpi_==dx&&rows_.size()==session.tracks.size())return;
    if(!write_){
        if(FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(write_.GetAddressOf()))))return;
    }
    if(!left_&&FAILED(write_->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,12.f,L"",left_.GetAddressOf())))return;
    if(!right_&&FAILED(write_->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,11.f,L"",right_.GetAddressOf())))return;
    left_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);right_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    left_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    right_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    right_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};
    Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
    if(SUCCEEDED(write_->CreateEllipsisTrimmingSign(left_.Get(),ellipsis.GetAddressOf())))
        left_->SetTrimming(&trimming,ellipsis.Get());
    const int physical=std::max(12,int(std::lround(12.f*dx/96.f)));
    if(const auto* g=goden::pixel::icon(goden::pixel::IconId::StarFilled,12))
        (void)pixels_.prepare(*g,physical,physical,starOn_);
    if(const auto* g=goden::pixel::icon(goden::pixel::IconId::StarOutline,12))
        (void)pixels_.prepare(*g,physical,physical,starOff_);
    if(session_!=session.sessionId)overrides_.clear();
    session_=session.sessionId;sessionRevision_=session.revision;generation_=gen;width_=w;dpi_=dx;++layoutRevision_;
    rows_.clear();rows_.reserve(session.tracks.size());
    for(std::size_t i=0;i<session.tracks.size();++i){
        const auto& item=session.tracks[i];Row row;row.handle=item.handle;
        const auto k=key(item.handle);row.key=utf8ToWide(k);row.title=item.title;
        row.number=std::to_wstring(item.track>0?std::size_t(item.track):i+1);
        row.count=L"—";row.duration=L"—";
        if(library&&library->tracks){
            const auto at=library->handleIndex.find(k);
            if(at!=library->handleIndex.end()&&at->second<library->tracks->size()){
                const auto& data=(*library->tracks)[at->second];row.rating=std::clamp(data.rating,0,5);
                row.count=std::to_wstring(std::max(0,data.playCount));row.duration=timeText(data.durationSeconds);
            }
        }
        if(row.duration==L"—"&&item.handle.is_valid())row.duration=timeText(item.handle->get_length());
        if(auto it=overrides_.find(row.key);it!=overrides_.end()){
            if(row.rating==it->second)overrides_.erase(it);else row.rating=it->second;
        }
        // Layout creation is lazy for visible rows, not the entire album.
        rows_.push_back(std::move(row));
    }
}
void TrackExtras::draw(PanelD2D& render,const AlbumModeLayout& layout,
    const AlbumSessionSnapshot& session,float scroll,bool glass)
{
    if(layout.tracks.right<=layout.tracks.left){
        if(region_.right>region_.left)++layoutRevision_;region_={};return;
    }
    prepare(render,layout,session);
    const auto bounded=std::clamp(scroll,0.f,albumTrackMaximumScroll(layout,rows_.size()));
    if(!sameRect(region_,layout.tracks)||lastScroll_!=bounded)++layoutRevision_;
    region_=layout.tracks;lastScroll_=bounded;
    auto* target=render.target();if(!target)return;
    auto background=region_;background.bottom=std::min(background.bottom,
        background.top+38+float(rows_.size())*28+8);
    if(!glass)render.fill(background,PanelColor::panel,.40f);
    render.text(L"TRACKS",{region_.left+10,region_.top,region_.right,region_.top+38},PanelText::strong,PanelColor::text);
    auto viewport=region_;viewport.top=std::min(viewport.bottom,viewport.top+38);
    target->PushAxisAlignedClip(viewport,D2D1_ANTIALIAS_MODE_ALIASED);
    const auto first=std::min(rows_.size(),std::size_t(lastScroll_/28));
    float y=region_.top+38+float(first)*28-lastScroll_;
    for(std::size_t i=first;i<rows_.size()&&y<region_.bottom;++i,y+=28){
        const auto rowRect=D2D1::RectF(region_.left+4,y,region_.right-4,y+27);
        const auto c=columns({rowRect.left,rowRect.top,rowRect.right,rowRect.bottom});
        const bool playing=session.actualPlayingIndex&&*session.actualPlayingIndex==i&&!session.outsideSession;
        if(playing)render.fill(rowRect,PanelColor::selected,.88f);
        const auto color=playing?PanelColor::accent:PanelColor::text;
        auto& row=rows_[i];const std::array<Rect,4> boxes{c.number,c.title,c.plays,c.duration};
        const std::array<const std::wstring*,4> values{&row.number,&row.title,&row.count,&row.duration};
        for(unsigned j=0;j<4;++j){
            if(boxes[j].r<=boxes[j].l)continue;
            if(!row.text[j]&&write_&&left_&&right_){const auto& text=*values[j];
                (void)write_->CreateTextLayout(text.data(),static_cast<UINT32>(text.size()),
                    j<2?left_.Get():right_.Get(),boxes[j].r-boxes[j].l,27,row.text[j].GetAddressOf());
            }
            if(row.text[j])target->DrawTextLayout({boxes[j].l,y},row.text[j].Get(),
                render.brush(j<2?color:PanelColor::muted),D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if(c.rating.r>c.rating.l)for(int star=0;star<5;++star){const float x=c.rating.l+14*star+1;
            auto* starBrush=render.brush(star<row.rating?PanelColor::bright:PanelColor::muted);
            if(starBrush==nullptr)continue;
            const auto opacity=starBrush->GetOpacity();
            if(star>=row.rating)starBrush->SetOpacity(opacity*.32f);
            (void)pixels_.drawPrepared(target,star<row.rating?starOn_:starOff_,
                {x,y+7,x+12,y+19},starBrush);
            starBrush->SetOpacity(opacity);
        }
    }
    target->PopAxisAlignedClip();
}
std::optional<RatingHit> TrackExtras::hit(float x,float y)const noexcept
{
    if(!std::isfinite(x)||!std::isfinite(y)||y<region_.top+38||y>=region_.bottom)return {};
    const auto i=std::size_t((y-region_.top-38+lastScroll_)/28);if(i>=rows_.size())return {};
    const auto rowY=region_.top+38+float(i)*28-lastScroll_;
    const auto c=columns({region_.left+4,rowY,region_.right-4,rowY+27});
    if(c.rating.r<=c.rating.l||x<c.rating.l||x>=c.rating.r)return {};
    return RatingHit{session_,layoutRevision_,i,std::clamp(int((x-c.rating.l)/14)+1,1,5),rows_[i].handle};
}
bool TrackExtras::validate(const RatingHit& token)const noexcept
{
    const auto state=AlbumSessionService::instance().snapshot();
    return state&&token.session==state->sessionId&&token.layout==layoutRevision_&&
        token.row<state->tracks.size()&&token.row<rows_.size()&&
        state->tracks[token.row].handle==token.handle&&rows_[token.row].handle==token.handle;
}
bool TrackExtras::apply(const RatingHit& token,HWND owner)
{
    if(!validate(token))return false;
    auto& row=rows_[token.row];const int desired=row.rating==token.value?0:token.value;
    if(!applyPlaybackStatisticsRating(token.handle,desired)){
        MessageBoxW(owner,L"Rating could not be saved through Playback Statistics. No tag or database was modified by the Album view.",
            L"Rating unavailable",MB_OK|MB_ICONINFORMATION);return false;
    }
    overrides_[row.key]=desired;row.rating=desired;LibraryModel::instance().invalidateTrack(token.handle);return true;
}
} // namespace nekorox::component::a4
