"""Capture real dashboard screenshots for README test report."""
import subprocess
import time
import os
import sys

from playwright.sync_api import sync_playwright

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
IMG_DIR = os.path.join(SCRIPT_DIR, "docs", "images")
SERVER_EXE = os.path.join(SCRIPT_DIR, "build", "servicescope.exe")
PORT = 8080
URL = f"http://localhost:{PORT}"

os.makedirs(IMG_DIR, exist_ok=True)

# Start server
print("Starting server...")
server = subprocess.Popen(
    [SERVER_EXE, str(PORT), "8"],
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
time.sleep(2)
print("Server started.")


def run_stress(concurrency=50, duration=15, mode="mixed"):
    """Run stress test in background."""
    script = os.path.join(SCRIPT_DIR, "stress_test.py")
    return subprocess.Popen(
        [sys.executable, script, "--concurrency", str(concurrency),
         "--duration", str(duration), "--mode", mode],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )


def screen(path, element=None, full_page=False):
    """Take a screenshot."""
    if element:
        element.screenshot(path=path)
    else:
        page.screenshot(path=path, full_page=full_page)
    size_kb = os.path.getsize(path) / 1024
    print(f"  -> {os.path.basename(path)} ({size_kb:.0f} KB)")


with sync_playwright() as pw:
    browser = pw.chromium.launch(headless=True)
    context = browser.new_context(
        viewport={"width": 1440, "height": 900},
        color_scheme="dark",
    )
    page = context.new_page()

    # ================================================================
    # 1. Dashboard Overview — warm up, then capture full page
    # ================================================================
    print("\n[1/5] Dashboard overview...")
    page.goto(URL, wait_until="networkidle", timeout=15000)
    time.sleep(4)  # let metrics populate

    # Run a quick load so there's data to show
    proc = run_stress(concurrency=30, duration=10, mode="mixed")
    time.sleep(8)

    screen(os.path.join(IMG_DIR, "dashboard_overview.png"), full_page=True)

    # ================================================================
    # 2. QPS & Latency trend — scroll to charts area
    # ================================================================
    print("\n[2/5] QPS & latency trend...")
    # Take full page after some load has run
    time.sleep(5)  # more data accumulation
    proc.wait()    # stress test done

    # Scroll to the QPS history chart
    qps_chart = page.locator("canvas#qps-chart")
    if qps_chart.count() > 0:
        qps_chart.scroll_into_view_if_needed()
        time.sleep(0.5)
    screen(os.path.join(IMG_DIR, "qps_latency_trend.png"), full_page=True)

    # ================================================================
    # 3. Latency histogram — scroll to histogram section
    # ================================================================
    print("\n[3/5] Latency histogram...")
    hist_chart = page.locator("canvas#hist-chart")
    if hist_chart.count() > 0:
        hist_chart.scroll_into_view_if_needed()
        time.sleep(0.5)
    screen(os.path.join(IMG_DIR, "latency_histogram.png"), full_page=True)

    # ================================================================
    # 4. Fault injection — inject 2000ms delay and capture
    # ================================================================
    print("\n[4/5] Fault injection delay...")

    # Reset first
    page.evaluate("""async () => {
        await fetch('/api/fault/config', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({enabled: true, delay_ms: 2000, delay_jitter_ms: 500, error_rate: 0, drop_rate: 0})
        });
    }""")
    time.sleep(2)

    # Run light load to show delay effect
    proc2 = run_stress(concurrency=20, duration=10, mode="fast")
    time.sleep(8)

    # Get AI analysis result too
    page.evaluate("""async () => {
        await fetch('/api/logs/analysis');
    }""")
    time.sleep(1)

    # Scroll to top to show fault status + metrics + anomalies
    page.evaluate("window.scrollTo(0, 0)")
    time.sleep(0.5)
    screen(os.path.join(IMG_DIR, "fault_injection_delay.png"), full_page=True)
    proc2.wait()

    # ================================================================
    # 5. Anomaly detection — scroll to anomalies section
    # ================================================================
    print("\n[5/5] Anomaly detection...")

    # Trigger anomalies: inject errors to generate error_spike
    page.evaluate("""async () => {
        await fetch('/api/fault/config', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({enabled: true, delay_ms: 0, delay_jitter_ms: 0, error_rate: 0.6, drop_rate: 0})
        });
    }""")
    time.sleep(2)

    # Run more load with errors
    proc3 = run_stress(concurrency=30, duration=10, mode="fast")
    time.sleep(8)

    # Click heuristic analysis to generate report
    page.evaluate("""async () => {
        await fetch('/api/logs/analysis');
    }""")
    time.sleep(1)

    # Scroll to anomalies + AI analysis area
    anomalies_div = page.locator("#anomalies")
    if anomalies_div.count() > 0:
        anomalies_div.scroll_into_view_if_needed()
        time.sleep(0.5)
    screen(os.path.join(IMG_DIR, "anomaly_detection.png"), full_page=True)
    proc3.wait()

    # Cleanup: disable fault
    page.evaluate("""async () => {
        await fetch('/api/fault/config', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({enabled: false, error_rate: 0})
        });
    }""")

    browser.close()

# Stop server
print("\nStopping server...")
server.terminate()
server.wait(timeout=5)
print("Done! All screenshots saved to docs/images/")
