#version 450 core
#extension GL_EXT_control_flow_attributes : enable
// GODEN_C8_FOCAL: refinamento local; Original permanece separado.
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
 float age=clamp(uv.y*512.0-.5,0.0,min(511.0,g_meta.w-1.0));
 float a=floor(age),f=fract(age),r0=mod(g_meta.z-a+512.0,512.0),r1=mod(r0-1.0+512.0,512.0);
 return mix(texture(GTex8,vec2(clamp(uv.x,.5/512.0,511.5/512.0),(r0+.5)/512.0)),
            texture(GTex8,vec2(clamp(uv.x,.5/512.0,511.5/512.0),(r1+.5)/512.0)),f);
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

// C7: local musical control. Silence adds no musical force; no camera impulse.
// Kept outside the analyzer: each visual owns its mapping and exposure.
float c7_level(float id){
 float b=max(g_band(id),0.0),r=g_relative(id);
 return clamp(g_options.x*max(0.0,b+.22*r*sqrt(b)),0.0,1.4);
}
float c7_event(float id){return clamp(g_options.x*g_hit(id),0.0,1.4);}
float c7_group(float first,float last){
 float v=0.0;for(int k=0;k<5;++k){float e=c7_level(mix(first,last,(float(k)+.5)/5.0));v+=e*e;}
 return sqrt(v/5.0);
}
float c7_exposure(float e){return e*e*(.72+.65*e);}
float c7_bandAt(float x){float a=floor(clamp(x,0.0,63.0));return mix(c7_level(a),c7_level(min(a+1.0,63.0)),fract(clamp(x,0.0,63.0)));}
// g_motion.x integrates (.35 + .45*low), so this is an integral, NOT time*energy.
float c7_bassTravel(){return .65*u_time+2.22222222*(g_motion.x-.35*u_time);}
vec3 c7_pigment(vec3 authored,float phase,float influence){return g_material(authored,g_weights(phase),influence);}
vec2 c7_source(int j){
 float z=float(j),g=floor(z*.5),sgn=mod(z,2.0)*2.0-1.0;
 float phase=g_motion[int(g)]*(.26+.045*g)+g*2.399963+sgn*.65;
 return vec2(.5)+vec2(.32*sin(phase)+.075*sin(phase*1.71+z),
                         .30*cos(phase*1.13+z*.7)+.07*sin(phase*.83));
}
float c7_sourceBand(int j){const float b[6]=float[6](4,11,23,34,46,58);return b[j];}
float c7_sourceRadius(int j){return .052+.032*min(c7_level(c7_sourceBand(j)),1.0);}

// Darken the complete image uniformly. A scalar exposure preserves hue and
// local contrast; unlike the previous luminance shoulder it does not compress
// highlights toward the midtones. The final clamp remains only an output guard.
vec3 c8_cloudExposure(vec3 color){return max(color,vec3(0))*.70;}



#define GODEN_VARIANT_MUSIC 1
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 @diatribes
//[LICENSE] https://opensource.org/licenses/MIT
    // viewport size in pixels (width, height)
         // seconds since playback started
   // seconds since previous frame
          // frame counter
         // xy: cursor pos (px), zw: click pos (negative when not pressed)
          // cumulative pixel offset from click-and-drag
       // linear scroll from mouse wheel (uncapped); use exp(u_scroll) for zoom
          // year, month, day, seconds since midnight
 // display refresh rate in Hz
    // 3D camera position (WASD/arrows to move)
    // 3D camera forward direction (drag to look)

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 @diatribes
//[LICENSE] https://opensource.org/licenses/MIT

/*
    see @bug's "Archipelago", too
        https://www.shadertoy.com/view/wfKcDR        
*/



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




