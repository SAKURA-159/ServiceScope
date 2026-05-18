#pragma once
#include <atomic>
#include <cstdio>
#include <random>
#include <chrono>
#include <thread>
#include <string>

namespace servicescope {

struct FaultConfig {
    std::atomic<bool> enabled{false};
    std::atomic<int> delay_ms{0};           // base delay
    std::atomic<int> delay_jitter_ms{0};    // random jitter +/- this
    std::atomic<double> error_rate{0.0};    // 0.0 - 1.0, probability of error
    std::atomic<int> error_http_code{500};  // HTTP code for injected errors
    std::atomic<double> drop_rate{0.0};     // 0.0 - 1.0, connection drop probability
    std::atomic<bool> pause{false};         // pause all requests
};

class FaultInjector {
public:
    FaultInjector() : rng_(std::random_device{}()) {}

    FaultInjector(const FaultInjector&) = delete;
    FaultInjector& operator=(const FaultInjector&) = delete;

    static FaultInjector& instance() {
        static FaultInjector inst;
        return inst;
    }

    // Called before processing a request. Returns: 0=ok, >0=HTTP error code to return
    int check_request() {
        if (config_.pause.load()) {
            return 503; // Service Unavailable
        }
        if (!config_.enabled.load()) {
            return 0;
        }

        // Check drop rate
        double drop = config_.drop_rate.load();
        if (drop > 0.0) {
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            if (dist(rng_) < drop) {
                return -1; // special: drop connection
            }
        }

        // Check error rate
        double err = config_.error_rate.load();
        if (err > 0.0) {
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            if (dist(rng_) < err) {
                return config_.error_http_code.load();
            }
        }

        // Inject delay
        int delay = config_.delay_ms.load();
        int jitter = config_.delay_jitter_ms.load();
        if (delay > 0) {
            int total_delay = delay;
            if (jitter > 0) {
                std::uniform_int_distribution<int> dist(-jitter, jitter);
                total_delay += dist(rng_);
                if (total_delay < 0) total_delay = 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(total_delay));
        }

        return 0; // ok
    }

    FaultConfig& config() { return config_; }

    std::string status_json() const {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            R"({"enabled":%s,"delay_ms":%d,"delay_jitter_ms":%d,"error_rate":%.3f,"error_http_code":%d,"drop_rate":%.3f,"pause":%s})",
            config_.enabled.load() ? "true" : "false",
            config_.delay_ms.load(),
            config_.delay_jitter_ms.load(),
            config_.error_rate.load(),
            config_.error_http_code.load(),
            config_.drop_rate.load(),
            config_.pause.load() ? "true" : "false"
        );
        return buf;
    }

private:
    FaultConfig config_;
    std::mt19937 rng_;
};

} // namespace servicescope
