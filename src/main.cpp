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

using json = nlohmann::json;
using namespace servicescope;

// Global state
static Metrics g_metrics;
static LogAnalyzer g_analyzer;
static std::atomic<bool> g_running{true};
static std::mt19937 g_rng(std::random_device{}());

// Structured logging
void log_event(const std::string& level, const std::string& message,
               const std::string& path = "", int latency_ms = 0) {
    auto t = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(t);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&time_t));

    g_analyzer.add_log({ts, level, message, latency_ms, path});

    std::cout << "[" << ts << "] [" << level << "] " << message;
    if (!path.empty()) std::cout << " path=" << path;
    if (latency_ms > 0) std::cout << " latency=" << latency_ms << "ms";
    std::cout << std::endl;
}

// Middleware: wrap each handler with metrics + fault injection
using Handler = std::function<void(const httplib::Request&, httplib::Response&)>;

Handler with_observability(Handler next) {
    return [next](const httplib::Request& req, httplib::Response& res) {
        auto& fault = FaultInjector::instance();
        int fault_code = fault.check_request();

        if (fault_code == -1) {
            // Simulate connection drop by setting close and returning empty
            res.set_header("Connection", "close");
            res.status = 444; // Nginx-style "connection closed without response"
            return;
        }
        if (fault_code > 0) {
            json err = {{"error", "Fault injected"}, {"code", fault_code}};
            res.set_content(err.dump(), "application/json");
            res.status = fault_code;
            g_metrics.record_request(req.path, 0, true);
            log_event("WARN", "Fault injected error", req.path, 0);
            return;
        }

        auto start = std::chrono::steady_clock::now();
        try {
            next(req, res);
        } catch (const std::exception& e) {
            res.status = 500;
            json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
            log_event("ERROR", std::string("Exception: ") + e.what(), req.path);
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        bool is_error = (res.status >= 400);
        g_metrics.record_request(req.path, static_cast<int>(elapsed), is_error);

        if (is_error) {
            log_event("ERROR", "Request failed with status " + std::to_string(res.status),
                      req.path, static_cast<int>(elapsed));
        } else if (elapsed > 200) {
            log_event("WARN", "Slow request", req.path, static_cast<int>(elapsed));
        }
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
            {"timestamp", sr.timestamp}
        });
    }
    j["slow_requests"] = slow;

    return j;
}

