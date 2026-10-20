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
// =============================================================================
// Settings & Input Definitions
// =============================================================================
#define iSpin 0.997114514   //必须与BufferA中iSpin一致！It must be modified synchronously with iSpin in BufferA！
const float CONST_M = 0.5;//DONT CHANGE THIS
// Keycodes
const int KEY_W = 87;
const int KEY_A = 65;
const int KEY_S = 83;
const int KEY_D = 68;
const int KEY_Q = 81;
const int KEY_E = 69;
const int KEY_R = 82;
const int KEY_F = 70;

// Movement Settings
const float MOVE_SPEED = 2.5; 
const float MOUSE_SENSITIVITY = 0.003;
const float ROLL_SPEED = 2.0;

// Helper to check key status
bool isKeyPressed(int key) {
    return texelFetch(iChannel3, ivec2(key, 0), 0).x > 0.5;
}

// Rotation matrix helper
mat3 rotAxis(vec3 axis, float angle) {
    float s = sin(angle);
    float c = cos(angle);
    float oc = 1.0 - c;
    return mat3(
        oc * axis.x * axis.x + c,           oc * axis.x * axis.y - axis.z * s,  oc * axis.z * axis.x + axis.y * s,
        oc * axis.x * axis.y + axis.z * s,  oc * axis.y * axis.y + c,           oc * axis.y * axis.z - axis.x * s,
        oc * axis.z * axis.x - axis.y * s,  oc * axis.y * axis.z + axis.x * s,  oc * axis.z * axis.z + c
    );
}

// =============================================================================
// Original Bloom Functions (Unchanged Logic)
// =============================================================================

vec3 ColorFetch(vec2 coord)
{
 	return texture(iChannel0, coord).rgb;   
}

vec3 Grab1(vec2 coord, const float octave, const vec2 offset)
{
 	float scale = exp2(octave);
    coord += offset;
    coord *= scale;
   	if (coord.x < 0.0 || coord.x > 1.0 || coord.y < 0.0 || coord.y > 1.0) return vec3(0.0);   
    return ColorFetch(coord);
}

vec3 Grab4(vec2 coord, const float octave, const vec2 offset)
{
 	float scale = exp2(octave);
    coord += offset;
    coord *= scale;
   	if (coord.x < 0.0 || coord.x > 1.0 || coord.y < 0.0 || coord.y > 1.0) return vec3(0.0);   
    
    vec3 color = vec3(0.0);
    float weights = 0.0;
    const int oversampling = 4;
    for (int i = 0; i < oversampling; i++) {    	    
        for (int j = 0; j < oversampling; j++) {
			vec2 off = (vec2(i, j) / iResolution.xy + vec2(-float(oversampling)*0.5) / iResolution.xy) * scale / float(oversampling);
            color += ColorFetch(coord + off);
            weights += 1.0;
        }
    }
    return color / weights;
}

vec3 Grab8(vec2 coord, const float octave, const vec2 offset)
{
 	float scale = exp2(octave);
    coord += offset;
    coord *= scale;
   	if (coord.x < 0.0 || coord.x > 1.0 || coord.y < 0.0 || coord.y > 1.0) return vec3(0.0);   
    
    vec3 color = vec3(0.0);
    float weights = 0.0;
    const int oversampling = 8;
    for (int i = 0; i < oversampling; i++) {    	    
        for (int j = 0; j < oversampling; j++) {
			vec2 off = (vec2(i, j) / iResolution.xy + vec2(-float(oversampling)*0.5) / iResolution.xy) * scale / float(oversampling);
            color += ColorFetch(coord + off);
            weights += 1.0;
        }
    }
    return color / weights;
}

