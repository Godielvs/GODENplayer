#ifndef GODEN_C11_ALLOY_V2
#define GODEN_C11_ALLOY_V2
// All RGB values in/out of this field are linear. The existing final encode pass
// is the only sRGB conversion. This is an authored analytic ribbon material, not
// a claimed literal port of the Ribbons author source.
struct C11Alloy {
    float4 control; // master, count, aspect, valid
    float4 material;
    float4 art;
    float4 motion;
    float4 pigment[8];
    float4 accent[8];
    // E0..D#10, one entry per semitone: energy, texture, angle, half-width.
    float4 miniband[120];
    float4 broadLevel[2];
    float4 broadTexture[2];
};
float c11Hash(float n){return frac(sin(n*127.1+311.7)*43758.5453);}

float c11FilteredSin(float p,float footprint){return sin(p)*exp2(-.72*footprint*footprint);}

float3 c11BroadColor(C11Alloy c){
    float3 color=0;
    [unroll]for(int i=0;i<8;++i)color+=c.pigment[i].rgb*c.pigment[i].w;
    return color;
}
float3 c12LitPigment(float3 color){
    // Dark ink still reflects light. A bounded material gain preserves channel
    // ratios and is independent of music; no white is added to a coloured ink.
    float luminance=dot(color,float3(.2126,.7152,.0722));
    return color*max(1,min(8,.18/max(luminance,.0001)));
}

float3 c12Gradient(C11Alloy c,float2 uv){
    // Nearby pigments form one material gradient. Opposing hues are reserved
    // for localized reflections, never cycled down the height of a note lane.
    float3 family=0;float total=0;
    [unroll]for(int i=0;i<8;++i){
        float weight=i<(int)c.control.y?c.pigment[i].w*c.accent[i].w:0;
        family+=c.pigment[i].rgb*weight;total+=weight;
    }
    family=total>1e-5?family/total:c.pigment[0].rgb;
    float gradient=smoothstep(.08,.92,.35*uv.x+.65*uv.y);
    return lerp(c12LitPigment(c.pigment[0].rgb)*.72,c12LitPigment(family),gradient);
}

float3 c12Finish(float3 rgb){
    rgb=max(rgb,float3(0,0,0));
    float peak=max(rgb.x,max(rgb.y,rgb.z));
    // Fixed, hue-preserving contrast toe. Dim overlapping skirts fall back
    // into shadow; strong cores retain their radiance. No per-song exposure,
    // moving histogram, channel-wise gamma or hard silence threshold.
    float contrast=.45+.55*smoothstep(.025,.62,peak);
    rgb*=contrast;peak*=contrast;
    const float knee=.65,ceiling=.9473065367;
    if(peak<=knee)return rgb;
    float mapped=knee+(ceiling-knee)*(1-exp(-(peak-knee)/(ceiling-knee)));
    return rgb*(mapped/max(peak,1e-6));
}
struct C12Surface {
    float3 normal;
    float3 tangent;
    float rib,specPower,anisotropy;
};
C12Surface c12Surface(float2 uv,float2 fieldPixels,C11Alloy c){
    C12Surface s;
    float t=c.motion.x,aspect=max(c.control.z,.1);
    float hardness=saturate(c.material.y),roughness=saturate(c.material.x);
    float diagonal=.32+.08*sin(c.material.w);
    float2 q=float2(uv.x*aspect-diagonal*uv.y,uv.y-4.5*t);
    float a=.016+.024*hardness,b=.010+.014*hardness;
    float p=9*q.x+.6*sin(4*q.y+21*t);
    float z=18*q.x-2*q.y+.28*sin(17*t);
    float footprint=aspect/max(1,fieldPixels.x);
    // The derivative gets the same pixel filtering as the relief, rather than
    // retaining aliased normals after the height itself has been filtered.
    float af=a*exp2(-.72*81*footprint*footprint);
    float bf=b*exp2(-.72*324*footprint*footprint);
    float dx=af*9*cos(p)+bf*18*cos(z);
    float dy=af*cos(p)*2.4*cos(4*q.y+21*t)-2*bf*cos(z)-diagonal*dx;
    s.normal=normalize(float3(-dx,-dy,1));
    s.tangent=normalize(float3(1,diagonal,0));
    s.rib=pow(saturate(.5+.5*sin(p)),lerp(1.2,3.4,hardness));
    s.specPower=lerp(18,94,hardness)*(1.10-.40*roughness);
    s.anisotropy=saturate(c.material.z);
    return s;
}

