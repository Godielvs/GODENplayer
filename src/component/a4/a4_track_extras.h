#pragma once
#include "a4_model.h"
#include "album_session_service.h"
#include "album_mode_view.h"
#include "panel_d2d.h"
#include "pixel_ui/pixel_icon_painter.hpp"
#include <optional>
#include <vector>
#include <map>
#include <dwrite.h>
namespace nekorox::component {
// Bridge to the exact command resolver already used by Library, not a new DB.
bool applyPlaybackStatisticsRating(const metadb_handle_ptr&,int) noexcept;
namespace a4 {
struct RatingHit {
 std::uint64_t session=0,layout=0;std::size_t row=0;int value=0;
 metadb_handle_ptr handle;
};
class TrackExtras final {
public:
 void draw(PanelD2D&,const AlbumModeLayout&,const AlbumSessionSnapshot&,float scroll,bool glass);
 std::optional<RatingHit> hit(float x,float y) const noexcept;
 bool apply(const RatingHit&,HWND owner);
  bool validate(const RatingHit&) const noexcept;
 void invalidate() noexcept {generation_=~std::uint64_t{0};}
private:
 struct Row {metadb_handle_ptr handle;std::wstring key;int rating=-1;
  std::wstring title,number,count,duration;
  std::array<Microsoft::WRL::ComPtr<IDWriteTextLayout>,4> text;
 };
 void prepare(PanelD2D&,const AlbumModeLayout&,const AlbumSessionSnapshot&);
 std::vector<Row> rows_;
 std::map<std::wstring,int> overrides_;
 std::uint64_t session_=0,sessionRevision_=0,generation_=~std::uint64_t{0},layoutRevision_=0;
 float width_=0,lastScroll_=0,dpi_=0;
  std::uintptr_t factoryIdentity_=0;
 D2D1_RECT_F region_{};
 Microsoft::WRL::ComPtr<IDWriteFactory> write_;
 Microsoft::WRL::ComPtr<IDWriteTextFormat> left_,right_;
 goden::pixel::PixelIconPainter pixels_;
 goden::pixel::PixelIconPainter::Handle starOn_,starOff_;
};
} // namespace a4
} // namespace nekorox::component
