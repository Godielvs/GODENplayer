#pragma once
#include <array>
#include <atomic>
#include <cstdint>
namespace goden::c11 {
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
// Exactly-once acknowledgement per observed reason generation. No allocation,
// no mutex and no clearing of the producer's counters.
class ReasonTickets {
public:
    struct Snapshot {
        std::uint32_t mask=0;
        std::array<std::uint64_t,32> issued{};
    };
    void mark(std::uint32_t mask)noexcept{
        for(unsigned i=0;i<32;++i)if(mask&(std::uint32_t(1)<<i))
            issued_[i].fetch_add(1,std::memory_order_release);
    }
    Snapshot pending()const noexcept{
        Snapshot s;
        for(unsigned i=0;i<32;++i){
            s.issued[i]=issued_[i].load(std::memory_order_acquire);
            if(s.issued[i]>ack_[i].load(std::memory_order_acquire))s.mask|=std::uint32_t(1)<<i;
        }
        return s;
    }
    void commit(const Snapshot& s)noexcept{
        for(unsigned i=0;i<32;++i)if(s.mask&(std::uint32_t(1)<<i)){
            auto old=ack_[i].load(std::memory_order_relaxed);
            while(old<s.issued[i]&&!ack_[i].compare_exchange_weak(
                old,s.issued[i],std::memory_order_release,std::memory_order_relaxed)){}
        }
    }
    // Only when all publishers/consumer have stopped; never on a rejected frame.
    void resetQuiescent()noexcept{for(unsigned i=0;i<32;++i){issued_[i].store(0);ack_[i].store(0);}}
private:
    std::array<std::atomic<std::uint64_t>,32> issued_{},ack_{};
};
} // namespace goden::c11
