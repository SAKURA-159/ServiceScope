#pragma once
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <sstream>
#include "metrics.h"

namespace servicescope {

struct LogEntry {
    std::string timestamp;
    std::string level;    // INFO, WARN, ERROR
    std::string message;
    int latency_ms;
    std::string path;
};

struct Anomaly {
    std::string timestamp;
    std::string type;         // latency_spike, error_spike, connection_flood, slow_request_surge
    std::string severity;     // low, medium, high, critical
    std::string description;
    double confidence;        // 0.0 - 1.0
};

class LogAnalyzer {
public:
    LogAnalyzer() = default;

    void add_log(const LogEntry& entry) {
        std::lock_guard<std::mutex> lock(mutex_);
        logs_.push_back(entry);
        if (logs_.size() > kMaxLogs) {
            logs_.pop_front();
        }
    }

    std::vector<Anomaly> analyze(const Metrics::Snapshot& snapshot) {
        std::vector<Anomaly> anomalies;

        // Baseline tracking
        add_qps_sample(snapshot.qps_5s);
        add_error_sample(snapshot.error_rate_pct);
        add_latency_sample(snapshot.avg_latency_ms);

        // Anomaly 1: QPS spike
        if (baseline_qps_ > 10 && snapshot.qps_5s > baseline_qps_ * 3.0) {
            double conf = std::min(1.0, (snapshot.qps_5s / baseline_qps_ - 1.0) / 5.0);
            std::string desc = "QPS spike detected: " + std::to_string(static_cast<int>(snapshot.qps_5s))
                + " req/s vs baseline " + std::to_string(static_cast<int>(baseline_qps_)) + " req/s";
            anomalies.push_back({now_str(), "qps_spike",
                snapshot.qps_5s > baseline_qps_ * 5.0 ? "critical" : "high", desc, conf});
        }

        // Anomaly 2: Error rate spike
        if (baseline_error_rate_ > 0.1 && snapshot.error_rate_pct > baseline_error_rate_ * 5.0) {
            double conf = std::min(1.0, (snapshot.error_rate_pct / std::max(0.1, baseline_error_rate_) - 1.0) / 10.0);
            std::string desc = "Error rate spike: " + std::to_string(snapshot.error_rate_pct)
                + "% vs baseline " + std::to_string(baseline_error_rate_) + "%";
            std::string sev = snapshot.error_rate_pct > 50.0 ? "critical" :
                              snapshot.error_rate_pct > 20.0 ? "high" : "medium";
            anomalies.push_back({now_str(), "error_spike", sev, desc, conf});
        } else if (snapshot.error_rate_pct > 10.0) {
            std::string desc = "Elevated error rate: " + std::to_string(snapshot.error_rate_pct) + "%";
            anomalies.push_back({now_str(), "error_spike", "high", desc, snapshot.error_rate_pct / 100.0});
        }

        // Anomaly 3: Latency spike
        if (baseline_latency_ms_ > 2.0 && snapshot.avg_latency_ms > baseline_latency_ms_ * 3.0) {
            double conf = std::min(1.0, (snapshot.avg_latency_ms / baseline_latency_ms_ - 1.0) / 4.0);
            std::string desc = "Latency spike: avg " + std::to_string(static_cast<int>(snapshot.avg_latency_ms))
                + "ms vs baseline " + std::to_string(static_cast<int>(baseline_latency_ms_)) + "ms";
            std::string sev = snapshot.avg_latency_ms > baseline_latency_ms_ * 10.0 ? "critical" : "high";
            anomalies.push_back({now_str(), "latency_spike", sev, desc, conf});
        }

        // Anomaly 4: P99 latency degradation
        if (snapshot.p99_ms > 1000) {
            std::string desc = "P99 latency critical: " + std::to_string(snapshot.p99_ms) + "ms";
            anomalies.push_back({now_str(), "latency_spike", "critical", desc,
                std::min(1.0, snapshot.p99_ms / 5000.0)});
        } else if (snapshot.p99_ms > 500) {
            std::string desc = "P99 latency elevated: " + std::to_string(snapshot.p99_ms) + "ms";
            anomalies.push_back({now_str(), "latency_spike", "high", desc, 0.7});
        }

        // Anomaly 5: Connection count
        if (snapshot.active_connections > 500) {
            anomalies.push_back({now_str(), "connection_flood", "critical",
                "Very high connection count: " + std::to_string(snapshot.active_connections), 0.95});
        } else if (snapshot.active_connections > 200) {
            anomalies.push_back({now_str(), "connection_flood", "high",
                "High connection count: " + std::to_string(snapshot.active_connections), 0.8});
        }

        // Anomaly 6: Slow request surge
        int recent_slow = static_cast<int>(snapshot.slow_requests.size());
        if (recent_slow > 100) {
            anomalies.push_back({now_str(), "slow_request_surge", "critical",
                "Slow request queue overflow: " + std::to_string(recent_slow) + " entries", 0.95});
        } else if (recent_slow > 50) {
            anomalies.push_back({now_str(), "slow_request_surge", "high",
                "Many slow requests: " + std::to_string(recent_slow) + " entries", 0.75});
        }

        // Log-based heuristics (inline to avoid recursive lock)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            int err_count = 0;
            for (const auto& l : logs_) {
                if (l.level == "ERROR") ++err_count;
            }
            if (err_count > 50) {
                anomalies.push_back({now_str(), "error_spike", "critical",
                    "Log flood: " + std::to_string(err_count) + " errors in recent logs",
                    std::min(1.0, err_count / 100.0)});
            }
        }

