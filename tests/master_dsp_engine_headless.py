"""Generate the real DSP engine with only SDK IR decoding/host services replaced.

No player, UI, file decoder or performance benchmark is exercised. Production
compiler, graph exchange, utility nodes and audio callbacks remain verbatim.
"""
import argparse
import hashlib
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'src/component/master_dsp_engine.cpp').read_text(encoding='utf-8-sig')
for marker in ('#include "sdk_compat.h"', '#include "text_util.h"',
               'std::filesystem::path irPath(', 'struct NodeSlot {'):
    if source.count(marker) != 1:
        raise RuntimeError('DSP fixture extraction boundary changed: ' + marker)
start = source.index('std::filesystem::path irPath(')
end = source.index('struct NodeSlot {', start)
source = source[:start] + '''struct DecodedIr { std::vector<std::vector<float>> channels; };
DecodedIr decodeIr(const std::string& key, std::uint32_t) {
    if (key.empty()) return {};
    throw std::runtime_error("IR decoding is outside this headless fixture");
}
''' + source[end:]
source = source.replace('#include "sdk_compat.h"', '#include <windows.h>')
source = source.replace('#include "text_util.h"', '')
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text('// production SHA256 ' + hashlib.sha256(
    (root / 'src/component/master_dsp_engine.cpp').read_bytes()).hexdigest() + '\n' + source + r'''

#include <iostream>
#include <stdexcept>
namespace nekorox::component {
static std::atomic<unsigned> compiledStages{0};
MasterDspController& MasterDspController::instance() { static MasterDspController c; return c; }
goden_audio::MasterDspStateV1 MasterDspController::snapshot() const {
    std::lock_guard lock(mutex_); return state_;
}
std::uint64_t MasterDspController::revision() const noexcept { return revision_.load(); }
bool MasterDspController::enabledRt() const noexcept { return enabled_.load(); }
bool MasterDspController::bypassedRt() const noexcept { return bypassed_.load(); }
std::size_t MasterDspController::activeModuleCountRt() const noexcept { return activeModuleCount_.load(); }
bool MasterDspController::telemetryDemandRt() const noexcept { return telemetryDemand_.load(); }
void MasterDspController::publishPreparing(bool) noexcept {}
void MasterDspController::publishTelemetry(const goden_audio::MasterDspTelemetryV1&) noexcept {}
void MasterDspController::publishFault(std::string) noexcept {}
void MasterDspController::setEnabled(bool value) {
    {std::lock_guard lock(mutex_); state_.enabled=value; enabled_=value;
     state_.revision=revision_.fetch_add(1)+1;}
    goden_audio::notifyMasterDspCompilerWorkers();
}
bool MasterDspController::addModule(const std::string& type, bool terminal) {
    {std::lock_guard lock(mutex_); goden_audio::MasterDspModuleV1 m;
     m.uuid=std::to_string(revision_.load()); m.type=type; m.terminal=terminal;
     state_.rackA.modules.push_back(std::move(m)); activeModuleCount_=state_.rackA.modules.size();
     state_.revision=revision_.fetch_add(1)+1;}
    goden_audio::notifyMasterDspCompilerWorkers(); return true;
}
void MasterDspController::clearRack() {
    {std::lock_guard lock(mutex_); state_.rackA.modules.clear(); activeModuleCount_=0;
     state_.revision=revision_.fetch_add(1)+1;}
    goden_audio::notifyMasterDspCompilerWorkers();
}
bool rolloutFlagEnabled(RolloutFlag) noexcept { return true; }
PerformanceTelemetry::PerformanceTelemetry() noexcept = default;
PerformanceTelemetry::~PerformanceTelemetry() = default;
PerformanceTelemetry& PerformanceTelemetry::instance() noexcept { static PerformanceTelemetry t; return t; }
void PerformanceTelemetry::event(PerformanceEventKindV2,TelemetrySurface,
    std::uint64_t a0,std::uint64_t a1,std::uint64_t,std::uint64_t,std::uint64_t) noexcept {
    if(a0==0x47445053ULL && a1==4) ++compiledStages;
}
}
static void requireEngine(bool value,const char* text) { if(!value) throw std::runtime_error(text); }
static void prepared(nekorox::goden_audio::MasterDspProcessor& p) {
    const auto revision=nekorox::component::MasterDspController::instance().revision();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    for(;;) {
        const auto t=p.prepareTicket();
        if(t.rackRevision==revision && t.state==nekorox::goden_audio::MasterDspPrepareState::ready) return;
        requireEngine(std::chrono::steady_clock::now()<deadline,"latest graph did not prepare");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
static std::vector<float> pump(nekorox::goden_audio::MasterDspProcessor& p,unsigned blocks=16) {
    std::vector<float> pcm(512*2);
    for(unsigned i=0;i<blocks;++i) { std::fill(pcm.begin(),pcm.end(),.25f);
        (void)p.processV2(pcm.data(),512,2,48000); }
    return pcm;
}
int main() {
  try {
    using namespace nekorox;
    using namespace nekorox::goden_audio;
    auto& c=component::MasterDspController::instance(); c.setEnabled(true);
    MasterDspProcessor p(MasterDspDomain::player);
    (void)p.prewarm(48000,2);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto dry=pump(p);
    requireEngine(component::compiledStages.load()==0 && dry.front()==.25f,
        "fresh empty rack compiled or changed untouched audio");
    c.addModule("utility.polarity"); (void)p.prewarm(48000,2); prepared(p);
    c.clearRack(); // stale nonempty stage must not be admitted by callback
    requireEngine(pump(p,1).front()==.25f,"obsolete stage became audible");
    prepared(p); (void)pump(p);
    c.addModule("utility.polarity"); prepared(p);
    requireEngine(pump(p).back()<-.249f,"polarity graph was not activated");
    c.clearRack(); prepared(p); // staged empty graph, old polarity still active
    c.clearRack(); prepared(p); // supersede empty stage before any callback
    requireEngine(pump(p).back()>.249f,"two clears stranded the old active graph");
    c.addModule("utility.channelDelay"); prepared(p); (void)pump(p);
    requireEngine(p.latencySamples()>0,"delay fixture did not establish PDC");
    c.clearRack(); prepared(p); (void)pump(p);
    requireEngine(p.tailSamples()==0,"empty graph kept obsolete processor tail");
    p.flush();
    requireEngine(p.latencySamples()==0 && pump(p).back()>.249f,"flush did not release retained PDC");
    c.addModule("utility.dcBlocker"); prepared(p); (void)pump(p);
    const auto original=_mm_getcsr(); const auto control=original & ~0x8040U;
    _mm_setcsr(control); const auto pcm=pump(p,1); const auto after=_mm_getcsr(); _mm_setcsr(original);
    requireEngine((after & 0xffc0U)==(control & 0xffc0U),"utility leaked MXCSR control flags");
    requireEngine(std::all_of(pcm.begin(),pcm.end(),[](float f){return std::isfinite(f);}),
        "utility produced nonfinite audio");
    std::cout<<"DSP actual engine: startup, stale stages, double clear, PDC and MXCSR PASS\n";
    return 0;
  } catch(const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
}
''', encoding='utf-8')
