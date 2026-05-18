#pragma once
#include <string>
#include <cstdlib>
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace servicescope {

using json = nlohmann::json;

struct AiConfig {
    std::string endpoint;    // API endpoint URL
    std::string api_key;     // API key or "ollama" for local
    std::string model;       // model name
    int timeout_secs = 30;   // request timeout
    bool enabled = false;
};

struct AiResult {
    std::string content;     // AI response text
    std::string model_used;
    int latency_ms = 0;
    bool ok = false;
    std::string error;
};

class AiClient {
public:
    static AiClient& instance() {
        static AiClient inst;
        return inst;
    }

    AiClient() {
        load_from_env();
    }

    void load_from_env() {
        const char* ep = std::getenv("AI_ENDPOINT");
        const char* key = std::getenv("AI_API_KEY");
        const char* model = std::getenv("AI_MODEL");
        const char* timeout = std::getenv("AI_TIMEOUT");

        if (ep && key) {
            config_.endpoint = ep;
            config_.api_key = key;
            config_.model = model ? model : "gpt-4o-mini";
            if (timeout) config_.timeout_secs = std::atoi(timeout);
            config_.enabled = true;
        } else if (ep && !key) {
            // Local Ollama — no API key needed
            config_.endpoint = ep;
            config_.model = model ? model : "llama3.2";
            if (timeout) config_.timeout_secs = std::atoi(timeout);
            config_.enabled = true;
        }
    }

    void configure(const std::string& endpoint, const std::string& api_key,
                   const std::string& model, int timeout = 30) {
        config_.endpoint = endpoint;
        config_.api_key = api_key;
        config_.model = model;
        config_.timeout_secs = timeout;
        config_.enabled = true;
    }

    bool is_enabled() const { return config_.enabled; }
    const AiConfig& config() const { return config_; }

    // Call AI with all context, get analysis
    AiResult analyze(const std::string& metrics_json,
                     const std::string& anomalies_json,
                     const std::string& logs_json) {
        AiResult result;
        if (!config_.enabled) {
            result.error = "AI not configured";
            return result;
        }

        std::string prompt = build_prompt(metrics_json, anomalies_json, logs_json);

        auto start = std::chrono::steady_clock::now();

        try {
            // Parse endpoint to get host, port, path
            std::string host, path;
            int port = 443;
            bool use_ssl = true;

            parse_url(config_.endpoint, host, port, path, use_ssl);

            httplib::Client cli(host, port);

            cli.set_connection_timeout(config_.timeout_secs, 0);
            cli.set_read_timeout(config_.timeout_secs, 0);

            json req_body;
            std::string full_path = path;

            // Detect API type from endpoint
            if (config_.endpoint.find("anthropic.com") != std::string::npos) {
                // Anthropic Claude API format
                req_body["model"] = config_.model;
                req_body["max_tokens"] = 1024;
                req_body["system"] = "You are an SRE expert. Be concise.";
                json msg = {{"role", "user"}, {"content", prompt}};
                req_body["messages"] = json::array({msg});

                httplib::Headers headers = {
                    {"x-api-key", config_.api_key},
                    {"anthropic-version", "2023-06-01"},
                    {"Content-Type", "application/json"}
                };

                auto res = cli.Post(full_path, headers, req_body.dump(), "application/json");
                if (!res) {
                    result.error = "Connection failed: " + httplib::to_string(res.error());
                    return result;
                }

                auto resp = json::parse(res->body);
                if (resp.contains("content") && resp["content"].is_array() && !resp["content"].empty()) {
                    result.content = resp["content"][0]["text"].get<std::string>();
                } else if (resp.contains("error")) {
                    result.error = resp["error"]["message"].get<std::string>();
                    return result;
                }
                result.model_used = resp.value("model", config_.model);

            } else {
                // OpenAI-compatible API (OpenAI, Ollama, vLLM, etc.)
                req_body["model"] = config_.model;
                req_body["max_tokens"] = 1024;
                req_body["temperature"] = 0.3;
                json sys_msg = {{"role", "system"}, {"content", "You are an SRE expert. Be concise and specific."}};
                json user_msg = {{"role", "user"}, {"content", prompt}};
                req_body["messages"] = json::array({sys_msg, user_msg});

                httplib::Headers headers = {
                    {"Authorization", "Bearer " + config_.api_key},
                    {"Content-Type", "application/json"}
                };

                auto res = cli.Post(full_path, headers, req_body.dump(), "application/json");
                if (!res) {
                    result.error = "Connection failed: " + httplib::to_string(res.error());
                    return result;
                }

                auto resp = json::parse(res->body);
                if (resp.contains("choices") && !resp["choices"].empty()) {
                    result.content = resp["choices"][0]["message"]["content"].get<std::string>();
                } else if (resp.contains("error")) {
                    result.error = resp["error"]["message"].get<std::string>();
                    return result;
                }
                result.model_used = resp.value("model", config_.model);
            }

            result.ok = true;
            result.latency_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count());

        } catch (const std::exception& e) {
            result.error = std::string("Exception: ") + e.what();
        }

        return result;
    }

    // Quick test: send a simple prompt to verify connectivity
    AiResult test() {
        return analyze(
            R"({"qps": 100, "status": "healthy"})",
            "[]",
            R"([{"level":"INFO","message":"All systems normal"}])"
        );
    }

private:
    AiConfig config_;

    void parse_url(const std::string& url, std::string& host, int& port,
                   std::string& path, bool& use_ssl) {
        // Parse: https://host:port/path  or  http://host:port/path
        size_t proto_end = url.find("://");
        std::string remainder;

        if (proto_end != std::string::npos) {
            std::string proto = url.substr(0, proto_end);
            use_ssl = (proto == "https");
            remainder = url.substr(proto_end + 3);
        } else {
            use_ssl = false;
            remainder = url;
        }

        size_t path_start = remainder.find('/');
        if (path_start != std::string::npos) {
            path = remainder.substr(path_start);
            remainder = remainder.substr(0, path_start);
        } else {
            path = "/";
        }

        size_t port_start = remainder.find(':');
        if (port_start != std::string::npos) {
            host = remainder.substr(0, port_start);
            port = std::atoi(remainder.substr(port_start + 1).c_str());
        } else {
            host = remainder;
            port = use_ssl ? 443 : 80;
        }
    }

    std::string build_prompt(const std::string& metrics_json,
                             const std::string& anomalies_json,
                             const std::string& logs_json) {
        // Pretty-print metrics if possible
        std::string metrics_pretty = metrics_json;
        std::string anomalies_pretty = anomalies_json;
        try {
            metrics_pretty = json::parse(metrics_json).dump(2);
            auto anom = json::parse(anomalies_json);
            // Only include last 10 anomalies to keep prompt concise
            if (anom.is_array() && anom.size() > 10) {
                json recent = json::array();
                for (size_t i = anom.size() > 10 ? anom.size() - 10 : 0; i < anom.size(); ++i) {
                    recent.push_back(anom[i]);
                }
                anomalies_pretty = recent.dump(2);
            } else {
                anomalies_pretty = anom.dump(2);
            }
        } catch (...) {}

        return R"(You are analyzing a production C++ high-concurrency service called ServiceScope.

## Current Metrics
```json
)" + metrics_pretty + R"(
```

## Recent Anomalies
```json
)" + anomalies_pretty + R"(
```

## Recent Logs
```json
)" + logs_json + R"(
```

Please provide:
1. **Health Assessment**: Is the service healthy/degraded/critical? (one sentence)
2. **Key Issues**: What are the main problems right now? (bullet points)
3. **Root Cause Analysis**: What is the most likely root cause?
4. **Recommended Actions**: What should the operator do? (numbered list)
5. **Confidence**: How confident are you in this analysis? (percentage)

Be specific and reference actual metric values. Keep the response under 500 words.)";
    }
};

} // namespace servicescope
