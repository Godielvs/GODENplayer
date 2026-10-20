"""Real-source channel admission/dispatch oracle; FFT and host are explicit seams.

Usage: OUTPUT ANALYSIS_CPP baseline|candidate. Native visual QA covers the
complete default FFT path. This test does not simulate FFT math or rendering.
"""
from pathlib import Path
import hashlib,json,re,sys
out=Path(sys.argv[1]);src=Path(sys.argv[2]);mode=sys.argv[3]
assert mode in ('baseline','candidate');out.mkdir(parents=True,exist_ok=False)
repo=Path(__file__).resolve().parents[1];fork=repo/'out/third_party/spectrum-analyzer/foo_vis_spectrum_analyzer'
s=src.read_text(encoding='utf-8-sig');constants=(fork/'Constants.h').read_text();base=(fork/'Analyzers/Analyzer.h').read_text()
def method(text,name):
 a=text.index(name);start=text.index('{',a);depth=1;i=start+1
 while depth:
  if text[i]=='{':depth+=1
  elif text[i]=='}':depth-=1
  i+=1
 return text[a:i]
enums='\n'.join(re.search(r'enum class '+n+r'\b[^}]+};',constants)[0] for n in ['Channels','VisualizationType','Transform','WeightingType','SmoothingMethod'])
cpp=r'''
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>
__ENUMS__
using audio_sample=double;
struct audio_chunk {
 uint32_t rate=44100,count=1,config=4;std::vector<double> samples{.25};
 uint32_t get_sample_rate()const{return rate;} uint32_t get_channel_count()const{return count;}
 uint32_t get_channel_config()const{return config;} const double* get_data()const{return samples.data();}
 size_t get_sample_count()const{return count?samples.size()/count:0;}
};
struct state_t {
 VisualizationType _VisualizationType=VisualizationType::Bars;Transform _Transform=Transform::FFT;
 WeightingType _WeightingType=WeightingType::None;SmoothingMethod _SmoothingMethod=SmoothingMethod::None;
 int _WindowFunction=0,_WindowParameter=0,_WindowSkew=0,_Truncate=0,_KernelShape=0,_KernelShapeParameter=0,_KernelAsymmetry=0,_BinCount=4096;
 double _SmoothingFactor=.5;
};
struct graph_description_t {uint32_t _SelectedChannels=3;};
struct window_function_t {template<class... T>static window_function_t* Create(T...){return new window_function_t;}};
struct band {double Value=0;};using frequency_bands_t=std::vector<band>;
struct analyzer_t {
 uint32_t _ChannelConfig;int calls=0;uint32_t lastMask=0;
 template<class... T>analyzer_t(const state_t*,uint32_t,uint32_t,uint32_t config,T&&...):_ChannelConfig(config){}
 __AVERAGE__
 void Initialize(frequency_bands_t&){}
 void AnalyzeSamples(const double* f,size_t n,uint32_t mask,frequency_bands_t& bands) {
  ++calls;lastMask=mask;bands[0].Value=n?AverageSamples(f,mask):0;
 }
};
using fft_analyzer_t=analyzer_t;using cqt_analyzer_t=analyzer_t;
using swift_analyzer_t=analyzer_t;using analog_style_analyzer_t=analyzer_t;
struct analysis_t {
 const state_t* _State;const graph_description_t* _GraphDescription;
 uint32_t _SampleRate=0,_ChannelCount=0,_ChannelConfig=0,_ChannelMask=0;double _NyquistFrequency=0;
 int resets=0,other=0;frequency_bands_t _FrequencyBands{{}};
 analyzer_t *_FFTAnalyzer=nullptr,*_CQTAnalyzer=nullptr,*_SWIFTAnalyzer=nullptr,*_AnalogStyleAnalyzer=nullptr;
 window_function_t *_WindowFunction=nullptr,*_BrownPucketteKernel=nullptr;
 analysis_t(const state_t* s,const graph_description_t* g):_State(s),_GraphDescription(g){}
 ~analysis_t(){Reset();}
 void Reset(){++resets;_SampleRate=_ChannelCount=_ChannelConfig=0;
  delete _FFTAnalyzer;delete _CQTAnalyzer;delete _SWIFTAnalyzer;delete _AnalogStyleAnalyzer;
  _FFTAnalyzer=_CQTAnalyzer=_SWIFTAnalyzer=_AnalogStyleAnalyzer=nullptr;
  delete _WindowFunction;delete _BrownPucketteKernel;_WindowFunction=_BrownPucketteKernel=nullptr;_FrequencyBands[0].Value=0;
 }
 void MeterProcessing(const audio_chunk&){++other;}void OscilloscopeProcessing(const audio_chunk&){++other;}void BitMeterProcessing(const audio_chunk&){++other;}
 void ApplyAcousticWeighting(){}void Normalize(){}void NormalizeWithAverageSmoothing(double){}void NormalizeWithPeakSmoothing(double){}
 void Process(const audio_chunk&)noexcept;void SpectrumProcessing(const audio_chunk&)noexcept;
 analyzer_t* active(){switch(_State->_Transform){case Transform::FFT:return _FFTAnalyzer;case Transform::CQT:return _CQTAnalyzer;case Transform::SWIFT:return _SWIFTAnalyzer;case Transform::AnalogStyle:return _AnalogStyleAnalyzer;}return nullptr;}
};
__PROCESS__
__SPECTRUM__
constexpr bool candidate=__CANDIDATE__;int checks=0;
void require(bool p,const char* why){++checks;if(!p)throw std::runtime_error(why);}
int main(){try{
 require(uint32_t(Channels::ConfigMono)==4&&uint32_t(Channels::ConfigStereo)==3,"SDK masks");
 for(auto transform:{Transform::FFT,Transform::CQT,Transform::SWIFT,Transform::AnalogStyle}){
  for(auto visual:{VisualizationType::Bars,VisualizationType::Curve,VisualizationType::Spectrogram,VisualizationType::RadialBars,VisualizationType::RadialCurve}){
   state_t st;st._Transform=transform;st._VisualizationType=visual;graph_description_t gd;analysis_t a(&st,&gd);
   audio_chunk mono,stereo{44100,2,3,{.75,-.25}};
   a.Process(mono);require(a._ChannelMask==(candidate?4u:0u),"default mono admission");
   require(a._FrequencyBands[0].Value==(candidate?.25:0.),"real mono sample reaches selected analyzer");
   require(gd._SelectedChannels==3,"saved preference unchanged");
   if(candidate)require(a.active()&&a.active()->lastMask==4,"effective mask reaches transform");
   a.Process(stereo);require(a._ChannelMask==3&&a._FrequencyBands[0].Value==.25,"stereo unchanged");
   const auto reset=a.resets;a.Process(stereo);require(a.resets==reset,"stable layout no reset");
   a.Process(mono);require(a.resets==reset+1,"mono transition resets");
   require(a._FrequencyBands[0].Value==(candidate?.25:0.),"mono after stereo");
   a.Process(stereo);require(a.resets==reset+2&&a._FrequencyBands[0].Value==.25,"stereo after mono");
   mono.rate=48000;mono.samples={0.};a.Process(mono);require(a._FrequencyBands[0].Value==0.,"silence stays silent after rate change");
   for(uint32_t mask:{0u,1u,2u,4u,8u,5u,7u}){
    gd._SelectedChannels=mask;analysis_t custom(&st,&gd);mono.samples={.125};custom.Process(mono);
    require(custom._ChannelMask==(mask&4u),"custom selection exact");
    require(custom._FrequencyBands[0].Value==((mask&4u)?.125:0.),"custom selected sample exact");
   }
   gd._SelectedChannels=3;
   for(auto chunk:{audio_chunk{44100,2,48,{.5,.5}},audio_chunk{44100,2,4,{.5,.5}},audio_chunk{44100,0,0,{}}}){
    analysis_t custom(&st,&gd);custom.Process(chunk);require(custom._ChannelMask==0&&custom.active()==nullptr,"unsupported or inconsistent layout not reinterpreted");
   }
   audio_chunk surround{44100,6,63,{.125,.375,.8,.9,-.5,-.4}};analysis_t custom(&st,&gd);custom.Process(surround);
   require(custom._ChannelMask==3&&custom._FrequencyBands[0].Value==.25,"surround stereo subset unchanged");
  }
 }
 for(auto visual:{VisualizationType::PeakMeter,VisualizationType::LevelMeter,VisualizationType::Oscilloscope,VisualizationType::BitMeter,VisualizationType::Tester}){
  state_t st;st._VisualizationType=visual;graph_description_t gd;analysis_t a(&st,&gd);a.Process(audio_chunk{});
  require(a._ChannelMask==0&&a.other==0,"non-spectrum behavior preserved");
 }
 std::cout<<"{\"status\":\"PASS\",\"checks\":"<<checks<<",\"candidate\":"<<(candidate?"true":"false")<<"}\n";
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
'''
for token,value in {'__ENUMS__':enums,'__AVERAGE__':method(base,'audio_sample AverageSamples('),'__PROCESS__':method(s,'void analysis_t::Process('),'__SPECTRUM__':method(s,'void analysis_t::SpectrumProcessing('),'__CANDIDATE__':str(mode=='candidate').lower()}.items():cpp=cpp.replace(token,value)
(out/'probe.cpp').write_text(cpp,encoding='utf-8')
(out/'source.json').write_text(json.dumps({'analysisSource':str(src),'sha256':hashlib.sha256(src.read_bytes()).hexdigest(),'mode':mode,'verbatim':['Process','SpectrumProcessing','AverageSamples','production enums'],'seams':'Audio chunk container, Reset allocation cleanup, transform kernels, normalization and non-spectrum consumers. Tests dispatch and actual channel averaging; no FFT numeric or performance claim.'},indent=2)+'\n')
(out/'build.cmd').write_text('@echo off\ncall "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\ncd /d "'+str(out.resolve())+'"\ncl /nologo /std:c++20 /utf-8 /DNDEBUG /EHsc /MT /O2 /W4 probe.cpp /Fe:probe.exe\nexit /b %errorlevel%\n')
print('Extracted',mode)
