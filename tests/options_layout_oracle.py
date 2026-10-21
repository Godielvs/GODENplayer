"""Exercise complete production Options layout/navigation with a fake window tree.

No player, UI input, profile or network is accessed. Geometry does not establish
native text rendering, keyboard behavior or human visual approval.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def function(text, name):
    start = text.index('void OptionsWindow::' + name + '(')
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

PREFIX = r'''
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>
#define private public
#include "options_window.h"
#undef private
#include "ui_form_layout.h"
#include "presence_cover_policy.h"
using namespace nekorox::component;
struct Node { int x=0,y=0,w=0,h=0;bool visible=true;std::wstring text; };
std::map<HWND,Node> nodes;
std::vector<HWND> children;
std::set<HWND> combos;
int scale=96,provider=6;bool imgbb=false;
HWND handle(unsigned n){return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(n));}
BOOL testGetClientRect(HWND h,RECT* r){*r={0,0,nodes[h].w,nodes[h].h};return TRUE;}
UINT testGetDpiForWindow(HWND){return scale;}
void move(HWND h,int x,int y,int w,int height){auto& n=nodes[h];n.x=x;n.y=y;n.w=w;n.h=combos.contains(h)?MulDiv(30,scale,96):height;}
int dip(HWND,int value){return MulDiv(value,scale,96);}
BOOL testShowWindow(HWND h,int state){nodes[h].visible=state!=SW_HIDE;return TRUE;}
BOOL testSetWindowTextW(HWND h,LPCWSTR text){nodes[h].text=text;return TRUE;}
BOOL testInvalidateRect(HWND,const RECT*,BOOL){return TRUE;}
HWND testGetWindow(HWND h,UINT which){
 if(which==GW_CHILD)return children.empty()?nullptr:children.front();
 auto i=std::find(children.begin(),children.end(),h);
 return i==children.end()||++i==children.end()?nullptr:*i;
}
LRESULT testSendMessageW(HWND,UINT message,WPARAM,LPARAM){return message==CB_GETCURSEL?provider:imgbb?BST_CHECKED:BST_UNCHECKED;}
namespace nekorox::component {
void FormViewport::scrollTo(int value){scrollOffset_=std::clamp(value,0,std::max(0,contentHeight_-nodes[viewport_].h));}
void FormViewport::setBounds(int x,int y,int w,int h,int contentHeight){
 nodes[viewport_]={x,y,w,h};nodes[content_].w=w-MulDiv(17,scale,96);nodes[content_].h=contentHeight;
 contentHeight_=contentHeight;scrollTo(scrollOffset_);
}
}
#define GetClientRect testGetClientRect
#define GetDpiForWindow testGetDpiForWindow
#define ShowWindow testShowWindow
#define SetWindowTextW testSetWindowTextW
#define InvalidateRect testInvalidateRect
#define GetWindow testGetWindow
#define SendMessageW testSendMessageW
'''

MAIN = r'''
unsigned checks=0,cases=0;
void require(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
int main(){try{
 OptionsWindow o;
 unsigned next=10;
 __ASSIGN__
 for(auto& h:o.sectionLabels_){h=handle(next++);children.push_back(h);}
 for(auto& h:o.navigation_)h=handle(next++);
 o.viewport_.viewport_=handle(4);o.viewport_.content_=handle(5);
 o.window_=handle(1);
 combos={o.motion_,o.listenEffects_,o.albumAmbient_,o.keepMode_,o.coverProvider_};
 for(auto dpi:{96,120,144,192})for(auto dimensions:{std::pair{620,480},std::pair{720,760},std::pair{1280,900}})
 for(int page=0;page<5;++page)for(provider=0;provider<7;++provider)for(bool advanced:{false,true})for(bool key:{false,true}){
  imgbb=key;
  scale=dpi;nodes[o.window_].w=MulDiv(dimensions.first,scale,96);nodes[o.window_].h=MulDiv(dimensions.second,scale,96);
  o.page_=page;o.presenceAdvanced_=advanced;o.updateLayout();++cases;
  const auto& content=nodes[o.viewport_.content_];
  std::vector<Node> shown;
  for(auto h:children)if(nodes[h].visible){
   const auto& n=nodes[h];require(n.w>0&&n.h>0&&n.x>=0&&n.y>=0&&n.x+n.w<=content.w&&n.y+n.h<=content.h,"visible control outside content");
   shown.push_back(n);
  }
  for(std::size_t a=0;a<shown.size();++a)for(std::size_t b=a+1;b<shown.size();++b){
   const auto& x=shown[a];const auto& y=shown[b];
   require(x.x+x.w<=y.x||y.x+y.w<=x.x||x.y+x.h<=y.y||y.y+y.h<=x.y,"visible controls overlap");
  }
  for(auto h:{o.footerStatus_,o.closeButton_})require(nodes[h].y+nodes[h].h<=nodes[o.window_].h,"footer outside window");
  require(nodes[o.applyPresence_].visible==(page==3),"Apply leaked outside Discord page");
  require(nodes[o.applicationId_].visible==(page==3&&advanced),"advanced ID visibility wrong");
  require(nodes[o.autoUguu_].visible==(page==3&&advanced&&provider==6),"automatic options visibility wrong");
  require(nodes[o.imgbbKey_].visible==(page==3&&(provider==4||(provider==6&&imgbb))),"required key hidden or leaked");
  for(std::size_t i=0;i<5;++i)require(nodes[o.sectionLabels_[i]].visible==(i==static_cast<std::size_t>(page)),"wrong section visible");
 }
 o.refreshing_=true;o.presenceDirty_=false;o.markPresenceDirty();require(!o.presenceDirty_,"refresh dirtied preferences");
 o.refreshing_=false;o.markPresenceDirty();require(o.presenceDirty_,"edit did not mark draft");
 for(std::size_t page=0;page<5;++page){o.selectPage(page);require(o.presenceDirty_,"navigation discarded draft");}
 require(nodes[o.navigation_[3]].text==L"Discord *","draft indicator missing");
 std::cout<<"PASS cases="<<cases<<" checks="<<checks<<" production layout/navigation; fake window geometry only\n";
 }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
'''

def main():
    compiler = shutil.which('cl')
    if not compiler:
        raise RuntimeError('Import the supported MSVC environment first')
    text = (ROOT/'src/component/options_window.cpp').read_text(encoding='utf-8')
    header = (ROOT/'src/component/options_window.h').read_text(encoding='utf-8')
    fields = re.findall(r'    HWND (\w+)_ = nullptr;', header)
    outside = {'window','owner','title','footerStatus','closeButton','applyPresence'}
    assign = '\n'.join(f'o.{field}_=handle(next++);'+('' if field in outside else f'children.push_back(o.{field}_);') for field in fields)
    methods = '\n'.join(function(text, name) for name in ['selectPage','markPresenceDirty','updateFooter','updateLayout'])
    source = PREFIX + methods + MAIN.replace('__ASSIGN__', assign)
    with tempfile.TemporaryDirectory(prefix='goden-options-layout-') as temporary:
        folder = Path(temporary); cpp=folder/'layout.cpp'; exe=folder/'layout.exe'
        cpp.write_text(source,encoding='utf-8')
        built=subprocess.run([compiler,'/nologo','/std:c++20','/EHsc','/W3','/O2','/MT','/DUNICODE','/D_UNICODE','/DNOMINMAX',
            '/I'+str(ROOT/'src/component'),str(cpp),'/Fe:'+str(exe),'/Fo:'+str(folder)+'/', '/link','/cgthreads:1','User32.lib'],capture_output=True,text=True)
        if built.returncode:
            raise RuntimeError(built.stdout+built.stderr)
        subprocess.run([str(exe)],check=True)

if __name__=='__main__':
    main()