        return anomalies;
    }

    std::vector<LogEntry> recent_logs([[maybe_unused]] int seconds = 60) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (logs_.empty()) return {};
        std::vector<LogEntry> result;
        for (const auto& l : logs_) {
            result.push_back(l);
        }
        if (result.size() > 500) {
            result.erase(result.begin(), result.end() - 500);
        }
        return result;
    }

    std::string ai_summary(const Metrics::Snapshot& snapshot) {
        auto anomalies = analyze(snapshot);
        if (anomalies.empty()) {
            return "System operating normally. No anomalies detected.";
        }

        std::ostringstream oss;
        oss << "## AI Log Analysis Summary\n\n";
        oss << "**System Status**: " << (anomalies.size() > 2 ? "DEGRADED" : "WARNING") << "\n\n";
        oss << "**Detected " << anomalies.size() << " anomalies**:\n\n";

        // Group by severity
        int critical = 0, high = 0, medium = 0;
        for (const auto& a : anomalies) {
            if (a.severity == "critical") ++critical;
            else if (a.severity == "high") ++high;
            else ++medium;
        }

        oss << "| Severity | Count |\n|----------|------|\n";
        if (critical) oss << "| Critical | " << critical << " |\n";
        if (high) oss << "| High     | " << high << " |\n";
        if (medium) oss << "| Medium   | " << medium << " |\n";

        oss << "\n**Top Anomalies**:\n\n";
        int shown = 0;
        for (const auto& a : anomalies) {
            if (shown++ >= 8) break;
            std::string emoji = a.severity == "critical" ? "🔴" :
                                a.severity == "high" ? "🟠" : "🟡";
            oss << "- " << emoji << " [" << a.type << "] " << a.description
                << " (confidence: " << static_cast<int>(a.confidence * 100) << "%)\n";
        }

        oss << "\n**Recommendations**:\n\n";
        if (critical > 0) {
            oss << "- Immediately check fault injection settings and recent deployments\n";
            oss << "- Review error logs for root cause patterns\n";
        }
        if (high > 0 || critical > 0) {
            oss << "- Monitor connection pool and thread utilization\n";
            oss << "- Consider enabling rate limiting if QPS continues to rise\n";
        }
        oss << "- Run `/api/logs/analysis` for detailed breakdown\n";

        return oss.str();
    }

    size_t log_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return logs_.size();
    }

private:
    void add_qps_sample(double qps) {
        qps_samples_.push_back(qps);
        if (qps_samples_.size() > kBaselineSamples) qps_samples_.pop_front();
        baseline_qps_ = std::accumulate(qps_samples_.begin(), qps_samples_.end(), 0.0) / qps_samples_.size();
    }

    void add_error_sample(double err_rate) {
        err_samples_.push_back(err_rate);
        if (err_samples_.size() > kBaselineSamples) err_samples_.pop_front();
        baseline_error_rate_ = std::accumulate(err_samples_.begin(), err_samples_.end(), 0.0) / err_samples_.size();
    }

    void add_latency_sample(double lat) {
        lat_samples_.push_back(lat);
        if (lat_samples_.size() > kBaselineSamples) lat_samples_.pop_front();
        baseline_latency_ms_ = std::accumulate(lat_samples_.begin(), lat_samples_.end(), 0.0) / lat_samples_.size();
    }

    std::string now_str() const {
        auto t = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(t);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&time_t));
        return buf;
    }

    mutable std::mutex mutex_;
    std::deque<LogEntry> logs_;
    static constexpr size_t kMaxLogs = 10000;

    std::deque<double> qps_samples_;
    std::deque<double> err_samples_;
    std::deque<double> lat_samples_;
    static constexpr size_t kBaselineSamples = 30;

    double baseline_qps_ = 0.0;
    double baseline_error_rate_ = 0.0;
    double baseline_latency_ms_ = 0.0;
};

} // namespace servicescope
