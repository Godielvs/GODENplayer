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

// 1. Initialize State
// 2. Advect Velocities
// 3. Add External Forces
// Output: Updated Velocities (non-zero divergence), Updated Density

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

float fbm(vec2 p, int octaves) {
	float s = 0.0, m = 0.0, a = 1.0;
	for(int i = 0; i < octaves; i++) {
		s += a * noise(p);
		m += a;
		a *= 0.5;
		p *= mat2(1.6, 1.2, -1.2, 1.6);
	}
	return s / m;
}

vec3 hash33(vec3 p) {
	  p = fract(p / vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}

float noise3(vec3 p) {
	 vec3 s = floor(p + dot(p, vec3(1.0 / 3.0)));
	 vec3 x = p - s + dot(s, vec3(1.0 / 6.0));
	 vec3 e = step(vec3(0), x - x.yzx);
	 vec3 i1 = e * (1.0 - e.zxy);
	 vec3 i2 = 1.0 - e.zxy * (1.0 - e);
	 vec3 x1 = x - i1 + 1.0 / 6.0;
	 vec3 x2 = x - i2 + 1.0 / 3.0;
	 vec3 x3 = x - 0.5;
	 vec4 w = vec4(dot(x, x), dot(x1, x1), dot(x2, x2), dot(x3, x3));
	 w = max(0.6 - w, 0.0);
	 vec4 d = vec4(dot(hash33(s) - 0.5, x), 
                 dot(hash33(s + i1) - 0.5, x1),
            	   dot(hash33(s + i2) - 0.5, x2), 
                 dot(hash33(s + 1.0) - 0.5, x3));
	 w *= w;
	 w *= w;
	 d *= w;
	 return dot(d, vec4(26)) + 0.5;
}

float fbm3(vec3 p, int octaves) {
	float s = 0.0, m = 0.0, a = 1.0;
	for(int i = 0; i < octaves; i++) {
		s += a * noise3(p);
		m += a;
		a *= 0.5;
		p *= 2.0;
	}
	return s / m;
}

vec2 turbulance(vec2 uv) {
		vec2 e = vec2(0.001, 0.0);
    float n1 = fbm3(vec3(uv * 1.0, u_time * 1.0), 6);
    float nx = fbm3(vec3((uv + e.xy) * 1.0, u_time * 1.0), 6);
    float ny = fbm3(vec3((uv + e.yx) * 1.0, u_time * 1.0), 6);
    vec2 curl = vec2(ny - n1, n1 - nx) / e.x * 0.25;
    return curl;
}

void get_min_max(vec2 uv, out vec4 min_val, out vec4 max_val) {
    vec4 h = vec4(0.5, 0.5, -0.5, -0.5) / u_resolution.xyxy;
    
    // Sample the 4 immediate bilinear neighbors
    vec4 tl = textureLod(u_pass7, uv + h.zw, 0.0);
    vec4 tr = textureLod(u_pass7, uv + h.xw, 0.0);
    vec4 bl = textureLod(u_pass7, uv + h.zy, 0.0);
    vec4 br = textureLod(u_pass7, uv + h.xy, 0.0);

    min_val = min(min(tl, tr), min(bl, br));
    max_val = max(max(tl, tr), max(bl, br));
}

vec2 rk4_backtrack(vec2 uv, vec2 vel, vec2 dt) {
    // k1 = velocity at current position
    vec2 k1 = vel;
    
    // k2 = velocity at midpoint using k1
    vec2 p2 = uv - 0.5 * k1 * dt;
    vec2 k2 = textureLod(u_pass7, p2, 0.0).xy;
    
    // k3 = velocity at midpoint using k2
    vec2 p3 = uv - 0.5 * k2 * dt;
    vec2 k3 = textureLod(u_pass7, p3, 0.0).xy;
    
    // k4 = velocity at full step using k3
    vec2 p4 = uv - k3 * dt;
    vec2 k4 = textureLod(u_pass7, p4, 0.0).xy;
    
    // RK4 weighted average
    vec2 k = (k1 + 2.0 * k2 + 2.0 * k3 + k4) / 6.0;
		
		return uv - k * dt;
}

vec4 advect(vec2 uv, vec4 c, vec2 dt) {
	  if (METHOD == 0) { // BFECC
	      vec2 fwp = rk4_backtrack(uv, c.xy, dt);
		    vec4 fwd = textureLod(u_pass7, fwp, 0.0);
		    vec4 bwd = textureLod(u_pass7, rk4_backtrack(fwp, fwd.xy, -dt), 0.0);
		    vec4 error = c - bwd;
        vec4 corrected = fwd + 0.5 * error;
		    vec4 min_val, max_val;
        get_min_max(fwp, min_val, max_val);
        return clamp(corrected, min_val, max_val);
	  } else if (METHOD == 1)	{ // RK4
        return textureLod(u_pass7, rk4_backtrack(uv, c.xy, dt), 0.0);
    } else if (METHOD == 2) { // RK2
		    vec2 k1 = textureLod(u_pass7, uv - c.xy * dt, 0.0).xy;
		    return textureLod(u_pass7, uv - k1 * dt, 0.0);
	  } else { // RK1
		    return textureLod(u_pass7, uv - c.xy * dt, 0.0);
		}
}

void goden_source_main() {
		ivec2 fc = ivec2(gl_FragCoord);
    vec2 uv = gl_FragCoord.xy / u_resolution;
    vec2 nuv = (gl_FragCoord.xy * 2.0 - u_resolution) / min(u_resolution.x, u_resolution.y);
		vec2 dt = 200.0 / u_resolution * max(0.005, u_time_delta);

    // sample current pixel
    vec4 c = texelFetch(u_pass7, fc, 0);

    // advect prev -> curr
		vec4 prev = advect(uv, c, dt);

    // add external forces
    if (SCENE == 0) { // mouse draw + slight wind turbulance
        vec2 m = (u_mouse.xy * 2.0 - u_resolution) / min(u_resolution.x, u_resolution.y);
        float dm = distance(nuv, m);
        float md = step(0.0, u_mouse.z);
        vec2 dir = normalize(vec2(fbm((nuv - u_time * 0.1) * 32.0 - 31.51, 2) * 2.0 - 1.0, 1));
        vec3 add = vec3(3.0 * dir.x, 3.0 * dir.y, 1) * smoothstep(0.05, 0.0, dm) * md;
        vec2 turb = turbulance(nuv);
        prev.xyz = mix(prev.xyz, add, step(abs(prev.xyz), abs(add)));
        prev.xy += 0.002 * turb;
        // prev.xyz *= 0.999;
    }
    else if (SCENE == 1) { // strong wind
        vec2 turb = turbulance(nuv);
        vec3 add = vec3(vec2(1, sin(uv.x * tau * 4.0)) * 16.0, 1) * step(abs(gl_FragCoord.y - u_resolution.y * 0.5), 1.0);
        prev.xyz = mix(prev.xyz, add, step(abs(prev.xyz), abs(add)));
        prev.xyz *= 0.99;
    }
    else if (SCENE == 2) { // strong wind
        vec2 m = (vec2(fbm(vec2(u_time, u_time) * 0.5, 3), fbm(vec2(u_time, -u_time) * 0.5, 3)) * 2.0 - 1.0) * u_resolution / min(u_resolution.x, u_resolution.y);
        float dm = distance(nuv, m);
        float md = smoothstep(0.2, 0.5, fbm(vec2(-u_time, u_time), 3));
        vec2 dir = normalize(vec2(fbm((nuv - u_time * 0.1) * 32.0 - 31.51, 2) * 2.0 - 1.0, 1));
        vec3 add = vec3(3.0 * dir.x, 3.0 * dir.y, 1) * smoothstep(0.15, 0.0, dm) * md;
        vec2 turb = turbulance(nuv);
        prev.xyz = mix(prev.xyz, add, step(abs(prev.xyz), abs(add)));
        prev.xy += 0.002 * turb;
        prev.xyz *= 0.999;
    }
    fragColor = vec4(prev.xyz, 0);
}
void main(){fragColor=vec4(0);goden_source_main();GodenOutput=fragColor;}