vec3 Grab16(vec2 coord, const float octave, const vec2 offset)
{
 	float scale = exp2(octave);
    coord += offset;
    coord *= scale;
   	if (coord.x < 0.0 || coord.x > 1.0 || coord.y < 0.0 || coord.y > 1.0) return vec3(0.0);   
    
    vec3 color = vec3(0.0);
    float weights = 0.0;
    const int oversampling = 16;
    for (int i = 0; i < oversampling; i++) {    	    
        for (int j = 0; j < oversampling; j++) {
			vec2 off = (vec2(i, j) / iResolution.xy + vec2(-float(oversampling)*0.5) / iResolution.xy) * scale / float(oversampling);
            color += ColorFetch(coord + off);
            weights += 1.0;
        }
    }
    return color / weights;
}

vec2 CalcOffset(float octave)
{
    vec2 offset = vec2(0.0);
    vec2 padding = vec2(10.0) / iResolution.xy;
    offset.x = -min(1.0, floor(octave / 3.0)) * (0.25 + padding.x);
    offset.y = -(1.0 - (1.0 / exp2(octave))) - padding.y * octave;
	offset.y += min(1.0, floor(octave / 3.0)) * 0.35;
 	return offset;   
}

// =============================================================================
// Camera & State Logic
// =============================================================================

#define OFFSET_UP    1  // W-1: Up Vector (Buffer A reads this)
#define OFFSET_RIGHT 2  // W-2: Right Vector (Buffer A reads this)
#define OFFSET_POS   3  // W-3: Position (Buffer A reads this)
#define OFFSET_FWD   4  // W-4: Forward Vector (Internal State)
#define OFFSET_MOUSE 5  // W-5: Last Mouse (Internal State)
#define OFFSET_TIME  6  // W-6: Game Time

void UpdateCameraState(out vec4 fragColor, in vec2 fragCoord)
{
    int pxIndex = int(iResolution.x) - int(fragCoord.x);
    int width = int(iResolution.x);
    vec3  up      = texelFetch(iChannel1, ivec2(width - OFFSET_UP, 0), 0).xyz;
    vec3  right   = texelFetch(iChannel1, ivec2(width - OFFSET_RIGHT, 0), 0).xyz;
    vec3  pos     = texelFetch(iChannel1, ivec2(width - OFFSET_POS, 0), 0).xyz;
    vec3  fwd     = texelFetch(iChannel1, ivec2(width - OFFSET_FWD, 0), 0).xyz;
    vec4  lastMouse = texelFetch(iChannel1, ivec2(width - OFFSET_MOUSE, 0), 0);
    vec4  timeData = texelFetch(iChannel1, ivec2(width - OFFSET_TIME, 0), 0);
    float gTime   = timeData.x;
    float uniSign = timeData.y; 
    vec3 oldPos = pos; 
    if (iFrame <= 5 || length(fwd) < 0.1) {
        pos = vec3(-2.0, -3.6, 22.0); 
        fwd = vec3(0.0, 0.15, -1.0);
        fwd = normalize(fwd);
        right = normalize(cross(fwd, vec3(-0.5, 1.0, 0.0)));
        up    = normalize(cross(right, fwd));
        gTime = 0.0;
        lastMouse = iMouse;
        uniSign = 1.0; 
    }

    // 3. 处理鼠标旋转
    if (iMouse.z > 0.0) {
        vec2 mouseDelta = iMouse.xy - lastMouse.xy;
        
        // 防止点击瞬间跳变
        if (lastMouse.z < 0.0) mouseDelta = vec2(0.0);
        
        float yaw = -mouseDelta.x * MOUSE_SENSITIVITY;
        float pitch = mouseDelta.y * MOUSE_SENSITIVITY;
        
        // 绕 Up 轴偏航 (Yaw)
        fwd = rotAxis(up, yaw) * fwd;
        right = rotAxis(up, yaw) * right;
        
        // 绕 Right 轴俯仰 (Pitch)
        fwd = rotAxis(right, pitch) * fwd;
        
        // 重新正交化，消除误差
        up = normalize(cross(right, fwd));
        right = normalize(cross(fwd, up));
    }
    
    // 4. 处理滚转 (Roll Q/E)
    float roll = 0.0;
    if (isKeyPressed(KEY_Q)) roll -= ROLL_SPEED * iTimeDelta;
    if (isKeyPressed(KEY_E)) roll += ROLL_SPEED * iTimeDelta;
    
    if (roll != 0.0) {
        right = rotAxis(fwd, roll) * right;
        up = normalize(cross(right, fwd));
    }

    // 5. 处理位移 (WSAD RF)
    vec3 moveDir = vec3(0.0);
    if (isKeyPressed(KEY_W)) moveDir += fwd;
    if (isKeyPressed(KEY_S)) moveDir -= fwd;
    if (isKeyPressed(KEY_A)) moveDir -= right;
    if (isKeyPressed(KEY_D)) moveDir += right;
    if (isKeyPressed(KEY_R)) moveDir += up; // 上浮
    if (isKeyPressed(KEY_F)) moveDir -= up; // 下沉

    pos += moveDir * MOVE_SPEED * iTimeDelta * (length(pos) > 3.0 ? 1.0 : (length(pos) > 0.5 ? 0.1 + 0.9 * (length(pos) - 0.5) / 2.5 : 0.1));
    
    float spinRadius = abs(iSpin * CONST_M);
    if (oldPos.y * pos.y < 0.0) 
    {
        float t = oldPos.y / (oldPos.y - pos.y);
        vec3 crossPoint = mix(oldPos, pos, t);
        
        if (length(crossPoint.xz) < spinRadius) {
            uniSign *= -1.0;
        }
    }
    // 6. 更新时间
    gTime += iTimeDelta;

    // 7. 写入数据 (根据 pxIndex 匹配 Buffer A 的读取位置)
    fragColor = vec4(0.0);
    
    if (pxIndex == OFFSET_UP)    fragColor = vec4(up, 1.0);     // W-1 -> Up
    if (pxIndex == OFFSET_RIGHT) fragColor = vec4(right, 1.0);  // W-2 -> Right
    if (pxIndex == OFFSET_POS)   fragColor = vec4(pos, 1.0);    // W-3 -> Pos
    if (pxIndex == OFFSET_FWD)   fragColor = vec4(fwd, 1.0);    // W-4 -> Fwd (Internal)
    if (pxIndex == OFFSET_MOUSE) fragColor = iMouse;            // W-5 -> Mouse
    if (pxIndex == OFFSET_TIME)  fragColor = vec4(gTime, 0.0, 0.0, 1.0); // W-6 -> Time
    if (pxIndex == OFFSET_TIME)  fragColor = vec4(gTime, uniSign, 0.0, 1.0); 
}

