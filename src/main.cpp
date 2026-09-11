#include <httplib.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <thread>
#include <csignal>
#include <ctime>
#include <random>

#include "metrics.h"
#include "fault_injector.h"
#include "log_analyzer.h"
#include "ai_client.h"
#include "trace.h"

using json = nlohmann::json;
using namespace servicescope;

// Global state
static Metrics g_metrics;
static LogAnalyzer g_analyzer;
static TraceCollector g_tracer;
static std::atomic<bool> g_running{true};
static std::string g_base_url = "http://localhost:8080";
thread_local std::mt19937 g_rng(std::random_device{}());

// Structured logging
void log_event(const std::string& level, const std::string& message,
               const std::string& path = "", int latency_ms = 0,
               const std::string& trace_id = "") {
    auto t = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(t);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&time_t));

    g_analyzer.add_log({ts, level, message, latency_ms, path, trace_id});

    std::cout << "[" << ts << "] [" << level << "] " << message;
    if (!path.empty()) std::cout << " path=" << path;
    if (latency_ms > 0) std::cout << " latency=" << latency_ms << "ms";
    if (!trace_id.empty()) std::cout << " trace=" << trace_id;
    std::cout << std::endl;
}

// Middleware: wrap each handler with metrics + fault injection
using Handler = std::function<void(const httplib::Request&, httplib::Response&)>;

Handler with_observability(Handler next) {
    return [next](const httplib::Request& req, httplib::Response& res) {
        auto trace_start = std::chrono::steady_clock::now();
        Trace trace;
        trace.trace_id = g_tracer.next_trace_id();
        trace.endpoint = req.path;
        trace.timestamp = TraceCollector::now_str();

        auto& fault = FaultInjector::instance();

        // --- Fault inject span ---
        auto fault_start = std::chrono::steady_clock::now();
        int fault_code = fault.check_request();
        auto fault_end = std::chrono::steady_clock::now();

        Span fault_span;
        fault_span.span_id = g_tracer.next_span_id();
        fault_span.name = "fault_inject";
        fault_span.start_offset_us = std::chrono::duration_cast<std::chrono::microseconds>(
            fault_start - trace_start).count();
        fault_span.duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
            fault_end - fault_start).count();
        fault_span.status = (fault_code == 0) ? "ok" : "error";
        trace.spans.push_back(fault_span);

        if (fault_code == -1) {
            res.set_header("Connection", "close");
            res.status = 444;
            trace.http_status = 444;
            trace.total_duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - trace_start).count();
            g_tracer.store(trace);
            return;
        }
        if (fault_code > 0) {
            json err = {{"error", "Fault injected"}, {"code", fault_code}};
            res.set_content(err.dump(), "application/json");
            res.status = fault_code;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - trace_start).count();
            g_metrics.record_request(req.path, static_cast<int>(elapsed), true, trace.trace_id);
            log_event("WARN", "Fault injected error", req.path, 0, trace.trace_id);
            trace.http_status = fault_code;
            trace.total_duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - trace_start).count();
            g_tracer.store(trace);
            return;
        }

        // --- Handler span ---
        auto handler_start = std::chrono::steady_clock::now();
        try {
            next(req, res);
        } catch (const std::exception& e) {
            res.status = 500;
            json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
            log_event("ERROR", std::string("Exception: ") + e.what(), req.path, 0, trace.trace_id);
        }
        auto handler_end = std::chrono::steady_clock::now();

        Span handler_span;
        handler_span.span_id = g_tracer.next_span_id();
        handler_span.parent_span_id = fault_span.span_id;
        handler_span.name = "handler:" + std::string(req.path);
        handler_span.start_offset_us = std::chrono::duration_cast<std::chrono::microseconds>(
            handler_start - trace_start).count();
        handler_span.duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
            handler_end - handler_start).count();
        handler_span.status = (res.status >= 400) ? "error" : "ok";
        trace.spans.push_back(handler_span);

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - trace_start).count();
        bool is_error = (res.status >= 400);
        g_metrics.record_request(req.path, static_cast<int>(elapsed), is_error, trace.trace_id);

        if (is_error) {
            log_event("ERROR", "Request failed with status " + std::to_string(res.status),
                      req.path, static_cast<int>(elapsed), trace.trace_id);
        } else if (elapsed > 200) {
            log_event("WARN", "Slow request", req.path, static_cast<int>(elapsed), trace.trace_id);
        }

        trace.http_status = (res.status == -1) ? 200 : res.status;
        trace.total_duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - trace_start).count();
        g_tracer.store(trace);
    };
}

