#pragma once
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>
namespace goden::c11 {
// Main-thread-owned coordinator; workers receive immutable requests and return
// a token. Callbacks and complete() are never called from the worker itself.
template<class Key,class Handle>
class ChangeInbox {
public:
    struct Job {std::uint64_t lifetime=0,ticket=0;bool full=false;std::vector<Handle> handles;};
    struct Completion {bool recognized=false,publish=false,more=false;};
    void invalidateAll(){pendingFull_=true;pending_.clear();}
    bool modify(Key key,Handle handle){
        if(pendingFull_)return true;
        if(pending_.size()>=4096 && !pending_.contains(key)){invalidateAll();return true;}
        return !pending_.insert_or_assign(std::move(key),std::move(handle)).second;
    }
    std::optional<Job> take(){
        if(active_||(!pendingFull_&&pending_.empty()))return {};
        Job j{lifetime_,++ticket_,pendingFull_,{}};
        if(!pendingFull_){j.handles.reserve(pending_.size());for(auto& pair:pending_)j.handles.push_back(std::move(pair.second));}
        pending_.clear();pendingFull_=false;active_=j.ticket;return j;
    }
    Completion complete(const Job& j,bool success,bool superseded=false){
        if(j.lifetime!=lifetime_||!active_||*active_!=j.ticket)return {};
        active_.reset();
        // A full refresh demanded after this job started supersedes its result.
        const bool publish=success&&!superseded&&!pendingFull_;
        if(!success&&!superseded){pendingFull_=true;pending_.clear();}
        return {true,publish,pendingFull_||!pending_.empty()};
    }
    void stop(){++lifetime_;active_.reset();pending_.clear();pendingFull_=false;}
    bool active()const noexcept{return active_.has_value();}
    bool fullPending()const noexcept{return pendingFull_;}
    std::uint64_t lifetime()const noexcept{return lifetime_;}
private:
    std::uint64_t lifetime_=1,ticket_=0;
    std::optional<std::uint64_t> active_;
    bool pendingFull_=false;
    std::unordered_map<Key,Handle> pending_;
};
} // namespace goden::c11
