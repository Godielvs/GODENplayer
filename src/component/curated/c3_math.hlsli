#ifndef GODEN_C3_MATH
#define GODEN_C3_MATH
// Pure numerical kernel shared by HLSL production and the GLSL numeric probe.
// No textures, device state, input sampling, wall-clock or host ABI here.
float2 c3BoundVector(float2 value,float limit){
 return value/(1.0+length(value)/max(limit,1e-6));
}
float2 c3OpticalWarp(float2 p,float t,float3 e,float3 h,float2 stateWarp){
 float shear=.035*(e.x-e.y);
 float2 q=float2(p.x+shear*p.y,p.y-.035*e.y*p.x);
 float a=4.9*q.x+2.2*q.y+.34*t;
 float b=-2.7*q.x+5.6*q.y-.26*t;
 float2 d=float2((.042+.008*e.x)*sin(a)*cos(.55*b),
                  (.033+.006*e.y)*cos(b)*sin(.63*a));
 float2 center=float2(.24*sin(.31*t),.19*cos(.27*t));
 float2 v=q-center;
 float r2=dot(v,v);
 d+=v*exp(-r2/(.12+.045*e.x))*(.10+.06*e.x);
 float ripple=.0015+.003*e.z+.0015*h.z;
 d+=ripple*float2(sin(13.*q.y+3.*q.x+1.0*t),
                    cos(12.*q.x-4.*q.y-.9*t));
 // Broad folds remain the water's dominant scale; driven ripples modulate them.
 return c3BoundVector(d+c3BoundVector(stateWarp,.22),.46);
}
// q and velocity are physical state, not source Newton-iteration diagnostics.
// Coordinates are isotropic (one unit == grid short edge), Y downward.
float c3WaveExcitation(float2 p,float t,float3 e,float3 h){
 float result=0.;
 [unroll]for(int j=0;j<6;++j){
  int band=j%3;float z=float(j),angle=z*2.399963+.032*t;
  float2 center=float2(.43*cos(angle),.27*sin(angle*1.17+z));
  center+=float2(.070*sin(.075*t+z),.06*cos(.087*t+z));
  float radius=band==0?.145:band==1?.085:.050;
  float2 d=(p-center)/radius;float r2=dot(d,d),g=exp(-r2);
  float shape=band==0?(1.-r2)*g:band==1?dot(d,float2(cos(angle),sin(angle)))*g:(d.x*d.x-d.y*d.y)*g;
  float level=max(e[band]-.035,0.);float hit=max(h[band],0.);
  // A held note establishes pressure; changes/attacks launch propagating fronts.
  // In silence the force is exactly zero. No sin(rate*t) invents beat cadence.
  float sustained=(band==0?24.:band==1?34.:46.)*level*level*level;
  float transient=(band==0?95.:band==1?110.:135.)*hit*(.30+.70*level);
  result+=shape*(sustained+transient)*((j<3)?1.:-1.);
 }
 return result;
}
float2 c3WaveStepState(float q,float velocity,float laplacian,float force,float dt){
 // Stable damped driven wave: c=.42, restoring coefficient=3, gamma=2.2.
 // At 512 cells/short edge and dt=1/480, c*dt/dx=.448 < 1/sqrt(2).
 float v=(velocity+dt*(.1764*laplacian+force-3.*q))*exp(-2.2*dt);
 return float2(q+dt*v,v);
}
float3 c3WaveRadiance(float q,float velocity,float2 gradient){
 // gradient is displacement per cell; not resolution-dependent diagnostic data.
 float energy=.55*abs(q)+.045*abs(velocity)+3.*length(gradient);
 float amplitude=1.-exp(-energy);
 float phase=.5+.5*q/(1.+abs(q));
 float3 tint=lerp(float3(.065,.24,.55),float3(.80,.57,.24),phase);
 float ridge=1.-exp(-7.*length(gradient));
 return tint*amplitude+float3(.32,.42,.46)*ridge*.45;
}
float3 c3PhotonWeights(float steps){
 // A0 is AFTER the preceding .9 decay. For n frozen steps, pre-output:
 // .9^(n-1)*A0 + (1-.9^n)/.1*D; final stored state is .9 times that.
 float n=clamp(steps,1.,6.);
 return float3(pow(.9,n-1.),(1.-pow(.9,n))/.1,.9);
}
#endif
