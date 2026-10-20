#version 450 core
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

//Realtime 2D Global Illumination using Radiance Cascades
//Ported from my old shadertoy : https://www.shadertoy.com/view/3cKczD
//Holographic version : https://fragcoord.xyz/s/hrouahvq

//Finally we have a way to do unlimited passes !
//The other beautiful thing here is you can visualize each cascade individually and see how it converges to the final result
//Cascades are computed from CascadeN to Cascade0

//Cascade 0
void goden_source_main()
{
    fragColor = texelFetch(u_pass2, ivec2(gl_FragCoord.xy), 0);
    vec4 scene=texelFetch(u_pass1,ivec2(gl_FragCoord.xy),0);
    float surface=1.0-smoothstep(.25,1.5,max(scene.a,0.0)*max(u_resolution.x,u_resolution.y));
    // Keep radiating cores distinct from their broad indirect illumination.
    vec3 linearRadiance=mix(max(fragColor.rgb,vec3(0))*.42,max(scene.rgb,vec3(0))*1.25,surface);
    float peak=max(linearRadiance.r,max(linearRadiance.g,linearRadiance.b));
    float shoulder=1.0-exp(-1.10*peak);
    // Map intensity once instead of compressing RGB channels independently;
    // this retains the album pigment in bright accumulated GI.
    fragColor.rgb=g_encoded(linearRadiance*(shoulder/max(peak,1e-6)));
    //fragColor = sqrt(fragColor);
}
void main(){fragColor=vec4(0);goden_source_main();GodenOutput=vec4(clamp(fragColor.rgb,vec3(0),vec3(1)),1.0);}
