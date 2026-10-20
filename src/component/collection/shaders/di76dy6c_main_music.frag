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
 //Seconds since playback started
 //Pass resolution in pixels

// Disco Hive
// By Noztol

#define PI 3.1415926535

// Global time
float time;

// Modified colors to use PI for seamless wrapping
vec3 discoColors(vec2 p){
 vec2 cell=round(p*16.0);cell.x=mod(cell.x+16.0,32.0)-16.0;
 float id=g_id(cell),e=c7_level(id),h=c7_event(id);
 float threshold=.16+.18*g_hash(cell+vec2(5,17));
 float lit=smoothstep(threshold,1.05,e);
 float ribbon=.55+.45*sin(cell.x*.19634954+cell.y*.175-g_motion.y*.5);
 float sweep=pow(.5+.5*cos(cell.x*.19634954-cell.y*.14-c7_bassTravel()*.7),4.0);
 vec3 dye=c7_pigment(vec3(.85,.29,.38),id*.23+cell.y*.17+.5*e,.95);
 vec3 spot=c7_pigment(vec3(.78,.65,.4),id*.23+1.4+.3*h,.78);
 return dye*(.012+2.1*lit*lit)*(.52+.48*ribbon)+spot*sweep*(.018+.70*lit+.32*h);
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
    
    float sm = clamp(fwidth(d)*.75,.014,.09);
    
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

    time = c7_bassTravel() + 1.0;
    vec2 uv = (fragCoord.xy - 0.5 * u_resolution.xy) / u_resolution.y;
    
    float camSpeed = 1.3;
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
    float roll = 0.0; // C5: camera is not an audio actuator.
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
