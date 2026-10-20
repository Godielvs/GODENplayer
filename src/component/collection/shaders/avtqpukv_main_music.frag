#version 450 core
#extension GL_EXT_control_flow_attributes : enable
// GODEN C5 musical material ABI; size is unchanged. Texture row zero is logical bottom for these ports.
// D3D final presentation flips Y exactly once; feedback buffers never flip.
#ifndef GODEN_MUSIC
#define GODEN_MUSIC 1
#endif
layout(std140,set=0,binding=0) uniform GodenFrame {
 vec4 g_size;       // pass width,height,elapsed effect seconds,fixed dt
 vec4 g_meta;       // frame index,recursion,indexed history head,history rows valid
 vec4 g_bands;      // broad low,mid,high,gate
 vec4 g_attacks;
 vec4 g_motion;     // phase/travel low,mid,high,integrated transport
 vec4 g_options;    // intensity,range end Hz,reserved,reserved
 vec4 g_palette[4];
 vec4 g_levels[16];
 vec4 g_onsets[16];
 vec4 g_relatives[16];
};
layout(set=1,binding=0) uniform sampler2D GTex0;
layout(set=1,binding=1) uniform sampler2D GTex1;
layout(set=1,binding=2) uniform sampler2D GTex2;
layout(set=1,binding=3) uniform sampler2D GTex3;
layout(set=1,binding=4) uniform sampler2D GTex4;
layout(set=1,binding=5) uniform sampler2D GTex5;
layout(set=1,binding=6) uniform sampler2D GTex6;
layout(set=1,binding=7) uniform sampler2D GTex7;
layout(set=1,binding=8) uniform sampler2D GTex8;
layout(set=1,binding=9) uniform sampler2D GTex9;
layout(set=1,binding=10) uniform sampler2D GTex10;

#define u_resolution g_size.xy
#define u_time g_size.z
#define u_time_delta g_size.w
#define u_frame int(g_meta.x)
#define u_recursion int(g_meta.y)
#define u_mouse vec4(0.0)
#define u_drag vec2(0.0)
#define u_scroll 0.0
#define u_date vec4(2026.0,9.0,16.0,0.0)
#define u_refresh_rate 60.0
#define u_camera_pos vec3(0.0)
#define u_camera_dir vec3(0.0,0.0,1.0)
#define i_frame_rate 60.0
#define i_channel_time float[4](0.0,0.0,0.0,0.0)
#define i_channel_resolution vec3[4](vec3(1.0),vec3(1.0),vec3(1.0),vec3(1.0))
#define u_pass1 GTex0
#define u_pass_1 GTex0
#define u_pass2 GTex1
#define u_pass_2 GTex1
#define u_pass3 GTex2
#define u_pass_3 GTex2
#define u_pass4 GTex3
#define u_pass_4 GTex3
#define u_pass5 GTex4
#define u_pass_5 GTex4
#define u_pass6 GTex5
#define u_pass_6 GTex5
#define u_pass7 GTex6
#define u_pass_7 GTex6

