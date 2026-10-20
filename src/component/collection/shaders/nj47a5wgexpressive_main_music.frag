#version 450 core
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
float c8_nebulaClock();
#define u_time c8_nebulaClock()
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

// C8: field clock accelerates with sustained multi-band motion. Illumination
// coefficients are untouched. Integrals remain continuous across musical drops.
float c8_nebulaClock(){
    float t=g_size.z;
    vec3 area=max(vec3(0),(g_motion.xyz-vec3(.35,.45,.65)*t)/vec3(.45,.40,.55));
    return .52*t+.52*area.x+.83*area.y+.62*area.z;
}



#define GODEN_VARIANT_MUSIC 1







// Tempestade Expressiva: independent spectra share a single warped volume.
// Preserve differences above unity instead of flattening loud bands at 1.4.
vec2 storm_shoulder(vec2 x){
 vec2 excess=max(x-1.0,vec2(0));
 return min(x,vec2(1.0))+excess/(1.0+2.5*excess);
}
vec2 storm_band(float coordinate,uint first,uint last){
 float band=clamp(coordinate,float(first),float(last));
 uint a=uint(band),b=min(a+1u,last);
 float f=band-float(a);
 // Nonnegative indices avoid SM5 signed divide/modulo fixups at every tap.
 vec2 level=max(vec2(g_levels[a>>2u][a&3u],g_levels[b>>2u][b&3u]),vec2(0));
 vec2 relative=vec2(g_relatives[a>>2u][a&3u],g_relatives[b>>2u][b&3u]);
 vec2 hit=vec2(g_onsets[a>>2u][a&3u],g_onsets[b>>2u][b&3u]);
 level=max(vec2(0),level+.22*relative*sqrt(level));
 // Interpolate the neighbouring signals before their smooth, C1 shoulder.
 // Two rational evaluations replace four exponentials at each group/sample.
 return storm_shoulder(g_options.x*vec2(mix(level.x,level.y,f),
                                       max(0.0,mix(hit.x,hit.y,f))));
}

vec3 storm_palette(float phase){
 float role=clamp((phase-.25)/1.975,0.0,2.0);uint a=uint(role),b=min(a+1u,2u);
 return mix(g_palette[a].rgb,g_palette[b].rgb,role-float(a));
}
// Same authored-luminance and linear-album transfer as g_material. The three
// convex palette mixtures are invariant along the ray, so form them once.
vec3 storm_material(vec3 authored,vec3 pigment){
 if(g_options.w<.5)return authored;
 // Pigment chooses hue; light chooses luminance. A dark cover must not dim
 // every emitter through a capped gain. This is fixed material calibration,
 // independent of the track's loudness or its recent peak.
 float peak=max(pigment.r,max(pigment.g,pigment.b));
 vec3 hue=peak>1e-7?g_encoded(pigment/peak):vec3(1);
 return hue*(dot(authored,vec3(.2126,.7152,.0722))/max(dot(hue,vec3(.2126,.7152,.0722)),1e-5));
}

