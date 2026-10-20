#ifndef GODEN_C3_MUSIC
#define GODEN_C3_MUSIC
#include "c3_math.hlsli"
// Called after the frozen C2 reference helpers. Bit 2 explicitly selects Legacy.
bool c3Music(){return (Dimensions.w&1U)!=0U&&(Dimensions.w&4U)==0U;}
float c3SpectralShoulder(float value){value=max(value,0.);float tail=max(value-1.,0.);return min(value,1.)+tail/(1.+2.5*tail);}
float2 c3Spectral(uint band){
 band=min(band,63U);float e=max(Levels[band/4U][band%4U],0.);
 e=max(0.,e+.22*Relatives[band/4U][band%4U]*sqrt(e));
 return float2(c3SpectralShoulder(e*Bands.w),c3SpectralShoulder(Onsets[band/4U][band%4U]*Bands.w));
}
float4 c3WaterRegion(float2 pixel){
 float2 uv=pixel/float2(Dimensions.xy);float t=Timing.x;
 // Cells only accelerate neighbour lookup. The visible sources overlap at
 // irregular positions in a warped, slowly travelling material domain.
 float2 grid=8.*uv+.42*float2(sin(6.2831853*uv.y+.071*t),sin(6.2831853*uv.x-.053*t));
 int2 cell=int2(floor(grid));float2 force=0;float pressure=0,pigment=0,weight=0;
 [loop]for(int y=-2;y<=2;++y)[loop]for(int x=-2;x<=2;++x){
  int2 c=cell+int2(x,y);uint2 wrapped=uint2((c%8+8)%8);uint band=wrapped.x+8U*wrapped.y;
  float4 seed=hash44(float4(float2(wrapped),17.3,41.9));
  float2 center=float2(c)+.5+.66*(seed.xy-.5);
  center+=.10*float2(sin(.13*t+seed.z*20.),cos(.11*t+seed.w*20.));
  float radius=.75+.37*seed.z;float2 q=(grid-center)/radius;
  float angle=6.2831853*seed.w;float2 direction=float2(cos(angle),sin(angle));
  float2 axis=float2(dot(q,direction),dot(q,float2(-direction.y,direction.x)));
  axis*=float2(.83,1.12);float r2=dot(axis,axis);if(r2>=1.)continue;
  float support=pow(1.-r2,3.);float2 audio=c3Spectral(band);
  float e=audio.x/(1.+.4*audio.x),h=audio.y/(1.+.8*audio.y);
  float2 tangent=float2(-q.y,q.x)*(seed.x<.5?-1.:1.);
  if(band<15U){force+=.013*q*support*(.32*e+1.15*h);pressure+=.010*(1.-5.*r2)*support*(.50*e+1.30*h);}
  else if(band<43U){force+=.012*(tangent+.22*direction)*support*(e+.70*h);pressure+=.007*axis.x*support*(.45*e+h);}
  else {force+=.007*direction*sin(4.*axis.y)*support*(.35*e+h);pressure+=.007*(axis.x*axis.x-axis.y*axis.y)*support*(.30*e+1.25*h);}
  pigment+=support*(.12+.76*seed.y+.10*axis.x);weight+=support;
 }
 return float4(force,pressure,pigment/max(weight,.001));
}
[numthreads(16,16,1)]void VelocityCS(uint3 id:SV_DispatchThreadID){
 if(!c3Music()){VelocityCS_Reference(id);return;}
 if(!inside(id.xy))return;float2 p=float2(id.xy)+.5;
 float4 r=advect(p);
 r.x-=.25*(advect(p+float2(1,0)).z-advect(p-float2(1,0)).z);
 r.y-=.25*(advect(p+float2(0,1)).z-advect(p-float2(0,1)).z);
 if(Dimensions.z<3U)r=0;
 float4 local=c3WaterRegion(p);
 r.xy+=local.xy;
 // W transports the water's optical material alongside its velocity.
 r.w=Dimensions.z<3U?local.w:lerp(r.w,local.w,.004);
 r.xy*=.9985;StateOut[id.xy]=r;
}
[numthreads(16,16,1)]void PressureCS(uint3 id:SV_DispatchThreadID){
 if(!c3Music()){PressureCS_Reference(id);return;}
 if(!inside(id.xy))return;int2 i=int2(id.xy);float2 p=float2(i)+.5;
 float4 r=pointA(i);
 r.z-=.25*(pointA(i+int2(1,0)).x-pointA(i-int2(1,0)).x+
              pointA(i+int2(0,1)).y-pointA(i-int2(0,1)).y);
 float t=float(Dimensions.z)/120.;
 float2 o=float2(Dimensions.xy)*.5*(1.+.75*float2(cos(t/15.),sin(2.7*t/15.)));
 r.xy=lerp(r.xy,float2(.5*sin(2*t)*sin(t),0),exp(-.2*length(p-o)));
 float4 local=c3WaterRegion(p);
 r.z+=local.z;
 r.z=r.z/(1.+abs(r.z)/32.);StateOut[id.xy]=r;
}
[numthreads(16,16,1)]void CausticsDecayCS(uint3 id:SV_DispatchThreadID){
 if(!c3Music()||!inside(id.xy))return;
 uint k=(id.x+id.y*Dimensions.x)*4U;
 float a=c3PhotonWeights(Attacks.w).x;
 Photons[k]=uint(float(Photons[k])*a);
 Photons[k+1]=uint(float(Photons[k+1])*a);
 Photons[k+2]=uint(float(Photons[k+2])*a);
}
[numthreads(16,16,1)]void CausticsCS(uint3 id:SV_DispatchThreadID){
 if(!c3Music()){CausticsCS_Reference(id);return;}
 if(!inside(id.xy))return;
 float2 size=float2(Dimensions.xy);float scale=min(size.x,size.y);
 float deposit=c3PhotonWeights(Attacks.w).y;
 // Never exceed the existing scale-64 photon bound, including dense 200% input.
 float musicalDeposit=.72;
 // Water normals and optical path determine pigment transport, not the
 // source's fixed spectral grid. Colour travels with refracted photon bundles.
 float2 center=float2(id.xy)+.5;
 float2 gradient=.0625*float2(sampleB(center+float2(4,0)).z-sampleB(center-float2(4,0)).z,
                             sampleB(center+float2(0,4)).z-sampleB(center-float2(0,4)).z);
 float curvature=sampleB(center+float2(1,0)).z+sampleB(center-float2(1,0)).z+
                 sampleB(center+float2(0,1)).z+sampleB(center-float2(0,1)).z-4.*sampleB(center).z;
 [loop]for(uint j=0;j<25U;++j){
  float4 rnd=hash44(float4(float2(id.xy),float(Dimensions.z),float(j)));
  float2 p=float2(id.xy)+rnd.xy;float z=lerp(.78,1.,rnd.z);
  float3 color=max(cos(z*6.2+float3(1,2,3)),0.);
  if((Dimensions.w&2U)!=0U){
   float focus=saturate(abs(curvature)*12.+length(gradient)*4.);
   float opticalRole=clamp(.10+1.72*sampleB(center).w+.24*focus+.12*(z-.89),0.,2.);
   float3 pigment=opticalRole<1.?lerp(Palette0.rgb,Palette1.rgb,opticalRole):
                                  lerp(Palette1.rgb,Palette2.rgb,opticalRole-1.);
   color=pigment/max(max(pigment.r,max(pigment.g,pigment.b)),.001);
  }
  float2 d=c3OpticalWarp((p-.5*size)/scale,Timing.x,
                       saturate(Bands.xyz),saturate(Attacks.xyz),2500.*gradient/scale);
  p=frac((p+scale*d*z)/size)*size;
  uint2 q=min(uint2(p),Dimensions.xy-1U);uint k=(q.x+q.y*Dimensions.x)*4U;
  // Scale 64 bounds worst-case steady concentration below uint32 for this grid.
  InterlockedAdd(Photons[k],uint(color.x*64.*deposit*musicalDeposit+.5));
  InterlockedAdd(Photons[k+1],uint(color.y*64.*deposit*musicalDeposit+.5));
  InterlockedAdd(Photons[k+2],uint(color.z*64.*deposit*musicalDeposit+.5));
 }
}
[numthreads(16,16,1)]void CausticsImageCS(uint3 id:SV_DispatchThreadID){
 if(!c3Music()){CausticsImageCS_Reference(id);return;}
 if(!inside(id.xy))return;uint k=(id.x+id.y*Dimensions.x)*4U;
 uint3 n=uint3(Photons[k],Photons[k+1],Photons[k+2]);
 float3 counts=float3(n)/64.;
 float3 radiance;
 if((Dimensions.w&2U)!=0U){
  float y=dot(counts,float3(.2126,.7152,.0722));
  radiance=counts*sqrt(max(y,0.))*.36/5000.;
  radiance/=1.+max(radiance.r,max(radiance.g,radiance.b));
  // Submerged ambient light keeps the water visible between focused bundles.
  float3 waterTint=lerp(Palette0.rgb,Palette1.rgb,.20);
  waterTint/=max(max(waterTint.r,max(waterTint.g,waterTint.b)),.001);
  radiance+=waterTint*(.010+.008*saturate(y/120.));
 }else{
  radiance=counts*sqrt(max(counts,0.))/5000.*float3(.5,.75,1.);
  radiance/=1.+.50*dot(radiance,float3(.2126,.7152,.0722));
 }
 ImageOut[id.xy]=float4(max(radiance,0.),0.);
 Photons[k]=uint(float(n.x)*.9);Photons[k+1]=uint(float(n.y)*.9);Photons[k+2]=uint(float(n.z)*.9);
}
[numthreads(16,16,1)]void WaveInitCS(uint3 id:SV_DispatchThreadID){
 if(!c3Music()){WaveInitCS_Reference(id);return;}
 if(!inside(id.xy))return;
 // No amplitude-4 initial impulses obscuring the response to the actual music.
 StateOut[id.xy]=0;ImageOut[id.xy]=0;
}
[numthreads(16,16,1)]void WaveStepCS(uint3 id:SV_DispatchThreadID){
 if(!c3Music()){WaveStepCS_Reference(id);return;}
 if(!inside(id.xy))return;uint2 p=id.xy;
 if(any(p==0U)||any(p==Dimensions.xy-1U)){StateOut[p]=0;return;}
 int2 i=int2(p);float4 a=pointA(i);
 float neighbor=pointA(i-int2(1,0)).x+pointA(i+int2(1,0)).x+
                pointA(i-int2(0,1)).x+pointA(i+int2(0,1)).x;
 float scale=float(min(Dimensions.x,Dimensions.y));
 float lap=(neighbor-4.*a.x)*scale*scale;
 float2 pos=(float2(p)+.5-.5*float2(Dimensions.xy))/scale;
 float force=c3WaveExcitation(pos,Timing.x,saturate(Bands.xyz),saturate(Attacks.xyz));
 float2 next=c3WaveStepState(a.x,a.y,lap,force,Timing.y);
 // Numeric escape guard, not normalization; native probes count any hits.
 bool escaped=any(abs(next)>float2(20.,160.));
 next=clamp(next,float2(-20.,-160.),float2(20.,160.));
 StateOut[p]=float4(next,max(a.z,escaped?1.:0.),0.);
}
[numthreads(16,16,1)]void WaveImageCS(uint3 id:SV_DispatchThreadID){
 if(!c3Music()){WaveImageCS_Reference(id);return;}
 if(!inside(id.xy))return;int2 p=int2(id.x,Dimensions.y-1U-id.y);
 float4 a=pointA(p);
 float2 gradient=.5*float2(pointA(p+int2(1,0)).x-pointA(p-int2(1,0)).x,
                          pointA(p+int2(0,1)).x-pointA(p-int2(0,1)).x);
 // Normalize gradient display to 288-cell short edge, independent of test grid.
 gradient*=float(min(Dimensions.x,Dimensions.y))/288.;
 ImageOut[id.xy]=float4(c3WaveRadiance(a.x,a.y,gradient),a.x);
}
[numthreads(16,16,1)]void DisplayCS(uint3 id:SV_DispatchThreadID){
 if((Dimensions.w&11U)==11U){
  if(!inside(id.xy))return;
  float3 pigment=RawImage.Load(int3(id.xy,0)).rgb;
  ImageOut[id.xy]=float4(saturate(encodeSRGB(pigment)),1.);return;
 }
 DisplayCS_Reference(id);
}
#endif