#define u_main GTex7
#define u_audio GTex8
#define u_tex1 GTex9
#define iChannel0 GTex0
#define iChannel1 GTex1
#define iChannel2 GTex2
#define iChannel3 GTex3
#define iResolution vec3(g_size.xy,1.0)
#define iTime g_size.z
#define iTimeDelta g_size.w
#define iFrame int(g_meta.x)
#define iMouse vec4(0.0)
#define iDate vec4(2026.0,9.0,16.0,0.0)
#define iChannelResolution vec3[4](vec3(g_size.xy,1.0),vec3(g_size.xy,1.0),vec3(g_size.xy,1.0),vec3(g_size.xy,1.0))
float g_band(float x){int i=clamp(int(floor(x)),0,63);return g_levels[i/4][i%4];}
float g_hit(float x){int i=clamp(int(floor(x)),0,63);return g_onsets[i/4][i%4];}
float g_relative(float x){int i=clamp(int(floor(x)),0,63);return g_relatives[i/4][i%4];}
float g_energy(float x){return clamp(g_options.x*g_band(x),0.0,1.0);}
float g_hash(vec2 x){return fract(sin(dot(x,vec2(12.9898,78.233)))*43758.5453);}
float g_id(vec2 cell){return floor(g_hash(cell)*64.0);}
float g_bass(){return g_options.x*g_bands.x;} float g_mid(){return g_options.x*g_bands.y;}float g_high(){return g_options.x*g_bands.z;}
float g_bhit(){return g_options.x*g_attacks.x;}float g_mhit(){return g_options.x*g_attacks.y;}float g_hhit(){return g_options.x*g_attacks.z;}
float g_pulse(float x){return g_options.x*g_hit(x);}
// Audio history is a bounded ring, row 0=now at API level. Never copy the
// complete history texture per frame. Explicit two taps handle ring seams.
vec4 g_audioSample(vec2 uv){
 if(g_meta.w<1.0)return vec4(0.0,.5,0.0,0.0);
 float age=max(uv.y*512.0-.5+g_options.z,0.0);
 if(age>min(511.0,g_meta.w-1.0))return vec4(0.0,.5,0.0,0.0);
 float a=floor(age),f=fract(age),r0=mod(g_meta.z-a+512.0,512.0),r1=mod(r0-1.0+512.0,512.0);
 vec4 newer=textureLod(GTex8,vec2(clamp(uv.x,.5/512.0,511.5/512.0),(r0+.5)/512.0),0.0);
 vec4 older=textureLod(GTex8,vec2(clamp(uv.x,.5/512.0,511.5/512.0),(r1+.5)/512.0),0.0);
 // Never interpolate across an explicit missing observation.
 if(newer.a<=.00001||(f>.00001&&older.a<=.00001))return vec4(0.0,.5,0.0,0.0);
 return mix(newer,older,f);
}
layout(location=0) out vec4 GodenOutput;
vec4 fragColor;

// C5: source-space pigment/light mixing, never a final-screen recoloring filter.
vec3 g_linear(vec3 c){c=max(c,vec3(0));return mix(c/12.92,pow((c+.055)/1.055,vec3(2.4)),step(vec3(.04045),c));}
vec3 g_encoded(vec3 c){c=max(c,vec3(0));return mix(c*12.92,1.055*pow(c,vec3(1.0/2.4))-.055,step(vec3(.0031308),c));}
vec4 g_weights(float x){vec4 w=.5+.5*cos(vec4(x,x+2.0943951,x+4.1887902,x+1.0471976));w=pow(w,vec4(3.0))+vec4(.035);w.w*=.22;return w/max(dot(w,vec4(1)),1e-5);}
vec3 g_material(vec3 authored,vec4 weights,float influence){
 float amount=clamp(g_options.w*influence,0.0,1.0);
 if(amount<=0.0)return authored;
 vec3 a=g_linear(authored);vec4 w=max(weights,vec4(0));w/=max(dot(w,vec4(1)),1e-6);
 // The Flow palette is uploaded in linear RGB; decoding it again crushes
 // mid-level chroma and makes colourful artwork look neutral.
 vec3 pigment=w.x*g_palette[0].rgb+w.y*g_palette[1].rgb+w.z*g_palette[2].rgb+w.w*g_palette[3].rgb;
 float la=max(dot(a,vec3(.2126,.7152,.0722)),.00001),lp=max(dot(pigment,vec3(.2126,.7152,.0722)),.008);
 // Preserve the authored light level; tint is selected inside each emitter/material.
 pigment*=clamp(la/lp,.05,8.0);
 return g_encoded(mix(a,pigment,amount));
}
vec3 g_softColor(vec3 x){x=max(x,vec3(0));float m=max(x.r,max(x.g,x.b));return x/(1.0+.38*m);}

#define GODEN_VARIANT_MUSIC 1
    // viewport size in pixels (width, height)
          // cumulative pixel offset from click-and-drag
       // linear scroll from mouse wheel; use exp(u_scroll) for zoom
 // https://soundcloud.com/charmlink/ryong-victor-espasandin

// Credit: @MilkDrop2077 (https://fragcoord.xyz/u/MilkDrop2077)
// Original shader: https://fragcoord.xyz/s/ao6dc217

// SPDX-License-Identifier: CC0-1.0
// Copyright (c) Public domain
//[LICENSE] https://creativecommons.org/publicdomain/zero/1.0/

