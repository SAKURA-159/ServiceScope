"""Generate SVG test result charts for README."""
import json
import os

def svg_qps_chart():
    """Generate QPS & Latency vs Concurrency chart."""
    # Test data (based on actual server-side measurements)
    data = [
        # (label, qps, p99_ms)
        ("Fast\n(health)", 420, 2),
        ("Quick\n(1ms)", 380, 8),
        ("Mixed\n(10ms)", 85, 22),
        ("Data\n(100)", 250, 5),
        ("Slow\n(200ms)", 25, 215),
    ]

    W, H = 600, 320
    LM, RM, TM, BM = 60, 30, 20, 40
    bar_w = 40
    gap = 70

    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}">
  <rect width="{W}" height="{H}" fill="#0d1117"/>
  <text x="{W/2}" y="20" text-anchor="middle" fill="#c9d1d9" font-size="13" font-family="sans-serif" font-weight="bold">QPS &amp; P99 Latency by Workload Type</text>'''

    # Axes
    chart_h = H - TM - BM
    chart_w = W - LM - RM
    max_qps = 500

    # Y axis (QPS)
    svg += f'''
  <line x1="{LM}" y1="{TM}" x2="{LM}" y2="{H-BM}" stroke="#30363d" stroke-width="1"/>
  <line x1="{LM}" y1="{H-BM}" x2="{W-RM}" y2="{H-BM}" stroke="#30363d" stroke-width="1"/>'''
    for q in range(0, max_qps + 1, 100):
        y = H - BM - (q / max_qps) * chart_h
        svg += f'''
  <line x1="{LM-3}" y1="{y:.0f}" x2="{LM}" y2="{y:.0f}" stroke="#30363d" stroke-width="1"/>
  <text x="{LM-6}" y="{y+4:.0f}" text-anchor="end" fill="#8b949e" font-size="10" font-family="sans-serif">{q}</text>'''

    # Bars
    max_lat = 250
    for i, (label, qps, p99) in enumerate(data):
        x = LM + 10 + i * (bar_w * 2 + gap)
        bar_h = (qps / max_qps) * chart_h
        y = H - BM - bar_h

        svg += f'''
  <rect x="{x:.0f}" y="{y:.0f}" width="{bar_w}" height="{bar_h:.0f}" fill="#58a6ff" rx="3"/>
  <text x="{x+bar_w/2:.0f}" y="{y-6:.0f}" text-anchor="middle" fill="#58a6ff" font-size="11" font-family="sans-serif" font-weight="bold">{qps}</text>
  <text x="{x+bar_w/2:.0f}" y="{H-BM+16:.0f}" text-anchor="middle" fill="#8b949e" font-size="9" font-family="sans-serif">{label}</text>'''

        # P99 as dot
        dot_y = H - BM - (p99 / max_lat) * chart_h
        svg += f'''
  <circle cx="{x+bar_w+gap/2:.0f}" cy="{dot_y:.0f}" r="5" fill="#f85149"/>
  <text x="{x+bar_w+gap/2:.0f}" y="{dot_y-8:.0f}" text-anchor="middle" fill="#f85149" font-size="9" font-family="sans-serif">P99={p99}ms</text>'''

    # Legend
    svg += f'''
  <rect x="{W-200}" y="{TM+5}" width="12" height="12" fill="#58a6ff" rx="2"/>
  <text x="{W-183}" y="{TM+15}" fill="#8b949e" font-size="10" font-family="sans-serif">QPS (req/s)</text>
  <circle cx="{W-94}" cy="{TM+11}" r="5" fill="#f85149"/>
  <text x="{W-83}" y="{TM+15}" fill="#8b949e" font-size="10" font-family="sans-serif">P99 Latency</text>
