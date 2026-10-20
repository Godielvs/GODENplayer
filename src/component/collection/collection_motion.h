#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace nekorox::component::collection {
// C5 transport speed, not an audio transport. Hidden/device failure is a hard
// suspend; user pause is a 4-second stop with an exact time integral. No debt.
class MotionRate final {
    double began_=0, duration_=0, from_=0, to_=0;
    static double smooth(double u) noexcept {u=std::clamp(u,0.0,1.0);return u*u*(3.0-2.0*u);}
    double primitive(double t) const noexcept {
        if(duration_<=0)return to_*(t-began_);
        const double x=t-began_;
        if(x<=0)return from_*x;
        if(x>=duration_)return duration_*(from_+to_)*.5+to_*(x-duration_);
        const double u=x/duration_;
        return from_*x+(to_-from_)*duration_*(u*u*u-.5*u*u*u*u);
    }
public:
    double sample(double now)const noexcept {
        return duration_<=0?to_:from_+(to_-from_)*smooth((now-began_)/duration_);
    }
    double integral(double a,double b)const noexcept {
        if(!std::isfinite(a)||!std::isfinite(b)||b<=a)return 0;
        return std::max(0.0,primitive(b)-primitive(a));
    }
    void set(double target,double now,double seconds) noexcept {
        if(!std::isfinite(now)||!std::isfinite(target)||!std::isfinite(seconds))return;target=std::clamp(target,0.0,2.0);
        if(target==to_)return;from_=sample(now);to_=target;began_=now;duration_=std::max(0.0,seconds);
    }
    void stop(double now)noexcept {from_=to_=0;duration_=0;began_=now;}
    bool moving(double now)const noexcept {return to_>0||(duration_>0&&now<began_+duration_&&from_>0);}
};
struct CoalescedClock final {
    MotionRate rate;
    double anchor=0,next=0,elapsed=0,debt=0,dropped=0,lastDelta=0;
    bool running=false,deferred=false;
    // Caller distinguishes pause from loss of visibility/permission. Repeated
    // paused observations do not restart the tail.
    void transport(bool playing,bool pauseAllowed,double now) noexcept {
        if(!std::isfinite(now))return;
        if(!playing&&!pauseAllowed){suspend(now);return;}
        rate.set(playing?1.0:0.0,now,playing?.25:4.0);
        const bool want=rate.moving(now);
        if(want&&!running){anchor=next=now;debt=0;deferred=false;}
        running=want;
    }
    void suspend(double now) noexcept {rate.stop(now);running=false;anchor=next=now;debt=lastDelta=0;deferred=false;}
    void reset(double now) noexcept {anchor=next=now;elapsed=debt=dropped=lastDelta=0;deferred=false;}
    bool due(double now)const noexcept {return running&&std::isfinite(now)&&now+1e-9>=next;}
    void defer(double now,unsigned hz) noexcept {
        if(!std::isfinite(now))return;
        if(!deferred){deferred=true;anchor=now;debt=0;}
        const double p=1.0/std::clamp(hz,1U,120U);
        if(next<=now+1e-9)next+=(std::floor(std::max(0.0,now-next)/p)+1)*p;
    }
    unsigned advance(double now,unsigned hz,double simulationRate=1.0) noexcept {
        lastDelta=0;if(!due(now))return 0;
        if(deferred){deferred=false;anchor=now;debt=0;}
        const double start=std::max(anchor,now-.10);
        dropped+=std::max(0.0,now-anchor-.10);
        lastDelta=rate.integral(start,now)*std::clamp(simulationRate,.5,2.0);anchor=now;elapsed+=lastDelta;debt+=lastDelta;
        unsigned whole=unsigned(std::floor((debt+1e-9)*60.0));
        if(whole>6){dropped+=double(whole-6)/60.0;debt-=double(whole-6)/60.0;whole=6;}
        debt=std::max(0.0,debt-double(whole)/60.0);
        const double p=1.0/std::clamp(hz,1U,120U);
        if(next<=now+1e-9)next+=(std::floor(std::max(0.0,now-next)/p)+1)*p;
        running=rate.moving(now);
        return whole;
    }
};
// Per-emitter control has a smooth level and a separate fast transient. Audio
// analysis remains in its worker; this envelope does not estimate a baseline.
struct BandSlew final {
    float value=0;
    float step(float target,double dt) noexcept {
        target=std::isfinite(target)?std::clamp(target,0.f,1.f):0;
        dt=std::clamp(dt,0.0,.10);
        const double tau=target>value?.055:.22;
        value+=float(-std::expm1(-dt/tau))*(target-value);
        return value;
    }
};
} // namespace