/*
    "FFT Terrain"
    Renders the full spectrogram as a 3D height-field landscape,
    viewed from above and to the side.
    Use mouse click/drag and scroll to zoom

    Axes:
      World X (-4..4) → frequency bin, mirrored about 0 (bass at center)
      World Z (0..8)  → time history  (now → past)
      World Y         → FFT magnitude (terrain height)

    u_audio: 512×512  R=weighted FFT, G=waveform, B=raw FFT

    Song: ryong & victor espasandín - meaningless duet
    other songs to try:
        https://soundcloud.com/inmytalonsishapeclay/you-ok-never
        https://soundcloud.com/rapallosky/summer-walker-unloyal-rapallo-ijji-remix
        https://soundcloud.com/moapillar/boots-of-blinding-speed-2

*/

// Random | Dave Hoskins (Shadertoy)
//[SNIPPET] https://fragcoord.xyz/snippet/5e2620d5-6000-4a00-8000-ff8240d536468000
float rand1(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}


// Material-space response; no frame noise, feedback gain or audio-driven camera.
float v_soft(float x){x=max(x,0.0);float tail=max(x-1.0,0.0);return min(x,1.0)+tail/(1.0+2.5*tail);}
vec2 v_band(float coordinate){
 float b=clamp(coordinate,0.0,63.0);uint a=uint(b),c=min(a+1u,63u);float f=b-float(a);
 vec2 e=max(vec2(g_levels[a>>2u][a&3u],g_levels[c>>2u][c&3u]),vec2(0));
 vec2 r=vec2(g_relatives[a>>2u][a&3u],g_relatives[c>>2u][c&3u]);
 e=max(e+.22*r*sqrt(e),vec2(0));
 float h=mix(g_onsets[a>>2u][a&3u],g_onsets[c>>2u][c&3u],f);
 return vec2(v_soft(g_options.x*mix(e.x,e.y,f)),v_soft(g_options.x*h));
}
vec2 v_wrapBand(float coordinate,uint first,uint count){
 float a=floor(coordinate),f=coordinate-a;uint i=uint(max(a,0.0))%count;
 return mix(v_band(float(first+i)),v_band(float(first+(i+1u)%count)),f);
}
float v_group(uint first,uint last){float sum=0.;for(uint i=first;i<=last;++i){float x=max(g_levels[i>>2u][i&3u],0.);sum+=x*x;}return v_soft(g_options.x*sqrt(sum/float(last-first+1u)));}
vec3 v_pigment(float role){
 float x=clamp(role,0.0,2.0);uint a=uint(x),b=min(a+1u,2u);
 return mix(g_palette[a].rgb,g_palette[b].rgb,x-float(a));
}
vec3 v_color(vec3 authored,float role){
 if(g_options.w<.5)return authored;
 vec3 p=v_pigment(role);float y=max(dot(g_linear(authored),vec3(.2126,.7152,.0722)),.00001);
 float py=max(dot(p,vec3(.2126,.7152,.0722)),.008);
 return g_encoded(p*clamp(y/py,.05,8.0));
}
vec3 v_tone(vec3 c){c=max(c,vec3(0));return c/(1.0+max(c.r,max(c.g,c.b)));}
vec3 v_glow(vec3 c){c=max(c,vec3(0));float peak=max(c.r,max(c.g,c.b));float decay=exp(-2.0*peak);return c*((1.0-decay)/((1.0+decay)*max(peak,1e-6)));}