// Helper: build metrics JSON
json metrics_json() {
    auto s = g_metrics.get_snapshot();
    json j;

    // Summary
    j["total_requests"] = s.total_requests;
    j["total_errors"] = s.total_errors;
    j["active_connections"] = s.active_connections;
    j["uptime_seconds"] = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    // QPS
    j["qps"] = {
        {"1s", s.qps_1s},
        {"5s", s.qps_5s},
        {"60s", s.qps_60s}
    };

    // Latency
    j["latency"] = {
        {"avg_ms", s.avg_latency_ms},
        {"p50_ms", s.p50_ms},
        {"p90_ms", s.p90_ms},
        {"p99_ms", s.p99_ms},
        {"p999_ms", s.p999_ms}
    };

    // Error rate
    j["error_rate_pct"] = s.error_rate_pct;

    // Histogram
    json hist = json::array();
    for (int i = 0; i < 16; ++i) {
        hist.push_back({
            {"bound_ms", s.bucket_bounds[i]},
            {"count", s.latency_buckets[i]}
        });
    }
    j["latency_histogram"] = hist;

    // Slow requests (last 20)
    json slow = json::array();
    int count = 0;
    for (const auto& sr : s.slow_requests) {
        if (count++ >= 20) break;
        slow.push_back({
            {"path", sr.path},
            {"latency_ms", sr.latency_ms},
            {"timestamp", sr.timestamp},
            {"trace_id", sr.trace_id}
        });
    }
    j["slow_requests"] = slow;

    return j;
}

#include "dashboard_html.h"

