#define D2D_INPUT_COUNT 0
#define D2D_REQUIRES_SCENE_POSITION
#include <d2d1effecthelpers.hlsli>

cbuffer ScopeConstants : register(b0) {
    float4 bounds;
    float4 controls; // count, scaleX, scaleY, reserved
    float4 accent;
    float4 bright;
    float4 columns[512]; // x, top, bottom, midpoint in physical pixels
};

float4 over(float4 dst, float4 ink, float coverage) {
    float a = ink.a * saturate(coverage);
    return float4(ink.rgb * a, a) + dst * (1 - a);
}
float rectangleCoverage(float2 p, float4 r) {
    float2 covered = max(0, min(p + .5, r.zw) - max(p - .5, r.xy));
    return covered.x * covered.y;
}

// Clip a flat-cap segment's four corners to this pixel. The polygon remains
// convex and has at most eight vertices, so this computes bounded box-filter
// coverage without time-varying sample patterns or lowering curve resolution.
float segmentCoverage(float2 p, float2 a, float2 b, float halfWidth) {
    if (any(p + .5 < min(a,b) - halfWidth) || any(p - .5 > max(a,b) + halfWidth)) return 0;
    float2 d = b - a;
    float len2 = dot(d,d);
    if (len2 < 1e-12) return 0;
    float2 n = float2(-d.y,d.x) * (halfWidth * rsqrt(len2));
    float2 poly[8];
    poly[0] = a+n-p; poly[1] = b+n-p; poly[2] = b-n-p; poly[3] = a-n-p;
    uint count=4;
    [loop] for (uint side=0; side<4; ++side) {
        float2 next[8]; uint written=0;
        float2 last=poly[count-1];
        float dl=side==0?last.x+.5:side==1?.5-last.x:side==2?last.y+.5:.5-last.y;
        [loop] for (uint j=0; j<count; ++j) {
            float2 here=poly[j];
            float dh=side==0?here.x+.5:side==1?.5-here.x:side==2?here.y+.5:.5-here.y;
            if ((dl>=0)!=(dh>=0)) next[written++]=lerp(last,here,dl/(dl-dh));
            if (dh>=0) next[written++]=here;
            last=here; dl=dh;
        }
        count=written;
        if (count<3) return 0;
        [loop] for (uint k=0; k<count; ++k) poly[k]=next[k];
    }
    float area=0; float2 last=poly[count-1];
    [loop] for(uint q=0;q<count;++q) {
        float2 here=poly[q]; area+=last.x*here.y-last.y*here.x; last=here;
    }
    return saturate(abs(area)*.5);
}

D2D_PS_ENTRY(main) {
    float2 p=D2DGetScenePosition().xy;
    int count=(int)controls.x;
    // Derive spacing from the full span to avoid accumulating subtraction
    // rounding from just the first two columns at large window coordinates.
    float step=(columns[count-1].x-columns[0].x)/(count-1);
    float padding=controls.y+.5;
    int first=max(0,(int)floor((p.x-padding-columns[0].x)/step));
    int last=min(count-1,(int)ceil((p.x+padding-columns[0].x)/step));
    float4 result=0;
    [loop] for(int i=first;i<=last;++i) {
        float4 c=columns[i];
        float lo=min(c.y,c.z),hi=max(c.y,c.z);
        result=over(result,accent,rectangleCoverage(p,float4(c.x-controls.y,lo,c.x+controls.y,hi)));
        result=over(result,bright,rectangleCoverage(p,float4(c.x-.5*controls.y,lo,c.x+.5*controls.y,hi)));
    }
    // Include the segment entering the first nearby column as well.
    first=max(1,first);
    last=min(count-1,last+1);
    [loop] for(int segment=first;segment<=last;++segment) {
        float2 a=columns[segment-1].xw,b=columns[segment].xw;
        // The CPU owner rejects non-square DPI and transformed targets.
        result=over(result,bright,segmentCoverage(p,a,b,.5*controls.y));
    }
    return result;
}
