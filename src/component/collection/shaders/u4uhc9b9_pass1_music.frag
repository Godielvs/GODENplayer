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
float c7_exposure(float e){e=max(e,0.0);return e*e*(.22+1.68*e);} // C8 emitter contrast; not post-tonemapping
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

float c8_radianceSpin(){return .24*g_motion.y+.10*g_motion.z;}



#define GODEN_VARIANT_MUSIC 1
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 @youssef_afella
//[LICENSE] https://opensource.org/licenses/MIT





// SPDX-License-Identifier: MIT
// Copyright (c) 2026 @youssef_afella
//[LICENSE] https://opensource.org/licenses/MIT


// from Common by FragCoord
#define PI         3.14159265359
#define TAU        6.28318530718
#define HPI        1.57079632679
#define SQRT_HALF  0.70710678118
#define SQRT2      1.41421356237
#define SQRT_THIRD 0.57735026919

#define CASCADE_COUNT 6

vec4 RayMarch(vec2 rayOrigin, vec2 rayDirection, vec2 rayRange){

    vec4 hit = vec4(0.0, 0.0, 0.0, 1.0);
    float t = rayRange.x;

    for (int i = 0; i < 32; i++)
    {
        vec2 p = rayOrigin + t * rayDirection;
        if (t > rayRange.y || p.x < 0.0 || p.y < 0.0 || p.x > u_resolution.x || p.y > u_resolution.y) break;

        vec4 s = texture(u_pass1, p / u_resolution);
        s.a *= max(u_resolution.x, u_resolution.y);

        if (s.a < 1.0)
        {
            hit = vec4(s.rgb, 0.0);
            break;
        }

        t += s.a;
    }

    return hit;
}

vec2 computeRayRange(int index)
{
    int start = (1 << (index*2)) - 1;
    int end = (1 << (index*2 + 2)) - 1;

    return vec2(start, end) * 0.5;
}
            
vec4 CastAndMerge(vec2 p, int cascadeLevel, sampler2D higherCascade)
{
    vec4 finalResult = vec4(0.0);

    float blockSqrtCount = float(1 << cascadeLevel);
    vec2 blockDim = u_resolution / blockSqrtCount;
    vec2 block2DIndex = floor(p / blockDim);
    float blockIndex = block2DIndex.x + block2DIndex.y * blockSqrtCount;

    vec2 coordsInBlock = floor(mod(p, blockDim));

    vec2 rayOrigin = (coordsInBlock + vec2(0.5)) * blockSqrtCount;
    vec2 rayRange = computeRayRange(cascadeLevel);

    for (float i = 0.0; i < 4.0; i++)
    {
        float angleIndex = blockIndex * 4.0 + i;
        float angleStep = TAU / (blockSqrtCount * blockSqrtCount * 4.0);
        float angle = (angleIndex + 0.5) * angleStep;

        vec2 rayDirection = vec2(cos(angle), sin(angle));

        vec4 radiance = RayMarch(rayOrigin, rayDirection, rayRange);

        if (cascadeLevel < CASCADE_COUNT-1){//if we are at the highest cascade we don't merge
        
            if(radiance.a != 0.0){//if we already hit something we don't merge
            
                //Merging
                vec2 position = coordsInBlock * 0.5 + 0.25;
                vec2 positionOffset = floor(vec2(mod(angleIndex, blockSqrtCount * 2.0), angleIndex / (blockSqrtCount * 2.0))) * (blockDim * 0.5);

                position.x = clamp(position.x, 0.5, blockDim.x * 0.5 - 0.5);
                position.y = clamp(position.y, 0.5, blockDim.y * 0.5 - 0.5);

                vec4 higherRadiance = texture(higherCascade, (position + positionOffset) / u_resolution);

                radiance.rgb += higherRadiance.rgb * radiance.a;
                radiance.a *= higherRadiance.a;
                
            }
        }

        finalResult += radiance * 0.25;
    }

    return finalResult;
}
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 @youssef_afella
//[LICENSE] https://opensource.org/licenses/MIT

#define SDF_INDEX 2 //cane be 0, 1, 2

//SDF 1 Params
#define FIB_COUNT 80.0
#define FIB_RADIUS 0.2
#define FIB_DISK_RADIUS 0.01

//SDF 2 Params
#define RING_COUNT 10.0
#define RING_MAX_RADIUS 0.3
#define ARC_MAX_RADIUS 0.003

float hash(float i) {
    return fract(sin(i) * 43758.5453123);
}

vec4 opU( vec4 d1, vec4 d2 ) {
    return (d1.w<d2.w) ? d1 : d2;
}
vec2 rotate(vec2 p, float r)
{
    float s = sin(r);
    float c = cos(r);
    mat2x2 m = mat2x2(c, -s, s, c);
    return p * m;
}

