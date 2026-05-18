# ServiceScope

High-concurrency C++ service with real-time monitoring dashboard, fault injection, and AI-powered log analysis.

## What is this?

A self-contained observability demo that shows the full链路 of a production service:

```
Metrics Collection → Live Dashboard → Fault Injection → Anomaly Detection → AI Analysis
```

- **C++ HTTP server** with thread-pool concurrency
- **Web Dashboard** with real-time charts (QPS, latency, errors, connections)
- **Fault injection** — inject latency, errors, or connection drops at runtime
- **AI log analysis** — auto-detects 6 types of anomalies and generates reports

## Quick Start

### Prerequisites

- CMake 3.16+
- C++17 compiler (MSVC, GCC, or Clang)
- Git

### Build

```bash
# Linux / macOS
./build.sh

# Windows (MinGW)
build.bat mingw

# Windows (MSVC)
build.bat
```

### Run

```bash
./build/servicescope              # default: port 8080, 8 threads
./build/servicescope 3000 16      # custom port and thread count
```

Then open **http://localhost:8080** in your browser.

## Dashboard

The dashboard refreshes every second and shows:

| Panel | Description |
|---|---|
| **QPS** | Requests per second (1s / 5s / 60s windows) |
| **Latency** | P50 / P90 / P99 / P999 percentiles |
| **Error Rate** | Percentage of failed requests |
| **Active Connections** | Current open connections |
| **QPS Chart** | 60-second QPS trend line |
| **Latency Histogram** | Distribution across 16 exponential buckets (1ms–32s) |
| **Percentile Trends** | P50 / P90 / P99 over time |
| **Slow Requests** | Requests exceeding 200ms |
| **Anomalies** | Auto-detected issues with severity labels |
| **AI Analysis** | Markdown report with recommendations |

## API Endpoints

| Method | Path | Description |
|---|---|---|
| `GET` | `/` | Dashboard HTML |
| `GET` | `/metrics` | Full metrics JSON |
| `GET` | `/api/health` | Health check |
| `GET` | `/api/work?delay=N` | Simulate work with N ms delay |
| `GET` | `/api/random` | Random 0–200ms delay |
| `GET` | `/api/data?count=N` | Return N synthetic records |
| `GET` | `/api/fault/config` | Get current fault injection config |
| `POST` | `/api/fault/config` | Update fault injection config |
| `GET` | `/api/logs` | Recent log entries |
| `GET` | `/api/logs/analysis` | AI-generated analysis report |
| `GET` | `/api/logs/anomalies` | Detected anomalies list |
| `POST` | `/api/reset` | Reset all metrics |

## Fault Injection

Configure at runtime without restart:

```bash
# Inject 2-second delay
curl -X POST localhost:8080/api/fault/config \
  -H "Content-Type: application/json" \
  -d '{"enabled":true, "delay_ms":2000}'

# Inject 50% error rate
curl -X POST localhost:8080/api/fault/config \
  -H "Content-Type: application/json" \
  -d '{"enabled":true, "error_rate":0.5}'

# Disable all faults
curl -X POST localhost:8080/api/fault/config \
  -H "Content-Type: application/json" \
  -d '{"enabled":false}'
```

Available fault parameters:

| Parameter | Type | Description |
|---|---|---|
| `enabled` | bool | Master switch |
| `delay_ms` | int | Base latency to inject |
| `delay_jitter_ms` | int | Random jitter (±ms) |
| `error_rate` | float | Probability of error (0.0–1.0) |
| `error_http_code` | int | HTTP code for injected errors |
| `drop_rate` | float | Probability of connection drop (0.0–1.0) |
| `pause` | bool | Return 503 for all requests |

## Stress Testing

```bash
# 100 concurrent workers for 30 seconds, mixed workload
python stress_test.py --concurrency 100 --duration 30

# Slow-request workload
python stress_test.py --mode slow --concurrency 20 --duration 10

# Data-heavy workload
python stress_test.py --mode data --concurrency 50 --duration 20
```

## AI Analysis

The log analyzer watches metrics and logs in real-time, detecting:

| Anomaly Type | Trigger |
|---|---|
| `qps_spike` | QPS exceeds 3× baseline |
| `error_spike` | Error rate exceeds 5× baseline or >10% |
| `latency_spike` | Avg latency exceeds 3× baseline or P99 > 500ms |
| `connection_flood` | Active connections > 200 |
| `slow_request_surge` | Slow request queue > 50 entries |
| `error_flood` | >50 ERROR logs in 60 seconds |

Each anomaly includes a severity level (`critical`, `high`, `medium`) and confidence score.

## Project Structure

```
ServiceScope/
├── CMakeLists.txt          # CMake build (auto-fetches dependencies)
├── build.bat / build.sh    # Build scripts
├── run.bat                 # Windows launcher
├── stress_test.py          # Load generator
├── src/
│   ├── main.cpp            # HTTP server + embedded Dashboard HTML
│   ├── metrics.h           # Atomic lock-free metrics collector
│   ├── fault_injector.h    # Runtime fault injection engine
│   ├── log_analyzer.h      # Heuristic anomaly detection + report generator
│   └── ai_client.h         # LLM client (OpenAI / Claude / Ollama)
└── .gitignore
```

## AI Model Integration

ServiceScope can call a real LLM (not just heuristic rules) to analyze your service metrics. It supports any OpenAI-compatible API.

### Quick Setup

**Option 1: Local Ollama (free, no network needed)**
```bash
# Install Ollama: https://ollama.com
ollama pull llama3.2

# Start ServiceScope with AI
export AI_ENDPOINT="http://localhost:11434/v1/chat/completions"
export AI_API_KEY="ollama"
export AI_MODEL="llama3.2"
./build/servicescope
```

**Option 2: OpenAI API**
```bash
export AI_ENDPOINT="https://api.openai.com/v1/chat/completions"
export AI_API_KEY="sk-your-key-here"
export AI_MODEL="gpt-4o-mini"
./build/servicescope
```

**Option 3: Claude API (Anthropic)**
```bash
export AI_ENDPOINT="https://api.anthropic.com/v1/messages"
export AI_API_KEY="sk-ant-your-key-here"
export AI_MODEL="claude-haiku-4-5"
./build/servicescope
```

Or configure at runtime:
```bash
curl -X POST localhost:8080/api/ai/config \
  -H "Content-Type: application/json" \
  -d '{"endpoint":"http://localhost:11434/v1/chat/completions","api_key":"ollama","model":"llama3.2"}'
```

### AI Endpoints

| Method | Path | Description |
|---|---|---|
| `POST` | `/api/ai/analyze` | Call AI to analyze current metrics |
| `GET` | `/api/ai/config` | Check AI configuration |
| `POST` | `/api/ai/config` | Update AI configuration at runtime |
| `GET` | `/api/ai/test` | Test AI API connectivity |

### How it works

1. The server collects current metrics, recent anomalies, and log entries
2. Builds a structured prompt with all context
3. Sends to the configured LLM
4. Returns the AI's analysis: health assessment, root cause, recommended actions

If no AI is configured, `/api/ai/analyze` gracefully falls back to the built-in heuristic analyzer.

## Dependencies

- [cpp-httplib](https://github.com/yhirose/cpp-httplib) — header-only HTTP server + client
- [nlohmann/json](https://github.com/nlohmann/json) — header-only JSON library
- [Chart.js](https://www.chartjs.org/) — dashboard charts (CDN)

All C++ dependencies are fetched automatically by CMake. No manual installation needed.

## License

MIT
