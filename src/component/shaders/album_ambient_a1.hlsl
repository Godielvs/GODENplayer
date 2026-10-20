// Flow A3.1: a finite zero-input procedural pass. Artwork contributes only
// an immutable palette and seed; no cover or previous frame is sampled.
#define D2D_INPUT_COUNT 0
#define D2D_REQUIRES_SCENE_POSITION
#include "d2d1effecthelpers.hlsli"
struct FlowParams { float4 geometry,levels,hits,phases,colorLow,colorMid,colorHigh,colorBase,lyricsRect,trackRect,controls; };
cbuffer FlowConstants : register(b0) { FlowParams f; float4 textureBounds; };
float sat1(float x){return clamp(x,0.0,1.0);}
float smooth1(float a,float b,float x){float t=sat1((x-a)/max(b-a,0.00001));return t*t*(3.0-2.0*t);}
float luma(float3 c){return dot(c,float3(0.2126,0.7152,0.0722));}
float softBand(float d,float w){float a=d/max(w,0.005);return exp2(-1.442695*a*a);}
float2 rotate2(float2 p,float angle){float s=sin(angle),c=cos(angle);return float2(c*p.x-s*p.y,s*p.x+c*p.y);}
float2 velocity(float2 p,FlowParams v){
 float2 n0=float2(0.894427,0.447214),n1=float2(-0.514496,0.857493),n2=float2(0.371391,0.928477);
 float a=(0.055+0.19*v.levels.x+0.08*v.hits.x)*cos(2.1*dot(n0,p)+v.phases.x+v.phases.w);
 float b=(0.045+0.17*v.levels.y+0.09*v.hits.y)*cos(4.3*dot(n1,p)-v.phases.y+0.7*v.phases.w);
 float c=(0.014+0.045*v.levels.z+0.038*v.hits.z)*cos(8.1*dot(n2,p)+v.phases.z+1.3*v.phases.w);
 return a*float2(n0.y,-n0.x)+b*float2(n1.y,-n1.x)+c*float2(n2.y,-n2.x);
}
float protectedRect(float2 uv,FlowParams v,float4 rect){
 if(rect.z<=rect.x||rect.w<=rect.y)return 0.0;
 float feather=max(0.002,v.controls.w/max(v.geometry.y,1.0));float fx=feather/max(v.geometry.x,0.2);
 float x=smooth1(rect.x-fx,rect.x+fx,uv.x)*(1.0-smooth1(rect.z-fx,rect.z+fx,uv.x));
 float y=smooth1(rect.y-feather,rect.y+feather,uv.y)*(1.0-smooth1(rect.w-feather,rect.w+feather,uv.y));return x*y;
}
float3 flowLinear(float2 uv,FlowParams v){
 float aspect=clamp(v.geometry.x,0.25,8.0);float2 p=(uv-float2(0.5,0.5))*float2(aspect,1.0);
 // Stable artwork seed changes the shallow current direction, never its phase
 // cadence. 0.22 rad is 12.6 degrees.
 float tilt=0.22*sin(v.phases.w*1.6180339);p=rotate2(p,tilt);
 float live=sat1(v.levels.w);float2 q=p;float h=0.43*live;float2 vel=velocity(q,v);
 q=q-h*velocity(q-0.5*h*vel,v);vel=velocity(q,v);q=q-h*velocity(q-0.5*h*vel,v);
 float seed=v.phases.w;
 float lc=0.24+0.14*sin(1.55*q.x+v.phases.x+seed)+0.05*sin(3.7*q.x-v.phases.y);
 float low=softBand(q.y-lc,0.21+0.08*v.levels.x+0.05*v.hits.x);
 float mc=-0.12+0.18*sin(3.1*q.x-v.phases.y+seed)+0.06*sin(6.1*q.x+v.phases.x);
 float mid=softBand(q.y-mc,0.105+0.032*v.levels.y);
 float ridge=sin(9.4*q.x+10.8*q.y+1.15*sin(3.6*q.x-2.8*q.y+v.phases.y)+v.phases.z+seed);
 float high=softBand(ridge,0.20)*softBand(q.y+0.25-0.12*sin(4.1*q.x+v.phases.z),0.18);
 float3 w=float3(low*(0.12+live*(0.42*v.levels.x+0.34*v.hits.x)),mid*(0.12+live*(0.42*v.levels.y+0.28*v.hits.y)),high*(0.08+live*(0.25*v.levels.z+0.34*v.hits.z)));
 float3 bw=float3(0.24+0.35*uv.y,0.24+0.25*(1.0-uv.y),0.12+0.25*uv.x);
 float3 base=(v.colorLow.rgb*bw.x+v.colorMid.rgb*bw.y+v.colorHigh.rgb*bw.z)*0.030;
 float3 w2=w*w;float3 dye=(v.colorLow.rgb*w2.x+v.colorMid.rgb*w2.y+v.colorHigh.rgb*w2.z)/max(w2.x+w2.y+w2.z,0.018);
 float3 c=v.colorBase.rgb+base+dye*((w.x+w.y+w.z)*0.34)*sat1(v.controls.x);
 float protection=max(protectedRect(uv,v,v.lyricsRect),protectedRect(uv,v,v.trackRect));
 float cap=lerp(v.controls.y,v.controls.z,protection);float lum=luma(c);
 // A C1-continuous shoulder retains gradients below the cap instead of
 // flattening every bright pixel onto one luminance plane.
 float knee=cap*0.72;float mapped=lum;
 if(lum>knee){float span=max(cap-knee,0.00001);mapped=knee+span*(1.0-exp(-(lum-knee)/span));}
 c*=mapped/max(lum,0.000001);return clamp(c,0.0,1.0);
}
float encodeChannel(float x){return x<=0.0031308?12.92*x:1.055*pow(x,1.0/2.4)-0.055;}
D2D_PS_ENTRY(main){
 float2 uv=(D2DGetScenePosition().xy-textureBounds.xy)/max(textureBounds.zw,float2(1.0,1.0));
 float3 c=flowLinear(uv,f);return float4(encodeChannel(c.r),encodeChannel(c.g),encodeChannel(c.b),1.0);
}
