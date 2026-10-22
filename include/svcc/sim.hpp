// Discrete-event scheduler with virtual time. SIP signalling, RTP/RTCP media and the
// contact-center timers all run on it, so a minute of calls executes in milliseconds and
// every run with the same seed produces the same packets.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <utility>

namespace svcc {

using Micros = std::int64_t;

constexpr Micros msec(std::int64_t v) { return v * 1000; }

class Scheduler {
public:
    using TaskId = std::uint64_t;

    Micros now() const { return now_; }

    TaskId schedule(Micros delay, std::function<void()> fn) {
        TaskId id = ++seq_;
        Micros at = now_ + (delay < 0 ? 0 : delay);
        queue_.emplace(std::make_pair(at, id), std::move(fn));
        index_[id] = at;
        return id;
    }

    void cancel(TaskId id) {
        auto it = index_.find(id);
        if (it == index_.end()) return;
        queue_.erase(std::make_pair(it->second, id));
        index_.erase(it);
    }

    // Repeats fn every period until it returns false.
    void every(Micros period, std::function<bool()> fn) {
        schedule(period, [this, period, fn]() {
            if (fn()) every(period, fn);
        });
    }

    // Runs every task due at or before t, in time order, then sets the clock to t.
    void runUntil(Micros t) {
        while (!queue_.empty() && queue_.begin()->first.first <= t) {
            auto node = queue_.extract(queue_.begin());
            index_.erase(node.key().second);
            now_ = node.key().first;
            node.mapped()();
        }
        if (t > now_) now_ = t;
    }

    std::size_t pending() const { return queue_.size(); }

private:
    Micros now_ = 0;
    TaskId seq_ = 0;
    std::map<std::pair<Micros, TaskId>, std::function<void()>> queue_;
    std::unordered_map<TaskId, Micros> index_;
};

}  // namespace svcc