// History is a continuous triangulated height field. Cell traversal intersects
// the real mesh instead of stepping past thin peaks and leaving screen holes.
const vec2 spectrumExtent=vec2(9.2,8.0);
const vec2 spectrumCells=vec2(144.,152.);
vec3 spectrumAt(vec2 position){
 float frequency=clamp((position.x+4.6)/9.2,0.,1.);
 float age=(position.y*(300./8.)+.5)/512.;
 if(position.y<-.08||position.y>8.08)return vec3(0);
 float nyquist=max(g_options.y,100.),top=min(18000.,nyquist*.96);
 float hz=25.*pow(top/25.,frequency);
 vec2 coordinate=vec2((511.*hz/nyquist+.5)/512.,age);
 vec4 row=g_audioSample(coordinate);
 if(row.a<.00001)return vec3(0);
 // A log-space mesh cell spans several linear FFT bins in the treble.
 // Retain the measured peak over that exact footprint instead of sampling
 // between bins and losing a narrow tone. This adds no invented frequencies.
 float halfCell=.60*log(top/25.)/spectrumCells.x;
 float centreBin=511.*hz/nyquist;
 float footprint=.5*centreBin*(exp(halfCell)-exp(-halfCell));
 float firstBin=max(0.,floor(511.*hz*exp(-halfCell)/nyquist));
 float lastBin=min(511.,ceil(511.*hz*exp(halfCell)/nyquist));
 float peak=row.b;
 [[dont_unroll]]for(int tap=0;tap<26;++tap){
  float bin=firstBin+float(tap);if(bin>lastBin)break;
  vec4 measured=g_audioSample(vec2((bin+.5)/512.,age));
  // Fade each bin into the footprint continuously. Rounding the footprint
  // outward and taking an unweighted max creates broad bass terraces, then
  // jumps when the next FFT bin enters. This stays within measured extrema.
  float coverage=clamp(footprint+.5-abs(bin-centreBin),0.,1.);
  peak=max(peak,measured.b*coverage*step(.00001,measured.a));
 }
 // Light temporal reconstruction; a missing centre row remains a real gap.
 float sum=peak*.66,weight=.66;
 for(int tap=0;tap<2;++tap){
  vec4 adjacent=g_audioSample(coordinate+vec2(0,(tap==0?-1.:1.)/512.));
  float valid=step(.00001,adjacent.a);sum+=adjacent.b*.17*valid;weight+=.17*valid;
 }
 row.b=sum/weight;
 float gainDb=20.*log2(max(g_options.x,.001))*.301029996;
 float amplitude=clamp((row.b+gainDb/78.-.12)/.88,0.,1.);
 amplitude*=step(.00001,row.a)*step(.000001,row.b);
 return vec3(2.25*pow(amplitude,1.65),amplitude,row.a);
}
float spectrumTriangle(vec3 ro,vec3 rd,vec2 corner,vec2 spacing,vec3 a,vec3 b,vec3 c,bool upper,
                       float begin,float end,out vec3 normal,out float amplitude){
 vec2 slope=vec2(b.x-a.x,c.x-a.x)/spacing;
 if(upper)slope=-slope;
 normal=vec3(-slope.x,1.,-slope.y);
 vec3 point=vec3(corner.x,a.x,corner.y);
 float t=dot(normal,point-ro)/dot(normal,rd);
 vec2 uv=(ro.xz+t*rd.xz-corner)/spacing;
 if(upper)uv=-uv;
 amplitude=a.y+(b.y-a.y)*uv.x+(c.y-a.y)*uv.y;
 if(t<begin-.0001||t>end+.0001||min(uv.x,uv.y)<-.0001||uv.x+uv.y>1.0001)return 1e9;
 if(min(a.z,min(b.z,c.z))<.00001)return 1e9;
 return t;
}
void goden_source_main(){
 vec2 uv=(gl_FragCoord.xy-.5*u_resolution)/min(u_resolution.x,u_resolution.y);
 vec3 target=vec3(0,.40,3.5),origin=target+vec3(.9,5.2,-10.2);
 vec3 forward=normalize(target-origin),right=normalize(cross(vec3(0,1,0),forward));
 vec3 up=cross(forward,right);
 float focal=.90*min(u_resolution.x/u_resolution.y,2.7);
 vec3 ray=normalize(uv.x*right+uv.y*up+focal*forward);
 vec3 lo=(vec3(-4.6,-.01,0)-origin)/ray,hi=(vec3(4.6,2.26,8)-origin)/ray;
 vec3 nearV=min(lo,hi),farV=max(lo,hi);
 float first=max(0.,max(nearV.x,max(nearV.y,nearV.z))),last=min(farV.x,min(farV.y,farV.z));
 vec3 color=vec3(0);float hitDistance=1e9;
 if(last>first){
  vec2 spacing=spectrumExtent/vec2(144.,150.);
  // Vertices are tied to every second acquired row, never resampled against
  // a moving integer head. Offset the lattice for continuous history travel.
  vec2 latticeOrigin=vec2(-4.6,(mod(g_meta.z-g_options.z,2.0)-2.0)*(8./300.));
  vec2 start=origin.xz+(first+.0001)*ray.xz;
  vec2 cell=clamp(floor((start-latticeOrigin)/spacing),vec2(0),spectrumCells-1.);
  vec2 direction=sign(ray.xz);
  vec2 delta=spacing/max(abs(ray.xz),vec2(1e-7));
  vec2 boundary=latticeOrigin+(cell+step(vec2(0),direction))*spacing;
  vec2 crossing=(boundary-origin.xz)/ray.xz;
  float begin=first;
  [[dont_unroll]]for(int i=0;i<340;++i){
   float end=min(last,min(crossing.x,crossing.y));
   vec2 corner=latticeOrigin+cell*spacing;
   vec3 a=spectrumAt(corner),b=spectrumAt(corner+vec2(spacing.x,0));
   vec3 c=spectrumAt(corner+vec2(0,spacing.y)),d=spectrumAt(corner+spacing);
   vec3 n0,n1;float amp0,amp1;
   float t0=spectrumTriangle(origin,ray,corner,spacing,a,b,c,false,begin,end,n0,amp0);
   float t1=spectrumTriangle(origin,ray,corner+spacing,spacing,d,c,b,true,begin,end,n1,amp1);
   float t=min(t0,t1);
   if(t<1e8){
    vec3 p=origin+t*ray;float amplitude=t0<t1?amp0:amp1;
    if(amplitude>.001)hitDistance=t;
    // A display-sized derivative filters lighting, without adding spectral
    // peaks or altering the mesh/history data underneath it.
    float dx=max(.065,t/u_resolution.y*1.5),dz=max(.060,t/u_resolution.y*1.5);
    float sx=spectrumAt(p.xz+vec2(dx,0)).x-spectrumAt(p.xz-vec2(dx,0)).x;
    float sz=spectrumAt(p.xz+vec2(0,dz)).x-spectrumAt(p.xz-vec2(0,dz)).x;
    vec3 normal=normalize(vec3(-sx/(2.*dx),1.,-sz/(2.*dz)));
    float diffuse=.5+.5*dot(normal,normalize(vec3(-.65,1.,-.45)));
    float contour=pow(max(dot(reflect(normalize(vec3(.65,-1.,.45)),normal),-ray),0.),18.);
    float frequency=(p.x+4.6)/9.2;
    vec3 authored=.48+.40*cos(vec3(0,2,4)+frequency*2.7);
    vec3 pigment;
    if(g_options.w>.5){
     float low=smoothstep(.10,.58,amplitude),high=smoothstep(.63,.93,amplitude);
     vec3 p0=mix(g_palette[3].rgb,g_palette[0].rgb,low);
     p0=mix(p0,g_palette[1].rgb,high*.80);
     float crest=pow(smoothstep(.66,.98,amplitude),2.);
     p0=mix(p0,g_palette[2].rgb,crest*.74);
     pigment=g_encoded(p0/max(max(p0.r,max(p0.g,p0.b)),.08));
    }else pigment=authored;
    float energy=pow(max(amplitude,0.),1.30)*(.20+.72*diffuse)+.10*contour*amplitude;
    energy*=exp(-.045*p.z);
    color=v_tone(pigment*energy*1.85);
    break;
   }
   if(end>=last)break;
   bool nextX=crossing.x<crossing.y;
   if(nextX){cell.x+=direction.x;crossing.x+=delta.x;}
   else{cell.y+=direction.y;crossing.y+=delta.y;}
   if(any(lessThan(cell,vec2(0)))||any(greaterThanEqual(cell,spectrumCells)))break;
   begin=end;
  }
 }
 // Quiet frequency/time guides belong to the base plane and are occluded by
 // measured peaks. They disappear where history is unavailable, including gaps.
 float floorT=(-.025-origin.y)/ray.y;
 vec2 floorP=origin.xz+floorT*ray.xz;
 if(floorT>0.&&floorT<hitDistance&&abs(floorP.x)<4.6&&floorP.y>0.&&floorP.y<8.){
  float age=(floorP.y*(300./8.)+.5)/512.;float valid=g_audioSample(vec2(.5,age)).a;
  vec2 grid=(floorP+vec2(4.6,0))/vec2(1.15,.5);
  vec2 line=abs(fract(grid+.5)-.5)*vec2(1.15,.5);
  float aa=max(.009,floorT/u_resolution.y);
  float guides=1.-smoothstep(0.,aa,min(line.x,line.y));
  vec3 ink=g_options.w>.5?g_encoded(g_palette[0].rgb):vec3(.20,.32,.38);
  color+=ink*.026*guides*step(.00001,valid);
 }
 fragColor=vec4(color,1);
}
void main(){fragColor=vec4(0);goden_source_main();GodenOutput=vec4(clamp(fragColor.rgb,vec3(0),vec3(1)),1.0);}