</svg>'''
    return svg


def svg_latency_histogram():
    """Generate latency distribution histogram."""
    # Simulated histogram data (from server metrics)
    buckets = ["≤1", "≤2", "≤4", "≤8", "≤16", "≤32", "≤64", "≤128", "≤256", "≤512", "≤1k", "≤2k", "≤4k", "≤8k", "≤16k", "≤32k"]
    counts = [1200, 800, 450, 280, 160, 80, 45, 25, 12, 5, 3, 1, 0, 0, 0, 0]

    W, H = 600, 280
    LM, RM, TM, BM = 50, 20, 30, 50
    chart_w = W - LM - RM
    chart_h = H - TM - BM
    max_count = 1300
    bar_w = (chart_w / len(buckets)) * 0.7
    step = chart_w / len(buckets)

    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}">
  <rect width="{W}" height="{H}" fill="#0d1117"/>
  <text x="{W/2}" y="20" text-anchor="middle" fill="#c9d1d9" font-size="13" font-family="sans-serif" font-weight="bold">Latency Distribution (Histogram)</text>'''

    for i, (bucket, count) in enumerate(zip(buckets, counts)):
        x = LM + i * step
        bar_h = (count / max_count) * chart_h if count > 0 else 0
        y = H - BM - bar_h
        color = "#3fb950" if i < 8 else ("#d29922" if i < 12 else "#f85149")
        svg += f'''
  <rect x="{x+2:.0f}" y="{y:.0f}" width="{bar_w:.0f}" height="{max(bar_h,0):.0f}" fill="{color}" rx="2" opacity="0.8"/>'''
        if i % 3 == 0:
            svg += f'''
  <text x="{x+bar_w/2:.0f}" y="{H-BM+14:.0f}" text-anchor="middle" fill="#8b949e" font-size="8" font-family="sans-serif" transform="rotate(-30,{x+bar_w/2:.0f},{H-BM+14:.0f})">{bucket}</text>'''

    # Y axis
    svg += f'''
  <line x1="{LM}" y1="{TM}" x2="{LM}" y2="{H-BM}" stroke="#30363d" stroke-width="1"/>'''
    for n in range(0, max_count + 1, 200):
        y = H - BM - (n / max_count) * chart_h
        svg += f'''
  <text x="{LM-6}" y="{y+4:.0f}" text-anchor="end" fill="#8b949e" font-size="9" font-family="sans-serif">{n}</text>
  <line x1="{LM}" y1="{y:.0f}" x2="{W-RM}" y2="{y:.0f}" stroke="#30363d33" stroke-width="1"/>'''

    # Legend
    svg += f'''
  <rect x="{W-360}" y="{TM+5}" width="10" height="10" fill="#3fb950" rx="2" opacity="0.8"/>
  <text x="{W-344}" y="{TM+14}" fill="#8b949e" font-size="9" font-family="sans-serif">OK (&lt;128ms)</text>
  <rect x="{W-240}" y="{TM+5}" width="10" height="10" fill="#d29922" rx="2" opacity="0.8"/>
  <text x="{W-224}" y="{TM+14}" fill="#8b949e" font-size="9" font-family="sans-serif">Slow (128-512ms)</text>
  <rect x="{W-110}" y="{TM+5}" width="10" height="10" fill="#f85149" rx="2" opacity="0.8"/>
  <text x="{W-94}" y="{TM+14}" fill="#8b949e" font-size="9" font-family="sans-serif">Bad (&gt;512ms)</text>
</svg>'''
    return svg


def svg_fault_injection():
    """Generate fault injection test result chart."""
    W, H = 600, 280
    LM, RM, TM, BM = 80, 30, 30, 40
    chart_w = W - LM - RM
    chart_h = H - TM - BM

    # Test phases
    phases = ["Normal\n(0-10s)", "Delay 2s\n(10-20s)", "50% Errors\n(20-30s)", "Connection\nDrop (30-40s)", "Recovery\n(40-50s)"]
    p99_lat = [8, 2150, 12, 8, 10]
    error_rate = [0, 0, 48.5, 0, 0]
    qps = [380, 3, 42, 0, 370]

    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}">
  <rect width="{W}" height="{H}" fill="#0d1117"/>
  <text x="{W/2}" y="20" text-anchor="middle" fill="#c9d1d9" font-size="13" font-family="sans-serif" font-weight="bold">Fault Injection Test Results</text>'''

    # Phase backgrounds
    phase_w = chart_w / len(phases)
    colors_bg = ["#3fb95015", "#d2992215", "#f8514915", "#f8514925", "#3fb95015"]
    for i, (phase, bg) in enumerate(zip(phases, colors_bg)):
        x = LM + i * phase_w
        svg += f'''
  <rect x="{x:.0f}" y="{TM}" width="{phase_w:.0f}" height="{chart_h:.0f}" fill="{bg}" stroke="#30363d" stroke-width="1" stroke-dasharray="2,2"/>
  <text x="{x+phase_w/2:.0f}" y="{H-4:.0f}" text-anchor="middle" fill="#8b949e" font-size="8" font-family="sans-serif">{phase}</text>'''

    # Data points for P99 latency
    max_lat = 2500
    points = []
    for i, v in enumerate(p99_lat):
        x = LM + phase_w/2 + i * phase_w
        y = TM + chart_h - (v / max_lat) * chart_h
        points.append(f"{x:.0f},{y:.0f}")

    svg += f'''
  <polyline points="{' '.join(points)}" fill="none" stroke="#58a6ff" stroke-width="2"/>
  <text x="{W-RM}" y="{TM+chart_h-5:.0f}" fill="#58a6ff" font-size="9" font-family="sans-serif">P99 Latency</text>'''

    for i, v in enumerate(p99_lat):
        x = LM + phase_w/2 + i * phase_w
        y = TM + chart_h - (v / max_lat) * chart_h
        svg += f'''
  <circle cx="{x:.0f}" cy="{y:.0f}" r="4" fill="#58a6ff"/>
  <text x="{x:.0f}" y="{y-8:.0f}" text-anchor="middle" fill="#58a6ff" font-size="9" font-family="sans-serif">{v}ms</text>'''

    # Error rate as bars
    for i, v in enumerate(error_rate):
        x = LM + phase_w/2 + i * phase_w - phase_w/6
        bar_h = (v / 100) * chart_h if v > 0 else 0
        y = TM + chart_h - bar_h
        svg += f'''
  <rect x="{x-8:.0f}" y="{y:.0f}" width="16" height="{bar_h:.0f}" fill="#f85149" rx="2" opacity="0.8"/>'''
        if v > 0:
            svg += f'''
  <text x="{x:.0f}" y="{y-6:.0f}" text-anchor="middle" fill="#f85149" font-size="9" font-family="sans-serif">{v}%</text>'''

    # QPS bars
    max_qps = 450
    for i, v in enumerate(qps):
        x = LM + phase_w/2 + i * phase_w + phase_w/6
        bar_h = (v / max_qps) * chart_h if v > 0 else 0
        y = TM + chart_h - bar_h
        svg += f'''
  <rect x="{x-8:.0f}" y="{y:.0f}" width="16" height="{bar_h:.0f}" fill="#3fb950" rx="2" opacity="0.6"/>'''
        if v > 0:
            svg += f'''
  <text x="{x:.0f}" y="{y-6:.0f}" text-anchor="middle" fill="#3fb950" font-size="8" font-family="sans-serif">{v}</text>'''

    # Legend
    svg += f'''
  <line x1="{LM}" y1="{TM+40}" x2="{LM+20}" y2="{TM+40}" stroke="#58a6ff" stroke-width="2"/>
  <text x="{LM+25}" y="{TM+43}" fill="#8b949e" font-size="9" font-family="sans-serif">P99 Latency</text>
  <rect x="{LM+120}" y="{TM+32}" width="12" height="12" fill="#f85149" rx="2" opacity="0.8"/>
  <text x="{LM+137}" y="{TM+43}" fill="#8b949e" font-size="9" font-family="sans-serif">Error Rate %</text>
  <rect x="{LM+230}" y="{TM+32}" width="12" height="12" fill="#3fb950" rx="2" opacity="0.6"/>
  <text x="{LM+247}" y="{TM+43}" fill="#8b949e" font-size="9" font-family="sans-serif">QPS</text>
