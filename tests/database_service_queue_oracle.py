"""Generate a host-independent oracle from the actual DatabaseService admission code.

Usage: python tests/database_service_queue_oracle.py --output <artifacts directory>
Optional --source-root selects a checkpoint's files for the baseline comparison.
Compile generated oracle.cpp using MSVC /std:c++20 /EHsc /O2 and src/core/include.
Run with 'baseline' only for the old, non-coalescing source; no argument asserts
the fixed contract. The SDK declarations and main-thread publication are stubbed;
the real header, enqueue and loadPacketCost bodies are copied, never reimplemented.
This tests admission/ownership, not filesystem, SQLite, or main-thread dispatch.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source-root", type=Path,
                        default=Path(__file__).resolve().parents[1])
    parser.add_argument('--expect-baseline', action='store_true')
    args = parser.parse_args()
    component = args.source_root / "src/component"
    header = (component / "database_service.h").read_text(encoding="utf-8-sig")
    source = (component / "database_service.cpp").read_text(encoding="utf-8-sig")
    if not args.expect_baseline and 'stopped before identity binding' not in source:
        raise RuntimeError('Candidate is missing rejected-rebind completion')
    methods = []
    for method in ("enqueue", "loadPacketCost", "rebindTagRevisions"):
        match = re.search(r"^(?:void|bool) DatabaseService::" + method +
                          r"\([\s\S]*?^}\n", source, re.M)
        if not match:
            raise RuntimeError(f"Production method missing: {method}")
        methods.append(match.group())
    if header.count('#include "sdk_compat.h"') != 1 or header.count("private:") != 1:
        raise RuntimeError("Header boundary changed; review the oracle")
    header = header.replace('#include "sdk_compat.h"',
                            'class metadb_handle_ptr;\n'
                            'namespace play_control { enum t_stop_reason : int; }')
    header = header.replace("private:", "public: // Oracle access only.")
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "database_service_test.h").write_text(header, encoding="utf-8")
    (args.output / "oracle.cpp").write_text(
        '#include "database_service_test.h"\n#include <algorithm>\n'
        '#include <iostream>\n#include <stdexcept>\n#include <iomanip>\n'
        '#define REBIND_CALLBACK_FIXED ' + str(int('stopped before identity binding' in source)) + '\n' +
        'namespace qa { inline std::vector<std::function<void()>> callbacks; }\n'
        'namespace fb2k { template<class C> void inMainThread(C c) { qa::callbacks.emplace_back(std::move(c)); } }\n'
        'namespace nekorox::component {\n' + "\n".join(methods) +
        '\nvoid DatabaseService::publishPacketCostResult(PacketCostCacheCallback callback, '
        'PacketCostCachePayload payload) { if (callback) callback(std::move(payload)); }\n}\n'
        + TESTS, encoding="utf-8")
    (args.output / "source-manifest.json").write_text(json.dumps({
        "sourceRoot": str(args.source_root.resolve()),
        "methods": dict(zip(("enqueue", "loadPacketCost", "rebindTagRevisions"),
                            (hashlib.sha256(x.encode()).hexdigest() for x in methods))),
        "limits": "SDK declarations and publication stubbed; admission bodies verbatim",
    }, indent=2), encoding="utf-8")


TESTS = r'''
using namespace nekorox;
using namespace nekorox::component;
using Service = DatabaseService;
using Event = Service::PendingEvent;
using Kind = Service::AnalysisTask::Kind;
bool baseline = false;
int checks = 0;
void require(bool value, const char* label) {
    ++checks;
    if (!value) throw std::runtime_error(label);
}
Event analysis(Kind kind, std::string label) {
    Event event; event.eventUid = label;
    event.analysis = std::make_unique<Service::AnalysisTask>();
    event.analysis->kind = kind; event.analysis->trackPath = label;
    event.analysis->resultJson = "retained data " + label;
    return event;
}
std::size_t loads(Service& service) {
    return std::count_if(service.queue_.begin(), service.queue_.end(), [](const auto& e) {
        return e.analysis && e.analysis->kind == Kind::loadPacketCost;
    });
}
Event pop(Service& service) {
    std::lock_guard<std::mutex> lock(service.queueMutex_);
    auto event = std::move(service.queue_.front()); service.queue_.pop_front();
    return event;
}
void finish(Service& service, Event event) {
    require(event.analysis != nullptr, "Callback event retained");
    PacketCostCachePayload payload; payload.found = true;
    payload.trackPath = event.analysis->trackPath;
    service.publishPacketCostResult(std::move(event.analysis->packetCostCallback),
                                   std::move(payload));
}
void burst() {
    Service s; s.started_ = true;
    std::vector<int> callbacks(121, 0);
    for (int i = 0; i < 121; ++i) {
        s.loadPacketCost(std::to_string(i), [&, i](auto p) {
            require(p.trackPath == std::filesystem::path(std::to_string(i)), "Correct callback path");
            ++callbacks[i];
        });
    }
    require(loads(s) == (baseline ? 121 : 1), "Bounded latest pending load");
    std::cout << "burst_pending_reads=" << loads(s) << '\n';
    while (!s.queue_.empty()) finish(s, pop(s));
    require(callbacks.back() == 1, "Current callback completes once");
    require(std::count(callbacks.begin(), callbacks.end(), 1) == (baseline ? 121 : 1),
            "Superseded callbacks not invoked");
}
void inFlightAndSamePath() {
    Service s; s.started_ = true;
    std::vector<int> callbacks(4, 0);
    s.loadPacketCost("A", [&](auto) { ++callbacks[0]; });
    auto inFlight = pop(s); // Already taken by worker; must never be cancelled here.
    s.loadPacketCost("B", [&](auto) { ++callbacks[1]; });
    s.loadPacketCost("A", [&](auto) { ++callbacks[2]; });
    s.loadPacketCost("A", [&](auto) { ++callbacks[3]; }); // Same path, new request.
    require(loads(s) == (baseline ? 3 : 1), "Same path is still latest-only");
    finish(s, std::move(inFlight));
    while (!s.queue_.empty()) finish(s, pop(s));
    require(callbacks[0] == 1 && callbacks[3] == 1, "In-flight and latest ownership");
    require(callbacks[1] == (baseline ? 1 : 0) && callbacks[2] == (baseline ? 1 : 0),
            "A-B-A and same-generation resume supersede queued closures");
}
void mixedQueue() {
    Service s; s.started_ = true;
    s.enqueue(analysis(Kind::storePacketCost, "packet-store"));
    s.loadPacketCost("old", {});
    s.enqueue(analysis(Kind::loadDeep, "deep-load"));
    s.enqueue(analysis(Kind::storeDeep, "deep-store"));
    Event lyrics; lyrics.eventUid = "lyrics";
    lyrics.lyrics = std::make_unique<Service::LyricsTask>(); s.enqueue(std::move(lyrics));
    Event identity; identity.eventUid = "identity";
    identity.identities = std::make_unique<Service::IdentityBatchTask>();
    s.enqueue(std::move(identity));
    Event rebind; rebind.eventUid = "rebind";
    rebind.revisionRebind = std::make_unique<Service::RevisionRebindTask>();
    s.enqueue(std::move(rebind));
    Event user; user.eventUid = "user";
    user.userState = std::make_unique<Service::UserStateTask>(); s.enqueue(std::move(user));
    Event persona; persona.eventUid = "persona";
    persona.persona = std::make_unique<Service::PersonaTask>(); s.enqueue(std::move(persona));
    Event retry; retry.eventUid = "retry"; retry.retryDatabase = true; s.enqueue(std::move(retry));
    s.loadPacketCost("latest", {});
    std::vector<std::string> actual;
    for (const auto& e : s.queue_) {
        if (e.analysis && e.analysis->kind == Kind::loadPacketCost) {
            actual.push_back(e.analysis->trackPath.string());
        } else {
            actual.push_back(e.eventUid);
            if (e.analysis) require(e.analysis->resultJson == "retained data " + e.eventUid,
                                    "Unrelated analysis payload intact");
        }
    }
    std::vector<std::string> expected{"packet-store", "deep-load", "deep-store",
        "lyrics", "identity", "rebind", "user", "persona", "retry", "latest"};
    if (baseline) expected.insert(expected.begin() + 1, "old");
    require(actual == expected, "Other event FIFO preserved, latest appended at end");
}
void playbackContracts() {
    Service s; s.started_ = true;
    s.admittedPlayback_ = std::make_shared<Service::PlaybackContext>();
    s.enqueue(analysis(Kind::storePacketCost, "keep-store"));
    for (int i = 0; i < 40; ++i) {
        Event e; e.type = PlayEventType::seek; e.positionMs = i; s.enqueue(std::move(e));
    }
    require(s.queue_.size() == 2 && s.queue_.back().positionMs == 39,
            "Existing seek debounce retained");
    Event pause; pause.type = PlayEventType::pause; s.enqueue(std::move(pause));
    require(!s.queue_[1].deferred, "Pause flushes deferred seek");
    Event seek; seek.type = PlayEventType::seek; seek.positionMs = 123; s.enqueue(std::move(seek));
    require(s.queue_.size() == 4, "Seek cannot merge across pause");
    for (int i = 0; i < 1100; ++i) {
        Event e; e.type = PlayEventType::start; e.eventUid = std::to_string(i);
        s.enqueue(std::move(e));
    }
    require(s.queue_.size() == 1025 && s.queue_.front().analysis &&
            s.queue_.front().analysis->resultJson == "retained data keep-store",
            "Playback cap preserves non-playback writes");
    require(s.queue_.back().eventUid == "1099", "Newest playback retained");
}
void stoppedAdmission() {
    Service s; int calls = 0;
    auto callback = [&](auto p) {
        require(!p.found && !p.error.empty() && p.trackPath == L"rejected",
                "Rejection reports original path and error"); ++calls;
    };
    s.loadPacketCost("rejected", callback);
    require(calls == 1 && s.queue_.empty(), "Not-started callback completes once");
    s.started_ = true; s.stopping_ = true;
    s.loadPacketCost("rejected", callback);
    require(calls == (baseline ? 1 : 2) && s.queue_.empty(),
            "Stopping admission completes callback once");
    std::cout << "rejected_callbacks=" << calls << '\n';
    s.stopping_ = false;
    s.loadPacketCost("accepted", {});
    require(loads(s) == 1, "Restart admission works");
}
struct DestructionProbe {
    std::mutex* mutex;
    bool* outsideLock;
    ~DestructionProbe() {
        std::thread check([&] {
            *outsideLock = mutex->try_lock();
            if (*outsideLock) mutex->unlock();
        });
        check.join();
    }
};
void closureDestruction() {
    Service s; s.started_ = true;
    bool outsideLock = false;
    auto owner = std::make_shared<DestructionProbe>();
    owner->mutex = &s.queueMutex_; owner->outsideLock = &outsideLock;
    std::weak_ptr<DestructionProbe> weak = owner;
    s.loadPacketCost("old", [owner](auto) {}); owner.reset();
    s.loadPacketCost("new", {});
    require(weak.expired() == !baseline, "Old closure released promptly");
    if (!baseline) require(outsideLock, "Old closure destroyed outside queue lock");
    s.queue_.clear(); // Explicit before local result flag dies.
}
void producers() {
    Service s; s.started_ = true;
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.emplace_back([&, t] {
        for (int i = 0; i < 250; ++i) {
            s.loadPacketCost(std::to_string(t * 250 + i), {});
        }
    });
    for (auto& t : threads) t.join();
    require(loads(s) == (baseline ? 1000 : 1), "Concurrent producers preserve single pending");
    int current = 0; s.loadPacketCost("final", [&](auto) { ++current; });
    while (!s.queue_.empty()) finish(s, pop(s));
    require(current == 1, "Latest after concurrent producers completes");
}
void admissionCost() {
    for (std::size_t n : {0u, 1024u, 10000u}) {
      for (const auto position : {"head", "middle", "tail", "absent"}) {
        Service s; s.started_ = true;
        for (std::size_t i = 0; i < n; ++i) s.queue_.push_back(analysis(Kind::storeDeep, "sentinel"));
        std::vector<double> samples;
        for (int i = 0; i < 300; ++i) {
            const std::string placement(position);
            const auto oldIndex = placement == "head" ? 0u : placement == "middle" ? n / 2 : n;
            if (placement != "absent") {
                s.loadPacketCost("prior", {});
                auto old = std::move(s.queue_.back()); s.queue_.pop_back();
                s.queue_.insert(s.queue_.begin() + oldIndex, std::move(old));
            }
            const auto begin = std::chrono::steady_clock::now();
            s.loadPacketCost("current", {});
            samples.push_back(std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - begin).count());
            s.queue_.pop_back();
            if (baseline && placement != "absent") s.queue_.erase(s.queue_.begin() + oldIndex);
        }
        std::sort(samples.begin(), samples.end());
        std::cout << "admission_other_events=" << n << " old_position=" << position << " p50_us=" << samples[150]
                  << " p95_us=" << samples[285] << '\n';
        require(s.queue_.size() == n, "Timing leaves unrelated events intact");
      }
    }
}
void rebindAdmission() {
    for (int mode=0;mode<3;++mode) {
        Service s;s.started_=mode!=0;s.stopping_=mode==1;
        int calls=0;bool okay=false;
        s.rebindTagRevisions({{"previous",L"current.flac",7}},[&](bool success,std::string error){
            ++calls;okay=success;require(success||!error.empty(),"Rejected rebind includes reason");});
        require(calls==0,"Rebind completion is deferred from admission");
        auto callbacks=std::move(qa::callbacks);qa::callbacks.clear();for(auto& callback:callbacks)callback();
        if(mode<2){require(s.queue_.empty(),"Rejected binding not queued");
            require(calls==(mode==0||REBIND_CALLBACK_FIXED?1:0)&&!okay,"Stopped admission completes exactly once when fixed");}
        else {require(s.queue_.size()==1&&calls==0,"Admitted binding owns callback in worker queue");
            auto event=pop(s);require(event.revisionRebind&&event.revisionRebind->revisions.size()==1,"Binding payload retained");
            event.revisionRebind->callback(true,"");require(calls==1&&okay,"Admitted callback completes once");}
    }
}
int main(int argc, char** argv) {
    baseline = argc > 1 && std::string(argv[1]) == "baseline";
    try {
        burst(); inFlightAndSamePath(); mixedQueue(); playbackContracts();
        stoppedAdmission(); closureDestruction(); producers(); admissionCost();rebindAdmission();
        std::cout << "PASS checks=" << checks << " mode=" << (baseline ? "baseline" : "candidate") << '\n';
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
'''


if __name__ == "__main__":
    main()