// ========== HTML Dashboard (inline for single-binary deployment) ==========
const char* DASHBOARD_HTML = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ServiceScope Dashboard</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
<style>
:root{--bg:#0d1117;--card:#161b22;--border:#30363d;--text:#c9d1d9;--green:#3fb950;--red:#f85149;--yellow:#d29922;--blue:#58a6ff;--purple:#a371f7}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;padding:20px}
h1{font-size:24px;margin-bottom:4px}.subtitle{color:#8b949e;font-size:14px;margin-bottom:20px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:16px;margin-bottom:16px}
.card{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:16px}
.card h2{font-size:14px;color:#8b949e;text-transform:uppercase;letter-spacing:0.5px;margin-bottom:12px;font-weight:600}
.metric-row{display:flex;justify-content:space-between;align-items:baseline;padding:6px 0;border-bottom:1px solid var(--border)}
.metric-row:last-child{border-bottom:none}
.metric-label{font-size:13px;color:#8b949e}
.metric-value{font-size:18px;font-weight:700;font-variant-numeric:tabular-nums}
.green{color:var(--green)}.red{color:var(--red)}.yellow{color:var(--yellow)}.blue{color:var(--blue)}.purple{color:var(--purple)}
.chart-container{position:relative;height:200px;width:100%}
canvas{width:100%!important}
.full-width{grid-column:1/-1}
.controls{display:flex;flex-wrap:wrap;gap:12px;align-items:center;margin-bottom:16px}
.controls label{font-size:13px;color:#8b949e}
.controls input,.controls select{background:var(--bg);border:1px solid var(--border);color:var(--text);padding:6px 10px;border-radius:4px;font-size:13px;width:80px}
.controls button{background:var(--blue);color:#fff;border:none;padding:6px 14px;border-radius:4px;cursor:pointer;font-size:13px}
.controls button:hover{opacity:0.85}
.controls button.danger{background:var(--red)}
.controls button.warn{background:var(--yellow);color:#000}
#fault-status{font-size:12px;padding:4px 10px;border-radius:12px;font-weight:600}
.fault-on{background:#3fb95022;color:var(--green);border:1px solid var(--green)}
.fault-off{background:#8b949e22;color:#8b949e;border:1px solid #8b949e}
#anomalies{max-height:300px;overflow-y:auto}
.anomaly{padding:10px;margin-bottom:8px;border-radius:6px;border-left:3px solid;font-size:13px}
.anomaly.critical{background:#f8514922;border-color:var(--red)}
.anomaly.high{background:#d2992222;border-color:var(--yellow)}
.anomaly.medium{background:#58a6ff22;border-color:var(--blue)}
.anomaly-type{font-weight:600;text-transform:uppercase;font-size:11px;margin-bottom:2px}
#ai-summary{font-family:monospace;font-size:13px;line-height:1.6;white-space:pre-wrap;max-height:400px;overflow-y:auto}
#slow-table{width:100%;font-size:13px;border-collapse:collapse}
#slow-table th{text-align:left;padding:6px 8px;border-bottom:2px solid var(--border);color:#8b949e;font-size:11px;text-transform:uppercase}
#slow-table td{padding:6px 8px;border-bottom:1px solid var(--border)}
.badge{display:inline-block;padding:2px 8px;border-radius:10px;font-size:11px;font-weight:600}
.badge-ok{background:#3fb95022;color:var(--green)}
.badge-warn{background:#d2992222;color:var(--yellow)}
.badge-bad{background:#f8514922;color:var(--red)}
.pulse{animation:pulse 2s infinite}@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.5}}
</style>
</head>
<body>
<h1>ServiceScope Dashboard</h1>
<p class="subtitle">High-concurrency service monitor — auto-refresh 1s <span class="pulse" id="alive-dot" style="color:var(--green)">●</span></p>

<div class="controls">
  <span id="fault-status" class="fault-off">FAULT: OFF</span>
  <label>Delay (ms):</label>
  <input type="number" id="fault-delay" value="0" min="0" max="5000">
  <label>Jitter (±ms):</label>
  <input type="number" id="fault-jitter" value="0" min="0" max="2000">
  <label>Error Rate:</label>
  <input type="number" id="fault-error-rate" value="0" min="0" max="100" step="1">%
  <label>Drop Rate:</label>
  <input type="number" id="fault-drop-rate" value="0" min="0" max="100" step="1">%
  <button onclick="toggleFault()" id="btn-toggle">Enable Fault</button>
  <button onclick="applyFault()">Apply Config</button>
  <button class="warn" onclick="injectSlow()">Inject 2s Delay</button>
  <button class="danger" onclick="injectErrors()">Inject 50% Errors</button>
  <button onclick="resetFault()">Reset All</button>
</div>

<div class="grid">
  <div class="card">
    <h2>Requests / Second</h2>
    <div class="metric-row"><span class="metric-label">QPS (1s)</span><span class="metric-value blue" id="qps-1s">—</span></div>
    <div class="metric-row"><span class="metric-label">QPS (5s avg)</span><span class="metric-value blue" id="qps-5s">—</span></div>
    <div class="metric-row"><span class="metric-label">QPS (60s avg)</span><span class="metric-value blue" id="qps-60s">—</span></div>
    <div class="metric-row"><span class="metric-label">Total Requests</span><span class="metric-value" id="total-req">—</span></div>
  </div>
  <div class="card">
    <h2>Latency (ms)</h2>
    <div class="metric-row"><span class="metric-label">Average</span><span class="metric-value" id="lat-avg">—</span></div>
    <div class="metric-row"><span class="metric-label">P50</span><span class="metric-value green" id="lat-p50">—</span></div>
    <div class="metric-row"><span class="metric-label">P90</span><span class="metric-value yellow" id="lat-p90">—</span></div>
    <div class="metric-row"><span class="metric-label">P99</span><span class="metric-value red" id="lat-p99">—</span></div>
    <div class="metric-row"><span class="metric-label">P999</span><span class="metric-value red" id="lat-p999">—</span></div>
  </div>
  <div class="card">
    <h2>Health</h2>
    <div class="metric-row"><span class="metric-label">Error Rate (60s)</span><span class="metric-value" id="err-rate">—</span></div>
    <div class="metric-row"><span class="metric-label">Active Connections</span><span class="metric-value purple" id="conn-count">—</span></div>
    <div class="metric-row"><span class="metric-label">Total Errors</span><span class="metric-value red" id="total-err">—</span></div>
    <div class="metric-row"><span class="metric-label">Health Status</span><span id="health-badge">—</span></div>
  </div>
</div>

<div class="grid">
  <div class="card full-width">
    <h2>QPS History (last 60s)</h2>
    <div class="chart-container"><canvas id="qps-chart"></canvas></div>
  </div>
</div>

<div class="grid">
  <div class="card">
    <h2>Latency Distribution (Histogram)</h2>
    <div class="chart-container"><canvas id="hist-chart"></canvas></div>
  </div>
  <div class="card">
    <h2>Latency Percentiles Over Time</h2>
    <div class="chart-container"><canvas id="latency-chart"></canvas></div>
  </div>
</div>

<div class="grid">
  <div class="card">
    <h2>Recent Slow Requests (>200ms)</h2>
    <div style="max-height:200px;overflow-y:auto">
      <table id="slow-table">
        <thead><tr><th>Time</th><th>Path</th><th>Latency</th></tr></thead>
        <tbody id="slow-tbody"></tbody>
      </table>
    </div>
  </div>
  <div class="card">
    <h2>Detected Anomalies</h2>
    <div id="anomalies"><p style="color:#8b949e;font-size:13px">No anomalies detected</p></div>
  </div>
</div>

<div class="grid">
  <div class="card full-width">
    <h2>AI Log Analysis</h2>
    <button onclick="fetchAI()" style="margin-bottom:8px">Run Analysis</button>
    <div id="ai-summary">Click "Run Analysis" to generate AI log analysis</div>
  </div>
</div>

<script>
const historyLen = 60;
let qpsHistory = new Array(historyLen).fill(0);
let p50History = new Array(historyLen).fill(0);
let p90History = new Array(historyLen).fill(0);
let p99History = new Array(historyLen).fill(0);
let labels = new Array(historyLen).fill('');

const qpsCtx = document.getElementById('qps-chart').getContext('2d');
const qpsChart = new Chart(qpsCtx, {
  type:'line',
  data:{
    labels:labels,
    datasets:[{
      label:'QPS',data:qpsHistory,
      borderColor:'#58a6ff',backgroundColor:'#58a6ff22',
      fill:true,tension:0.3,pointRadius:0
    }]
  },
  options:{
    responsive:true,maintainAspectRatio:false,
    scales:{
      x:{display:true,grid:{color:'#30363d44'},ticks:{color:'#8b949e',maxTicksLimit:10}},
      y:{beginAtZero:true,grid:{color:'#30363d44'},ticks:{color:'#8b949e'}}
    },
    plugins:{legend:{display:false}}
  }
});

const histCtx = document.getElementById('hist-chart').getContext('2d');
const histChart = new Chart(histCtx, {
  type:'bar',
  data:{labels:[],datasets:[{label:'Count',data:[],backgroundColor:'#58a6ff66',borderColor:'#58a6ff'}]},
  options:{
    responsive:true,maintainAspectRatio:false,
    scales:{
      x:{grid:{color:'#30363d44'},ticks:{color:'#8b949e'}},
      y:{beginAtZero:true,grid:{color:'#30363d44'},ticks:{color:'#8b949e'}}
    },
    plugins:{legend:{display:false}}
  }
});

const latCtx = document.getElementById('latency-chart').getContext('2d');
const latChart = new Chart(latCtx, {
  type:'line',
  data:{
    labels:labels,
    datasets:[
      {label:'P50',data:p50History,borderColor:'#3fb950',tension:0.3,pointRadius:0},
      {label:'P90',data:p90History,borderColor:'#d29922',tension:0.3,pointRadius:0},
      {label:'P99',data:p99History,borderColor:'#f85149',tension:0.3,pointRadius:0}
    ]
  },
  options:{
    responsive:true,maintainAspectRatio:false,
    scales:{
      x:{grid:{color:'#30363d44'},ticks:{color:'#8b949e',maxTicksLimit:10}},
      y:{beginAtZero:true,grid:{color:'#30363d44'},ticks:{color:'#8b949e'}}
    }
  }
});

async function fetchMetrics(){
  try{
    const r = await fetch('/metrics');
    const m = await r.json();
    // QPS
    document.getElementById('qps-1s').textContent = m.qps['1s'].toFixed(1);
    document.getElementById('qps-5s').textContent = m.qps['5s'].toFixed(1);
    document.getElementById('qps-60s').textContent = m.qps['60s'].toFixed(1);
    document.getElementById('total-req').textContent = m.total_requests.toLocaleString();
    // Latency
    document.getElementById('lat-avg').textContent = m.latency.avg_ms.toFixed(1);
    document.getElementById('lat-p50').textContent = m.latency.p50_ms;
    document.getElementById('lat-p90').textContent = m.latency.p90_ms;
    document.getElementById('lat-p99').textContent = m.latency.p99_ms;
    document.getElementById('lat-p999').textContent = m.latency.p999_ms;
    // Health
    const errPct = m.error_rate_pct;
    const errEl = document.getElementById('err-rate');
    errEl.textContent = errPct.toFixed(2) + '%';
    errEl.className = 'metric-value ' + (errPct > 5 ? 'red' : errPct > 1 ? 'yellow' : 'green');

    document.getElementById('conn-count').textContent = m.active_connections;
    document.getElementById('total-err').textContent = m.total_errors.toLocaleString();

    const badge = document.getElementById('health-badge');
    if(errPct > 10 || m.latency.p99_ms > 1000){
      badge.innerHTML='<span class="badge badge-bad">UNHEALTHY</span>';
    }else if(errPct > 2 || m.latency.p99_ms > 500){
      badge.innerHTML='<span class="badge badge-warn">DEGRADED</span>';
    }else{
      badge.innerHTML='<span class="badge badge-ok">HEALTHY</span>';
    }

    // QPS history
    qpsHistory.push(m.qps['1s']);
    qpsHistory.shift();
    p50History.push(m.latency.p50_ms);
    p50History.shift();
    p90History.push(m.latency.p90_ms);
    p90History.shift();
    p99History.push(m.latency.p99_ms);
    p99History.shift();
    const now = new Date().toLocaleTimeString();
    labels.push(now);
    labels.shift();

    qpsChart.data.datasets[0].data = qpsHistory;
    qpsChart.data.labels = labels;
    qpsChart.update('none');

    latChart.data.datasets[0].data = p50History;
    latChart.data.datasets[1].data = p90History;
    latChart.data.datasets[2].data = p99History;
    latChart.data.labels = labels;
    latChart.update('none');

    // Histogram
    const h = m.latency_histogram;
    histChart.data.labels = h.map(b => '<=' + b.bound_ms + 'ms');
    histChart.data.datasets[0].data = h.map(b => b.count);
    histChart.update('none');

    // Slow requests
    const tbody = document.getElementById('slow-tbody');
    tbody.innerHTML = m.slow_requests.map(s =>
      '<tr><td>'+s.timestamp+'</td><td>'+s.path+'</td><td style="color:'+(s.latency_ms>500?'#f85149':'#d29922')+'">'+s.latency_ms+'ms</td></tr>'
    ).join('');

    // Anomalies
    fetchAnomalies();
  }catch(e){console.error('Fetch metrics error:',e)}
}

async function fetchAnomalies(){
  try{
    const r = await fetch('/api/logs/anomalies');
    const anomalies = await r.json();
    const div = document.getElementById('anomalies');
    if(!anomalies || anomalies.length===0){
      div.innerHTML='<p style="color:#8b949e;font-size:13px">No anomalies detected</p>';
      return;
    }
    div.innerHTML = anomalies.map(a =>
      '<div class="anomaly '+a.severity+'">'+
      '<div class="anomaly-type">'+a.type.replace(/_/g,' ')+' ['+a.severity.toUpperCase()+'] '+
      '('+(a.confidence*100).toFixed(0)+'% conf.)</div>'+
      a.description+'</div>'
    ).join('');
  }catch(e){}
}

async function fetchAI(){
  try{
    document.getElementById('ai-summary').textContent = 'Analyzing...';
    const r = await fetch('/api/logs/analysis');
    const data = await r.json();
    document.getElementById('ai-summary').textContent = data.summary || data.error || 'No analysis available';
  }catch(e){
    document.getElementById('ai-summary').textContent = 'Error: '+e.message;
  }
}

async function fetchFaultStatus(){
  try{
    const r = await fetch('/api/fault/config');
    const f = await r.json();
    const el = document.getElementById('fault-status');
    el.textContent = f.enabled ? 'FAULT: ON' : 'FAULT: OFF';
    el.className = f.enabled ? 'fault-on' : 'fault-off';
    document.getElementById('btn-toggle').textContent = f.enabled ? 'Disable Fault' : 'Enable Fault';
    document.getElementById('fault-delay').value = f.delay_ms;
    document.getElementById('fault-jitter').value = f.delay_jitter_ms;
    document.getElementById('fault-error-rate').value = (f.error_rate*100).toFixed(0);
    document.getElementById('fault-drop-rate').value = (f.drop_rate*100).toFixed(0);
  }catch(e){}
}

async function toggleFault(){
  const el = document.getElementById('fault-status');
  const enabling = el.className === 'fault-off';
  await fetch('/api/fault/config',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({enabled:enabling})
  });
  fetchFaultStatus();
}

async function applyFault(){
  await fetch('/api/fault/config',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({
      delay_ms:parseInt(document.getElementById('fault-delay').value)||0,
      delay_jitter_ms:parseInt(document.getElementById('fault-jitter').value)||0,
      error_rate:(parseFloat(document.getElementById('fault-error-rate').value)||0)/100,
      drop_rate:(parseFloat(document.getElementById('fault-drop-rate').value)||0)/100
    })
  });
  fetchFaultStatus();
}

async function injectSlow(){
  await fetch('/api/fault/config',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({enabled:true,delay_ms:2000,delay_jitter_ms:0,error_rate:0,drop_rate:0})
  });
  fetchFaultStatus();
}

async function injectErrors(){
  await fetch('/api/fault/config',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({enabled:true,delay_ms:0,error_rate:0.5,drop_rate:0})
  });
  fetchFaultStatus();
}

async function resetFault(){
  await fetch('/api/fault/config',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({enabled:false,delay_ms:0,delay_jitter_ms:0,error_rate:0,drop_rate:0,pause:false})
  });
  fetchFaultStatus();
}

setInterval(fetchMetrics, 1000);
fetchMetrics();
fetchFaultStatus();
fetchAI();
</script>
</body>
</html>)rawliteral";

// ========== Main Server ==========
int main(int argc, char* argv[]) {
    int port = 8080;
    int threads = std::thread::hardware_concurrency();
    if (threads < 4) threads = 4;

    if (argc > 1) port = std::atoi(argv[1]);
    if (argc > 2) threads = std::atoi(argv[2]);

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
            auto start = std::chrono::steady_clock::now();
            volatile double x = 1.0;
            while (std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start).count() < delay_ms) {
                x = std::sin(x) * 1.5;
            }
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
            auto start = std::chrono::steady_clock::now();
            volatile double x = 1.0;
            while (std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start).count() < delay) {
                x = std::sin(x) * 1.5;
            }
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
            if (j.contains("delay_ms")) cfg.delay_ms.store(j["delay_ms"].get<int>());
            if (j.contains("delay_jitter_ms")) cfg.delay_jitter_ms.store(j["delay_jitter_ms"].get<int>());
            if (j.contains("error_rate")) cfg.error_rate.store(j["error_rate"].get<double>());
            if (j.contains("error_http_code")) cfg.error_http_code.store(j["error_http_code"].get<int>());
            if (j.contains("drop_rate")) cfg.drop_rate.store(j["drop_rate"].get<double>());
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
                {"latency_ms", l.latency_ms}
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