// ========== Main Server ==========
int main(int argc, char* argv[]) {
    int port = 8080;
    int threads = std::thread::hardware_concurrency();
    if (threads < 4) threads = 4;

    if (argc > 1) port = std::atoi(argv[1]);
    if (argc > 2) threads = std::atoi(argv[2]);
    g_base_url = "http://localhost:" + std::to_string(port);

    std::cout << R"(
  ServiceScope v1.0.0
  High-Concurrency Service Monitor
  ================================
)" << std::endl;

    httplib::Server svr;

    // ---- Signal handling (must be before listen) ----
    #ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
    #endif
    signal(SIGINT, [](int) {
        g_running.store(false);
    });

    // Connection tracking
    svr.set_pre_routing_handler([](const httplib::Request&, httplib::Response&) {
        g_metrics.increment_connection();
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // ---- Dashboard ----
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(DASHBOARD_HTML, "text/html");
    });

    // ---- Metrics API ----
    svr.Get("/metrics", with_observability([](const httplib::Request&, httplib::Response& res) {
        res.set_content(metrics_json().dump(), "application/json");
    }));

    // ---- Health check ----
    svr.Get("/api/health", with_observability([](const httplib::Request&, httplib::Response& res) {
        auto s = g_metrics.get_snapshot();
        json health = {
            {"status", s.error_rate_pct > 10.0 ? "unhealthy" : "healthy"},
            {"active_connections", s.active_connections},
            {"qps", s.qps_5s}
        };
        res.set_content(health.dump(), "application/json");
    }));

    // ---- Work simulation endpoints ----
    // /api/work?delay=N — simulate work with configurable delay
    svr.Get("/api/work", with_observability([](const httplib::Request& req, httplib::Response& res) {
        int delay_ms = 0;
        if (req.has_param("delay")) {
            delay_ms = std::atoi(req.get_param_value("delay").c_str());
            if (delay_ms < 0) delay_ms = 0;
            if (delay_ms > 10000) delay_ms = 10000;
        }

        if (delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }

        json result = {
            {"status", "ok"},
            {"work_delay_ms", delay_ms},
            {"thread_id", std::hash<std::thread::id>{}(std::this_thread::get_id())}
        };
        res.set_content(result.dump(), "application/json");
    }));

    // /api/data — synthetic data payload
    svr.Get("/api/data", with_observability([](const httplib::Request& req, httplib::Response& res) {
        int count = 100;
        if (req.has_param("count")) {
            count = std::atoi(req.get_param_value("count").c_str());
            if (count < 1) count = 1;
            if (count > 10000) count = 10000;
        }

        std::uniform_real_distribution<double> dist(0.0, 100.0);
        json arr = json::array();
        for (int i = 0; i < count; ++i) {
            arr.push_back({
                {"id", i},
                {"value", std::round(dist(g_rng) * 100.0) / 100.0},
                {"category", std::string(1, 'A' + (i % 5))}
            });
        }

        json result = {{"count", count}, {"data", arr}};
        res.set_content(result.dump(), "application/json");
    }));

    // /api/random — random delay simulating real-world variance
    svr.Get("/api/random", with_observability([](const httplib::Request&, httplib::Response& res) {
        std::uniform_int_distribution<int> dist(0, 200);
        int delay = dist(g_rng);

        if (delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }

        res.set_content(R"({"status":"ok","random_delay_ms":)" + std::to_string(delay) + "}", "application/json");
    }));

    // ---- Fault Injection API ----
    svr.Get("/api/fault/config", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(FaultInjector::instance().status_json(), "application/json");
    });

    svr.Post("/api/fault/config", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = json::parse(req.body);
            auto& cfg = FaultInjector::instance().config();

            if (j.contains("enabled")) cfg.enabled.store(j["enabled"].get<bool>());
            if (j.contains("delay_ms")) {
                int v = j["delay_ms"].get<int>();
                if (v < 0 || v > 30000) {
                    res.status = 400;
                    res.set_content(json{{"error", "delay_ms must be 0-30000"}}.dump(), "application/json");
                    return;
                }
                cfg.delay_ms.store(v);
            }
            if (j.contains("delay_jitter_ms")) {
                int v = j["delay_jitter_ms"].get<int>();
                if (v < 0 || v > 10000) {
                    res.status = 400;
                    res.set_content(json{{"error", "delay_jitter_ms must be 0-10000"}}.dump(), "application/json");
                    return;
                }
                cfg.delay_jitter_ms.store(v);
            }
            if (j.contains("error_rate")) {
                double v = j["error_rate"].get<double>();
                if (v < 0.0 || v > 1.0) {
                    res.status = 400;
                    res.set_content(json{{"error", "error_rate must be 0.0-1.0"}}.dump(), "application/json");
                    return;
                }
                cfg.error_rate.store(v);
            }
            if (j.contains("error_http_code")) {
                int v = j["error_http_code"].get<int>();
                if (v < 400 || v > 599) {
                    res.status = 400;
                    res.set_content(json{{"error", "error_http_code must be 400-599"}}.dump(), "application/json");
                    return;
                }
                cfg.error_http_code.store(v);
            }
            if (j.contains("drop_rate")) {
                double v = j["drop_rate"].get<double>();
                if (v < 0.0 || v > 1.0) {
                    res.status = 400;
                    res.set_content(json{{"error", "drop_rate must be 0.0-1.0"}}.dump(), "application/json");
                    return;
                }
                cfg.drop_rate.store(v);
            }
            if (j.contains("pause")) cfg.pause.store(j["pause"].get<bool>());

            log_event("INFO", "Fault configuration updated");
            res.set_content(FaultInjector::instance().status_json(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // ---- Logs & Analysis API ----
    svr.Get("/api/logs", [](const httplib::Request& req, httplib::Response& res) {
        int seconds = 300;
        if (req.has_param("seconds")) {
            seconds = std::atoi(req.get_param_value("seconds").c_str());
            if (seconds < 1) seconds = 1;
            if (seconds > 3600) seconds = 3600;
        }

        auto logs = g_analyzer.recent_logs(seconds);
        json arr = json::array();
        int count = 0;
        for (const auto& l : logs) {
            if (count++ >= 200) break;
            arr.push_back({
                {"timestamp", l.timestamp},
                {"level", l.level},
                {"message", l.message},
                {"path", l.path},
                {"latency_ms", l.latency_ms},
                {"trace_id", l.trace_id}
            });
        }
        res.set_content(arr.dump(), "application/json");
    });

    svr.Get("/api/logs/analysis", [](const httplib::Request&, httplib::Response& res) {
        auto snapshot = g_metrics.get_snapshot();
        std::string summary = g_analyzer.ai_summary(snapshot);

        json result = {
            {"summary", summary},
            {"anomaly_count", g_analyzer.analyze(snapshot).size()},
            {"log_count", g_analyzer.log_count()},
            {"generated_at", []{
                auto t = std::chrono::system_clock::now();
                auto time_t = std::chrono::system_clock::to_time_t(t);
                char buf[32];
                std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&time_t));
                return std::string(buf);
            }()}
        };
        res.set_content(result.dump(), "application/json");
    });

    svr.Get("/api/logs/anomalies", [](const httplib::Request&, httplib::Response& res) {
        auto snapshot = g_metrics.get_snapshot();
        auto anomalies = g_analyzer.analyze(snapshot);
        json arr = json::array();
        for (const auto& a : anomalies) {
            arr.push_back({
                {"timestamp", a.timestamp},
                {"type", a.type},
                {"severity", a.severity},
                {"description", a.description},
                {"confidence", a.confidence}
            });
        }
        res.set_content(arr.dump(), "application/json");
    });

    // ---- Trace API ----
    svr.Get("/api/traces", [](const httplib::Request& req, httplib::Response& res) {
        int limit = 50;
        if (req.has_param("limit")) {
            limit = std::atoi(req.get_param_value("limit").c_str());
            if (limit < 1) limit = 1;
            if (limit > 500) limit = 500;
        }
        auto traces = g_tracer.list_recent(limit);
        json arr = json::array();
        for (const auto& t : traces) {
            json spans_json = json::array();
            for (const auto& s : t.spans) {
                json tags = json::array();
                for (const auto& kv : s.tags) {
                    tags.push_back({{"key", kv.first}, {"value", kv.second}});
                }
                spans_json.push_back({
                    {"span_id", s.span_id},
                    {"parent_span_id", s.parent_span_id},
                    {"name", s.name},
                    {"start_offset_us", s.start_offset_us},
                    {"duration_us", s.duration_us},
                    {"status", s.status},
                    {"tags", tags}
                });
            }
            arr.push_back({
                {"trace_id", t.trace_id},
                {"timestamp", t.timestamp},
                {"endpoint", t.endpoint},
                {"http_status", t.http_status},
                {"total_duration_us", t.total_duration_us},
                {"spans", spans_json}
            });
        }
        res.set_content(arr.dump(), "application/json");
    });

    svr.Get(R"(/api/traces/([a-zA-Z0-9_]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.matches[1];
        auto trace = g_tracer.get_by_id(id);
        if (trace.trace_id.empty()) {
            res.status = 404;
            res.set_content(R"({"error":"trace not found"})", "application/json");
            return;
        }
        json spans_json = json::array();
        for (const auto& s : trace.spans) {
            json tags = json::array();
            for (const auto& kv : s.tags) {
                tags.push_back({{"key", kv.first}, {"value", kv.second}});
            }
            spans_json.push_back({
                {"span_id", s.span_id},
                {"parent_span_id", s.parent_span_id},
                {"name", s.name},
                {"start_offset_us", s.start_offset_us},
                {"duration_us", s.duration_us},
                {"status", s.status},
                {"tags", tags}
            });
        }
        json j = {
            {"trace_id", trace.trace_id},
            {"timestamp", trace.timestamp},
            {"endpoint", trace.endpoint},
            {"http_status", trace.http_status},
            {"total_duration_us", trace.total_duration_us},
            {"spans", spans_json}
        };
        res.set_content(j.dump(), "application/json");
    });

    // ---- AI Model Analysis ----
    svr.Get("/api/ai/config", [](const httplib::Request&, httplib::Response& res) {
        auto& ai = AiClient::instance();
        json cfg = {
            {"enabled", ai.is_enabled()},
            {"endpoint", ai.config().endpoint},
            {"model", ai.config().model},
            {"timeout_secs", ai.config().timeout_secs},
            {"has_api_key", !ai.config().api_key.empty()}
        };
        res.set_content(cfg.dump(), "application/json");
    });

    svr.Post("/api/ai/config", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = json::parse(req.body);
            AiClient::instance().configure(
                j.value("endpoint", ""),
                j.value("api_key", ""),
                j.value("model", "gpt-4o-mini"),
                j.value("timeout", 30)
            );
            auto& ai = AiClient::instance();
            json cfg = {
                {"enabled", ai.is_enabled()},
                {"endpoint", ai.config().endpoint},
                {"model", ai.config().model}
            };
            log_event("INFO", "AI configuration updated");
            res.set_content(cfg.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Get("/api/ai/test", [](const httplib::Request&, httplib::Response& res) {
        auto& ai = AiClient::instance();
        if (!ai.is_enabled()) {
            res.set_content(json{
                {"ok", false},
                {"error", "AI not configured. Set AI_ENDPOINT + AI_API_KEY env vars, or POST /api/ai/config"}
            }.dump(), "application/json");
            return;
        }

        auto result = ai.test();
        json r = {
            {"ok", result.ok},
            {"model_used", result.model_used},
            {"latency_ms", result.latency_ms},
            {"preview", result.ok ? result.content.substr(0, 300) : ""},
            {"error", result.error}
        };
        res.set_content(r.dump(), "application/json");
    });

    svr.Post("/api/ai/analyze", [](const httplib::Request&, httplib::Response& res) {
        auto& ai = AiClient::instance();
        if (!ai.is_enabled()) {
            json fallback = {
                {"source", "heuristic"},
                {"message", "AI not configured — using heuristic analysis instead. Set AI_ENDPOINT + AI_API_KEY."},
                {"summary", g_analyzer.ai_summary(g_metrics.get_snapshot())}
            };
            res.set_content(fallback.dump(), "application/json");
            return;
        }

        auto snapshot = g_metrics.get_snapshot();
        auto anomalies = g_analyzer.analyze(snapshot);
        auto logs = g_analyzer.recent_logs(300);

        // Build JSON inputs for AI
        json m_json = metrics_json();
        json anomalies_json = json::array();
        for (const auto& a : anomalies) {
            anomalies_json.push_back({
                {"type", a.type}, {"severity", a.severity},
                {"description", a.description}, {"confidence", a.confidence}
            });
        }
        json logs_json = json::array();
        int log_count = 0;
        for (const auto& l : logs) {
            if (log_count++ >= 50) break;
            logs_json.push_back({
                {"timestamp", l.timestamp}, {"level", l.level},
                {"message", l.message}, {"path", l.path},
                {"trace_id", l.trace_id}
            });
        }

        auto result = ai.analyze_via_arg(m_json, anomalies_json, logs_json, g_base_url);

        json response = {
            {"source", "ai"},
            {"model", result.model_used},
            {"latency_ms", result.latency_ms},
            {"ok", result.ok},
            {"iterations", result.iterations},
            {"tool_calls", result.tool_calls}
        };

        if (result.ok) {
            response["analysis"] = result.content;
            log_event("INFO", "AI analysis completed, latency=" + std::to_string(result.latency_ms) + "ms");
        } else {
            response["error"] = result.error;
            response["fallback_summary"] = g_analyzer.ai_summary(snapshot);
            log_event("WARN", "AI analysis failed: " + result.error);
        }

        res.set_content(response.dump(), "application/json");
    });

    // Update /api/logs/analysis to also show AI config hint
    // (The original endpoint is above; we keep it for backward compat)

    // ---- Reset ----
    svr.Post("/api/reset", [](const httplib::Request&, httplib::Response& res) {
        g_metrics.reset();
        log_event("INFO", "Metrics reset");
        res.set_content(R"({"status":"ok","message":"Metrics reset"})", "application/json");
    });

    // Connection tracking cleanup
    svr.set_post_routing_handler([](const httplib::Request&, httplib::Response&) {
        g_metrics.decrement_connection();
    });

    // Thread pool
    svr.new_task_queue = [threads] { return new httplib::ThreadPool(threads); };

    std::cout << "Listening on http://0.0.0.0:" << port << std::endl;
    std::cout << "Worker threads: " << threads << std::endl;
    std::cout << "Dashboard: http://localhost:" << port << "/\n" << std::endl;

    // Non-blocking listen so signal handler can stop us
    auto listen_thread = std::thread([&svr, port]() {
        svr.listen("0.0.0.0", port);
    });

    std::cout << "Press Ctrl+C to stop.\n" << std::endl;

    // Wait for shutdown signal
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\nShutting down gracefully..." << std::endl;
    svr.stop();
    if (listen_thread.joinable()) {
        listen_thread.join();
    }
    std::cout << "Server stopped. Goodbye." << std::endl;

    return 0;
}
