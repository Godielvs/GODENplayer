"""Extract actual Presence capture/helper code. Controlled SDK playback and
formatter seams reproduce a preceding backend handle without network or IPC.
Usage: OUTPUT SOURCE baseline|candidate
"""
from pathlib import Path
import hashlib,json,sys
repo=Path(__file__).resolve().parents[1];out=Path(sys.argv[1]);src=Path(sys.argv[2]);mode=sys.argv[3]
assert mode in ('baseline','candidate');out.mkdir(parents=True,exist_ok=False)
text=src.read_text(encoding='utf-8-sig');header=(repo/'src/component/discord_presence_service.h').read_text()
candidate='const auto usePlaybackMetadata = backendMatches' in text
assert candidate==(mode=='candidate'),'Explicit mode disagrees with source'
def block(s,sig):
 a=s.index(sig);b=s.index('{',a)+1;depth=1
 while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]
helpers=['bool hasCompletePlaybackRange(', 'std::string trimAscii(', 'std::string bounded(',
         'std::string metadata(', 'bool missingPresenceText(', 'template <std::size_t Size>\nvoid copyPresenceText(']
structs=['struct DiscordDawPresence {','struct PresenceUpdateV2 {','struct PresenceUpdateV3 {','struct DiscordTrackPresence {']
cpp=r'''
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "nekorox/contracts.h"
constexpr bool candidate=__CANDIDATE__;
unsigned checks=0;void require(bool v,const char* s){++checks;if(!v)throw std::runtime_error(s);}
struct Calls{unsigned get=0,playing=0,now=0,dynamicFormat=0,staticFormat=0,length=0;};Calls calls;
template<class T>struct Ptr{
 std::shared_ptr<T> p;Ptr()=default;Ptr(std::shared_ptr<T> x):p(std::move(x)){}bool is_valid()const{return bool(p);}T* get_ptr()const{return p.get();}T* operator->()const{return p.get();}bool operator==(const Ptr&)const=default;
};
namespace pfc {struct string8_fast{std::string value;const char* c_str()const{return value.c_str();}};}
struct titleformat_object{using ptr=Ptr<titleformat_object>;std::string expression;};
struct titleformat_compiler{
 static auto get(){static titleformat_compiler c;return &c;}
 void compile_safe(titleformat_object::ptr& out,const char* e){out=std::make_shared<titleformat_object>(titleformat_object{e});}
};
struct Track{
 std::map<std::string,std::string> fields;std::string path="file://E:/synthetic/B.flac";double length=180;bool success=true;
 const char* get_path()const{return path.c_str();}double get_length()const{return length;}
 bool format_title(void*,pfc::string8_fast& v,const titleformat_object::ptr& s,void*)const{++calls.staticFormat;if(!success)return false;auto i=fields.find(s->expression);v.value=i==fields.end()?"":i->second;return true;}
};
using metadb_handle_ptr=Ptr<Track>;
struct playback_control{
 enum{display_level_all=3};bool playing=true,nowSuccess=true,formatSuccess=true;double length=190;metadb_handle_ptr backend;
 std::map<std::string,std::string> dynamicFields;
 static Ptr<playback_control> get();
 bool is_playing(){++calls.playing;return playing;}
 bool get_now_playing(metadb_handle_ptr& out){++calls.now;out=backend;return nowSuccess&&backend.is_valid();}
 double playback_get_length(){++calls.length;return length;}
 bool playback_format_title(void*,pfc::string8_fast& v,const titleformat_object::ptr& s,void*,int){++calls.dynamicFormat;if(!formatSuccess)return false;auto i=dynamicFields.find(s->expression);if(i!=dynamicFields.end())v.value=i->second;else if(backend.is_valid()){auto t=backend->fields.find(s->expression);v.value=t==backend->fields.end()?"":t->second;}return true;}
};
std::shared_ptr<playback_control> control=std::make_shared<playback_control>();
Ptr<playback_control> playback_control::get(){++calls.get;return control;}
namespace nekorox::component {
__STRUCTS__
struct Runtime{
 PlaybackSnapshotV2 state;metadb_handle_ptr track;
 static auto& instance(){static Runtime r;return r;}
 auto playbackSnapshot(){return state;}auto currentTrackHandle(){return track;}
};
__HELPERS__
struct DiscordPresenceService{mutable std::mutex mutex_;DiscordDawPresence dawPresence_;DiscordTrackPresence captureCurrent()const;};
__CAPTURE__
}
using namespace nekorox;using namespace nekorox::component;
constexpr const char* artist="[%artist%]";
constexpr const char* codec="[%codec%][$if(%codec_profile%, / %codec_profile%,)]";
constexpr const char* bitrate="[$if(%bitrate%,%bitrate% kbps,)]";
constexpr const char* rate="[$if(%samplerate%,%samplerate% Hz,)]";
constexpr const char* depth="[$if(%bitspersample%,%bitspersample%-bit,)]";
constexpr const char* channels="[%channels%]";
constexpr const char* albumArtist="[%album artist%]";
constexpr const char* recording="[$lower($if3($meta(MUSICBRAINZ_TRACKID),$meta(MUSICBRAINZ TRACK ID)))]";
constexpr const char* release="[$lower($if3($meta(MUSICBRAINZ_ALBUMID),$meta(MUSICBRAINZ ALBUM ID)))]";
constexpr const char* filename="$filename(%path%)$if($ext(%path%),.$ext(%path%))";
constexpr const char* fallback="$if2(%title%,$filename(%path%))";
metadb_handle_ptr makeTrack(std::string prefix){auto t=std::make_shared<Track>();for(auto key:{artist,codec,bitrate,rate,depth,channels,albumArtist,recording,release,filename,fallback})t->fields[key]=prefix+key;t->path="file://E:/synthetic/"+prefix+".flac";return t;}
void reset(metadb_handle_ptr semantic,metadb_handle_ptr backend){
 *control=playback_control{};control->backend=backend;auto& r=Runtime::instance();r.track=semantic;r.state={};r.state.state=PlaybackState::playing;r.state.trackIdentityHash=42;r.state.durationSeconds=180;
 copyPresenceText(r.state.title,"New title");copyPresenceText(r.state.artist,"New artist");copyPresenceText(r.state.album,"New album");calls={};
}
void assertFields(const DiscordTrackPresence& v,const Track& expected){
 require(v.codec==bounded(trimAscii(expected.fields.at(codec))),"codec source");require(v.bitrate==bounded(trimAscii(expected.fields.at(bitrate))),"bitrate source");
 require(v.sampleRate==bounded(trimAscii(expected.fields.at(rate))),"sample rate source");require(v.bitDepth==bounded(trimAscii(expected.fields.at(depth))),"depth source");
 require(v.channels==bounded(trimAscii(expected.fields.at(channels))),"channel source");require(v.albumArtist==bounded(trimAscii(expected.fields.at(albumArtist))),"album artist source");
 require(v.musicBrainzRecordingId==bounded(trimAscii(expected.fields.at(recording))),"recording ID source");require(v.musicBrainzReleaseId==bounded(trimAscii(expected.fields.at(release))),"release ID source");
}
int main(){try{
 DiscordPresenceService s;auto a=makeTrack("Old-"),b=makeTrack("New-");
 // Each captured snapshot remains B; only the baseline accidentally enriches
 // it from A. Distinct handle objects also model two subsongs of one file.
 for(auto state:{PlaybackState::playing,PlaybackState::paused})for(unsigned path=0;path<2;++path){
  b->path=path?a->path:"file://E:/synthetic/New.flac";reset(b,a);Runtime::instance().state.state=state;
  auto v=s.captureCurrent();require(v.trackHandle==b&&v.playback.trackIdentityHash==42,"authoritative handle and identity");
  require(std::string(v.playback.title.data())=="New title"&&v.playback.state==state,"semantic snapshot preserved");
  assertFields(v,*(candidate?b:a).get_ptr());require(calls.staticFormat==(candidate?9U:0U)&&calls.dynamicFormat==(candidate?0U:9U),"mismatched formatter route");
 }
 reset(b,b);auto matched=s.captureCurrent();assertFields(matched,*b.get_ptr());
 require(calls.get==(candidate?1U:9U)&&calls.playing==(candidate?1U:9U),"backend query count per matched capture");
 require(calls.now==(candidate?1U:0U)&&calls.dynamicFormat==9&&calls.staticFormat==0,"matching dynamic route");
 std::cout<<"{\"matchedCaptureBackendGets\":"<<calls.get<<",\"isPlayingCalls\":"<<calls.playing<<",\"getNowPlayingCalls\":"<<calls.now<<",\"formatCalls\":"<<calls.dynamicFormat<<"}\n";
 // Stream fields must retain live data rather than being cached from tags.
 b->path="https://example.invalid/stream";reset(b,b);control->dynamicFields[codec]="Dynamic stream codec";control->dynamicFields[artist]="Live artist";
 auto live=s.captureCurrent();require(live.codec=="Dynamic stream codec"&&!live.finiteLocalFile,"stream dynamic formatting");control->dynamicFields[codec]="Changed codec";
 require(s.captureCurrent().codec=="Changed codec","dynamic values refreshed each capture");
 Runtime::instance().state.state=PlaybackState::paused;
 require(s.captureCurrent().codec=="Changed codec","matching paused stream retains dynamic metadata");
 // Missing artist fallback is attached to B even while A has tagged artist.
 b->path="file://E:/synthetic/B.flac";b->fields[artist]="";reset(b,a);
 auto untagged=s.captureCurrent();require(untagged.localFileFallback==candidate,"local fallback belongs to new track");
 require(untagged.fileName==(candidate?b->fields[filename]:""),"new filename fallback");
 Runtime::instance().state.title.fill(0);auto noTitle=s.captureCurrent();require(std::string(noTitle.playback.title.data())==(candidate?b:a)->fields[fallback],"new title fallback");
 b->path="https://example.invalid/stream";reset(b,a);auto unknown=s.captureCurrent();require(!unknown.localFileFallback&&!unknown.finiteLocalFile,"remote classification");
 require(std::string(unknown.playback.artist.data())==(candidate?"Unknown artist":"New artist"),"missing stream artist source");
 // During a not-yet-ready backend, the semantic track remains usable.
 b->path="file://E:/synthetic/B.flac";b->fields[artist]="New Artist";reset(b,b);control->playing=false;
 auto early=s.captureCurrent();if(candidate)assertFields(early,*b.get_ptr());else require(early.codec.empty(),"baseline empty when backend not playing");
 reset(b,{});auto noBackend=s.captureCurrent();if(candidate)assertFields(noBackend,*b.get_ptr());else require(noBackend.codec.empty(),"baseline lacks unavailable backend metadata");
 reset(b,b);control->nowSuccess=false;control->dynamicFields[codec]="Unconfirmed backend";
 require(s.captureCurrent().codec==(candidate?b->fields[codec]:"Unconfirmed backend"),"failed backend query excludes dynamic metadata");
 reset({},a);auto noTrack=s.captureCurrent();
 require(!noTrack.localFileFallback&&!noTrack.finiteLocalFile,"invalid semantic handle has no local flags");
 require(calls.staticFormat==0&&calls.dynamicFormat==(candidate?0U:9U),"invalid semantic handle never dereferenced");
 require(noTrack.codec==(candidate?"":bounded(trimAscii(a->fields[codec]))),"invalid semantic handle does not borrow backend metadata");
 // Durations are enriched only from the matching backend.
 reset(b,a);Runtime::instance().state.durationSeconds=0;b->length=12;control->length=999;
 require(s.captureCurrent().playback.durationSeconds==12&&calls.length==0,"wrong backend duration excluded");
 reset(b,b);Runtime::instance().state.durationSeconds=0;b->length=12;control->length=20;
 require(s.captureCurrent().playback.durationSeconds==20,"matching backend duration retained");
 reset(b,b);b->length=900;control->length=999;
 require(s.captureCurrent().playback.durationSeconds==180&&calls.length==0,"complete snapshot duration preserved");
 for(double invalid:{0.,-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){
  reset(b,a);b->length=invalid;Runtime::instance().state.durationSeconds=0;require(s.captureCurrent().playback.durationSeconds==0,"invalid duration excluded");
  reset(b,b);b->length=0;control->length=invalid;Runtime::instance().state.durationSeconds=0;require(s.captureCurrent().playback.durationSeconds==0,"invalid matching backend duration excluded");
 }
 for(bool aligned:{false,true}){
  b->fields[codec]=a->fields[codec]=std::string(127,'x')+"\xc3\xa9";reset(b,aligned?b:a);
  require(s.captureCurrent().codec==std::string(127,'x'),"UTF-8 incomplete endpoint removed");
  b->fields[codec]=a->fields[codec]=std::string(126,'x')+"\xc3\xa9";reset(b,aligned?b:a);
  require(s.captureCurrent().codec==std::string(126,'x')+"\xc3\xa9","UTF-8 complete endpoint retained");
 }
 // Both formatting routes preserve question-mark/missing handling, trimming,
 // byte limits and complete UTF-8 endpoints. Formatting failure stays empty.
 for(bool aligned:{false,true})for(std::string value:std::vector<std::string>{"?","   FLAC  ",std::string(140,'x'),std::string(127,'x')+"\xc3\xa9",std::string(126,'x')+"\xc3\xa9"}){
  b->fields[codec]=a->fields[codec]=value;reset(b,aligned?b:a);auto v=s.captureCurrent();
  auto expected=bounded(trimAscii(value));if(expected=="?")expected.clear();require(v.codec==expected,"bounded metadata semantics");
 }
 reset(b,b);control->formatSuccess=false;require(s.captureCurrent().codec.empty(),"dynamic formatter failure");
 reset(b,a);b->success=false;require(s.captureCurrent().codec==(candidate?"":bounded(trimAscii(a->fields[codec]))),"static formatter failure");b->success=true;
 // Stopped transport and DAW do not query the playback backend at all.
 reset(b,a);Runtime::instance().state.state=PlaybackState::stopped;auto stopped=s.captureCurrent();require(stopped.playback.state==PlaybackState::stopped&&calls.get==0,"stop early return");
 s.dawPresence_.active=true;s.dawPresence_.state=PlaybackState::playing;s.dawPresence_.focus="Stem";calls={};auto daw=s.captureCurrent();
 require(daw.dawMode&&daw.codec=="48 kHz project"&&calls.get==0,"DAW preserved independent of playback metadata");
 std::cout<<"{\"mode\":\""<<(candidate?"candidate":"baseline")<<"\",\"result\":\"PASS\",\"checks\":"<<checks<<",\"baselineReproducesHybridMetadata\":"<<(!candidate?"true":"false")<<"}\n";
 }catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
'''
cpp=cpp.replace('__CANDIDATE__',str(candidate).lower()).replace('__STRUCTS__','\n'.join(block(header,s)+';' for s in structs)).replace('__HELPERS__','\n'.join(block(text,s) for s in helpers)).replace('__CAPTURE__',block(text,'DiscordTrackPresence DiscordPresenceService::captureCurrent() const'))
(out/'oracle.cpp').write_text(cpp)
(out/'source.json').write_text(json.dumps(dict(source=str(src),sha256=hashlib.sha256(src.read_bytes()).hexdigest(),mode=mode,headerSha256=hashlib.sha256((repo/'src/component/discord_presence_service.h').read_bytes()).hexdigest(),real=['captureCurrent','metadata','bounds/trimming/duration/copy helpers','Presence structs','PlaybackSnapshotV2'],seams=['SDK backend and titleformat evaluated values','Runtime snapshot/handle'],noNetwork=True),indent=2)+'\n')
