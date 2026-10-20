#include "a4_trace.h"
#include "sdk_compat.h"
#include <TraceLoggingProvider.h>
TRACELOGGING_DEFINE_PROVIDER(g_godenR11,"GODENplayer.R11",
    (0x0edc9ef6,0x366f,0x4d8d,0x9f,0x85,0x4a,0x72,0x16,0xa4,0x20,0x11));
namespace nekorox::component::a4 {
namespace {
class TraceLifetime:public initquit {
 void on_init() override{TraceLoggingRegister(g_godenR11);}
 void on_quit() override{TraceLoggingUnregister(g_godenR11);}
};
FB2K_SERVICE_FACTORY(TraceLifetime);
}
void trace(const char* stage,std::uint64_t revision,std::int64_t code,double a,double b) noexcept {
 TraceLoggingWrite(g_godenR11,"Stage",TraceLoggingString(stage,"stage"),
    TraceLoggingUInt64(revision,"revision"),TraceLoggingInt64(code,"code"),
    TraceLoggingFloat64(a,"value0"),TraceLoggingFloat64(b,"value1"));
}
}
