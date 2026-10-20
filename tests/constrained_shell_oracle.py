"""Extract actual shell placement methods for a hidden-window integration probe."""
import argparse
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
source = (Path(__file__).resolve().parents[1] / 'src/component/player_shell_panel.cpp').read_text(encoding='utf-8-sig')
patterns = (r'^    void layoutChildren\(\) const noexcept\n    \{[\s\S]*?^    }',
            r'^    static void placeChild\([\s\S]*?^    }')
layout = re.findall(patterns[0], source, re.M)
placement = re.findall(patterns[1], source, re.M)
if len(layout) != 1 or len(placement) != 2:
    raise ValueError('Shell placement extraction boundary changed')
code = r'''
#include <windows.h>
#include "ui_design_system.h"
#include <iostream>
#include <stdexcept>
using namespace nekorox::component;
struct Child {
    HWND handle=nullptr;
    bool is_valid() const {return handle!=nullptr;}
    const Child* operator->() const {return this;}
    HWND get_wnd() const {return handle;}
};
namespace uie {using window_ptr=Child;}
enum class PlayerShellMode {normal,album};
static unsigned testDpi=96;
#define GetDpiForWindow(...) testDpi
struct Shell {
    HWND window_=nullptr;
    PlayerShellMode mode_=PlayerShellMode::normal;
    int spectrumHeightDip_=360;
    bool theater=false;
    Child workspace_,transport_,waveSeek_,spectrum_,caption_,cover_;
    bool albumTheaterMode() const {return theater;}
''' + '\n'.join(layout + placement) + r'''
};
#undef GetDpiForWindow
void require(bool ok,const char* what) {if(!ok)throw std::runtime_error(what);}
void check(Child child,UiPixelRect expected,HWND parent) {
    RECT r{};require(GetWindowRect(child.handle,&r),"read child bounds");
    MapWindowPoints(nullptr,parent,reinterpret_cast<POINT*>(&r),2);
    require(r.left==expected.x && r.top==expected.y && r.right-r.left==expected.width &&
        r.bottom-r.top==expected.height,"actual child retained stale/out-of-bounds geometry");
}
int main() {
    Shell shell;
    try {
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        shell.window_=CreateWindowExW(0,L"STATIC",L"offscreen shell fixture",WS_POPUP,
            0,0,1600,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        require(shell.window_!=nullptr,"create hidden fixture parent");
        for(auto* child:{&shell.workspace_,&shell.transport_,&shell.waveSeek_,
                &shell.spectrum_,&shell.caption_,&shell.cover_}) {
            child->handle=CreateWindowExW(0,L"STATIC",L"",WS_CHILD,0,0,1,1,
                shell.window_,nullptr,GetModuleHandleW(nullptr),nullptr);
            require(child->handle!=nullptr,"create hidden fixture child");
        }
        unsigned cases=0;
        for(const auto dpi:{96U,120U,144U,168U,192U,240U,288U,384U}) {
            testDpi=dpi;
            for(const auto size:{std::array<long,2>{1600,600},{1600,500},{320,200},
                    {640,480},{1080,1920},{3440,1440},{7680,2160},{1600,1200}}) {
                SetWindowPos(shell.window_,nullptr,0,0,size[0],size[1],SWP_NOACTIVATE|SWP_NOZORDER);
                for(const auto mode:{PlayerShellMode::normal,PlayerShellMode::album}) {
                    shell.mode_=mode;shell.theater=false;shell.layoutChildren();
                    const auto expected=computePlayerShellLayout(size[0],size[1],dpi,
                        mode==PlayerShellMode::album,shell.spectrumHeightDip_,false);
                    check(shell.transport_,expected.transport,shell.window_);
                    check(shell.waveSeek_,expected.waveSeek,shell.window_);
                    check(shell.spectrum_,expected.spectrum,shell.window_);
                    check(shell.caption_,expected.caption,shell.window_);
                    if(mode==PlayerShellMode::normal) {
                        check(shell.workspace_,expected.workspace,shell.window_);
                        check(shell.cover_,expected.cover,shell.window_);
                    }
                    ++cases;
                }
            }
        }
        require(!IsWindowVisible(shell.window_),"fixture became visible");
        DestroyWindow(shell.window_);
        std::cout<<"PASS actual shell placement: "<<cases<<" constrained/portrait/ultrawide/high-DPI hidden-window cases\n";
        return 0;
    } catch(const std::exception& error) {
        if(shell.window_)DestroyWindow(shell.window_);
        std::cerr<<error.what()<<'\n';return 1;
    }
}
'''
args.output.mkdir(parents=True, exist_ok=True)
(args.output / 'shell-oracle.cpp').write_text(code, encoding='utf-8')