//From IQ : https://www.shadertoy.com/view/3tdSDj
float sdSegment( in vec2 p, in vec2 a, in vec2 b )
{
    vec2 pa = p-a, ba = b-a;
    float h = clamp( dot(pa,ba)/dot(ba,ba), 0.0, 1.0 );
    return length( pa - ba*h );
}

float sdArc(vec2 p, vec2 sc, float ra, float rb)
{
    p.x = abs(p.x);
    return ((sc.y*p.x>sc.x*p.y) ? length(p-sc*ra) : abs(length(p)-ra)) - rb;
}

float sdCircle(in vec2 p, float radius) {
    return length(p) - radius;
}

// GODENplayer: independently expressed six-sector HSV conversion (MIT).
// Hue wraps; saturation and value keep the original [0,1] caller contract.
vec3 hsv2rgb(in vec3 c)
{
    float hue = fract(c.x) * 6.0;
    int sector = int(floor(hue));
    float fraction = hue - float(sector);
    float minimum = c.z * (1.0 - c.y);
    float falling = c.z * (1.0 - c.y * fraction);
    float rising = c.z * (1.0 - c.y * (1.0 - fraction));
    if (sector == 0) return vec3(c.z, rising, minimum);
    if (sector == 1) return vec3(falling, c.z, minimum);
    if (sector == 2) return vec3(minimum, c.z, rising);
    if (sector == 3) return vec3(minimum, falling, c.z);
    if (sector == 4) return vec3(rising, minimum, c.z);
    return vec3(c.z, minimum, falling);
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

vec4 sdf(in vec2 p)
{
    vec4 s = vec4(0.0, 0.0, 0.0, 100.0);

    #if SDF_INDEX == 0
        vec4 occ = vec4(vec3(0.0), sdCircle(p + vec2(0.15, 0.0), 0.01));
        vec4 light = vec4(vec3(1.0), sdCircle(p + vec2(-0.1, sin(u_time) * 0.1), sin(u_time * 0.4) * 0.015 + 0.02));
        s = opU(light, occ);
    #elif SDF_INDEX == 2

        for(uint i=0u;i<8u;++i){
            // Music pass1 interprets bands/attacks as eight integrated phases.
            float rotation=(i<4u?g_bands[i]:g_attacks[i-4u])+float(i)*.57;
            vec2 q=rotate(p,rotation);
            float arc=1.08+.42*sin(float(i)*2.1+g_motion.y*.78);
            float sector=clamp((atan(q.x,q.y)+arc)/(2.0*arc),0.0,1.0);
            vec2 sound=v_band(float(i*8u)+7.0*sector);float e=sound.x,h=sound.y;
            // Separate radial lanes: a local note must never collide with its
            // neighbour and make the nearest-emitter choice flicker.
            float radius=.063+.047*float(i)+.0035*e;
            radius+=.0025*e*sin(atan(q.x,q.y)*3.0+float(i));
            float width=.0035+.0015*e+.0018*h;
            float shape=sdArc(q,vec2(sin(arc),cos(arc)),radius,width);
            vec3 color=g_options.w>.5?v_pigment(float(i)/3.8):
                g_linear(hsv2rgb(vec3(float(i)/8.,1.,1.)));
            color*=min(.52/max(dot(color,vec3(.2126,.7152,.0722)),.008),12.0);
            color*=.014+3.4*e*e/(1.0+.35*e)+2.1*h/(.55+h);
            // The angular modulation is not an exact SDF. A conservative
            // bound prevents cascade rays from skipping the thin inner arcs.
            s=opU(s,vec4(color,shape*.72));
        }
        for(uint j=0u;j<4u;++j){
            float e=v_group(j*16u,j*16u+15u);
            float radius=.087+.094*float(j)+.003*e*sin(g_motion[j%3u]*.6+float(j));
            // The four occluders have their own integrated phases in meta.
            float rotation=g_meta[j]+float(j)*1.7;
            float arc=.25+.24*e,thickness=.003+.002*e;
            s=opU(s,vec4(vec3(0),sdArc(rotate(p,rotation),vec2(sin(arc),cos(arc)),radius,thickness)));
        }
    #endif
    
    return s;
}

void goden_source_main()
{
    // Keep all eight spectral arcs inside the canvas, including wide windows.
    vec2 uv = (gl_FragCoord.xy-.5*u_resolution)/min(u_resolution.x,u_resolution.y);
    fragColor = sdf(uv);
    fragColor.a*=min(u_resolution.x,u_resolution.y)/max(u_resolution.x,u_resolution.y);
}
void main(){fragColor=vec4(0);goden_source_main();GodenOutput=fragColor;}
