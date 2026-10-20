"""Compile the actual Spectrum resource-admission prefix with a fake GPU."""
import argparse
from pathlib import Path
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'out/third_party/spectrum-analyzer/foo_vis_spectrum_analyzer/UIElementRendering.cpp').read_text(encoding='utf-8-sig')
match = re.search(r'void uielement_t::Render\(\) noexcept\n\{([\s\S]*?)    _DeviceContext->BeginDraw\(\);', source)
idle = re.search(r'    const auto GetIdleWaitTimeout = \[&\]\(\) noexcept\n    \{[\s\S]*?    };', source)
if not match or not idle or '_ResourceRetryTick = Now + 250ULL;' not in match.group(1) or 'FALSE, GetIdleWaitTimeout());' not in source:
    raise RuntimeError('Spectrum resource-admission extraction boundary changed')
a.output.mkdir(parents=True, exist_ok=True)
(a.output / 'oracle.cpp').write_text(r'''
#include <cstdint>
#include <algorithm>
#include <iostream>
#include <stdexcept>
using HRESULT=long;
using DWORD=std::uint32_t;
using ULONGLONG=std::uint64_t;
#define INFINITE 0xffffffffU
#define FAILED(hr) ((hr)<0)
static std::uint64_t tick=0;
static std::uint64_t GetTickCount64(){return tick;}
struct uielement_t {
    std::uint64_t _ResourceRetryTick=0;
    unsigned attempts=0,cleanups=0,frames=0;
    bool visible=true;
    HRESULT CreateDeviceSpecificResources(){++attempts;return tick<1000?-1:0;}
    void DeleteDeviceSpecificResources(){++cleanups;}
    void Render() noexcept;
    DWORD idleTimeout() const;
};
DWORD uielement_t::idleTimeout() const {
    const auto IsRenderable=[&](){return visible;};
''' + idle.group() + r'''
    return GetIdleWaitTimeout();
}
void uielement_t::Render() noexcept {
''' + match.group(1) + r'''
    ++frames;
}
int main(){
    uielement_t view;
    if(view.idleTimeout()!=INFINITE)throw std::runtime_error("healthy idle woke unnecessarily");
    view.Render();tick=100;
    if(view.idleTimeout()!=150)throw std::runtime_error("idle retry lacks a finite deadline");
    view.visible=false;
    if(view.idleTimeout()!=INFINITE)throw std::runtime_error("hidden renderer scheduled retries");
    view.visible=true;tick=250;
    if(view.idleTimeout()!=1)throw std::runtime_error("expired retry cannot wake idle renderer");
    view=uielement_t{};
    for(tick=0;tick<1000;++tick)view.Render();
    if(view.attempts!=4 || view.cleanups!=4 || view.frames!=0)
        throw std::runtime_error("failed resources retried before their deadline");
    tick=1000;view.Render();
    if(view.attempts!=5 || view.frames!=1 || view._ResourceRetryTick!=0)
        throw std::runtime_error("recovered resources did not resume presentation");
    for(tick=1001;tick<1101;++tick)view.Render();
    if(view.frames!=101 || view.cleanups!=4)
        throw std::runtime_error("retry backoff reduced healthy presentation cadence");
    std::cout<<"Spectrum production resource retry/recovery prefix PASS\n";
}
''', encoding='utf-8')
