#ifndef A4_PASS
#define A4_PASS 0
#endif
#if A4_PASS == 0
#define D2D_INPUT_COUNT 0
#elif A4_PASS == 2
#define D2D_INPUT_COUNT 3
#define D2D_INPUT0_COMPLEX
#define D2D_INPUT1_COMPLEX
#define D2D_INPUT2_COMPLEX
#else
#define D2D_INPUT_COUNT 1
#define D2D_INPUT0_COMPLEX
#endif
#define D2D_REQUIRES_SCENE_POSITION
#include "d2d1effecthelpers.hlsli"
#include "a4_common.hlsli"
#include "clovis_optics.hlsli"
cbuffer A4Constants:register(b0){A4Params v;};
float3 a4FilmGrain(float3 c,float2 pos){
 float2 p=floor(pos);
 float n=a4hash(p,v.viewport.w)+a4hash(p+float2(37,19),v.viewport.w+13)-1.0;
 float correlated=a4hash(floor(pos*.5),v.viewport.w+29)-.5;
 n=.82*n+.18*correlated;
 // One scalar modulates the pigment. A missing red/green/blue channel must
 // not disable grain, and grain must not add a foreign hue to the artwork.
 float peak=max(c.r,max(c.g,c.b));
 float amplitude=min(max(v.tuning.x,0.),min(peak,max(1.0-peak,0.)));
 return c*(1.0+n*amplitude/max(peak,.0001));
}
D2D_PS_ENTRY(main){
 float2 pos=D2DGetScenePosition().xy;
 float2 uv=pos/max(v.outputSize.xy,float2(1,1));
#if A4_PASS == 0
 return float4(a4field(uv,v),1.0); // linear FP16
#elif A4_PASS == 1 // V1 glass; C12 keeps its refined material optics
 if(v.pointer.w>.5&&v.pointer.w<1.5){
  float2 clovisCenter=(v.rect.xy+v.rect.zw)*.5;
  float2 clovisHalfSize=max((v.rect.zw-v.rect.xy)*.5,1.);
  float clovisDpi=max(v.viewport.z,.5);
  float clovisRadius=min(18.*clovisDpi,min(clovisHalfSize.x,clovisHalfSize.y));
  float clovisSd=a4sdf(pos-clovisCenter,clovisHalfSize,clovisRadius);
  float clovisAlpha=(1.-a4smooth(-.8,.8,clovisSd))*saturate(v.tuning.w);
  if(clovisAlpha<=0.)return float4(0.,0.,0.,0.);
  float clovisEdge=1.-a4smooth(0.,18.*clovisDpi,-clovisSd);
  float2 clovisNormal=clovisRoundedNormal(pos-clovisCenter,clovisHalfSize,clovisRadius);
  float clovisLens=clovisEdge*clovisEdge*(3.-2.*clovisEdge);
  float2 clovisShift=clovisNormal*(10.*clovisDpi)*clovisLens;
  float clovisDispersion=(.45+1.2*saturate(v.material.z))*clovisDpi*clovisLens;
  float2 clovisToSource=v.input0.xy/max(v.outputSize.xy,1.);
  float2 clovisBase=(pos+clovisShift)*clovisToSource;
  float2 clovisOffset=clovisNormal*clovisDispersion*clovisToSource;
  float2 clovisHi=max(v.input0.xy-.5,.5);
  float3 clovisTransmitted=float3(
   D2DSampleInputAtPosition(0,clamp(clovisBase+clovisOffset,.5,clovisHi)).r,
   D2DSampleInputAtPosition(0,clamp(clovisBase,.5,clovisHi)).g,
   D2DSampleInputAtPosition(0,clamp(clovisBase-clovisOffset,.5,clovisHi)).b);
  float3 clovisColor=clovisTransmitted*lerp(.13,.86,clovisLens)+v.color3.rgb*.12;
  float clovisRim=exp(-pow((clovisSd+1.05*clovisDpi)/(.8*clovisDpi),2.));
  // Same retained illumination as transmission; no eight-emitter evaluation
  // over the complete native-resolution glass pane.
  float clovisIllumination=saturate(a4luma(clovisTransmitted)*4.0);
  float3 clovisReflection=clovisMaterialColor(v.color2.rgb,.50);
  clovisColor+=clovisReflection*clovisRim*(.12+.12*clovisIllumination);
  clovisColor=a4shoulder(max(clovisColor,0.),lerp(.055,.40,clovisLens));
  return float4(a4encoded(clovisColor)*clovisAlpha,clovisAlpha);
 }else{
 float2 center=(v.rect.xy+v.rect.zw)*.5;
 float2 halfSize=max((v.rect.zw-v.rect.xy)*.5,float2(1,1));
 float dpi=max(v.viewport.z,.5);float radius=min(v.tuning.y*dpi,min(halfSize.x,halfSize.y));
 float sd=a4sdf(pos-center,halfSize,radius);
 float alpha=(1.0-a4smooth(-.8,.8,sd))*saturate(v.tuning.w);if(alpha<=0.0)return float4(0,0,0,0);
 float edge=1.0-a4smooth(0.0,9.0*dpi,-sd);
 float2 n=normalize((pos-center)/max(halfSize-radius*.4,float2(1,1))+float2(.0001,.0002));
 float2 shift=n*min(v.tuning.z,6.0)*dpi*edge;
 float2 sourcePos=clamp((pos+shift)/v.outputSize.xy*v.input0.xy,float2(.5,.5),v.input0.xy-.5);
 float3 c=D2DSampleInputAtPosition(0,sourcePos).rgb;
 // Calm reading interior, but the curved rim retains directed light.
 float3 sourceLight=v.pointer.w>1.5?c:a4light(uv,v);
 c=c*lerp(.10,.42,edge)+v.color3.rgb*.20;
 float rim=a4band(sd+1.15*dpi,.70*dpi);
 c+=rim*(.018+.018*dot(sourceLight,float3(.4,.35,.25)))*lerp(v.color2.rgb,float3(.7,.7,.7),.5);
 c=a4shoulder(c,lerp(.025,.055,edge));
 return float4(a4encoded(c)*alpha,alpha);
 }
#elif A4_PASS == 2 // Clovis artwork optics; C12 receives a subtler diffuse wash
 if(v.pointer.w>.5){
  float2 clovisSize=max(v.rect.zw-v.rect.xy,1.);
  float2 clovisUv=(pos-v.rect.xy)/clovisSize;
  if(any(clovisUv<0.)||any(clovisUv>1.))return float4(0.,0.,0.,0.);
  float clovisDpi=max(v.viewport.z,.5);
  float clovisDistance=min(min(pos.x-v.rect.x,v.rect.z-pos.x),min(pos.y-v.rect.y,v.rect.w-pos.y));
  float clovisEdge=1.-a4smooth(0.,10.*clovisDpi,clovisDistance);
  float2 clovisDisplacement=float2(sin(v.phase.y),cos(v.phase.x))*.65*clovisDpi*clovisEdge*v.tuning.w;
  float2 clovisHalfTexel=.5*v.input0.xy/max(v.input0.zw,1.);
  float4 clovisSource=D2DSampleInputAtPosition(0,clamp((clovisUv+clovisDisplacement/clovisSize)*v.input0.xy,clovisHalfTexel,v.input0.xy-clovisHalfTexel));
  float clovisAlpha=saturate(clovisSource.a);
  float3 clovisColor=a4linear(clovisSource.rgb/max(clovisAlpha,.0001));
  float clovisDetail=D2DSampleInputAtPosition(1,clamp(clovisUv*v.input1.xy,.5,v.input1.xy-.5)).r;
  float2 clovisFieldSize=max(v.lightControl.zw,1.);
  float2 clovisFieldUv=pos/max(v.outputSize.xy,1.);
  float3 clovisFieldValue=D2DSampleInputAtPosition(2,
    clamp(clovisFieldUv*clovisFieldSize,.5,max(clovisFieldSize-.5,.5))).rgb;
  if(v.pointer.w>1.5){
   // Broad diffusion on artwork only: retain the original texture and remove
   // the old procedural ridge overlay and sharp miniband stripes on the cover.
   float2 spread=float2(.065,.045)*clovisFieldSize;
   float2 at=clovisFieldUv*clovisFieldSize;
   float2 hi=max(clovisFieldSize-.5,.5);
   clovisFieldValue*=.4;
   clovisFieldValue+=.15*D2DSampleInputAtPosition(2,clamp(at+float2(spread.x,0),.5,hi)).rgb;
   clovisFieldValue+=.15*D2DSampleInputAtPosition(2,clamp(at-float2(spread.x,0),.5,hi)).rgb;
   clovisFieldValue+=.15*D2DSampleInputAtPosition(2,clamp(at+float2(0,spread.y),.5,hi)).rgb;
   clovisFieldValue+=.15*D2DSampleInputAtPosition(2,clamp(at-float2(0,spread.y),.5,hi)).rgb;
  }
  float clovisIllumination=saturate(a4luma(clovisFieldValue)*4.0);
  float3 clovisTint=clovisMaterialColor(v.color1.rgb,.35);
  float clovisGain=v.pointer.w>1.5?.025:.05;
  clovisColor+=clovisTint*(clovisGain*clovisIllumination)*(1.-.84*clovisDetail)*(1.-clovisColor)*v.tuning.w;
  return float4(a4encoded(saturate(clovisColor))*clovisAlpha,clovisAlpha);
 }else{
 float2 size=max(v.rect.zw-v.rect.xy,float2(1,1));
 float2 local=(pos-v.rect.xy)/size;
 if(any(local<0)||any(local>1))return float4(0,0,0,0);
 float dpi=max(v.viewport.z,.5);
 float distance=min(min(pos.x-v.rect.x,v.rect.z-pos.x),min(pos.y-v.rect.y,v.rect.w-pos.y));
 float band=1.0-a4smooth(0.0,10.0*dpi,distance);
 float2 autoPose=float2(sin(v.phase.y)*.16,cos(v.phase.x)*.12)*v.levels.w;
 float2 displacement=autoPose;displacement/=max(1.0,length(displacement));
 displacement*=2.5*dpi*band*v.tuning.w;
 // The existing artwork bitmap may carry non-96 DPI. Input0.xy is DIP
 // size, .zw is physical size; sampling never confuses the two spaces.
 float2 halfTexel=.5*v.input0.xy/max(v.input0.zw,float2(1,1));
 float2 sourcePos=clamp((local+displacement/size)*v.input0.xy,halfTexel,v.input0.xy-halfTexel);
 float4 s=D2DSampleInputAtPosition(0,sourcePos);float alpha=a4sat(s.a);
 float3 c=a4linear(s.rgb/max(alpha,.0001));
 float detail=D2DSampleInputAtPosition(1,clamp(local*v.input1.xy,.5,v.input1.xy-.5)).r;
 float3 beams;
 if(v.pointer.w>1.5){
  float2 fieldSize=max(v.lightControl.zw,1.);
  beams=D2DSampleInputAtPosition(2,
   clamp(uv*fieldSize,.5,max(fieldSize-.5,.5))).rgb;
 }else beams=a4light(uv,v);
 float ridge=a4band(sin(14.0*uv.x+8.0*uv.y+.55*sin(v.phase.y))+.35*sin(24.0*uv.y-v.phase.z),.16);
 float illumination=ridge*dot(beams,float3(.50,.30,.20));
 float gain=.060*v.tuning.w*(1.0-.82*detail)*illumination;
 float3 tint=lerp(v.color1.rgb,v.color2.rgb,.45);
 c+=tint*gain*(1.0-c);
 c*=1.0-.05*band*(.5+.5*sin(v.phase.x+local.y*5.0))*v.tuning.w;
 c+=tint*a4band(distance/dpi-1.1,.8)*.020*v.tuning.w;
 return float4(a4encoded(saturate(c))*alpha,alpha);
 }
#elif A4_PASS == 3 // film: one pass, encoded domain, native pixels, before typography
 float4 s=D2DSampleInputAtPosition(0,clamp(pos,.5,v.input0.xy-.5));
 float3 c=s.rgb/max(s.a,.0001);
 c=a4FilmGrain(c,pos);return float4(c*s.a,s.a);
#else // native-resolution background; grain belongs to the background only.
 float3 linearField;
 if(v.pointer.w>.5){
  float2 source=clamp(uv*v.input0.xy,.5,max(v.input0.xy-.5,.5));
  linearField=D2DSampleInputAtPosition(0,source).rgb;
  linearField=v.pointer.w>1.5?c11NativeMicro(uv,v.outputSize.xy,v.alloyV2,linearField,saturate(v.tuning.x/.070)*saturate(v.outputSize.z)):clovisMicrostructure(linearField,uv,v);
 }else linearField=a4field(uv,v); // unchanged native-resolution legacy Alloy
 float3 c=a4encoded(linearField);
 if(v.tuning.x>0.0){
  c=a4FilmGrain(c,pos);
 }
 return float4(saturate(c),1.0);
#endif
}
