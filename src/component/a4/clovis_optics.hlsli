#ifndef GODEN_CLOVIS_OPTICS
#define GODEN_CLOVIS_OPTICS

#if A4_PASS == 1
float2 clovisRoundedNormal(float2 p,float2 halfSize,float radius){
    float2 q=abs(p)-halfSize+radius;
    float2 outside=max(q,0.);
    if(dot(outside,outside)>1e-8)return normalize(outside)*sign(p);
    return q.x>q.y?float2(sign(p.x),0):float2(0,sign(p.y));
}
#endif

#endif
