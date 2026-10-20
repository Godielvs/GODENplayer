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
 //Seconds since playback started
 //Pass resolution in pixels

// Disco Hive
// By Noztol

#define PI 3.1415926535

// Global time
float time;

// Modified colors to use PI for seamless wrapping
vec3 discoColors(vec2 p)
{
    p += vec2(time * 0.2);
    
    vec3 orange = vec3(1.0, 0.4, 0.1) * 2.0;
    vec3 purple = vec3(1.0, 0.2, 0.5) * 0.8;
    
    float l = pow(0.5 + 0.5 * cos(p.x * PI * 2.0 + cos(p.y) * 8.0) * sin(p.y * 2.0), 4.0) * 2.0;
    vec3 c = pow(l * (mix(orange, purple, 0.5 + 0.5 * cos(p.x * PI * 12.0 + sin(p.y * 10.0) * 3.0)) +
                      mix(orange, purple, 0.5 + 0.5 * cos(p.x * PI * 6.0 + sin(p.y * 3.0) * 3.0))), vec3(1.2)) * 0.7;
    
    c += vec3(1.0, 0.8, 0.4) * pow(0.5 + 0.5 * cos(p.x * PI * 6.0) * sin(p.y * 12.0), 20.0) * 2.0;
    
    c += vec3(0.1, 0.5 + 0.5 * cos(p * PI * 6.0)) * vec3(0.05, 0.1, 0.4).bgr * 0.7;
    
    return c;
}

//generates hexagonal grid
vec4 hexCoords(vec2 uv) 
{
    vec2 r = vec2(1.0, 1.7320508);
    vec2 h = r * 0.5;
    
    vec2 a = mod(uv, r) - h;
    vec2 b = mod(uv - h, r) - h;
    
    vec2 gv = dot(a, a) < dot(b, b) ? a : b;
    vec2 id = uv - gv;
    
    return vec4(gv.x, gv.y, id.x, id.y);
}

vec3 hexTex(vec2 p, float border)
{
    vec4 hc = hexCoords(p);
    vec2 gv = hc.xy;
    
    vec2 id = hc.zw * 0.125; 
    
    // Calculate distance to the hexagon's edge
    float d = max(abs(gv.x), dot(abs(gv), vec2(0.5, 0.8660254)));
    
    float sm = 0.03;
    
    // Create the wireframe grid
    float m = 1.0 - smoothstep(border - sm, border, d);
    
    // Add center glowing dot
    m += 1.0 - smoothstep(0.0, 0.22, length(gv));
    
    return m * discoColors(id);
}

// Function to bend the tunnel's path over Z
vec2 path(float z) 
{
    return vec2(sin(z * 0.15) * 2.5, cos(z * 0.1) * 2.0);
}

// Distance field for tunnel
float map(vec3 p) 
{
    // Subtract the path XY offsets to bend it, 2.5 is the tunnel radius
    return 2.5 - length(p.xy - path(p.z)); 
}

void goden_source_main() {
  vec2 fragCoord = gl_FragCoord.xy;

    time = u_time + 1.0;
    vec2 uv = (fragCoord.xy - 0.5 * u_resolution.xy) / u_resolution.y;
    
    float camSpeed = 4.0;
    float camZ = time * camSpeed;
    
    // Camera position
    vec3 ro = vec3(path(camZ), camZ);
    // Look-at target slightly ahead in the tunnel
    vec3 ta = vec3(path(camZ + 3.0), camZ + 3.0); 
    
    // Build camera orthonormal basis
    vec3 fwd = normalize(ta - ro);
    vec3 right = normalize(cross(fwd, vec3(0.0, 1.0, 0.0)));
    vec3 up = cross(right, fwd);
    
    // Add a slight banking roll to the camera as it turns
    float roll = sin(time * 0.6) * 0.2;
    mat2 rot = mat2(cos(roll), -sin(roll), sin(roll), cos(roll));
    uv *= rot;
    
    // Ray direction (1.2 acts as FOV)
    vec3 rd = normalize(uv.x * right + uv.y * up + 1.2 * fwd); 
    
    float t = 0.0;
    for(int i = 0; i < 80; i++) 
    {
        vec3 p = ro + rd * t;
        float d = map(p);
        
        if(d < 0.01 || t > 100.0) break;
        t += d * 0.7;
    }
    
    vec3 col = vec3(0.0);
    
    // tunnel walls
    if(t < 100.0) 
    {
        vec3 p = ro + rd * t;
        
        // Get local coordinates relative to the tunnel center at this Z
        vec2 tunObj = p.xy - path(p.z);
        
        // Calculate angle around the tunnel (-PI to PI)
        float a = atan(tunObj.y, tunObj.x);
        
        float hexesAround = 16.0; // Must be an even whole number to wrap seamlessly
        float xuv = (a / PI) * (hexesAround * 0.5); 
        
        
        float yuv = p.z * (hexesAround / (2.5 * 2.0 * PI)); 
        
        vec2 tUv = vec2(xuv, yuv);
        
        // 0.46 controls the thickness of the honeycomb wall
        col = hexTex(tUv, 0.46);
        
        // Fog based on distance to obscure the far end of the tunnel
        float fog = exp(-0.006 * t * t);
        col *= fog;
        
        // Brighten up the overall scene slightly
        col *= 1.3;
    }
    
    vec2 q = fragCoord.xy / u_resolution.xy;
    col *= 0.5 + 0.5 * pow(16.0 * q.x * q.y * (1.0 - q.x) * (1.0 - q.y), 0.1);
    
    fragColor = vec4(col, 1.0);
}
void main(){fragColor=vec4(0);goden_source_main();GodenOutput=vec4(clamp(fragColor.rgb,vec3(0),vec3(1)),1.0);}