// Preserve the original diatribes folded terrain, including its sharp ridges.
// Analytic gradients shade the actual implicit surface rather than a smooth hill.
vec4 bayTerrain(vec3 p,float footprint){
 float m=1.2+p.y-cos(p.x*.4);vec3 gradient=vec3(.4*sin(p.x*.4),1,0);
 for(float n=.01;n<.36;n*=1.5){
  vec3 phase=.3*p/n;float wave=dot(sin(phase),vec3(n));
  float detail=smoothstep(footprint*.25,footprint*.9,n);
  m+=mix(.85*n,abs(wave),detail);
  gradient+=detail*sign(wave)*.3*cos(phase);
 }
 return vec4(m,gradient);
}
vec3 bayCloudDomain(vec3 p,float travel){
 vec3 q=p;
 // The six folded octaves are the source cloud's form, not an added screen mask.
 for(float s=.01;s<.4;s*=2.0)
  q+=abs(dot(sin(.2*q.z-travel+.7*q/s),vec3(s*.25)));
 return q;
}
float bayCloud(vec3 p,float travel){
 return .03+.15*abs(bayCloudDomain(p,travel).y-1.0);
}
// A Brownian bridge expressed as nested midpoint displacements: the endpoints
// stay connected while each smaller scale introduces a new, angular kink.
// Geometry is deterministic in the material; audio changes discharge energy.
float bayRandom(uint seed,uint lane){
 uint x=seed^(lane*0x9e3779b9u);x^=x>>16u;x*=0x7feb352du;x^=x>>15u;x*=0x846ca68bu;x^=x>>16u;
 return float(x>>8u)*(1.0/16777216.0);
}
vec3 bayRandom3(uint seed,uint lane){
 uint x=seed^(lane*0x9e3779b9u);x^=x>>16u;x*=0x7feb352du;x^=x>>15u;x*=0x846ca68bu;x^=x>>16u;
 uint y=x*1664525u+1013904223u,z=y*1664525u+1013904223u;
 return vec3(x>>8u,y>>8u,z>>8u)*(1.0/16777216.0);
}
vec3 bayBoltDisplacement(float u,uint seed,int octaves){
 vec3 drift=vec3(0);float amplitude=1.,cells=1.;
 for(int octave=0;octave<octaves;++octave){
  float x=u*cells,cell=floor(x),tent=1.-abs(2.*fract(x)-1.);
  uint lane=uint(cell)+uint(octave)*131u;
  vec3 random=2.*bayRandom3(seed,lane)-1.;
  drift+=random*amplitude*tent;
  amplitude*=.53;cells*=2.;
 }
 return drift;
}
vec3 bayBoltPoint(vec3 top,float u,uint seed){
 vec3 end=vec3(7.0*(bayRandom(seed,501u)-.5),-top.y-2.0,
               5.0*(bayRandom(seed,502u)-.5));
 return top+end*u+bayBoltDisplacement(u,seed,5)*vec3(3.1,.80,2.1);
}
// Distance to a finite 3-D filament along this camera ray. Depth testing against
// terrain and a broad scattering lobe make the branches part of the scene.
float bayBoltSegment(vec3 ro,vec3 rd,vec3 a,vec3 b,float width,float limit,float veil){
 vec3 v=b-a,w=ro-a;float vv=dot(v,v),rv=dot(rd,v);
 float s=clamp((dot(v,w)-rv*dot(rd,w))/max(vv-rv*rv,1e-5),0.,1.);
 vec3 point=mix(a,b,s);float t=dot(point-ro,rd);
 if(t<=0.0||t>limit+.04)return 0.;
 vec3 separation=ro+t*rd-point;float distance2=dot(separation,separation);
 if(distance2>4.0)return 0.; // Below display precision even at the strongest flash.
 float pixel=max(width,1.35*t/u_resolution.y);
 float core=exp(-distance2/(pixel*pixel));
 float scattered=.13*exp(-distance2/.10)+.040*exp(-distance2/1.50);
 return (core*veil+scattered)*exp(-.012*t);
}
void goden_source_main(){
 vec2 uv=(2.0*gl_FragCoord.xy-u_resolution)/u_resolution.y;
 vec3 origin=vec3(0,0,g_motion.y*.42);
 vec3 ray=normalize(vec3(uv.x-.20,uv.y-.28,1.0));
 float travel=g_motion.y*.38;
 // CPU-owned event union: seed / age / birth world-Z / radiance.
 vec4 events[3]=vec4[3](g_meta,g_bands,g_attacks);
 float birthAspect[3]=float[3](g_options.y,g_options.z,g_motion.x);
 vec3 boltTop[3];float boltEnergy[3],growth[3];uint boltSeed[3];
 vec3 lightNodes[6];float lightEnergy[6];
 for(int i=0;i<3;++i){
  uint seed=uint(events[i].x);boltSeed[i]=seed;
  float depth=16.0+25.0*bayRandom(seed,603u);
  // Seed a position in the visible sky, then place the channel in world space.
  // Camera projection is (p.xy/p.z)+(.20,.28); using its actual aspect ratio
  // covers the sides of wide windows as well as the upper sky. The anchor
  // stays at its birth Z as the observer travels, including across resize.
  float aspect=clamp(birthAspect[i],.25,4.0);
  float skyX=(2.0*bayRandom(seed,601u)-1.0)*aspect*.92;
  float skyY=.20+.72*bayRandom(seed,602u);
  boltTop[i]=vec3((skyX-.20)*depth,max(2.4,(skyY-.28)*depth),events[i].z+depth);
  boltEnergy[i]=events[i].w;
  growth[i]=smoothstep(0.,.12+.18*bayRandom(seed,604u),events[i].y);
 }
 int leader=boltEnergy[1]>boltEnergy[0]?1:0;if(boltEnergy[2]>boltEnergy[leader])leader=2;
 for(int n=0;n<4;++n){
  float u=.15+.22*float(n);
  lightNodes[n]=bayBoltPoint(boltTop[leader],u,boltSeed[leader]);
  lightEnergy[n]=boltEnergy[leader]*smoothstep(u-.15,u+.05,growth[leader]);
 }
 // Cluster each weak echo's broad scattering. Its filament still keeps every
 // branch; only the distant atmospheric light uses its energy-weighted centre.
 for(int i=0;i<3;++i)if(i!=leader){int n=4+(i<leader?i:i-1);
  lightNodes[n]=bayBoltPoint(boltTop[i],.48,boltSeed[i]);
  lightEnergy[n]=boltEnergy[i]*3.2*smoothstep(.15,.65,growth[i]);
 }
 float distance=.02,groundDistance=65.;vec3 total=vec3(0);
 vec3 sky=v_color(vec3(.075,.10,.16),.12);
 vec3 body=v_color(vec3(.46,.51,.60),.42);
 vec3 edge=v_color(vec3(.75,.81,.92),1.6);
 vec3 rock=v_color(vec3(.30,.34,.43),1.1);
 [[dont_unroll]]for(int step=0;step<100;++step){
  vec3 p=origin+distance*ray;float footprint=distance/u_resolution.y;
  vec4 terrain=bayTerrain(p,max(.012,footprint));
  vec3 cloudDomain=bayCloudDomain(p,travel);
  vec3 material=cloudDomain;
  material+=3.2*vec3(sin(material.y*1.4+material.z*.21),sin(material.x*.37-material.y*.9),sin(material.z*.31+material.x*.27));
  vec3 spectral=vec3(dot(material,vec3(.083,.12,.045)),dot(material,vec3(-.074,.09,.079)),dot(material,vec3(.095,-.14,.051)));
  vec3 select=1.0-abs(2.0*fract(spectral)-1.0);
  vec2 low=v_band(14.*select.x),mid=v_band(15.+27.*select.y),high=v_band(43.+20.*select.z);
  float flash=0.;for(int j=0;j<6;++j){
   vec3 delta=p-lightNodes[j];
   flash+=lightEnergy[j]*1.1/(1.8+dot(delta,delta));
  }
  float cloud=.03+.15*abs(cloudDomain.y-1.0);
  float stepD=max(.0001,min(cloud,terrain.x*.6));
  if(terrain.x<.02)groundDistance=min(groundDistance,distance);
  // The source's inverse-distance accumulation exposes the folded cloud
  // filaments. There is no water plane or broad screen-space light source.
  vec3 sampleLight=body*(.00036+.00048*mid.x+.00023*mid.y)/cloud;
  sampleLight+=edge*(.00020*high.x+.00040*high.y+flash*.0080)/cloud;
  vec3 normal=normalize(terrain.yzw);
  float diffuse=.18+.82*max(dot(normal,normalize(vec3(-.6,.75,-.35))),0.);
  float nearGround=1.0-smoothstep(.02,.20,abs(terrain.x));
  sampleLight+=rock*nearGround*diffuse*(.0003+.0020*low.x+.0035*low.y+flash*.018);
  if(stepD<=.000101){
   // At an opaque terrain hit the source march repeatedly samples the same
   // minimum-distance point. Integrate that remaining stationary tail once.
   total+=sampleLight*float(100-step);
   break;
  }
  total+=sampleLight;
  distance+=stepD;
  if(distance>65.)break;
 }
 total+=sky*.10;
 vec3 lightning=v_color(vec3(1.45),1.85);
 if(g_options.w>.5){
  vec3 pigment=g_palette[0].rgb;
  for(int k=1;k<3;++k)if(dot(g_palette[k].rgb,vec3(.2126,.7152,.0722))>dot(pigment,vec3(.2126,.7152,.0722)))pigment=g_palette[k].rgb;
  lightning=g_encoded(pigment*min(1.2/max(dot(pigment,vec3(.2126,.7152,.0722)),.008),16.0));
 }
 for(int i=0;i<3;++i){
  if(boltEnergy[i]<.001)continue;
  // The higher sky anchors can have longer channels than the old fixed box.
  // Include their endpoints and asymmetric branches in the conservative bound.
  vec3 aabb0=(vec3(boltTop[i].x-12.,-5.,boltTop[i].z-9.)-origin)/ray;
  vec3 aabb1=(boltTop[i]+vec3(12.,3.,9.)-origin)/ray;
  vec3 nearV=min(aabb0,aabb1),farV=max(aabb0,aabb1);
  if(max(nearV.x,max(nearV.y,nearV.z))>min(farV.x,min(farV.y,farV.z)))continue;
  float rayDepth=max(0.,dot(boltTop[i]-vec3(0,4.75,0)-origin,ray));
  float cloudDepth=bayCloud(origin+ray*rayDepth*.65,travel);
  float veil=exp(-.08/max(cloudDepth,.03));
  uint seed=boltSeed[i];float filament=0.;
  vec3 trunk=boltTop[i];
  [[dont_unroll]]for(int j=0;j<32;++j){
   float u=float(j+1)/32.;if(float(j)/32.>=growth[i])break;
   vec3 b=bayBoltPoint(boltTop[i],min(u,growth[i]),seed);
   filament=max(filament,bayBoltSegment(origin,ray,trunk,b,.018,groundDistance,veil));
   trunk=b;
  }
  // Failed leaders depart at unrelated depths and mostly die close to the
  // main channel. They inherit its direction, not a symmetric tree silhouette.
  for(int branch=0;branch<5;++branch){
   uint branchSeed=seed+1319u+uint(branch)*571u;
   float u=floor((.10+.68*bayRandom(branchSeed,21u))*32.)/32.;
   float progress=clamp((growth[i]-u)/(.25+.35*bayRandom(branchSeed,22u)),0.,1.);
   if(progress<=0.)continue;
   vec3 start=bayBoltPoint(boltTop[i],u,seed),a=start;
   vec3 parent=normalize(start-bayBoltPoint(boltTop[i],u-1./32.,seed));
   float side=2.*step(.5,bayRandom(branchSeed,23u))-1.;
   float length=1.2+6.4*pow(bayRandom(branchSeed,24u),1.4);
   vec3 direction=normalize(parent*.48+vec3(side*(.55+.70*bayRandom(branchSeed,25u)),
                                           -.75,1.1*(bayRandom(branchSeed,26u)-.5)));
   vec3 end=direction*length;
   float strength=.40+.35*bayRandom(branchSeed,27u);
   for(int k=1;k<=8;++k){
    float f=min(float(k)/8.,progress);if(float(k-1)/8.>=progress)break;
    vec3 b=start+end*f+bayBoltDisplacement(f,branchSeed,3)*length*vec3(.23,.10,.18);
    float decay=pow(1.-.86*(f-.0625),.85);
    filament=max(filament,strength*decay*bayBoltSegment(origin,ray,a,b,.009,groundDistance,veil));
    // Only a few leaders fork again; their short, weak tips fade into the cloud.
    if(k==3&&bayRandom(branchSeed,28u)>.58){
     vec3 twigDirection=normalize(direction+vec3(-side*.9,-.15,.3));
     vec3 twig=b+twigDirection*length*.16+vec3(side*.1,-.08,.12);
     vec3 tip=b+twigDirection*length*.38;
     filament=max(filament,.20*bayBoltSegment(origin,ray,b,twig,.006,groundDistance,veil));
     filament=max(filament,.10*bayBoltSegment(origin,ray,twig,tip,.004,groundDistance,veil));
    }
    a=b;
   }
  }
  total+=lightning*boltEnergy[i]*filament*1.80;
 }
 total*=2.0;
 total*=pow(max(dot(total,vec3(.2126,.7152,.0722)),.00001),.25);
 fragColor=vec4(v_tone(total),1);
}
void main(){fragColor=vec4(0);goden_source_main();GodenOutput=vec4(clamp(fragColor.rgb,vec3(0),vec3(1)),1.0);}
