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
         // seconds since playback started

// Rainbow Travel
// By Noztol
// Based on https://fragcoord.xyz/s/fx196wc1 
// but using XorDev's color scheme 


// Calculates the base winding 3D path of the tunnel
vec3 getPathPosition(float z) {
    return vec3(12.0 * cos(z * vec2(0.1, 0.12)), z);
}

void goden_source_main() {
  vec2 fragCoord = gl_FragCoord.xy;

    // Normalize screen coordinates
    vec2 uv = (fragCoord - u_resolution.xy * 0.5) / u_resolution.y;
    
    // Time variables for animation
    float animTime = u_time * 4.0 + 5.0 + 5.0 * sin(u_time * 0.3);
    
    // Setup Camera
    vec3 rayOrigin = getPathPosition(animTime);
    vec3 lookTarget = getPathPosition(animTime + 4.0);
    
    vec3 forward = normalize(lookTarget - rayOrigin);
    vec3 right = normalize(vec3(-forward.z, 0.0, forward.x)); 
    vec3 up = cross(forward, right);
    vec3 rayDir = normalize(uv.x * right + uv.y * up + forward);

    // Initializations
    float stepDist = 1.0; 
    float totalDist = 0.0;
    float orbDist = 1.0;  
    vec3 accumulatedColor = vec3(0.0);
    vec3 rayPos = rayOrigin; 

    // Raymarching Loop
    for (float i = 1.0; i <= 28.0; i++) {
        if (totalDist >= 30.0) break;
        
        // 1. March Ray Forward
        rayPos += rayDir * stepDist;
        
        // 2. Get path center and time vars
        vec3 pathCenter = getPathPosition(rayPos.z);
        float sineTime = sin(u_time);
        
        // 3. Orb Geometry
        vec3 orbCenter = vec3(
            pathCenter.x + sineTime,
            pathCenter.y + sineTime * 2.0,
            6.0 + animTime + sineTime * 2.0
        );
        orbDist = length(rayPos - orbCenter) - 0.01;
        
        // 4. Base Tunnel Structure 
        float baseRadius = cos(rayPos.z * 0.6) * 2.0 + 4.0;
        
        float tunnelStructure = min(
            length(rayPos.xy - pathCenter.x - 6.0),
            length((rayPos - pathCenter).xy)
        );
        
        // Crisp architectural scoops
        float largeScoops = abs(dot(sin(0.4 * rayPos), vec3(0.25))) / 0.1;
        float detailTexture = abs(dot(sin(animTime + 16.0 * rayPos), vec3(0.22))) / 2.0;
        
        float carvedDist = baseRadius - tunnelStructure + largeScoops + detailTexture;
        
        // 5. FIREWALL FLUIDITY
        vec3 fluidPos = rayPos;
        for (float j = 1.0; j <= 7.0; j++) {
            fluidPos += sin(fluidPos.yzx * j + u_time + 0.5 * i) / j;
        }
        
        float fluidTunnelDist = 0.4 * length(vec4(0.3 * cos(fluidPos) - 0.3, carvedDist));
        
        // 6. Update Distances
        stepDist = max(.0001, min(orbDist, fluidTunnelDist));
        totalDist += stepDist;
        
        // 7. Accumulate Color 
        vec3 palette = 1.0 + cos(fluidPos.y + i * 0.4 + vec3(6.0, 1.0, 2.0));
        
        // BRIGHTNESS FIX: Multiplied the wall lighting (palette / stepDist) by 2.5
        // This makes the geometry glow much brighter against the dark carved-out holes
        accumulatedColor += (2.5 * palette / stepDist + 10.0 * palette / max(orbDist, 0.6)) / i;
    }
    
    // Tonemapping
    // Lowered divisor slightly to 1500.0 to raise the overall exposure
    fragColor = vec4(tanh(accumulatedColor * accumulatedColor / 1500.0), 1.0);
}
void main(){fragColor=vec4(0);goden_source_main();GodenOutput=vec4(clamp(fragColor.rgb,vec3(0),vec3(1)),1.0);}
