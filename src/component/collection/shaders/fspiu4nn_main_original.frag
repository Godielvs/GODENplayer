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
         // seconds since playback started
   // seconds since previous frame
         // xy: mouse pos, zw: click pos (negative when not pressed)

const float pi = 3.14159265358;
const float tau = 6.28318530718;

const int METHOD = 0;
const int SCENE = 2;
const float VORTICITY = 0.07; // 0.07

#define DAMP(curr_pass) mix(texelFetch(curr_pass, fc, 0).x, phi, 0.8)

// clamp
// #define FETCH(sampler, coord) texelFetch(sampler, clamp(coord, ivec2(0),  - 1), 0)

// repeat
#define FETCH(sampler, coord) texelFetch(sampler, (coord % textureSize(sampler, 0) + textureSize(sampler, 0)) % textureSize(sampler, 0), 0)
 //[nomipmap]
 //Pass resolution in pixels

#define LAST_PASS u_pass7
float hash12(vec2 p) {
	vec3 p3 = fract(p.xyx / 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = p - i;
	f *= f * (3.0 - 2.0 * f);
	float res = mix(
		mix(hash12(i), hash12(i + vec2(1, 0)), f.x),
		mix(hash12(i + vec2(0, 1)), hash12(i + vec2(1)), f.x), f.y);
	return res;	
}
void goden_source_main() {
    ivec2 fc = ivec2(gl_FragCoord);
    ivec3 h = ivec3(1, -1, 0);

    vec4 c = texelFetch(LAST_PASS, fc, 0);

    vec4 l = FETCH(LAST_PASS, fc + h.yz);
    vec4 r = FETCH(LAST_PASS, fc + h.xz);
    vec4 d = FETCH(LAST_PASS, fc + h.zy);
    vec4 u = FETCH(LAST_PASS, fc + h.zx);

    vec2 uv = gl_FragCoord.xy / u_resolution;
    int view = int(round(uv.y)) * 2 + int(round(uv.x));
    view = 0;

    if (view == 0) { // density view (-X -Y)
        float x = c.z;
        float v = length(c.xy);
        c.xyz = vec3(x * x, x * x, x * x * x) * mat3(1,2,-2,0.2,0.1,0.7,3.8,-8.9,5.8);
        c.xyz = log(mix(exp(c.xyx), exp(c.xxy), noise(noise(vec2(0, v) - uv + u_time * 0.5) + uv * 3.0 - u_time * 0.5))) * mat3(1,2,-2,0.2,0.1,0.7,3.8,-8.9,5.8);
        c.xyz = log2(max(c.xyz * 3.0 + 1.0,vec3(1e-5))) * 0.5;
        c.r = log(max(c.r * 2.0 + 1.0,1e-5)) * 0.91;
        c.xyz = log2(max(3.0 * c.xyz + 1.0,vec3(1e-5))) * 0.5;
    }
    else if (view == 1) { // redisual view (+X -Y, red)
        float div_vel = ((r.x - l.x) + (u.y - d.y)) / 2.0;
        float lap_phi = l.w + r.w + d.w + u.w - 4.0 * c.w;
        float res = div_vel - lap_phi;
        float res_test = log2(1.0 + abs(res) * 6.0);
        c.xyz = vec3(step(0.0, res) * res_test, res_test * 0.5, step(res, 0.0) * res_test);
        c.xyz = mix(c.xyz, vec3(1, 0, 0), step(u_resolution.x - 8.0, gl_FragCoord.x) * step(gl_FragCoord.y, 8.0));
    }
    else if (view == 2) { // pressure view (-X +Y, green)
        float p = log2(1.0 + abs(c.w) * 0.1);
        c.xyz = vec3(step(0.0, c.w) * p, abs(p) * 0.5, step(c.w, 0.0) * p);
        c.xyz = mix(c.xyz, vec3(0, 1, 0), step(gl_FragCoord.x, 8.0) * step(u_resolution.y - gl_FragCoord.y, 8.0));
    }
    else if (view == 3) { // divergence view (+X +Y, blue)
        float div_vel = ((r.x - l.x) + (u.y - d.y)) / 2.0;
        float div_vel_test = log2(1.0 + abs(div_vel) * 16.0);
        c.xyz = vec3(div_vel_test * step(div_vel, 0.0), div_vel_test * 0.5, div_vel_test * step(0.0, div_vel));
        c.xyz = mix(c.xyz, vec3(0, 0, 1), step(u_resolution.x - 8.0, gl_FragCoord.x) * step(u_resolution.y - 8.0, gl_FragCoord.y));
    }
    else if (view == 4) { // velocity view (magenta)
        vec2 s = sign(c.xy);
        vec2 a = s * c.xy;
        c.xyz = vec3(a, -dot(a, step(a.yx, a.xy) * sign(c.xy))) * 0.5;
        c.xyz = mix(c.xyz, vec3(1, 0, 1), step(gl_FragCoord.x, 8.0) * step(gl_FragCoord.y, 8.0));
    }

    fragColor = c;
}
void main(){fragColor=vec4(0);goden_source_main();GodenOutput=vec4(clamp(fragColor.rgb,vec3(0),vec3(1)),1.0);}