void goden_source_main(){
 float clock=c8_nebulaClock();
 vec3 groups=clamp(g_options.x*g_bands.xyz,vec3(0),vec3(1.4));
 // Large scales remain calm. Fine scales retain sustained musical motion.
 const float base[6]=float[6](.72,.68,.64,.60,.50,.42);
 const float drive[6]=float[6](.08,.08,.22,.22,.20,.18);
 float amplitude[6],phase[6];
 for(int oct=0;oct<6;++oct){
  float e=mix(c7_level(3.0+11.0*float(oct)),groups[oct/2],.35);
  amplitude[oct]=base[oct]+drive[oct]*e;
  phase[oct]=g_motion[oct%3]*(.12+.018*float(oct));
 }
 vec3 lowPigment=storm_palette(.25),midPigment=storm_palette(2.10),highPigment=storm_palette(4.20);
 vec3 ray=normalize(gl_FragCoord.rgb*2.0-vec3(u_resolution,1).xyy);
 float pixelAngle=2.0/max(u_resolution.y,1.0);
 vec3 total=vec3(0);float z=0.0;
 for(int i=0;i<100;++i){
  float sampleDepth=z,footprint=max(sampleDepth*pixelAngle,1e-5);
  vec3 p=z*ray;p.z-=.4*clock;
  float foldShape=0.0,fineShape=0.0;
  for(int oct=0;oct<6;++oct){
   float freq=exp2(float(oct));
   float detail=oct<4?1.0:1.0-smoothstep(.25,.50,footprint*freq*.15915494);
   vec3 wave=cos(p.yzx*freq+phase[oct]);
   if(oct==2||oct==3)foldShape+=.5*wave.x;
   if(oct==4||oct==5)fineShape+=.5*detail*wave.y;
   p+=amplitude[oct]*detail*wave/freq;
  }
  p.xy*=mat2(cos(z*.2+vec4(0,11,33,0)));
  // Read three different material coordinates BEFORE any attack displacement.
  // No plateau, cross-group index blend, or onset-driven emitter relocation.
  vec3 coordinate=.5+.5*sin(vec3(p.z*.37+p.y*.24,
                               p.z*.29-p.x*.22+1.7,
                               p.z*.26+p.x*.46+p.y*.38+3.4));
  vec2 low=storm_band(14.0*coordinate.x,0u,14u);
  vec2 mid=storm_band(15.0+27.0*coordinate.y,15u,42u);
  vec2 high=storm_band(43.0+20.0*coordinate.z,43u,63u);
  vec3 sustain=vec3(low.x,mid.x,high.x)/1.4;
  vec3 attack=vec3(low.y,mid.y,high.y)/1.4;
  float width=3.0*(1.0+.08*(groups.x/1.4)*sin(p.z*.19+g_motion.x*.08));
  float width1=.58*.90*width,width2=.44*.85*width;
  float meander=.90*sin(p.z*.23+clock*.05)+.45*sin(p.z*.11-clock*.037);
  // Each basis has L1 norm <= 1: sustained pressure <= 15%, hits <= 5%.
  // The existing warped folds supply surface response; no extra noise field.
  vec3 basis1=vec3(.50,.28*foldShape,.22*fineShape);
  vec3 basis2=vec3(.45,-.28*foldShape,-.27*fineShape);
  float pressure1=.15*dot(basis1,sustain)+.05*dot(basis1,attack);
  float pressure2=.15*dot(basis2,sustain)+.05*dot(basis2,attack);
  float d1=width1*(1.0+pressure1)-abs(p.x-.30*p.y-(.85+.5*meander));
  float d2=width2*(1.0+pressure2)-abs(p.x+.34*p.y+.95+.5*meander);
  float signedDist=max(d1,d2);
  float sheet=smoothstep(-.35,.35,d1-d2);
  float stepD=.03+.1*max(signedDist,-signedDist*.2);z+=stepD;

  float ribbonWidth=max(.045,1.5*footprint)*(1.0+.35*attack.z);
  float rim1=1.0-smoothstep(ribbonWidth,2.8*ribbonWidth,abs(d1));
  float rim2=1.0-smoothstep(ribbonWidth,2.8*ribbonWidth,abs(d2));
  float filament=mix(rim2,rim1,sheet);
  float body=mix(smoothstep(.10,.65,d2),smoothstep(.10,.65,d1),sheet);
  float folds=1.0-abs(foldShape);
  // Preserve the quiet material, but give musical light distinct supports.
  // Bass occupies the bodies, mids the folds, and highs the rims/fine crests.
  // The other emitters recede at a rim instead of washing out its transient.
  vec3 ambientRole=vec3(.28+.72*body,.45+.55*folds,.28+.72*filament);
  ambientRole/=dot(ambientRole,vec3(1));
  float crease=folds*folds;
  float crest=smoothstep(.35,.75,abs(fineShape))*body;
  vec3 role=vec3((.16+.84*body)*(1.0-.72*filament)*(1.0-.65*crease),
                 (.12+.88*crease)*(1.0-.58*filament),
                 .10+.90*filament+.30*crest);
  role/=dot(role,vec3(1));
  vec3 light=ambientRole*vec3(.15,.13,.08)+role*vec3(
      1.55*c7_exposure(low.x)+1.10*low.y,
      1.70*c7_exposure(mid.x)+1.45*mid.y,
      3.65*c7_exposure(high.x)+5.00*high.y*(.40+.90*filament));
  float power=dot(light,vec3(1));
  vec3 lightMix=light/max(power,1e-5);
  float colorPhase=dot(lightMix,vec3(-.45,.12,.75));
  vec3 authored=cos(signedDist*2.0+colorPhase-vec3(5,0,1))+1.4;
  vec3 pigment=lightMix.x*lowPigment+lightMix.y*midPigment+lightMix.z*highPigment;
  vec3 emission=storm_material(authored,pigment);
  float channel=1.0-smoothstep(.15,.85,.55*abs(d1-d2));
  // Fixed compensation preserves ambient material after clearing foreground.
  // It never adapts exposure to a song, and does not move or zoom the camera.
  float nearWeight=smoothstep(1.0,3.0,sampleDepth);
  total+=1.50*emission*power*nearWeight*(1.0-.75*channel)/(stepD*max(z,.01));
 }
 // Preserve quadratic punch; compress all channels with the same bounded
 // shoulder, so the terminal safety clamp cannot turn peaks flat white.
 vec3 scaled=total/632.4555;
 vec3 radiance=scaled*scaled;
 float shoulder=1.0+max(radiance.r,max(radiance.g,radiance.b));
 vec3 color=radiance/shoulder;
 if(g_options.w>.5){
  // Quadratic energy and one shared shoulder retain deep gaps and bright
  // local rims without independently squaring or clipping pigment channels.
  radiance=scaled*dot(scaled,vec3(.2126,.7152,.0722));
  color=radiance/(1.0+max(radiance.r,max(radiance.g,radiance.b)));
 }
 fragColor=vec4(color,1);
}
void main(){fragColor=vec4(0);goden_source_main();GodenOutput=vec4(clamp(fragColor.rgb,vec3(0),vec3(1)),1.0);}