float c12Specular(C12Surface s,float3 direction,float power){
    float3 halfVector=normalize(direction+float3(0,0,1));
    float directional=exp2(-s.anisotropy*12*pow(dot(halfVector,s.tangent),2));
    return pow(saturate(dot(s.normal,halfVector)),power)*lerp(1,directional,s.anisotropy);
}

float3 c12BroadRadiance(float2 uv,C11Alloy c,float3 flowPhase,C12Surface s,float3 pigment){
    float aspect=max(c.control.z,.1),master=max(c.control.x,0)*saturate(c.motion.w);
    float3 base=c11BroadColor(c);
    // Retain a static material only below perceptual black. No illuminated idle floor.
    float3 rgb=base*.0001;float shadowSum=0,overlap=0;
    [unroll]for(int light=0;light<8;++light){
        float level=0,spectral=0;
        if(light<4){level=c.broadLevel[0][light];spectral=c.broadTexture[0][light];}
        else{int j=light-4;level=c.broadLevel[1][j];spectral=c.broadTexture[1][j];}
        level=clamp(level,0,1.5);spectral=clamp(spectral,0,2);
        float seed=(float(light)+.5)/8;
        float phase=flowPhase[light%3]+6.2831853*seed;
        float side=(light%2)==0?-1:1;
        float slope=side*(.18+.17*c11Hash(light+3)+.09*sin(phase+1.7));
        float center=.06+.88*seed+.028*sin(phase)+slope*uv.y;
        // Spatially local response to actual spectral changes, not a global camera shake.
        center+=.012*spectral*sin(2*phase+4*uv.y);
        float width=clamp(.10+.045*c11Hash(light+9)+.055*level+.024*spectral,.075,.24);
        float d=(uv.x-center)/width;
        float aperture=exp2(-2.8*d*d)*lerp(.95,.42,saturate(uv.y));
        float crest=exp2(-5.8*d*d)*lerp(.95,.42,saturate(uv.y));
        float sd=(uv.x-center-side*width*.77)/(width*1.04);
        float shadow=exp2(-1.6*sd*sd);
        float3 direction=normalize(float3((center-uv.x)*aspect,-.65-.35*uv.y,.85));
        float diffuse=.35+.65*saturate(dot(s.normal,direction));
        float spec=c12Specular(s,direction,s.specPower*.55);
        float dose=master*c.motion.y*(.035*level+1.0*level*level);
        // Keep the aperture's full support, but concentrate radiance in its
        // crest. Eight overlapping diffuse skirts must not fill every valley.
        rgb+=dose*pigment*(.24*aperture*(.32+.45*diffuse)+
            crest*(.40+.55*diffuse+.35*spec))*(1+.10*spectral);
        shadowSum+=master*c.motion.y*level*level*shadow*.025;
        overlap+=aperture*smoothstep(.12,.70,level);
    }
    // These broad regions provide ambient relief. Reserve the visible musical
    // contrast for the independent semitones, even under a sustained dense mix.
    return .20*rgb*exp2(-min(shadowSum,.28))*lerp(.95,1.05,s.rib)/(1+.20*overlap);
}

