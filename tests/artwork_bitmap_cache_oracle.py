"""Extract production bitmap lookup/eviction; count warm-hit allocations.

The fake target counts GPU requests, not rendering performance. Native WGC
captures separately qualify pixel output; this executable ships nowhere.
"""
from pathlib import Path
import hashlib,json,sys
repo=Path(__file__).resolve().parents[1]
out=Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=False)
source=Path(sys.argv[2]).resolve() if len(sys.argv)>2 else repo/'src/component/workspace_panel.cpp'
expected_mode=sys.argv[3]
assert expected_mode in ('baseline','candidate')
text=source.read_text(encoding='utf-8-sig')
def method(signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:
        if text[end]=='{':depth+=1
        elif text[end]=='}':depth-=1
        end+=1
    return text[start:end]
typed='const ArtworkBitmapKey bitmapKey' in text
assert typed == (expected_mode=='candidate'), 'Production source does not match expected mode'
header=repo/'src/component/artwork_bitmap_key.h'
frozen_header=out/header.name
frozen_header.write_bytes(header.read_bytes())
key_header=frozen_header.as_posix()
cpp=r'''
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>
#include "__KEY_HEADER__"
using namespace nekorox::component;
bool counting=false;std::size_t allocations=0,allocatedBytes=0;
void* operator new(std::size_t n){if(counting){++allocations;allocatedBytes+=n;}if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
constexpr unsigned kArtworkGpuCacheMaximumEntries=192;
constexpr std::size_t kArtworkGpuCacheBudgetBytes=48ULL*1024*1024;
constexpr int DXGI_FORMAT_B8G8R8A8_UNORM=0,D2D1_ALPHA_MODE_PREMULTIPLIED=0,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR=0;
bool FAILED(int result){return result<0;}
struct D2D1_RECT_F{float left,top,right,bottom;};
struct ID2D1Bitmap{unsigned id;};
namespace D2D1{struct Size{unsigned width,height;};int PixelFormat(int,int){return 0;}int BitmapProperties(int,float,float){return 0;}Size SizeU(unsigned w,unsigned h){return {w,h};}}
struct BitmapPtr{
 std::shared_ptr<ID2D1Bitmap> owned;ID2D1Bitmap* pending=nullptr;
 ID2D1Bitmap** ReleaseAndGetAddressOf(){owned.reset();pending=nullptr;return &pending;}
 void adopt(){if(pending){owned.reset(pending);pending=nullptr;}}
 explicit operator bool(){adopt();return bool(owned);}
 ID2D1Bitmap* Get(){adopt();return owned.get();}
};
struct Target{unsigned uploads=0,draws=0,last=0;bool fail=false;
 int CreateBitmap(D2D1::Size,const void*,unsigned,int,ID2D1Bitmap** result){++uploads;if(fail)return -1;*result=new ID2D1Bitmap{uploads};return 0;}
 void DrawBitmap(ID2D1Bitmap* p,D2D1_RECT_F,float,int){++draws;last=p->id;}
};
struct ArtPixels{std::uint64_t contentHash;std::uint32_t width,height;std::vector<unsigned char> bgra;};
using Pixel=std::shared_ptr<const ArtPixels>;
Pixel pixel(std::uint64_t hash,unsigned w=64,unsigned h=64){return std::make_shared<ArtPixels>(ArtPixels{hash,w,h,{0,0,0,255}});}
class Cache{public:
 struct ArtBitmapEntry{BitmapPtr bitmap;std::size_t bytes=0;std::uint64_t lastUse=0;};
 __MAP__ artBitmaps_;
 float dpi_=96;std::uint64_t artBitmapUseCounter_=0;std::size_t artBitmapBytes_=0;
 Target targetStorage;Target* target_=&targetStorage;
 D2D1_RECT_F containedArtworkRectangle(D2D1_RECT_F r,unsigned,unsigned){return r;}
 __DRAW__
 __EVICT__
 bool draw(Pixel p){return drawPixels(p,{0,0,64,64});}
 void clear(){artBitmaps_.clear();artBitmapBytes_=0;artBitmapUseCounter_=0;}
};
unsigned checks=0;
void check(bool passed,const char* label){++checks;if(!passed){std::cerr<<label<<'\n';std::exit(1);}}
struct ConstantHash{std::size_t operator()(const ArtworkBitmapKey&)const{return 0;}};
int main(){
 Cache c;
 check(!c.draw({}),"null pixels");
 check(!c.draw(std::make_shared<ArtPixels>(ArtPixels{0,64,64,{}})),"empty pixels");
 check(c.targetStorage.uploads==0,"empty upload");
 check(c.draw(pixel(0)),"zero hash miss");auto first=c.targetStorage.last;
 check(c.draw(pixel(0))&&c.targetStorage.last==first,"equivalent pixels hit");
 check(c.targetStorage.uploads==1&&c.artBitmapBytes_==64*64*4,"hit accounting");
 for(auto p:{pixel(1ULL<<40),pixel(UINT64_MAX),pixel(0,128,64),pixel(0,64,128)})check(c.draw(p),"independent field miss");
 check(c.targetStorage.uploads==5&&c.artBitmaps_.size()==5,"field identities");
 c.dpi_=96.49f;c.draw(pixel(0));check(c.targetStorage.uploads==5,"DPI same rounded bucket");
 c.dpi_=96.5f;c.draw(pixel(0));check(c.targetStorage.uploads==6,"DPI adjacent bucket");
 c.clear();c.draw(pixel(0));check(c.artBitmaps_.size()==1&&c.targetStorage.uploads==7,"clear recreates");
 c.clear();c.targetStorage.fail=true;check(!c.draw(pixel(5)),"failed creation");
 check(c.artBitmaps_.empty()&&c.artBitmapBytes_==0,"failure not published");c.targetStorage.fail=false;
 c.clear();for(unsigned i=0;i<192;++i)c.draw(pixel(i,1,1));
 check(c.artBitmaps_.size()==192&&c.artBitmapBytes_==768,"entry bound");
 c.draw(pixel(0,1,1));const auto oldestKept=c.targetStorage.last;c.draw(pixel(192,1,1));
 auto uploads=c.targetStorage.uploads;c.draw(pixel(0,1,1));check(c.targetStorage.uploads==uploads&&c.targetStorage.last==oldestKept,"LRU touch retained");
 c.draw(pixel(1,1,1));check(c.targetStorage.uploads==uploads+1,"LRU evicted oldest");
 c.clear();c.draw(pixel(4,4096,2048));c.draw(pixel(5,4096,2048));
 check(c.artBitmaps_.size()==1&&c.artBitmapBytes_==32ULL*1024*1024,"byte bound");
 std::unordered_map<ArtworkBitmapKey,int,ConstantHash> collisions;
 for(unsigned i=0;i<32;++i)collisions.emplace(ArtworkBitmapKey{std::uint64_t(i)<<40,64+i,128-i,96+i},i);
 for(unsigned i=0;i<32;++i)check(collisions.at({std::uint64_t(i)<<40,64+i,128-i,96+i})==int(i),"collisions retain equality");
 ArtworkBitmapKeyHash hash;check(hash({0,64,128,96})==hash({0,64,128,96}),"equal hash stable");
 Cache hot;std::vector<Pixel> pixels;
 for(unsigned i=0;i<32;++i){pixels.push_back(pixel((UINT64_MAX-1234567)*i,64+i,128-i));hot.draw(pixels.back());}
 const auto beforeUploads=hot.targetStorage.uploads;
 constexpr unsigned iterations=200000;counting=true;
 const auto begin=std::chrono::steady_clock::now();
 for(unsigned i=0;i<iterations;++i)if(!hot.draw(pixels[i%pixels.size()]))return 3;
 const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();counting=false;
 check(hot.targetStorage.uploads==beforeUploads,"warm loop creates no bitmap");
 __ALLOCATION_ASSERT__
 std::cout<<"{\"checks\":"<<checks<<",\"iterations\":"<<iterations<<",\"allocations\":"<<allocations<<",\"allocatedBytes\":"<<allocatedBytes<<",\"milliseconds\":"<<elapsed<<",\"uploads\":"<<hot.targetStorage.uploads<<"}\n";
}
'''
mapping='std::unordered_map<ArtworkBitmapKey,ArtBitmapEntry,ArtworkBitmapKeyHash>' if typed else 'std::unordered_map<std::wstring,ArtBitmapEntry>'
cpp=cpp.replace('__KEY_HEADER__',key_header).replace('__MAP__',mapping).replace('__DRAW__',method('    bool drawPixels(')).replace('__EVICT__',method('    void evictArtworkBitmaps('))
cpp=cpp.replace('__ALLOCATION_ASSERT__','check(allocations==0,"typed warm lookup allocates");' if typed else 'check(allocations>0,"baseline warm allocation reproduced");')
(out/'oracle.cpp').write_text(cpp)
(out/'source.json').write_text(json.dumps(dict(source=str(source),sha256=hashlib.sha256(source.read_bytes()).hexdigest(),typed=typed,expectedMode=expected_mode,headerSha256=hashlib.sha256(frozen_header.read_bytes()).hexdigest(),scope='Exact production drawPixels and evictArtworkBitmaps; fake GPU target counts requests; no UI/GPU performance claim'),indent=2))
print('Extracted', 'typed' if typed else 'string', source)
