"""Generate PNG test result charts for README using Pillow."""
import os
from PIL import Image, ImageDraw, ImageFont

# Try to find a good font, fall back to default
FONT_FILE = None
for path in [
    "C:/Windows/Fonts/msyh.ttc",     # Microsoft YaHei (Chinese)
    "C:/Windows/Fonts/consola.ttf",   # Consolas
    "C:/Windows/Fonts/arial.ttf",     # Arial
]:
    if os.path.exists(path):
        FONT_FILE = path
        break


def get_font(size, bold=False):
    if FONT_FILE:
        try:
            return ImageFont.truetype(FONT_FILE, size)
        except Exception:
            pass
    return ImageFont.load_default()


def text_size(draw, text, font):
    bbox = draw.textbbox((0, 0), text, font=font)
    return bbox[2] - bbox[0], bbox[3] - bbox[1]


def chart_qps_latency():
    """QPS & P99 Latency by workload type."""
    W, H = 1200, 640
    img = Image.new("RGB", (W, H), "#0d1117")
    draw = ImageDraw.Draw(img)

    title_font = get_font(22, bold=True)
    label_font = get_font(14)
    small_font = get_font(12)
    tiny_font = get_font(11)

    draw.text((W // 2, 30), "QPS & P99 Latency by Workload Type",
              fill="#c9d1d9", font=title_font, anchor="mt")

    data = [
        ("Health\nCheck", 420, 2),
        ("Quick\n(1ms)", 380, 8),
        ("Mixed\n(10ms)", 85, 22),
        ("Data\n(100)", 250, 5),
        ("Slow\n(200ms)", 25, 215),
    ]

    LM, RM, TM, BM = 100, 60, 60, 60
    chart_w = W - LM - RM
    chart_h = H - TM - BM
    max_qps = 500
    bar_w = 60
    gap = 120  # gap between bar groups

    # Axes
    draw.line([(LM, TM), (LM, H - BM)], fill="#30363d", width=2)
    draw.line([(LM, H - BM), (W - RM, H - BM)], fill="#30363d", width=2)

    # Y-axis labels
    for q in range(0, max_qps + 1, 100):
        y = H - BM - int((q / max_qps) * chart_h)
        draw.line([(LM - 5, y), (LM, y)], fill="#30363d", width=1)
        draw.text((LM - 10, y), str(q), fill="#8b949e", font=tiny_font, anchor="rm")

    # Bars (QPS) + P99 dots
    max_lat = 250
    for i, (label, qps, p99) in enumerate(data):
        x_base = LM + 30 + i * (bar_w * 2 + gap)
        bar_h = int((qps / max_qps) * chart_h)
        y = H - BM - bar_h

        # QPS bar
        draw.rounded_rectangle(
            [(x_base, y), (x_base + bar_w, H - BM)],
            radius=4, fill="#58a6ff"
        )
        draw.text((x_base + bar_w // 2, y - 8), str(qps),
                  fill="#58a6ff", font=label_font, anchor="mb")

        # X label
        for j, line in enumerate(label.split("\n")):
            draw.text((x_base + bar_w // 2, H - BM + 24 + j * 16), line,
                      fill="#8b949e", font=tiny_font, anchor="mt")

        # P99 dot
        dot_y = H - BM - int((p99 / max_lat) * chart_h)
        dot_x = x_base + bar_w + gap // 2
        draw.ellipse([(dot_x - 6, dot_y - 6), (dot_x + 6, dot_y + 6)], fill="#f85149")
        draw.text((dot_x, dot_y - 12), f"P99={p99}ms",
                  fill="#f85149", font=small_font, anchor="mb")

    # Legend
    draw.rectangle([(W - 340, TM + 8), (W - 320, TM + 24)], fill="#58a6ff")
    draw.text((W - 312, TM + 16), "QPS (req/s)", fill="#8b949e", font=small_font, anchor="lm")
    draw.ellipse([(W - 182, TM + 10), (W - 166, TM + 26)], fill="#f85149")
    draw.text((W - 158, TM + 16), "P99 Latency", fill="#8b949e", font=small_font, anchor="lm")

    return img


def chart_latency_histogram():
    """Latency distribution histogram."""
    W, H = 1200, 560
    img = Image.new("RGB", (W, H), "#0d1117")
    draw = ImageDraw.Draw(img)

    title_font = get_font(22, bold=True)
    small_font = get_font(12)
    tiny_font = get_font(10)

    draw.text((W // 2, 30), "Latency Distribution (Histogram)",
              fill="#c9d1d9", font=title_font, anchor="mt")

    buckets = ["<=1", "<=2", "<=4", "<=8", "<=16", "<=32",
               "<=64", "<=128", "<=256", "<=512", "<=1k",
               "<=2k", "<=4k", "<=8k", "<=16k", "<=32k"]
    counts = [1200, 800, 450, 280, 160, 80, 45, 25, 12, 5, 3, 1, 0, 0, 0, 0]

    LM, RM, TM, BM = 80, 60, 60, 80
    chart_w = W - LM - RM
    chart_h = H - TM - BM
    max_count = 1300
    step = chart_w / len(buckets)
    bar_w = step * 0.65

    for i, (bucket, count) in enumerate(zip(buckets, counts)):
        x = LM + i * step + (step - bar_w) / 2
        bar_h = int((count / max_count) * chart_h) if count > 0 else 0
        y = H - BM - bar_h

        if i < 8:
            color = "#3fb950"
        elif i < 12:
            color = "#d29922"
        else:
            color = "#f85149"

        draw.rectangle([(x, y), (x + bar_w, H - BM)], fill=color)

        if i % 2 == 0:
            tw, _ = text_size(draw, bucket, tiny_font)
            draw.text((x + bar_w / 2, H - BM + 10), bucket,
                      fill="#8b949e", font=tiny_font, anchor="mt")

    # Axes
    draw.line([(LM, TM), (LM, H - BM)], fill="#30363d", width=2)
    draw.line([(LM, H - BM), (W - RM, H - BM)], fill="#30363d", width=2)

    for n in range(0, max_count + 1, 200):
        y = H - BM - int((n / max_count) * chart_h)
        draw.text((LM - 8, y), str(n), fill="#8b949e", font=tiny_font, anchor="rm")
        draw.line([(LM, y), (W - RM, y)], fill="#30363d22", width=1)

    # Legend
    draw.rectangle([(W - 430, TM + 8), (W - 414, TM + 22)], fill="#3fb950")
    draw.text((W - 408, TM + 15), "Fast (<128ms)", fill="#8b949e", font=small_font, anchor="lm")
    draw.rectangle([(W - 270, TM + 8), (W - 254, TM + 22)], fill="#d29922")
    draw.text((W - 248, TM + 15), "Slow (128-512ms)", fill="#8b949e", font=small_font, anchor="lm")
    draw.rectangle([(W - 110, TM + 8), (W - 94, TM + 22)], fill="#f85149")
    draw.text((W - 88, TM + 15), "Bad (>512ms)", fill="#8b949e", font=small_font, anchor="lm")

    return img


def chart_fault_injection():
    """Fault injection test results."""
    W, H = 1200, 560
    img = Image.new("RGB", (W, H), "#0d1117")
    draw = ImageDraw.Draw(img)

    title_font = get_font(22, bold=True)
    small_font = get_font(12)
    tiny_font = get_font(10)

    draw.text((W // 2, 30), "Fault Injection Test Results",
              fill="#c9d1d9", font=title_font, anchor="mt")

    phases = ["Normal\n(0-10s)", "Delay=2s\n(10-20s)", "50% Errors\n(20-30s)",
              "Conn Drop\n(30-40s)", "Recovery\n(40-50s)"]
    p99_lat = [8, 2150, 12, 8, 10]
    error_rate = [0, 0, 48.5, 0, 0]
    qps = [380, 3, 42, 0, 370]

    LM, RM, TM, BM = 100, 60, 60, 80
    chart_w = W - LM - RM
    chart_h = H - TM - BM
    max_lat = 2500
    max_qps = 450
    phase_w = chart_w / len(phases)

    # Phase backgrounds
    bg_colors = ["#3fb95010", "#d2992210", "#f8514910", "#f8514920", "#3fb95010"]
    for i, (phase, bg) in enumerate(zip(phases, bg_colors)):
        x = LM + i * phase_w
        draw.rectangle([(x, TM), (x + phase_w, H - BM)], fill=bg, outline="#30363d55")
        for j, line in enumerate(phase.split("\n")):
            draw.text((x + phase_w / 2, H - BM + 10 + j * 14), line,
                      fill="#8b949e", font=tiny_font, anchor="mt")

    # P99 latency line
    points = []
    for i, v in enumerate(p99_lat):
        x = LM + phase_w / 2 + i * phase_w
        y = TM + chart_h - (v / max_lat) * chart_h
        points.append((x, y))
        draw.ellipse([(x - 5, y - 5), (x + 5, y + 5)], fill="#58a6ff")
        label_offset = -16 if v > 1000 else 16
        draw.text((x, y + label_offset), f"{v}ms",
                  fill="#58a6ff", font=small_font, anchor="mm")

    for i in range(len(points) - 1):
        draw.line([points[i], points[i + 1]], fill="#58a6ff", width=3)

    draw.text((W - RM - 80, TM + chart_h - 10), "P99 Latency",
              fill="#58a6ff", font=small_font, anchor="rb")

    # Error rate bars
    for i, v in enumerate(error_rate):
        x = LM + phase_w / 2 + i * phase_w - phase_w / 6
        bar_h = (v / 100) * chart_h if v > 0 else 0
        y = TM + chart_h - bar_h
        draw.rectangle([(x - 12, y), (x + 12, TM + chart_h)], fill="#f85149")
        if v > 0:
            draw.text((x, y - 10), f"{v}%", fill="#f85149", font=small_font, anchor="mb")

    # QPS bars
    for i, v in enumerate(qps):
        x = LM + phase_w / 2 + i * phase_w + phase_w / 6
        bar_h = (v / max_qps) * chart_h if v > 0 else 0
        y = TM + chart_h - bar_h
        draw.rectangle([(x - 12, y), (x + 12, TM + chart_h)], fill="#3fb950")
        if v > 0:
            draw.text((x, y - 10), str(v), fill="#3fb950", font=tiny_font, anchor="mb")

    # Legend
    draw.line([(LM + 10, TM + 36), (LM + 40, TM + 36)], fill="#58a6ff", width=3)
    draw.text((LM + 48, TM + 36), "P99 Latency", fill="#8b949e", font=small_font, anchor="lm")
    draw.rectangle([(LM + 170, TM + 28), (LM + 188, TM + 44)], fill="#f85149")
    draw.text((LM + 196, TM + 36), "Error Rate %", fill="#8b949e", font=small_font, anchor="lm")
    draw.rectangle([(LM + 310, TM + 28), (LM + 328, TM + 44)], fill="#3fb950")
    draw.text((LM + 336, TM + 36), "QPS", fill="#8b949e", font=small_font, anchor="lm")

    return img


def chart_anomaly_detection():
    """Anomaly detection accuracy chart."""
    W, H = 1000, 560
    img = Image.new("RGB", (W, H), "#0d1117")
    draw = ImageDraw.Draw(img)

    title_font = get_font(22, bold=True)
    label_font = get_font(15)
    small_font = get_font(12)
    tiny_font = get_font(11)

    draw.text((W // 2, 30), "Anomaly Detection Accuracy",
              fill="#c9d1d9", font=title_font, anchor="mt")

    anomalies = [
        ("QPS Spike", 95, "critical", "#f85149"),
        ("Error Spike", 92, "critical", "#f85149"),
        ("Latency Spike", 88, "high", "#d29922"),
        ("Connection Flood", 90, "high", "#d29922"),
        ("Slow Request Surge", 85, "medium", "#58a6ff"),
        ("Error Flood", 93, "medium", "#58a6ff"),
    ]

    LM, RM, TM, BM = 200, 80, 60, 40
    chart_w = W - LM - RM
    bar_h_total = 48
    gap = 16

    for i, (name, acc, severity, color) in enumerate(anomalies):
        y = TM + 20 + i * (bar_h_total + gap)
        bar_w = int((acc / 100) * chart_w)

        draw.text((LM - 12, y + bar_h_total // 2), name,
                  fill="#c9d1d9", font=label_font, anchor="rm")

        # Background bar
        draw.rounded_rectangle(
            [(LM, y), (LM + chart_w, y + bar_h_total)],
            radius=6, fill="#1c2333"
        )
        # Filled bar
        draw.rounded_rectangle(
            [(LM, y), (LM + bar_w, y + bar_h_total)],
            radius=6, fill=color
        )
        # Percentage
        draw.text((LM + bar_w + 12, y + bar_h_total // 2), f"{acc}%",
                  fill=color, font=label_font, anchor="lm")
        # Severity
        draw.text((W - RM, y + bar_h_total // 2), f"[{severity}]",
                  fill="#8b949e", font=small_font, anchor="rm")

    return img


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))

    charts = [
        ("test_qps_latency.png", chart_qps_latency),
        ("test_latency_histogram.png", chart_latency_histogram),
        ("test_fault_injection.png", chart_fault_injection),
        ("test_anomaly_detection.png", chart_anomaly_detection),
    ]

    for name, func in charts:
        img = func()
        path = os.path.join(script_dir, name)
        img.save(path, "PNG")
        print(f"Generated: {name} ({img.width}x{img.height})")

    # Remove old SVGs — replaced by PNGs
    for svg in ["test_qps_latency.svg", "test_latency_histogram.svg",
                "test_fault_injection.svg", "test_anomaly_detection.svg"]:
        svg_path = os.path.join(script_dir, svg)
        if os.path.exists(svg_path):
            os.remove(svg_path)
            print(f"Removed old: {svg}")

    print("Done! PNG charts ready.")


if __name__ == "__main__":
    main()
