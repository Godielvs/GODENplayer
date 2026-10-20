#include "visual_capture_recovery.h"
#include "media_file_operations.h"
#include <limits>
#include <stdexcept>

namespace {
void check(bool value,const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

void runVisualCaptureTests() {
    using nekorox::component::VisualSourceRecovery;
    VisualSourceRecovery source;
    source.activate(0);
    check(!source.observe(.49,false),"Source repair was triggered by ordinary start latency");
    check(source.observe(.51,false),"Sustained missing PCM did not request source repair");
    check(source.observe(.75,false)&&source.observe(2,false),
        "Denied/cooldown repair prematurely spent the one-shot budget");
    source.commit();
    for(int i=1;i<100;++i)check(!source.observe(i,false),"Source repair retried without a bound");
    check(!source.observe(101,true)&&!source.observe(102,false)&&!source.observe(103,false),
        "Fresh data replenished the spent repair budget within one activation");
    source.reset();source.activate(200);
    for(int i=1;i<100;++i){
        check(!source.observe(200+i*.02,i%3!=0),"Short publication gaps restarted the source");
    }
    source.reset();source.activate(205);
    check(!source.observe(std::numeric_limits<double>::quiet_NaN(),false),"Invalid time caused repair");
    source.commit();source.activate(206);
    check(!source.observe(206.49,false)&&source.observe(206.51,false),
        "External stream discontinuity did not rearm source recovery");
    source.commit();check(!source.observe(210,false),"Committed source repair exceeded its budget");
    using nekorox::component::VisualCaptureRecovery;
    VisualCaptureRecovery recovery;
    for (int i=0;i<100;++i) {
        recovery.observe(i*.02,.03);
        check(!recovery.takeRebase(),"Normal capture must keep every sample");
    }
    recovery.observe(3,.329);
    check(!recovery.takeRebase(),"One late observation must allow natural catch-up");
    recovery.observe(3.04,.329);
    check(recovery.takeRebase(),"Successful reads with constant debt must rebase");
    check(!recovery.takeRebase(),"A rebase must be consumed once");
    recovery.observe(4,.18);
    recovery.observe(4.02,.16);
    check(!recovery.takeRebase(),"Fast natural recovery must preserve PCM");
    recovery.observe(4.04,.14);
    recovery.observe(4.06,.12);
    recovery.observe(4.08,.10);
    check(!recovery.takeRebase(),"Completed natural recovery must clear debt");
    recovery.observe(5,.11);
    recovery.observe(5.05,.12);
    check(recovery.takeRebase(),"Growing capture debt must be bounded");
    recovery.observe(6,.30);
    recovery.observe(6.04,.299);
    check(recovery.takeRebase(),"Very slow catch-up must not leave stale visuals");
    recovery.observe(7,.30);
    recovery.observe(7.04,std::numeric_limits<double>::quiet_NaN());
    check(!recovery.takeRebase(),"Invalid observations must not request a rebase");
    recovery.observe(8,.30);recovery.observe(8.04,.30);recovery.reset();
    check(!recovery.takeRebase(),"Track boundaries must discard pending recovery");

    std::stop_source cancellation;cancellation.request_stop();
    const auto plan=nekorox::component::buildMediaPlan({},{{L"unavailable.flac",L"target.flac"}},
        nekorox::component::MediaOrganizationMode::copy,
        nekorox::component::MediaCollisionPolicy::rename,cancellation.get_token());
    check(plan.items.empty(),"Cancelled organization preview must not expose a partial executable plan");
}
