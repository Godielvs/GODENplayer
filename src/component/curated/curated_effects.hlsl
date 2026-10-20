// Curated C1: purpose-built D3D11 ports of compute.toys examples.
// Caustics: David A. Roberts (2022).
// 2D Field Lagrangian Integrator: michael0884.
// Frozen source and upstream MIT notice: third_party/curated-c1/originals/.
// Music forces and integration are GODEN-specific adaptations. No universal importer.
// Curated C1. Explicit ports, not a WGSL importer.
// Caustics: David A Roberts (2022). Wave field: michael0884.
// Originals and provenance are shipped under originals/; see THIRD_PARTY.md.
// CS resources are distinct at every dispatch. The host unbinds UAVs between stages.
cbuffer C1 : register(b0) {
    uint4 Dimensions;
    float4 Bands;
    float4 Attacks;
    float4 Timing;
    float4 Palette0,Palette1,Palette2,Palette3;
    float4 Levels[16],Onsets[16],Relatives[16];
};
Texture2D<float4> StateIn : register(t0);
Texture2D<float4> RawImage : register(t1);
RWTexture2D<float4> StateOut : register(u0);
RWStructuredBuffer<uint> Photons : register(u1);
RWTexture2D<float4> ImageOut : register(u2);
SamplerState LinearRepeat : register(s0);
bool inside(uint2 p){return all(p<Dimensions.xy);}
float4 pointA(int2 p){if(any(p<0)||any(p>=int2(Dimensions.xy)))return 0;return StateIn.Load(int3(p,0));}
float4 sampleB(float2 p){return StateIn.SampleLevel(LinearRepeat,p/float2(Dimensions.xy),0);}
float4 advect(float2 p){return sampleB(p-sampleB(p).xy);}
float4 hash44(float4 p){float4 p4=frac(p*float4(.1031,.1030,.0973,.1099));p4+=dot(p4,p4.wzxy+33.33);return frac((p4.xxyz+p4.yzzw)*p4.zywx);}
float gaussian(float2 p,float2 center,float radius){float2 d=(p-center)/max(radius,.001);return exp(-dot(d,d)*.5);}
float softLimit(float value,float knee,float ceiling){
    float magnitude=abs(value);
    if(magnitude<=knee)return value;
    float span=max(ceiling-knee,.001);
    return sign(value)*(knee+span*(1-exp(-(magnitude-knee)/span)));
}
float4 stabilizeMusic(float4 state){
    // Preserve the entire normal source range.  Compress only pathological
    // excursions instead of clipping every Music frame at the old +/-3 limit.
    state.x=softLimit(state.x,5.,18.);
    state.y=softLimit(state.y,5.,18.);
    state.z=softLimit(state.z,20.,64.);
    state.w=saturate(state.w);
    return state;
}
float2 musicForce(float2 p){
    if((Dimensions.w&1U)==0)return 0;
    float2 size=float2(Dimensions.xy);float scale=min(size.x,size.y);
    float t=Timing.x;
    float lowMotion=.035+.055*Bands.x+.075*Attacks.x;
    float midMotion=.055+.045*Bands.y+.065*Attacks.y;
    float2 low=size*float2(.23,.67)+scale*lowMotion*float2(sin(t*.31+Bands.y),cos(t*.23+Attacks.x));
    float2 mid=size*float2(.69,.47)+scale*midMotion*float2(cos(t*.27+Bands.z),sin(t*.37+Attacks.y));
    float2 a=p-low,b=p-mid;
    float2 radial=a/(length(a)+scale*.025);
    float2 curl=float2(-b.y,b.x)/(length(b)+scale*.02);
    float2 force=radial*gaussian(p,low,scale*(.11+.025*Bands.x))*(.15*Bands.x+.62*Attacks.x);
    force+=curl*gaussian(p,mid,scale*(.16+.020*Bands.y))*(.12*Bands.y+.38*Attacks.y);
    [unroll]for(int j=0;j<3;++j){
      float2 h=size*float2(.26+.24*j,.24)+scale*(.015+.025*Bands.z+.035*Attacks.z)*float2(sin(t*.8+j),cos(t*.6+j));
      float2 d=p-h;
      force+=float2(-d.y,d.x)/(length(d)+scale*.014)*gaussian(p,h,scale*(.035+.012*Bands.z))*(.07*Bands.z+.26*Attacks.z);
    }
    // bindMusic already gates Bands and Attacks.  Multiplying by Bands.w here
    // used to apply the gate twice and made quiet/steady passages collapse.
    return force;
}
void VelocityCS_Reference(uint3 id){
 if(!inside(id.xy))return;float2 p=float2(id.xy)+.5;
 float4 r=advect(p);
 r.x-=.25*(advect(p+float2(1,0)).z-advect(p-float2(1,0)).z);
 r.y-=.25*(advect(p+float2(0,1)).z-advect(p-float2(0,1)).z);
 if(Dimensions.z<3)r=0;
 if(Dimensions.z>=3)r.xy+=musicForce(p);
 if((Dimensions.w&1U)!=0)r=stabilizeMusic(r);
 StateOut[id.xy]=r;
}
void PressureCS_Reference(uint3 id){
 if(!inside(id.xy))return;int2 i=int2(id.xy);float2 p=float2(i)+.5;
 float4 r=pointA(i);
 r.z-=.25*(pointA(i+int2(1,0)).x-pointA(i-int2(1,0)).x+pointA(i+int2(0,1)).y-pointA(i-int2(0,1)).y);
 float t=float(Dimensions.z)/120.;
 float music=(Dimensions.w&1U)!=0?1.:0.;
 float bass=music*(Bands.x+1.35*Attacks.x),middle=music*(Bands.y+Attacks.y);
 float treble=music*(Bands.z+Attacks.z);
 float2 orbitPhase=float2(t/15.+.70*middle-.24*treble,2.7*t/15.+.52*bass+.31*treble);
 float orbit=.75+music*(.10*Bands.x+.075*Attacks.x);
 float2 o=float2(Dimensions.xy)*.5*(1.+orbit*float2(cos(orbitPhase.x),sin(orbitPhase.y)));
 float emitter=.5*sin(2*t+music*.42*middle)*sin(t+music*.30*bass);
 emitter*=1.+music*(.42*Bands.x+.85*Attacks.x+.18*Bands.y);
 r=lerp(r,float4(emitter,0,r.z,1),exp(-(.2-music*.025*saturate(bass))*length(p-o)));
 if((Dimensions.w&1U)!=0){
   float2 size=float2(Dimensions.xy);float scale=min(size.x,size.y);
   float2 pressureCenter=size*float2(.72+.055*sin(Timing.x*.33+Bands.y),.25+.045*cos(Timing.x*.29+Bands.x));
   r.z+=gaussian(p,pressureCenter,scale*(.04+.02*Bands.z))*(.016*Bands.z*sin(Timing.x*7.1)+.055*Attacks.z);
   r=stabilizeMusic(r);
 }
 StateOut[id.xy]=r;
}
void CausticsCS_Reference(uint3 id){
 if(!inside(id.xy))return;
 [loop]for(uint j=0;j<25;++j){
  float4 h=hash44(float4(float2(id.xy),float(Dimensions.z),float(j)));
  float2 p=float2(id.xy)+h.xy;float z=lerp(.3,1.,h.z);
  float3 c=max(cos(z*6.2+float3(1,2,3)),0);
  float2 grad=.25*float2(sampleB(p+float2(1,0)).z-sampleB(p-float2(1,0)).z,sampleB(p+float2(0,1)).z-sampleB(p-float2(0,1)).z);
  p=frac((p+1e5*grad*z)/float2(Dimensions.xy))*float2(Dimensions.xy);
  uint2 q=min(uint2(p),Dimensions.xy-1);uint k=(q.x+q.y*Dimensions.x)*4;
  InterlockedAdd(Photons[k],uint(c.x*256.));
  InterlockedAdd(Photons[k+1],uint(c.y*256.));
  InterlockedAdd(Photons[k+2],uint(c.z*256.));
 }
}
void CausticsImageCS_Reference(uint3 id){
 if(!inside(id.xy))return;uint k=(id.x+id.y*Dimensions.x)*4;
 uint3 q=uint3(Photons[k],Photons[k+1],Photons[k+2]);
 float3 r=float3(q)/256.;r=r*sqrt(max(r,0))/5000.*float3(.5,.75,1);
 // A clamp protects the presentation from singular hot pixels; normal source range is unchanged.
 ImageOut[id.xy]=float4(min(r,64.),0);
 Photons[k]=uint(float(q.x)*.9);Photons[k+1]=uint(float(q.y)*.9);Photons[k+2]=uint(float(q.z)*.9);
}
float actionDerivative(float q,float previous,float prediction,float4 neighbors){
 // Same dual derivative as Action0 + Ltime(at2-a), evaluated without dual storage.
 return 2*(q-previous)+(neighbors.x-q)+(neighbors.y-q)+(neighbors.z-q)+(neighbors.w-q)
       -2*Timing.w*sin(Timing.w*q)+2*(q-prediction);
}
void WaveInitCS_Reference(uint3 id){
 if(!inside(id.xy))return;
 float2 p=32.*((float2(id.xy)-.5*float2(Dimensions.xy))/float(Dimensions.y));
 float q=4*exp(-dot(p,p))+4*exp(-dot(p-5.,p-5.));
 StateOut[id.xy]=float4(q,0,0,0);ImageOut[id.xy]=float4(q,0,0,0);
}
void WaveStepCS_Reference(uint3 id){
 if(!inside(id.xy))return;uint2 pos=id.xy;
 if(any(pos==0)||any(pos==Dimensions.xy-1)){StateOut[pos]=0;return;}
 int2 i=int2(pos);float4 a=pointA(i);
 float4 n=float4(pointA(i-int2(1,0)).x,pointA(i+int2(1,0)).x,pointA(i-int2(0,1)).x,pointA(i+int2(0,1)).x);
 float previous=a.x-a.y,prediction=a.x;uint j=0;
 if((Dimensions.w&1U)==0){
   [loop]for(j=0;j<64;++j){
     float f=actionDerivative(a.x,previous,prediction,n);
     float f1=actionDerivative(a.x,previous,prediction-.001,n);
     float f2=actionDerivative(a.x,previous,prediction+.001,n);
     float denom=f2-f1;
     if(abs(denom)<1e-12)break;
     float delta=f*.002/denom;prediction-=delta;
     if(abs(delta)<1e-6)break;
   }
 }else{
   prediction=.5*(n.x+n.y+n.z+n.w)-previous-Timing.w*sin(Timing.w*a.x);
   j=1;
   float2 p=float2(pos)/float2(Dimensions.xy);float aspect=float(Dimensions.x)/float(Dimensions.y);
   float2 metric=float2(aspect,1);
   float t=Timing.x;
   float low=exp(-dot((p-float2(.25,.64))*metric,(p-float2(.25,.64))*metric)/.015);
   float mid=exp(-dot((p-float2(.64,.43))*metric,(p-float2(.64,.43))*metric)/.005);
   float high=exp(-dot((p-float2(.78,.23))*metric,(p-float2(.78,.23))*metric)/.0012);
   // External forcing; never vary TimeStep or potential stiffness with the music.
   prediction+=Bands.w*(low*(.004*Bands.x*sin(t*4.7)+.012*Attacks.x)
       +mid*(.003*Bands.y*sin(t*9.1)+.009*Attacks.y)
       +high*(.002*Bands.z*sin(t*21.3)+.006*Attacks.z));
 }
 float avg=.5*(n.x+n.y);prediction=clamp(prediction,avg-8.,avg+8.);
 float velocity=prediction-a.x;float q=a.x+velocity*Timing.z;
 StateOut[pos]=clamp(float4(q,velocity,float(j)/32.,0),-35.,35.);
}
void WaveImageCS_Reference(uint3 id){
 if(!inside(id.xy))return;
 float3 value=pointA(int2(id.x,Dimensions.y-1-id.y)).xyz;
 float gray=.35*length(value);float grayLinear=pow(max(gray,0.0),2.2);float3 color=grayLinear.xxx;
 ImageOut[id.xy]=float4(min(color,64.),value.x);
}
float3 encodeSRGB(float3 v){
 v=max(v,0);return float3(v.x<=.0031308?12.92*v.x:1.055*pow(v.x,1./2.4)-.055,
  v.y<=.0031308?12.92*v.y:1.055*pow(v.y,1./2.4)-.055,
  v.z<=.0031308?12.92*v.z:1.055*pow(v.z,1./2.4)-.055);
}
void DisplayCS_Reference(uint3 id){
 if(!inside(id.xy))return;float4 source=RawImage.Load(int3(id.xy,0));float3 color=source.rgb;
 if((Dimensions.w&3U)==3U){
   float y=dot(color,float3(.2126,.7152,.0722));
   float m=saturate(.5+.14*source.a);
   float3 tint=lerp(Palette0.rgb,Palette1.rgb,m);
   tint=lerp(tint,Palette2.rgb,saturate(y)*.38);
   tint/=max(dot(tint,float3(.2126,.7152,.0722)),.04);
   color=lerp(color,tint*y,.7);
 }
 ImageOut[id.xy]=float4(saturate(encodeSRGB(min(color,64.))),1);
}

#include "c3_music.hlsli"