// =============================================================================
// Main Image
// =============================================================================

void mainImage( out vec4 fragColor, in vec2 fragCoord )
{
    // 定义数据存储区：底行 (y=0)，最右侧 8 个像素
    // 稍微放宽范围以防万一
    bool isDataPixel = (fragCoord.y < 1.0 && fragCoord.x > (iResolution.x - 8.5));

    if (isDataPixel) {
        UpdateCameraState(fragColor, fragCoord);
    } else {
        // 执行原始 Bloom/Mipmap 逻辑 (读取 iChannel0 = Buffer A)
        vec2 uv = fragCoord.xy / iResolution.xy;
        vec3 color = vec3(0.0);
        
        color += Grab1(uv, 1.0, vec2(0.0,  0.0)   );
        color += Grab4(uv, 2.0, vec2(CalcOffset(1.0))   );
        color += Grab8(uv, 3.0, vec2(CalcOffset(2.0))   );
        color += Grab16(uv, 4.0, vec2(CalcOffset(3.0))   );
        color += Grab16(uv, 5.0, vec2(CalcOffset(4.0))   );
        color += Grab16(uv, 6.0, vec2(CalcOffset(5.0))   );
        color += Grab16(uv, 7.0, vec2(CalcOffset(6.0))   );
        color += Grab16(uv, 8.0, vec2(CalcOffset(7.0))   );

        fragColor = vec4(color, 1.0);
    }
}

void goden_source_main(){mainImage(fragColor,gl_FragCoord.xy);}
void main(){fragColor=vec4(0);goden_source_main();GodenOutput=fragColor;}
