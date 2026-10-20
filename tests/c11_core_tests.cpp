#include "../src/component/c11/c11_note_lights.h"
#include "../src/component/c11/c11_art_profile.h"
#include "../src/component/c11/c11_alloy_gpu.h"
#include "../src/component/c11/c11_album_layout.h"
#include "../src/component/a4/a4_model.h"
#include "../src/component/a4/clovis_response.h"
#include "../src/component/library_presentation_policy.h"
#include "../src/component/c11/c11_change_inbox.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {
void requireC11(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

void runC11CoreTests()
{
    struct LayoutTrack {
        std::wstring genre,artist,albumArtist,album,title,year,codec,releaseId,path,searchFolded;
        std::vector<std::wstring> albumArtists;
        int disc=0,track=0,sampleRate=0,bitsPerSample=0,rating=0,playCount=0;
        double durationSeconds=0;
    };
    LayoutTrack before,after;after.rating=5;after.playCount=100;
    requireC11(nekorox::component::sameLibraryLayout(before,after),"statistics changed structural Library identity");
    after.album=L"New album";
    requireC11(!nekorox::component::sameLibraryLayout(before,after),"album edit did not invalidate structural view");
    goden::c11::ChangeInbox<int,int> inbox;
    for(int i=0;i<10000;++i)inbox.modify(i,i);
    const auto mass=inbox.take();
    requireC11(mass&&mass->full&&mass->handles.empty(),"mass edit exceeded bounded delta inbox");
    inbox.modify(1,1); inbox.invalidateAll();
    const auto cancelled=inbox.complete(*mass,false,true);
    requireC11(cancelled.recognized&&!cancelled.publish&&cancelled.more,
        "superseded job lost its pending full refresh");
    const auto replacement=inbox.take();
    requireC11(replacement&&replacement->full,"superseded job did not release its active ticket");
    inbox.stop();
    requireC11(!inbox.complete(*replacement,true).recognized,
        "stopped lifetime accepted a stale worker completion");
    using namespace goden::c11;
    requireC11(noteCount == 120, "C11 note grid is not E0..D#10");
    requireC11(minibandMaximumDegrees == 5.0f,
        "C13 physical miniband tilt is not five degrees");
    requireC11(sizeof(AlloyGpu) == 2304 &&
        sizeof(nekorox::component::a4::Constants) == 2624 &&
        offsetof(nekorox::component::a4::Constants, alloyV2) == 320,
        "C11 CPU constant-buffer ABI diverged");

    std::array<float, 1025> powers{};
    powers[43] = .02f; powers[42] = powers[44] = .008f;
    NoteProjector projector;
    NoteFrame frame;
    for (int i = 0; i < 100; ++i)
        frame = projector.ingest(powers.data(), powers.size(), 2048,
            48000, -12, .02f, 1, 1, i + 1);
    const auto peak = static_cast<std::size_t>(std::max_element(
        frame.level.begin(), frame.level.end()) - frame.level.begin());
    requireC11(noteHz(peak) > 750 && noteHz(peak) < 1300,
        "C11 note projector did not follow FFT power");
    requireC11(frame.confidence[0] < frame.confidence[72],
        "C11 low-frequency resolution confidence is not explicit");
    FocusTracker tracker;
    const auto first = tracker.update(frame, .02f);
    const auto repeated = tracker.update(frame, .02f);
    requireC11(first.sequence == repeated.sequence &&
        first.focus[0].energy == repeated.focus[0].energy &&
        first.miniband[peak].energy == repeated.miniband[peak].energy,
        "C11 focus tracker reintegrated the same publication");
    requireC11(first.miniband.size() == noteCount,
        "Clovis V1+120 did not preserve one miniband per semitone");
    requireC11(first.miniband[peak].energy > 0.0f,
        "Clovis V1+120 removed a real note response");
    for (const auto& band : first.miniband) {
        requireC11(std::abs(band.angle) <= minibandMaximumRadians + 1e-6f,
            "Clovis V1+120 miniband exceeded five degrees");
        requireC11(band.halfWidth >=
                minibandMinimumSemitones / float(noteCount) &&
            band.halfWidth <= minibandMaximumSemitones / float(noteCount),
            "Clovis V1+120 miniband width escaped its approved bounds");
    }

    std::vector<std::uint8_t> pixels(64U * 64U * 4U, 255U);
    for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x) {
        const auto k = (y * 64U + x) * 4U;
        pixels[k] = pixels[k + 1] = pixels[k + 2] =
            static_cast<std::uint8_t>(40U + x * 3U);
    }
    const auto neutral = analyzeArt(pixels.data(), 64, 64, 256, pixels.size());
    requireC11(neutral.valid && neutral.count > 0 && neutral.neutralFraction > .99f,
        "C11 neutral artwork profile fabricated or lost pigments");
    const auto gpu = makeGpu(neutral, first, 1.0f, 0.0, 1.0f);
    requireC11(gpu.miniband[peak].x == first.miniband[peak].energy &&
        gpu.miniband[peak].w == first.miniband[peak].halfWidth,
        "C13 GPU payload did not preserve the active V1+120 note");

    // A genuinely multicolour cover must retain all eight coherent pigments
    // through analysis and GPU packing.  The shader assigns these directly to
    // both broad bands and minibands instead of collapsing them to one average.
    constexpr std::array<std::array<std::uint8_t, 3>, 8> swatches{{
        {{245, 35, 45}}, {{25, 205, 240}}, {{250, 205, 25}}, {{35, 210, 80}},
        {{65, 45, 230}}, {{235, 55, 205}}, {{245, 130, 30}}, {{125, 70, 35}}
    }};
    std::vector<std::uint8_t> colourful(64U * 64U * 4U, 255U);
    for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x) {
        const auto& c = swatches[x / 8U];
        const auto k = (y * 64U + x) * 4U;
        colourful[k] = c[2]; colourful[k + 1] = c[1]; colourful[k + 2] = c[0];
    }
    const auto eight = analyzeArt(
        colourful.data(), 64, 64, 256, colourful.size());
    requireC11(eight.valid && eight.count == 8,
        "C13 multicolour artwork did not retain eight pigments");
    const auto eightGpu = makeGpu(eight, first, 1.0f, 0.0, 1.0f);
    requireC11(eightGpu.control.y == 8.0f,
        "C13 GPU payload did not advertise eight pigments");
    for (std::size_t i = 0; i < 8; ++i) {
        requireC11(eightGpu.pigment[i].w > 0.0f &&
            eightGpu.accent[i].x > 0.0f,
            "C13 dropped a valid pigment or its attention weight");
    }

    // Clovis presentation holds duplicate/stale publications and cannot alter
    // shared analysis, geometry or the independently fast spectral events.
    using nekorox::component::a4::ClovisResponse;
    using nekorox::component::collection::LightBands;
    ClovisResponse response;
    NoteLights quiet;quiet.sequence=1;quiet.master=.08f;
    quiet.miniband[54].energy=.60f;quiet.miniband[54].texture=.3f;
    quiet.miniband[54].loudness=.60f;
    quiet.miniband[54].absoluteDb=-26.f;
    quiet.miniband[54].angle=.03f;
    LightBands quietBroad;quietBroad.valid=true;quietBroad.master=.08f;
    quietBroad.level[3]=.6f;
    response.advance(quiet,quietBroad,1,0);
    requireC11(response.calmTexture()>.95f,"Clovis lost grain for quiet sparse music");
    auto presented=makeGpu(eight,quiet,1,0,1,quietBroad.level,quietBroad.texture,.08f);
    response.apply(presented);
    const auto held=presented;
    response.advance(quiet,quietBroad,1,.1);
    response.apply(presented);
    requireC11(presented.miniband[54].x==held.miniband[54].x&&
        presented.miniband[54].y==ClovisResponse::noteTargetDb(-26.f)&&presented.miniband[54].z==.03f&&
        presented.miniband[54].w==quiet.miniband[54].halfWidth,
        "Clovis redraw reintegrated audio or changed fast events/geometry");
    for(int tick=1;tick<=30;++tick){
        quiet.sequence++;response.advance(quiet,quietBroad,1,double(tick)/30);
    }
    response.apply(presented);
    requireC11(presented.miniband[54].x>.59f,
        "Clovis sustained light did not settle toward the real note");
    auto dense=quiet;
    for(auto& band:dense.miniband){band.energy=.35f;band.texture=.40f;}
    for(int tick=31;tick<=61;++tick){
        dense.sequence++;response.advance(dense,quietBroad,1,double(tick)/30);
    }
    requireC11(response.calmTexture()<.025f,
        "Clovis retained grain in quiet but spectrally dense music");
    const float suppressed=response.calmTexture();
    auto missing=quiet;missing.sequence=0;
    response.advance(missing,quietBroad,0,4);
    requireC11(response.calmTexture()==suppressed,
        "Clovis mistook missing audio for musical calm");
    quiet.sequence=dense.sequence+1;
    for(int tick=0;tick<45;++tick){
        quiet.sequence++;response.advance(quiet,quietBroad,1,4.+double(tick)/30);
    }
    requireC11(response.calmTexture()>.60f&&response.calmTexture()<.72f,
        "Clovis grain recovery is not gradual over the 1.5-second envelope");
    const float paused=response.calmTexture();
    response.advance(quiet,quietBroad,1,12);
    requireC11(response.calmTexture()==paused,
        "Clovis texture changed while its source publication was frozen");
    quiet.miniband[54].halfWidth=0;quiet.sequence++;
    response.advance(quiet,quietBroad,1,12.1);
    response.apply(presented);
    requireC11(presented.miniband[54].x==0,
        "Clovis retained light at an unsupported frequency");
    // A real note level, independent of relative salience and neighbouring
    // notes, must reach 63% in Light's 35 ms and decay to 37% in 180 ms.
    ClovisResponse timing;
    NoteLights pulse;pulse.sequence=1;
    timing.advance(pulse,quietBroad,1,0);
    pulse.sequence++;pulse.miniband[54].absoluteDb=-40.f;
    pulse.miniband[54].loudness=1.5f; // deliberately unrelated salience proxy
    timing.advance(pulse,quietBroad,1,.035);
    timing.apply(presented);
    const float attack=ClovisResponse::noteTargetDb(-40.f)*(1.f-std::exp(-1.f));
    requireC11(std::abs(presented.miniband[54].x-attack)<.00001f,
        "C12 no longer matches Light's attack timing or absolute level");
    pulse.miniband[54].absoluteDb=-120.f;
    for(unsigned i=0;i<3;++i){
        ++pulse.sequence;timing.advance(pulse,quietBroad,1,.095+double(i)*.060);
    }
    timing.apply(presented);
    requireC11(std::abs(presented.miniband[54].x-attack*std::exp(-1.f))<.00001f,
        "C12 no longer matches Light's release timing");
    requireC11(eightGpu.accent[0].w==1.f&&
        std::any_of(eightGpu.accent.begin(),eightGpu.accent.end(),
            [](const F4& accent){return accent.w<.1f;}),
        "Clovis did not distinguish nearby pigments from distant accent hues");

    // Absolute per-note power must survive changes in remote bands, global
    // loudness and local salience. The FFT's effective frequency resolution is
    // unchanged; use a resolved tone to keep its genuine support explicit.
    auto sparsePower=powers;
    auto remotePower=powers;
    for(std::size_t k=700;k<800;++k)remotePower[k]=.1f;
    NoteProjector isolatedProjector,denseProjector,quietProjector;
    auto quieterPower=powers;for(auto& p:quieterPower)p*=.01f;
    const auto isolated=isolatedProjector.ingest(sparsePower.data(),1025,2048,48000,-35,.02f,1,1,1);
    const auto busy=denseProjector.ingest(remotePower.data(),1025,2048,48000,-5,.02f,1,1,1);
    const auto softer=quietProjector.ingest(quieterPower.data(),1025,2048,48000,-55,.02f,1,1,1);
    requireC11(isolated.loudness[peak]==busy.loudness[peak],
        "Remote music changed the absolute volume of an unchanged semitone");
    requireC11(softer.loudness[peak]>0 && softer.loudness[peak]<isolated.loudness[peak],
        "Quieter semitone did not remain distinctly softer");
    requireC11(isolated.absoluteDb[peak]==busy.absoluteDb[peak] &&
        std::abs(softer.absoluteDb[peak]-isolated.absoluteDb[peak]+20.f)<.001f,
        "C12 measured note power changed with remote music or lost its dB range");
    requireC11(ClovisResponse::noteTargetDb(-70.f)<.035f &&
        ClovisResponse::noteTargetDb(-40.f)>.40f &&
        ClovisResponse::noteTargetDb(-40.f)<.45f &&
        ClovisResponse::noteTargetDb(-20.f)>.80f &&
        ClovisResponse::noteTargetDb(-80.f)==0.f &&
        ClovisResponse::noteTargetDb(std::numeric_limits<float>::quiet_NaN())==0.f,
        "C12 flattened quiet/medium/loud note levels or illuminated invalid input");
    for(std::size_t i=0;i<noteCount;++i)
        requireC11(std::isfinite(isolated.loudness[i]) && isolated.loudness[i]>=0 &&
            isolated.loudness[i]<=1,"Non-finite/out-of-range per-note loudness");
    auto redOutline=eight;redOutline.count=2;
    redOutline.pigments[0].linear={0,0,0};redOutline.pigments[0].broadWeight=.95f;
    redOutline.pigments[1].linear={.5f,0,0};redOutline.pigments[1].broadWeight=.05f;
    const auto red=nekorox::component::a4::clovisDominantPigment(redOutline);
    requireC11(red.x==.5f&&red.y==0&&red.z==0,
        "Black background erased the dominant red emitting pigment");
    const auto gray=nekorox::component::a4::clovisDominantPigment(neutral);
    requireC11(gray.x==gray.y&&gray.y==gray.z,"Monochrome cover acquired a foreign hue");

    for (int state = 0; state < 4; ++state) {
        const Visibility visible{(state & 1) != 0, (state & 2) != 0};
        const auto layout = layoutAlbum(1440, 820, visible, false);
        requireC11(visible.tracks || layout.tracks.empty(),
            "C11 hidden Tracks retained geometry");
        requireC11(visible.lyrics || layout.lyrics.empty(),
            "C11 hidden Lyrics retained geometry");
    }
    // Cover choices must drive the same bounded, non-overlapping rectangles
    // used for drawing, booklet composition, track input and lyrics input.
    for(float width:{320.f,699.f,700.f,980.f,1200.f,1920.f})
    for(float height:{320.f,820.f})for(bool focused:{false,true})
    for(unsigned state=0;state<8;++state)for(unsigned placement=0;placement<3;++placement){
        Visibility visible{bool(state&1),bool(state&2),bool(state&4),
            static_cast<ArtworkAlignment>(placement)};
        const auto layout=layoutAlbum(width,height,visible,focused);
        requireC11(!layout.panelsMenu.empty(),"Cover controls disappeared at a layout breakpoint");
        requireC11(visible.artwork||layout.art.empty(),"Hidden cover retained draw/hit geometry");
        const std::array<Rect,3> panes{layout.art,layout.tracks,layout.lyrics};
        for(const auto& r:panes)if(!r.empty())requireC11(
            r.l>=0&&r.t>=layout.headerBottom&&r.r<=width+.01f&&r.b<=height+.01f,
            "Album pane escaped its viewport");
        for(unsigned i=0;i<panes.size();++i)for(unsigned j=i+1;j<panes.size();++j){
            const auto&a=panes[i];const auto&b=panes[j];
            requireC11(a.empty()||b.empty()||a.r<=b.l+.01f||b.r<=a.l+.01f||
                a.b<=b.t+.01f||b.b<=a.t+.01f,"Cover placement overlaps another panel");
        }
        if(visible.artwork&&!layout.art.empty()&&placement==1)requireC11(
            std::abs((layout.art.l+layout.art.r)*.5f-width*.5f)<.01f,
            "Centered cover is not centered in the viewport");
    }
}
