#ifndef GODEN_A4_COMMON
#define GODEN_A4_COMMON
#include "c11_alloy_v2.hlsli"
struct A4Params {
 float4 viewport, outputSize, levels, hits, phase;
 float4 color0,color1,color2,color3;
 float4 material, rect, input0,input1,pointer,tuning;
 float4 lightLevel[2],lightTexture[2];
 float4 lightControl;
 C11Alloy alloyV2;
};
float a4sat(float x){return clamp(x,0.0,1.0);}
float a4smooth(float a,float b,float x){float t=a4sat((x-a)/max(b-a,1e-6));return t*t*(3.0-2.0*t);}
float a4luma(float3 c){return dot(c,float3(.2126,.7152,.0722));}
float3 a4linear(float3 s){return float3(s.x<=.04045?s.x/12.92:pow(max((s.x+.055)/1.055,0.0),2.4),s.y<=.04045?s.y/12.92:pow(max((s.y+.055)/1.055,0.0),2.4),s.z<=.04045?s.z/12.92:pow(max((s.z+.055)/1.055,0.0),2.4));}
float3 a4encoded(float3 c){c=max(c,0.0);return float3(c.x<=.0031308?12.92*c.x:1.055*pow(c.x,1.0/2.4)-.055,c.y<=.0031308?12.92*c.y:1.055*pow(c.y,1.0/2.4)-.055,c.z<=.0031308?12.92*c.z:1.055*pow(c.z,1.0/2.4)-.055);}
float3 a4shoulder(float3 c,float maximum){float y=a4luma(c);return c*(maximum/(maximum+max(y,0.0)));}
float a4hash(float2 p,float seed){float3 p3=frac(float3(p.x,p.y,p.x)*.1031+seed*.017);p3+=dot(p3,p3.yzx+33.33);return frac((p3.x+p3.y)*p3.z);}
float a4band(float distance,float width){float q=distance/max(width,.008);return exp2(-q*q*1.442695);}
float a4sdf(float2 p,float2 halfSize,float radius){float2 q=abs(p)-halfSize+radius;return min(max(q.x,q.y),0.0)+length(max(q,0.0))-radius;}
// The reference behaves as one coherent sheet.  Colors, shadow and specular
// are fixed in sheet coordinates; input deforms those coordinates together.
// x = distance from the moving focal point, y = sheet band coordinate,
// z = local opening, w = fold strength.
float4 a4alloySurface(float2 uv,A4Params v){
 float aspect=v.viewport.x/max(v.viewport.y,1.0);
 float2 p=(uv-.5)*float2(aspect,1.0);
 float live=a4sat(v.levels.w);float3 e=saturate(v.levels.xyz)*live;
 float3 h=saturate(v.hits.xyz)*live;
 float attack=max(h.x,max(h.y,h.z));
 float energy=dot(e,float3(.48,.34,.18));

 // The focus travels in one direction across the left half, then re-enters
 // outside the visible sheet.  This reads as a changing viewing angle rather
 // than a sinusoidal left/right wobble.
 float cycle=frac(v.phase.x*.1591549431+v.phase.w*.173);
 cycle=cycle*cycle*(3.0-2.0*cycle);
 float pivot=(-.64+.38*cycle)*aspect;
 float x=p.x-pivot;

 // Attacks create the reference's focal convergence; sustained energy keeps
 // the sheet breathing without detaching individual color bands.
 float fold=a4sat(.045+.24*energy+1.08*attack);
 float minimumOpening=lerp(.94,.16,fold);
 float radial=abs(x)/max(aspect*.72,.30);
 float opening=minimumOpening+fold*(.86+.22*e.x)*radial;
 opening=max(opening,.13);

 // pointer.z is the artwork structure-tensor orientation, not mouse input.
 float tilt=clamp(v.pointer.z,-1.5708,1.5708)*.035;
 tilt+=(e.y-e.x)*.075+(h.y-h.z)*.12+(cycle-.5)*.045;
 float bow=(.026*(e.x-e.z)+.070*(h.x-h.z))*x*abs(x)/max(aspect,.5);
 float center=tilt*x+bow;
 float sheetY=(p.y-center)/opening;
 return float4(x,sheetY,opening,fold);
}
// The same surface sample drives background, glass and artwork optics.
float3 a4lightLegacy(float2 uv,A4Params v){
 float4 surface=a4alloySurface(uv,v);float y=surface.y;
 float live=a4sat(v.levels.w);float3 e=saturate(v.levels.xyz)*live;
 float3 h=saturate(v.hits.xyz)*live;
 float low=pow(a4band(y-.38,.19),1.30);
 float mid=pow(a4band(y+.01,.17),1.38);
 float high=pow(a4band(y+.39,.14),1.48);
 return float3(low*(.46+.72*e.x+1.28*h.x),
  mid*(.44+.70*e.y+1.18*h.y),
  high*(.50+.78*e.z+1.36*h.z));
}
float3 a4fieldLegacy(float2 uv,A4Params v){
 float4 surface=a4alloySurface(uv,v);
 float x=surface.x,y=surface.y,opening=surface.z,fold=surface.w;
 float live=a4sat(v.levels.w);float3 e=saturate(v.levels.xyz)*live;
 float3 h=saturate(v.hits.xyz)*live;float attack=max(h.x,max(h.y,h.z));
 float3 low=v.color0.rgb,mid=v.color1.rgb,high=v.color2.rgb;
 float3 silver=lerp(high,float3(.82,.82,.82),.22);

 // Ordered chromatic layers are material coordinates, not moving lights.
 // Warping the sheet therefore compresses and fans every layer coherently.
 float3 upper=lerp(high,mid,a4smooth(-.58,-.05,y));
 float3 base=lerp(upper,low,a4smooth(.02,.62,y));
 float broadTop=a4band(y+.38,.30),broadMid=a4band(y,.27),broadLow=a4band(y-.38,.31);
 float3 chroma=(high*broadTop+mid*broadMid+low*broadLow)/
  max(broadTop+broadMid+broadLow,.035);
 base=lerp(base,chroma,.42+.18*a4sat(v.material.z));

 // Each bright edge owns an adjacent shadow, giving the sheet thickness and
 // the liquid-metal reflection visible in the reference.
 float edgeTop=pow(a4band(y+.43,.050),1.48);
 float edgeMid=pow(a4band(y+.02,.061),1.42);
 float edgeLow=pow(a4band(y-.40,.055),1.52);
 float shadowTop=a4band(y+.33,.085);
 float shadowMid=a4band(y-.085,.095);
 float shadowLow=a4band(y-.50,.090);

 float focus=a4sat((.54-opening)*2.7)*fold;
 float focalGlint=a4band(y,.13)*focus;
 float3 rgb=v.color3.rgb+base*(.19+.08*(1.0-fold)+.10*broadMid);
 rgb+=high*edgeTop*(.30+.42*e.z+.66*h.z);
 rgb+=mid*edgeMid*(.34+.46*e.y+.74*h.y);
 rgb+=silver*edgeLow*(.42+.52*e.x+.88*h.x);
 rgb+=silver*focalGlint*(.52+1.05*attack);
 rgb*=1.0-(.16+.18*attack)*(shadowTop+shadowMid*.82+shadowLow*.68);

 // Brushing is evaluated in sheet coordinates, so it bends and converges with
 // the material instead of floating over it as screen-space noise.
 float seed=v.phase.w*31.0;
 float brush=.55*sin(y*360.0+seed)+.28*sin(y*790.0-seed*1.3)+.17*sin(y*1430.0+seed*.7);
 float grainLine=.5+.5*sin(x*27.0+seed*.12);
 rgb*=1.0+brush*(.014+.018*(1.0-v.material.x));
 rgb+=silver*(edgeTop+edgeMid+edgeLow)*grainLine*.022;
 return a4shoulder(max(rgb,0.0),.40+.10*focus+.07*attack);
}
#include "clovis_material.hlsli"
float3 a4light(float2 uv,A4Params v){
 return v.pointer.w>1.5?c11AlloyField(uv,max(v.lightControl.zw,1.),v.alloyV2,v.phase.xyz).rgb:
  v.pointer.w>.5?clovisLight(uv,v):a4lightLegacy(uv,v);
}
float3 a4field(float2 uv,A4Params v){
 return v.pointer.w>1.5?c11AlloyField(uv,max(v.lightControl.zw,1.),v.alloyV2,v.phase.xyz).rgb:
  v.pointer.w>.5?clovisField(uv,v):a4fieldLegacy(uv,v);
}
#endif