float3 c12MinibandRadiance(float2 uv,C11Alloy c,C12Surface s,float3 material){
    float3 sustained=0,accents=0;float density=0;
    float aspect=max(c.control.z,.1);
    // Per-note absolute power already supplies loudness. Other notes and the
    // broadband master must not multiply this semitone's brightness.
    float availability=saturate(c.motion.w);
    float3 pigment=c12LitPigment(c.accent[0].rgb);
    const float maxAngle=.08726646260; // +/-5 degrees in physical screen coordinates.
    [loop]for(int i=0;i<120;++i){
        float4 band=c.miniband[i];
        if(band.x<.001||band.w<=0)continue;
        float angle=clamp(band.z,-maxAngle,maxAngle);
        float center=(float(i)+.5)/120.+tan(angle)*(uv.y-.5)/aspect;
        // A modestly wider C12 kernel; tracking, energy and tilt are unchanged.
        float width=.30*clamp(band.w,4.25/120.,6./120.);
        float d=(uv.x-center)/width;
        if(abs(d)>4.0)continue; // Far tails are subvisible; skip their palette/specular work.
        float beam=exp2(-2*d*d);
        // Bright notes sharpen their own luminous center; quieter notes keep
        // the soft halo. Geometry/support and the note's absolute level stay
        // unchanged, with no comparison against the other 119 notes.
        float energy=clamp(band.x,0,1.45);
        float crest=exp2(-lerp(3.5,6.0,saturate(energy))*d*d);
        float seed=frac((float(i)+.5)*.61803398875);
        float y0=.17+.66*seed;
        float depth=exp2(-2.4*pow((uv.y-y0)/.62,2));
        float3 direction=normalize(float3((center-uv.x)*aspect,-.8,.70));
        float spec=c12Specular(s,direction,s.specPower*.42);
        // Zero-anchored form of Light's quadratic aperture response. Quiet
        // notes stay dark and medium levels remain distinct from peaks.
        float dose=1.9*energy*energy;
        float emission=availability*c.motion.z*1.65*dose;
        sustained+=emission*pigment*(beam*(.13*depth+.08*spec+.025*s.rib)+
            crest*(.47*depth+.27*spec+.035*s.rib));
        // Fast reflections also read this note's absolute level, never relative
        // salience/flux (which can brighten a quiet note against quiet neighbours).
        float core=beam*exp2(-2.4*pow((uv.y-y0)/.24,2));
        float quick=clamp(band.y,0,1.5);
        float event=max(0,quick-energy)*quick;
        accents+=availability*c.motion.z*core*pigment*(.12*dose+.32*event);
        density+=beam*smoothstep(.035,.22,energy);
        // C12 deliberately removes the independent static godray/white-curtain
        // layer. Only a small transient halo remains, tied to local spectral flux.
        float halo=exp2(-.65*d*d)*.022*event*depth;
        accents+=availability*dose*halo*pigment;
    }
    // Overlapping semitone kernels are correlated. This soft density correction
    // prevents dense music washing out the image; it does not normalize by max note.
    return sustained/(1+.25*density)+accents/(1+.12*density);
}

float4 c11AlloyField(float2 uv,float2 fieldPixels,C11Alloy c,float3 flowPhase){
    if(c.control.w<.5)return float4(0,0,0,1);
    C12Surface s=c12Surface(uv,fieldPixels,c);
    float3 pigment=c12Gradient(c,uv);
    float3 broad=c12BroadRadiance(uv,c,flowPhase,s,pigment);
    return float4(c12Finish(broad+c12MinibandRadiance(uv,c,s,pigment)),1);
}

float3 c11NativeMicro(float2 uv,float2 pixels,C11Alloy c,float3 field,float textureAmount){
    float2 q=float2(uv.x*c.control.z-.32*uv.y,uv.y);
    float f=lerp(180,740,c.material.y);
    float footprint=f*c.control.z/max(1,pixels.x);
    float brush=c11FilteredSin(f*q.x+.08*sin(17*q.y),footprint)*(.5+.5*c.material.z)
        +c11FilteredSin(.7*f*q.y+.04*sin(13*q.x),.7*footprint)*(.5-.5*c.material.z);
    // Multiplicative texture cannot illuminate silence or contaminate UI text.
    return max(0,field*(1+(.018+.025*c.material.y)*brush*textureAmount));
}

#endif
