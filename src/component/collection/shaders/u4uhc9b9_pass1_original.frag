#version 450 core
// GODEN C4 canonical ABI. Texture row zero is logical bottom for these ports.
// D3D final presentation flips Y exactly once; feedback buffers never flip.
#ifndef GODEN_MUSIC
#define GODEN_MUSIC 0
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
float g_energy(float x){return clamp(g_options.x*(g_band(x)+.7*g_hit(x)),0.0,1.8);}
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

#define GODEN_VARIANT_ORIGINAL 1
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



vec4 sdf(in vec2 p)
{
    vec4 s = vec4(0.0, 0.0, 0.0, 100.0);

    #if SDF_INDEX == 0
        vec4 occ = vec4(vec3(0.0), sdCircle(p + vec2(0.15, 0.0), 0.01));
        vec4 light = vec4(vec3(1.0), sdCircle(p + vec2(-0.1, sin(u_time) * 0.1), sin(u_time * 0.4) * 0.015 + 0.02));
        s = opU(light, occ);
    #elif SDF_INDEX == 2
        for(float i = 1.0; i < RING_COUNT+1.0; i++)
        {
            float ri = floor((i-1.0) * 0.5)*2.0;
            vec3 color = mod(i, 2.0) == 0.0 ? hsv2rgb(vec3(ri / RING_COUNT + u_time*0.1, 1.0, 1.0)) : vec3(0.0);
            float range = (hash(i) * 0.75 + 0.25) * 2.5;
            range = (sin(u_time + hash(i * 11.0)) * 0.45 + 0.55) * range;
            float radius = (i / RING_COUNT) * RING_MAX_RADIUS;
            float rotation = u_time * (0.6 + hash(i*4.0) * 0.4) * (mod(i, 2.0) == 0.0 ? 1.0 : -1.0) + hash(i*5.0) * 3.14;
            float thickness = (hash(i * 8.0) * 0.75 + 0.25) * ARC_MAX_RADIUS;
            
            vec4 r = vec4(color, sdArc(rotate(p, rotation), vec2(sin(range), cos(range)), radius, thickness));
            
            s = opU(s, r);
        }
    #endif
    
    return s;
}

void goden_source_main()
{
    vec2 uv = (gl_FragCoord.xy - u_resolution.xy*0.5) / max(u_resolution.x, u_resolution.y);
    fragColor = sdf(uv);
}
void main(){fragColor=vec4(0);goden_source_main();GodenOutput=fragColor;}
