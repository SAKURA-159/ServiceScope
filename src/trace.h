#pragma once
#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace servicescope {

struct Span {
    std::string span_id;
    std::string name;
    uint64_t start_offset_us = 0;
    uint64_t duration_us = 0;
    std::string status; // "ok" or "error"
    std::vector<std::pair<std::string, std::string>> tags;
};

struct Trace {
    std::string trace_id;
    std::string timestamp;
    std::string endpoint;
    int http_status = 0;
    uint64_t total_duration_us = 0;
    std::vector<Span> spans;
};

class TraceCollector {
public:
    void store(const Trace& trace) {
        std::lock_guard<std::mutex> lock(mutex_);
        traces_.push_back(trace);
        if (traces_.size() > kMaxTraces) {
            traces_.pop_front();
        }
    }

    std::string next_trace_id() {
        return "trace_" + std::to_string(trace_id_counter_.fetch_add(1));
    }

    std::string next_span_id() {
        return "span_" + std::to_string(span_id_counter_.fetch_add(1));
    }

    std::vector<Trace> list_recent(int limit = 50) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Trace> result;
        int n = std::min(limit, static_cast<int>(traces_.size()));
        result.reserve(n);
        auto it = traces_.rbegin();
        for (int i = 0; i < n; ++i, ++it) {
            result.push_back(*it);
        }
        return result;
    }

    Trace get_by_id(const std::string& id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = traces_.rbegin(); it != traces_.rend(); ++it) {
            if (it->trace_id == id) return *it;
        }
        return Trace{};
    }

    size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return traces_.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        traces_.clear();
    }

    static std::string now_str() {
        auto t = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(t);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&time_t));
        return buf;
    }

private:
    mutable std::mutex mutex_;
    std::deque<Trace> traces_;
    static constexpr size_t kMaxTraces = 500;

    std::atomic<uint64_t> trace_id_counter_{0};
    std::atomic<uint64_t> span_id_counter_{0};
};

} // namespace servicescope
