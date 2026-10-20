#ifndef GODEN_CLOVIS_MATERIAL
#define GODEN_CLOVIS_MATERIAL
// C9: eight broad apertures ABOVE the surface. Linear-light energy, no camera
// impulses and no luminous floor. Original Alloy remains in a4_common.hlsli.
float3 clovisMaterialColor(float3 source,float lightness){
 float y=max(a4luma(source),0.0);
 float3 c=y>.0001?source/y:float3(1,1,1);
 return max(c,0.0)*(lightness/max(1.0,max(c.r,max(c.g,c.b))*lightness/.88));
}
float clovisLane(float4 v[2],int i){return v[i/4][i%4];}
// Returns emitted aperture energy and adjacent shadow energy. Both broad and
// mini lanes use this EXACT V1 transfer. Width/axis are supplied by their geometry.
float2 clovisBandResponse(float axis,float width,float direction,float y,
                          float e,float spectral,float master){
 float q=axis/max(width,.000001);
 float beam=exp(-q*q*.70);
 float shadowQ=(axis-direction*width*.77)/max(width*1.04,.000001);
 float shadow=exp(-shadowQ*shadowQ*1.1);
 float dose=master*(.015+1.9*e*e)*(1.0+.85*spectral);
 float falloff=(.95-.19*saturate(y))*(.65+.35*a4smooth(-.10,.50,y));
 return float2(max(beam-.28*shadow,0.0)*dose*falloff,shadow*dose*.07);
}
// RGB are coefficients of the three artwork pigments; A is adjacent shadow.
float4 clovisApertures(float2 uv,A4Params v){
 float aspect=max(v.viewport.x/max(v.viewport.y,1.0),.10);
 float x=uv.x*aspect,y=uv.y+.12;
 float4 sum=float4(0,0,0,0);
 float master=max(v.lightControl.x,0.0);
 [unroll] for(int i=0;i<8;++i){
  float z=float(i),seed=frac(v.phase.w*.173+z*.618033989);
  // Integer harmonics keep continuity when the Flow phase wraps at 2*pi.
  float phase=v.phase[i%3]+6.2831853*seed;
  float e=clamp(clovisLane(v.lightLevel,i),0.0,1.5);
  // `texture` is a reserved token in the D3D9-era FXC profiles used by
  // GODEN_A4.targets (/T ps_4_0), where it names the legacy texture-sampling
  // intrinsic; declaring a local with that name is error X3000. Renamed only.
  float spectral=clamp(clovisLane(v.lightTexture,i),0.0,2.0);
  float direction=(i%2)==0?-1.0:1.0;
  float origin=aspect*((z+.5)/8.0+.028*sin(phase));
  float slope=direction*(.18+.25*seed+.09*sin(phase+1.7));
  float axis=x-origin-slope*y;
  float width=.13+.09*e+.018*spectral;
  float2 response=clovisBandResponse(axis,width,direction,y,e,spectral,master);
  float3 weights=.12+.88*(.5+.5*cos(float3(0,2.0943951,4.1887902)+z*2.399963));
  weights/=max(weights.x+weights.y+weights.z,.001);
  sum.rgb+=weights*response.x;
  sum.a+=response.y;
 }
 return sum;
}
float3 clovisLight(float2 uv,A4Params v){return clovisApertures(uv,v).rgb;}
float3 clovisField(float2 uv,A4Params v){
 float aspect=v.viewport.x/max(v.viewport.y,1.0);
 float2 p=float2(uv.x*aspect,uv.y);
 float diagonal=dot(p,float2(.86,-.51));
 float seed=v.phase.w,rough=clamp(v.material.x,.12,.70);
 float relief=.12*sin(2.4*diagonal+seed)+.07*sin(5.1*diagonal-seed*.7);
 float reflectance=pow(saturate(.58+relief),lerp(2.6,1.3,rough));
 float3 low=clovisMaterialColor(v.color0.rgb,.21);
 float3 mid=clovisMaterialColor(v.color1.rgb,.29);
 float3 high=clovisMaterialColor(v.color2.rgb,.44);
 float4 light=clovisApertures(uv,v);
 float master=max(v.lightControl.x,0.0);
 float3 base=lerp(low,mid,a4smooth(-.3,.95,diagonal));
 float3 rgb=(low*light.x+mid*light.y+high*light.z)*( .70+.55*reflectance);
 rgb+=base*(.045*master)*( .35+reflectance);
 rgb*=exp(-min(light.a,.75));
 // Static near-black reflectance is <0.0002 linear; the prior coloured floor is
 // deliberately absent. Text and artwork are NOT attenuated by this control.
 rgb+=clovisMaterialColor(v.color3.rgb,.00010);
 return a4shoulder(max(rgb,0.0),.38);
}
// Evaluated once at native output pixels, after sampling the low-resolution
// optical field. This keeps tactile detail crisp without 8 apertures per pixel.
float3 clovisMicrostructure(float3 rgb,float2 uv,A4Params v){
 float2 pixel=uv*v.outputSize.xy;
 float angle=clamp(v.pointer.z,-1.5708,1.5708)*.12;
 float2 along=float2(.86,-.51)+angle*float2(.51,.86);
 float2 across=float2(-along.y,along.x);
 float fine=a4hash(floor(float2(dot(pixel,along)*.36,dot(pixel,across)*.75)),v.phase.w);
 float secondary=a4hash(floor(pixel*.53),v.phase.w+23.0);
 float grain=.62*(fine-.5)+.38*(secondary-.5);// renamed from `texture`: reserved in ps_4_0
 return max(rgb*(1.0+grain*(.08+.09*(1.0-clamp(v.material.x,.12,.70)))),0.0);
}

#endif