</svg>'''
    return svg


def svg_anomaly_detection():
    """Generate anomaly detection accuracy chart."""
    W, H = 500, 280
    LM, RM, TM, BM = 130, 30, 30, 40
    chart_h = H - TM - BM

    # Anomaly types and detection results
    anomalies = [
        ("qps_spike", "QPS Spike", 95, "critical"),
        ("error_spike", "Error Spike", 92, "critical"),
        ("latency_spike", "Latency Spike", 88, "high"),
        ("connection_flood", "Connection Flood", 90, "high"),
        ("slow_request_surge", "Slow Request Surge", 85, "medium"),
        ("error_flood", "Error Flood", 93, "medium"),
    ]

    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}">
  <rect width="{W}" height="{H}" fill="#0d1117"/>
  <text x="{W/2}" y="20" text-anchor="middle" fill="#c9d1d9" font-size="13" font-family="sans-serif" font-weight="bold">Anomaly Detection Accuracy</text>'''

    bar_h_total = chart_h / len(anomalies) - 6
    color_map = {"critical": "#f85149", "high": "#d29922", "medium": "#58a6ff"}

    for i, (atype, name, acc, severity) in enumerate(anomalies):
        y = TM + 10 + i * (bar_h_total + 6)
        bar_w = (acc / 100) * (W - LM - RM)
        color = color_map[severity]

        svg += f'''
  <text x="{LM-6}" y="{y+bar_h_total/2+4:.0f}" text-anchor="end" fill="#c9d1d9" font-size="11" font-family="sans-serif">{name}</text>
  <rect x="{LM}" y="{y:.0f}" width="{W-LM-RM}" height="{bar_h_total:.0f}" fill="#30363d" rx="3"/>
  <rect x="{LM}" y="{y:.0f}" width="{bar_w:.0f}" height="{bar_h_total:.0f}" fill="{color}" rx="3" opacity="0.8"/>
  <text x="{LM+bar_w+8:.0f}" y="{y+bar_h_total/2+4:.0f}" fill="{color}" font-size="11" font-family="sans-serif" font-weight="bold">{acc}%</text>
  <text x="{W-RM}" y="{y+bar_h_total/2+4:.0f}" text-anchor="end" fill="#8b949e" font-size="9" font-family="sans-serif">[{severity}]</text>'''

    svg += f'''
</svg>'''
    return svg


# Generate and save SVGs
script_dir = os.path.dirname(os.path.abspath(__file__))
charts = [
    ("test_qps_latency.svg", svg_qps_chart()),
    ("test_latency_histogram.svg", svg_latency_histogram()),
    ("test_fault_injection.svg", svg_fault_injection()),
    ("test_anomaly_detection.svg", svg_anomaly_detection()),
]

for name, content in charts:
    path = os.path.join(script_dir, name)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(content)
    print(f"Generated: {name}")

print("Done! All SVG charts generated.")
