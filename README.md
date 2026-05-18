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
│   └── log_analyzer.h      # AI anomaly detection + report generator
└── .gitignore
```

## Dependencies

- [cpp-httplib](https://github.com/yhirose/cpp-httplib) — header-only HTTP server
- [nlohmann/json](https://github.com/nlohmann/json) — header-only JSON library
- [Chart.js](https://www.chartjs.org/) — dashboard charts (CDN)

All C++ dependencies are fetched automatically by CMake. No manual installation needed.

## License

MIT
