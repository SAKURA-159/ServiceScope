#pragma once
#include <string>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <chrono>
#include <nlohmann/json.hpp>

namespace servicescope {

using json = nlohmann::json;

struct AiConfig {
    std::string endpoint;
    std::string api_key;
    std::string model;
    int timeout_secs = 30;
    bool enabled = false;
};

struct AiResult {
    std::string content;
    std::string model_used;
    int latency_ms = 0;
    int tokens_used = 0;
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

        if (ep && ep[0] != '\0') {
            config_.endpoint = ep;
            config_.api_key = key ? key : "";
            config_.model = model ? model : "deepseek-chat";
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
        config_.enabled = !endpoint.empty();
    }

    bool is_enabled() const { return config_.enabled; }
    const AiConfig& config() const { return config_; }

    AiResult analyze(const json& metrics, const json& anomalies, const json& logs) {
        AiResult result;
        if (!config_.enabled) {
            result.error = "AI not configured";
            return result;
        }

        auto start = std::chrono::steady_clock::now();

        // Build input for Python bridge
        json input;
        input["endpoint"] = config_.endpoint;
        input["api_key"] = config_.api_key;
        input["model"] = config_.model;
        input["timeout"] = config_.timeout_secs;
        input["metrics"] = metrics;
        input["anomalies"] = anomalies;
        input["logs"] = logs;

        std::string input_str = input.dump();

        // Call Python bridge via popen
        std::string cmd = "python \"" + find_bridge() + "\"";
        #ifdef _WIN32
        FILE* pipe = _popen(cmd.c_str(), "w");
        #else
        FILE* pipe = popen(cmd.c_str(), "w");
        #endif

        if (!pipe) {
            result.error = "Failed to start Python bridge";
            return result;
        }

        fwrite(input_str.c_str(), 1, input_str.size(), pipe);

        #ifdef _WIN32
        int rc = _pclose(pipe);
        #else
        int rc = pclose(pipe);
        #endif

        result.latency_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count());

        if (rc != 0) {
            result.error = "Python bridge exited with code " + std::to_string(rc)
                         + ". Make sure Python 3 is installed.";
            return result;
        }

        // Note: popen in write-mode doesn't capture stdout.
        // We need to switch to read mode or use a temp file approach.
        // Let's use a temp file for the output instead.
        result.error = "Bridge protocol error — use /api/ai/analyze endpoint instead";
        return result;
    }

    // Simplified: pass input as command argument (avoids popen write-only issue)
    AiResult analyze_via_arg(const json& metrics, const json& anomalies, const json& logs) {
        AiResult result;
        if (!config_.enabled) {
            result.error = "AI not configured";
            return result;
        }

        auto start = std::chrono::steady_clock::now();

        json input;
        input["endpoint"] = config_.endpoint;
        input["api_key"] = config_.api_key;
        input["model"] = config_.model;
        input["timeout"] = config_.timeout_secs;
        input["metrics"] = metrics;
        input["anomalies"] = anomalies;
        input["logs"] = logs;

        // Write input to temp file
        std::string tmp_in = find_bridge() + "_input.tmp";
        {
            std::string content = input.dump();
            #ifdef _WIN32
            FILE* f = fopen(tmp_in.c_str(), "w");
            #else
            FILE* f = fopen(tmp_in.c_str(), "w");
            #endif
            if (!f) {
                result.error = "Failed to write temp input";
                return result;
            }
            fwrite(content.c_str(), 1, content.size(), f);
            fclose(f);
        }

        // Call bridge
        std::string cmd = "python \"" + find_bridge() + "\" < \"" + tmp_in + "\"";
        #ifdef _WIN32
        FILE* pipe = _popen(cmd.c_str(), "r");
        #else
        FILE* pipe = popen(cmd.c_str(), "r");
        #endif

        if (!pipe) {
            result.error = "Failed to start Python bridge. Is python in PATH?";
            std::remove(tmp_in.c_str());
            return result;
        }

        char buffer[8192];
        std::string output;
        while (fgets(buffer, sizeof(buffer), pipe)) {
            output += buffer;
        }

        #ifdef _WIN32
        _pclose(pipe);
        #else
        pclose(pipe);
        #endif

        std::remove(tmp_in.c_str());

        result.latency_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count());

        if (output.empty()) {
            result.error = "Python bridge returned empty response";
            return result;
        }

        try {
            auto resp = json::parse(output);
            if (resp.value("ok", false)) {
                result.ok = true;
                result.content = resp.value("content", "");
                result.model_used = resp.value("model", config_.model);
                result.tokens_used = resp.value("tokens_used", 0);
            } else {
                result.error = resp.value("error", "Unknown error");
            }
        } catch (const std::exception& e) {
            result.error = std::string("Failed to parse bridge response: ") + e.what()
                         + "\nRaw: " + output.substr(0, 200);
        }

        return result;
    }

    AiResult test() {
        return analyze_via_arg(
            {{"qps", 100}, {"status", "healthy"}, {"error_rate_pct", 0.0}},
            json::array(),
            json::array()
        );
    }

private:
    AiConfig config_;

    std::string find_bridge() {
        // Look for ai_bridge.py in same directory as executable, then cwd
        const char* paths[] = {"ai_bridge.py", "D:/claude/ServiceScope/ai_bridge.py", nullptr};
        for (int i = 0; paths[i]; ++i) {
            #ifdef _WIN32
            FILE* f = fopen(paths[i], "r");
            #else
            FILE* f = fopen(paths[i], "r");
            #endif
            if (f) {
                fclose(f);
                return paths[i];
            }
        }
        return "ai_bridge.py"; // fallback
    }
};

} // namespace servicescope
