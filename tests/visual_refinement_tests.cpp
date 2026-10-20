#include "../src/component/collection/visual_refinement_state.h"
#include "../src/component/collection/collection_graph.h"
#include "../src/component/curated/curated_core.h"
#include <set>
#include <iostream>
#include <stdexcept>

namespace {
using namespace nekorox::component::collection;
unsigned checks=0;
void require(bool condition,const char* text){++checks;if(!condition)throw std::runtime_error(text);}
AudioFrame frame(){AudioFrame a{};a.valid=true;a.serial=1;a.recording=11;a.rate=48000;a.sequence=1;a.rmsDb=-18;return a;}
void storms(){
    StormState s;s.seed(23);auto a=frame();std::set<unsigned> seeds;
    for(unsigned band=0;band<64;++band){
        for(unsigned n=0;n<36;++n){a.hit={};a.level={};++a.sequence;s.step(&a,1./30,4,1,true);}
        a.hit[band]=.7f;a.level[band]=.6f;++a.sequence;
        auto before=s.births;s.step(&a,1./30,4,1,true);
        require(s.births==before+1,"every spectral region can initiate a discharge");
        unsigned born=0;for(auto&e:s.events)if(e.live()&&e.age==0){born=e.seed;seeds.insert(e.seed);}
        require(born!=0,"event must have a seed");
        for(unsigned n=0;n<4;++n){++a.sequence;s.step(&a,1./30,4,1,true);}
        require(s.births==before+1,"held onset must not spawn every frame");
        bool stable=false;for(auto&e:s.events)if(e.seed==born){stable=e.age>0&&e.birthTravel==4;}
        require(stable,"growth preserves geometry identity and world anchor");
    }
    require(seeds.size()==64,"event seeds repeated");
    auto births=s.births;s.step(&a,.1,5,1,true);require(s.births==births,"duplicate audio spawned an event");
    for(unsigned i=0;i<64;++i){a.hit[i]=1;a.level[i]=1;}++a.sequence;
    s.step(&a,.1,5,1,false);require(s.births==births,"pause created an event");
    s.step(nullptr,.1,5,1,true);require(s.births==births,"missing audio created an event");
    a.serial=2;a.recording=12;a.gap=true;++a.sequence;s.step(&a,.1,5,1,true);
    require(s.births==births,"identity transition manufactured an attack");
    for(auto&e:s.events)require(!e.live(),"track transition retained old discharge");
    StormState x,y;x.seed(934);y.seed(934);a=frame();
    for(unsigned i=0;i<600;++i){a.hit={};a.level.fill(.3f);if(i%6==0)a.hit[(i/6)%64]=.6f;++a.sequence;
        x.step(&a,1./30,float(i)*.01f,1,true);y.step(&a,1./30,float(i)*.01f,1,true);
        unsigned live=0;for(unsigned j=0;j<3;++j){live+=x.events[j].live();auto p=x.events[j].descriptor(),q=y.events[j].descriptor();
            require(p.x==q.x&&p.y==q.y&&p.z==q.z&&p.w==q.w,"fixed seed replay diverged");
            require(std::isfinite(p.w)&&p.w>=0,"invalid event energy");}
        require(live<=3,"event pool overflow");
    }
    require(x.births>40,"dense percussion cannot retrigger");
    StormState quiet,loud;quiet.seed(17);loud.seed(17);auto low=frame(),high=low;
    low.rmsDb=-57;
    for(unsigned i=0;i<180;++i){low.hit={};high.hit={};
        if(i%9==0)low.hit[8]=high.hit[8]=.7f;
        ++low.sequence;++high.sequence;quiet.step(&low,1./30,0,1,true);loud.step(&high,1./30,0,1,true);}
    require(quiet.births==0&&loud.births==20,"quiet fluctuations fired like strong percussion");
    StormState simultaneous;auto chord=frame();chord.hit[5]=.9f;chord.hit[29]=.8f;chord.hit[56]=.7f;
    simultaneous.step(&chord,1./30,0,1,true);
    require(simultaneous.births==3,"independent spectral onsets cannot coexist");
    auto frozen=simultaneous;
    const auto frozenBirths=frozen.births;
    require(!frozen.synchronizeIdentity(chord.serial,chord.recording),"same-track pause reset event state");
    require(frozen.events[0].live(),"same-track pause lost its discharge");
    require(frozen.synchronizeIdentity(chord.serial+1,chord.recording+1),"paused track change was ignored without audio");
    frozen.step(nullptr,0,0,1,false);
    for(const auto&e:frozen.events)require(!e.live(),"previous-track discharge survived identity reset");
    require(frozen.births==frozenBirths,"identity reset manufactured a discharge or reset PRNG history");
    unsigned principals=0;for(auto&e:simultaneous.events)principals+=e.rank>.5f;
    require(principals==1,"simultaneous echoes must remain subordinate");
    StormState correlated;chord=frame();for(unsigned b=20;b<26;++b)chord.hit[b]=.8f;
    correlated.step(&chord,1./30,0,1,true);
    require(correlated.births==1,"one correlated onset duplicated its discharge");
    StormState invalid;chord=frame();chord.hit.fill(1);chord.rmsDb=std::numeric_limits<float>::quiet_NaN();
    invalid.step(&chord,1./30,0,1,true);require(invalid.births==0,"non-finite energy generated lightning");
    // Fluctuations within a busy note are not fresh strikes.
    StormState wobble;auto held=frame();held.level.fill(.65f);held.hit[9]=.8f;
    wobble.step(&held,1./30,0,1,true);
    for(unsigned i=0;i<90;++i){held.hit[9]=i%2?.38f:.56f;++held.sequence;
        wobble.step(&held,1./30,0,1,true);}
    require(wobble.births==1,"sustained busy note kept retriggering lightning");
    for(unsigned i=0;i<30;++i){held.hit={};++held.sequence;wobble.step(&held,1./30,0,1,true);}
    held.hit[9]=.8f;++held.sequence;wobble.step(&held,1./30,0,1,true);
    require(wobble.births==2,"a real quiet valley failed to rearm the instrument");
    StormState sparse,busy;sparse.seed(73);busy.seed(73);auto isolated=frame(),dense=isolated;
    dense.level.fill(.7f);
    for(unsigned i=0;i<300;++i){isolated.hit={};dense.hit={};
        if(i%6==0)isolated.hit[6]=dense.hit[6]=.18f;
        ++isolated.sequence;++dense.sequence;
        sparse.step(&isolated,1./30,0,1,true);busy.step(&dense,1./30,0,1,true);}
    require(sparse.births>15&&busy.births<sparse.births/3,
        "small busy accents were treated like isolated calm onsets");
    StormState resized;held=frame();held.hit[7]=.8f;
    resized.step(&held,1./30,5,1,true,2.4f);const auto born=resized.events[0];
    ++held.sequence;resized.step(&held,1./30,6,1,true,1.f);
    require(resized.events[0].birthAspect==2.4f&&resized.events[0].seed==born.seed&&
        resized.events[0].birthTravel==5,"resize moved a live lightning anchor");
}
void phases(){
    RadianceMotionState base,driven;auto a=frame(),b=a;
    for(unsigned n=0;n<30;++n){b.level[25]=.9f;b.hit[25]=n%10==0?.7f:0.f;
        base.step(&a,1./30,1,true);driven.step(&b,1./30,1,true);}
    require(std::abs(driven.phase[3]-base.phase[3])>.4,"local arc did not accelerate");
    require(driven.phase[0]==base.phase[0]&&driven.phase[7]==base.phase[7],"unrelated arcs accelerated");
    auto before=driven.phase;driven.step(&b,0,1,true);require(before==driven.phase,"frozen motion advanced");
    for(unsigned i=0;i<12;++i)require(std::isfinite(driven.phase[i]),"non-finite integrated phase");
}
void scroll(){
    SpectrumScrollState scroll;double previous=-1;
    for(unsigned frame=1;frame<300;++frame){double now=double(frame)/30.;auto rows=std::uint64_t(std::floor(now*50));
        scroll.observe(rows,double(rows)/50.,false);scroll.advance(1./30);
        const double cursor=double(rows)-scroll.rowsBehind();
        if(frame>3){require(std::abs((cursor-previous)-50./30)<.0002,"history scroll is quantized");
            require(scroll.rowsBehind()>=2.5&&scroll.rowsBehind()<=4.1,"history latency escaped 60ms window");}
        previous=cursor;
    }
    float held=scroll.rowsBehind();scroll.advance(0);require(held==scroll.rowsBehind(),"paused history moved");
    scroll.reset();scroll.observe(600,12,true);scroll.advance(1./30);require(std::abs(scroll.rowsBehind()-3)<.001,"gap did not reanchor");
}
}
void runVisualRefinementTests(){
    require(sizeof(nekorox::component::collection::Constants)==928,"collection ABI changed");
    require(sizeof(nekorox::component::curated::Constants)==896,"Caustics ABI changed");
    storms();phases();scroll();std::cout<<"Visual refinement state: "<<checks<<" checks PASS\n";
}
#ifdef VISUAL_REFINEMENT_STANDALONE
int main(){try{runVisualRefinementTests();return 0;}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
#endif
