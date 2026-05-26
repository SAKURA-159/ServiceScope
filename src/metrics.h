#pragma once
#include <atomic>
#include <ctime>
#include <deque>
#include <mutex>
#include <chrono>
#include <string>
#include <vector>

namespace servicescope {

struct SlowRequest {
    std::string path;
    int latency_ms;
    std::string timestamp;
};

class Metrics {
public:
    Metrics() {
        for (int i = 0; i < kNumBuckets; ++i) {
            latency_buckets_[i].store(0);
        }
    }

    void record_request(const std::string& path, int latency_ms, bool is_error) {
        total_requests_.fetch_add(1);
        if (is_error) {
            total_errors_.fetch_add(1);
            std::lock_guard<std::mutex> lock(err_mutex_);
            error_timestamps_.push_back(std::chrono::steady_clock::now());
        }

        int bucket = bucket_index(latency_ms);
        latency_buckets_[bucket].fetch_add(1);

        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(ts_mutex_);
            request_timestamps_.push_back(now);
            if (request_timestamps_.size() > kMaxTimestamps) {
                request_timestamps_.pop_front();
            }
        }

        if (latency_ms > kSlowThresholdMs) {
            std::lock_guard<std::mutex> lock(slow_mutex_);
            auto t = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(t);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&time_t));
            slow_requests_.push_back({path, latency_ms, buf});
            if (slow_requests_.size() > kMaxSlowRequests) {
                slow_requests_.pop_front();
            }
        }
    }

    void increment_connection() { active_connections_.fetch_add(1); }
    void decrement_connection() { active_connections_.fetch_add(-1); }

    uint64_t total_requests() const { return total_requests_.load(); }
    uint64_t total_errors() const { return total_errors_.load(); }
    int active_connections() const { return active_connections_.load(); }

    double qps(int window_seconds = 1) const {
        auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(window_seconds);
        uint64_t count = 0;
        {
            std::lock_guard<std::mutex> lock(ts_mutex_);
            for (auto it = request_timestamps_.rbegin(); it != request_timestamps_.rend(); ++it) {
                if (*it >= cutoff) ++count;
                else break;
            }
        }
        return static_cast<double>(count) / window_seconds;
    }

    double error_rate(int window_seconds = 60) const {
        auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(window_seconds);

        uint64_t total_in_window = 0;
        {
            std::lock_guard<std::mutex> lock(ts_mutex_);
            for (auto it = request_timestamps_.rbegin(); it != request_timestamps_.rend(); ++it) {
                if (*it >= cutoff) ++total_in_window;
                else break;
            }
        }

        uint64_t errors_in_window = 0;
        {
            std::lock_guard<std::mutex> lock(err_mutex_);
            while (!error_timestamps_.empty() && error_timestamps_.front() < cutoff) {
                error_timestamps_.pop_front();
            }
            errors_in_window = error_timestamps_.size();
        }

        if (total_in_window == 0) return 0.0;
        return static_cast<double>(errors_in_window) / total_in_window;
    }

    int percentile(double p) const {
        uint64_t total = 0;
        uint64_t counts[kNumBuckets];
        for (int i = 0; i < kNumBuckets; ++i) {
            counts[i] = latency_buckets_[i].load();
            total += counts[i];
        }
        if (total == 0) return 0;

        uint64_t target = static_cast<uint64_t>(total * p / 100.0);
        uint64_t accum = 0;
        for (int i = 0; i < kNumBuckets; ++i) {
            accum += counts[i];
            if (accum >= target) return kBucketBounds[i];
        }
        return kBucketBounds[kNumBuckets - 1];
    }

    double avg_latency() const {
        uint64_t total_count = 0;
        double total_sum = 0.0;
        for (int i = 0; i < kNumBuckets; ++i) {
            uint64_t c = latency_buckets_[i].load();
            total_count += c;
            total_sum += c * static_cast<double>(kBucketBounds[i]);
        }
        if (total_count == 0) return 0.0;
        return total_sum / total_count;
    }

    std::vector<SlowRequest> recent_slow_requests() const {
        std::lock_guard<std::mutex> lock(slow_mutex_);
        return {slow_requests_.rbegin(), slow_requests_.rend()};
    }

    // Full metrics snapshot as JSON-serializable struct
    struct Snapshot {
        uint64_t total_requests;
        uint64_t total_errors;
        int active_connections;
        double qps_1s;
        double qps_5s;
        double qps_60s;
        double error_rate_pct;
        double avg_latency_ms;
        int p50_ms;
        int p90_ms;
        int p99_ms;
        int p999_ms;
        uint64_t latency_buckets[16];
        int bucket_bounds[16];
        std::vector<SlowRequest> slow_requests;
    };

    Snapshot get_snapshot() const {
        Snapshot s;
        s.total_requests = total_requests_.load();
        s.total_errors = total_errors_.load();
        s.active_connections = active_connections_.load();
        s.qps_1s = qps(1);
        s.qps_5s = qps(5);
        s.qps_60s = qps(60);
        s.error_rate_pct = error_rate(60) * 100.0;
        s.avg_latency_ms = avg_latency();
        s.p50_ms = percentile(50);
        s.p90_ms = percentile(90);
        s.p99_ms = percentile(99);
        s.p999_ms = percentile(99.9);
        for (int i = 0; i < kNumBuckets; ++i) {
            s.latency_buckets[i] = latency_buckets_[i].load();
            s.bucket_bounds[i] = kBucketBounds[i];
        }
        s.slow_requests = recent_slow_requests();
        return s;
    }

    void reset() {
        total_requests_.store(0);
        total_errors_.store(0);
        for (int i = 0; i < kNumBuckets; ++i) latency_buckets_[i].store(0);
        {
            std::lock_guard<std::mutex> l1(ts_mutex_);
            request_timestamps_.clear();
        }
        {
            std::lock_guard<std::mutex> l2(err_mutex_);
            error_timestamps_.clear();
        }
        {
            std::lock_guard<std::mutex> l3(slow_mutex_);
            slow_requests_.clear();
        }
    }

private:
    static int bucket_index(int latency_ms) {
        for (int i = 0; i < kNumBuckets; ++i) {
            if (latency_ms <= kBucketBounds[i]) return i;
        }
        return kNumBuckets - 1;
    }

    std::atomic<uint64_t> total_requests_{0};
    std::atomic<uint64_t> total_errors_{0};
    std::atomic<int32_t> active_connections_{0};
    std::atomic<uint64_t> latency_buckets_[16];

    static constexpr int kNumBuckets = 16;
    static constexpr int kBucketBounds[16] = {
        1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768
    };
    static constexpr size_t kMaxTimestamps = 100000;
    static constexpr size_t kMaxSlowRequests = 200;
    static constexpr int kSlowThresholdMs = 200;

    mutable std::mutex ts_mutex_;
    std::deque<std::chrono::steady_clock::time_point> request_timestamps_;

    mutable std::mutex err_mutex_;
    mutable std::deque<std::chrono::steady_clock::time_point> error_timestamps_;

    mutable std::mutex slow_mutex_;
    std::deque<SlowRequest> slow_requests_;
};

} // namespace servicescope
