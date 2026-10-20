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
    // viewport size in pixels (width, height)
          // cumulative pixel offset from click-and-drag
       // linear scroll from mouse wheel; use exp(u_scroll) for zoom
 // https://soundcloud.com/charmlink/ryong-victor-espasandin

// Credit: @MilkDrop2077 (https://fragcoord.xyz/u/MilkDrop2077)
// Original shader: https://fragcoord.xyz/s/ao6dc217

// SPDX-License-Identifier: CC0-1.0
// Copyright (c) Public domain
//[LICENSE] https://creativecommons.org/publicdomain/zero/1.0/

/*
    "FFT Terrain"
    Renders the full spectrogram as a 3D height-field landscape,
    viewed from above and to the side.
    Use mouse click/drag and scroll to zoom

    Axes:
      World X (-4..4) → frequency bin, mirrored about 0 (bass at center)
      World Z (0..8)  → time history  (now → past)
      World Y         → FFT magnitude (terrain height)

    u_audio: 512×512  R=weighted FFT, G=waveform, B=raw FFT

    Song: ryong & victor espasandín - meaningless duet
    other songs to try:
        https://soundcloud.com/inmytalonsishapeclay/you-ok-never
        https://soundcloud.com/rapallosky/summer-walker-unloyal-rapallo-ijji-remix
        https://soundcloud.com/moapillar/boots-of-blinding-speed-2

*/

// Random | Dave Hoskins (Shadertoy)
//[SNIPPET] https://fragcoord.xyz/snippet/5e2620d5-6000-4a00-8000-ff8240d536468000
float rand1(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void goden_source_main()
{
    vec2 uv = (gl_FragCoord.xy - 0.5 * u_resolution) / u_resolution.y;
    vec2 drag = u_drag / u_resolution;
    vec3 col = vec3(0);

    // Orbit camera: fixed at base angles, drag to offset
    float theta = 1.5 + drag.x * 3.0;
    float phi   = clamp(1.5 + drag.y * 3.0, 0.1, 1.5);
    float r     = max(11.-exp(u_scroll), 4.);

    vec3 ta = vec3(0.0, 0.8, 2.0);
    vec3 ro = ta + r * vec3(sin(phi) * sin(theta),
                            cos(phi),
                           -sin(phi) * cos(theta));

    vec3 ww = normalize(ta - ro),
         uu = normalize(cross(ww, vec3(0, 1, 0))),
         vv = cross(uu, ww),
         rd = normalize(uv.x * uu + uv.y * vv + 2.0 * ww);

    // Jitter starting t by a per-pixel hash to break up banding into noise
    float t = 1.5 + .1*rand1(gl_FragCoord.xy);
    for (float i = 0.; i < 150.; i++) {
         vec3 p = ro + t * rd;

        // Mirror X about 0: both sides sample the same frequency spectrum
        float fx = (abs(p.x)) / 8.0 ,   // x ∈ [-4..4] → freq ∈ [0..1]
              tz = p.z / 13.;
        // Only accumulate glow inside the terrain XZ slab; step fast outside
        if (fx > 1.0 || tz < 0.0 || tz > 1.0) {
            t += 0.12;
        } else {
            float fft = g_audioSample( vec2(fx, tz)).b;
            float h    = fft * 2.;
            float surf = p.y - h;

            if (surf > .02) {
                // Glow: color / |dist_to_surface|^2 / depth
                col += (cos(h * 4. +1.+ vec3(0, 2, 4))*.5+.5)
                        * (fft + 0.001)
                        / pow(abs(surf) + 0.1, 2.)
                        / (1.+t*1.5);
            }
            t += max(abs(surf) * 0.09, 0.0001);
        }

        if (t > 26.0) break;
    }

    fragColor = vec4(tanh(col / 55.0), 1);    
}
void main(){fragColor=vec4(0);goden_source_main();GodenOutput=vec4(clamp(fragColor.rgb,vec3(0),vec3(1)),1.0);}
