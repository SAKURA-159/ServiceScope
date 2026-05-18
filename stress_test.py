#!/usr/bin/env python3
"""
ServiceScope Stress Test Tool
Generates concurrent load to test the C++ service.
Usage: python stress_test.py [--url http://localhost:8080] [--concurrency 50] [--duration 30]
"""

import argparse
import concurrent.futures
import time
import statistics
import json
import sys
from urllib import request, error


def hit_endpoint(url, path, timeout=10):
    """Hit a single endpoint and return (path, status, latency_ms)."""
    start = time.time()
    try:
        req = request.Request(f"{url}{path}")
        with request.urlopen(req, timeout=timeout) as resp:
            resp.read()
            latency = (time.time() - start) * 1000
            return (path, resp.status, latency)
    except Exception as e:
        latency = (time.time() - start) * 1000
        return (path, 0, latency)


def worker(url, paths, stop_event, stats_list):
    """Worker thread: continuously hit random endpoints."""
    import random
    while not stop_event.is_set():
        path = random.choice(paths)
        result = hit_endpoint(url, path)
        stats_list.append(result)


def fetch_metrics(url):
    """Fetch current metrics from the service."""
    try:
        req = request.Request(f"{url}/metrics")
        with request.urlopen(req, timeout=5) as resp:
            return json.loads(resp.read().decode())
    except Exception:
        return None


def main():
    parser = argparse.ArgumentParser(description="ServiceScope Stress Test")
    parser.add_argument("--url", default="http://localhost:8080", help="Target URL")
    parser.add_argument("--concurrency", type=int, default=50, help="Concurrent workers")
    parser.add_argument("--duration", type=int, default=30, help="Test duration in seconds")
    parser.add_argument("--delay", type=int, default=0, help="Add work delay (ms) to requests")
    parser.add_argument("--mode", choices=["mixed", "fast", "slow", "data"],
                        default="mixed", help="Workload pattern")
    args = parser.parse_args()

    paths = {
        "mixed": ["/api/work", "/api/random", "/api/data",
                  "/api/work?delay=10", "/api/data?count=50"],
        "fast": ["/api/health", "/api/work", "/api/work?delay=1"],
        "slow": ["/api/work?delay=100", "/api/work?delay=500", "/api/random"],
        "data": ["/api/data?count=1000", "/api/data?count=500",
                 "/api/data?count=100", "/api/data?count=2000"],
    }[args.mode]

    if args.delay > 0:
        paths = [f"/api/work?delay={args.delay}"]

    print(f"ServiceScope Stress Test")
    print(f"  Target:      {args.url}")
    print(f"  Concurrency: {args.concurrency}")
    print(f"  Duration:    {args.duration}s")
    print(f"  Workload:    {args.mode}")
    print(f"  Endpoints:   {paths}")
    print()

    # Check server is up
    metrics = fetch_metrics(args.url)
    if metrics is None:
        print("ERROR: Cannot reach server. Make sure ServiceScope is running.")
        sys.exit(1)
    print(f"Server is up. Active connections: {metrics['active_connections']}")
    print()

    stats = []

    import threading
    stop_event = threading.Event()
    stats_lock = threading.Lock()

    def safe_worker():
        import random
        while not stop_event.is_set():
            path = random.choice(paths)
            result = hit_endpoint(args.url, path)
            with stats_lock:
                stats.append(result)

    print(f"Starting {args.concurrency} workers for {args.duration}s...")
    start_time = time.time()

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as executor:
        futures = [executor.submit(safe_worker) for _ in range(args.concurrency)]

        # Progress reporting
        last_print = 0
        while time.time() - start_time < args.duration:
            time.sleep(1)
            elapsed = time.time() - start_time
            count = len(stats)
            if elapsed - last_print >= 2 and count > 0:
                recent = stats[-1000:] if len(stats) > 1000 else stats
                latencies = [s[2] for s in recent]
                errors = sum(1 for s in recent if s[1] >= 400 or s[1] == 0)
                print(f"  [{elapsed:.0f}s] Requests: {count:>8d}  "
                      f"QPS: {len(recent)/2:.0f}  "
                      f"Avg Lat: {statistics.mean(latencies):.1f}ms  "
                      f"Errors: {errors}  "
                      f"P99: {sorted(latencies)[int(len(latencies)*0.99)]:.0f}ms")
                last_print = elapsed

        stop_event.set()

    duration = time.time() - start_time

    # Final report
    print(f"\n{'='*60}")
    print(f"STRESS TEST RESULTS")
    print(f"{'='*60}")
    total = len(stats)
    if total == 0:
        print("No requests completed.")
        return

    latencies = [s[2] for s in stats]
    errors = [s for s in stats if s[1] >= 400 or s[1] == 0]
    success = [s for s in stats if s[1] < 400 and s[1] > 0]

    sorted_lat = sorted(latencies)

    print(f"  Duration:        {duration:.1f}s")
    print(f"  Total Requests:  {total}")
    print(f"  Success:         {len(success)} ({100*len(success)/total:.1f}%)")
    print(f"  Errors:          {len(errors)} ({100*len(errors)/total:.1f}%)")
    print(f"  Avg QPS:         {total/duration:.1f}")
    print(f"  Latency Avg:     {statistics.mean(latencies):.1f}ms")
    print(f"  Latency P50:     {sorted_lat[len(sorted_lat)//2]:.0f}ms")
    print(f"  Latency P90:     {sorted_lat[int(len(sorted_lat)*0.9)]:.0f}ms")
    print(f"  Latency P99:     {sorted_lat[int(len(sorted_lat)*0.99)]:.0f}ms")
    print(f"  Latency P999:    {sorted_lat[int(len(sorted_lat)*0.999)]:.0f}ms")
    print(f"  Latency Max:     {max(latencies):.0f}ms")

    # Fetch server-side metrics
    print(f"\n{'='*60}")
    print(f"SERVER-SIDE METRICS")
    print(f"{'='*60}")
    metrics = fetch_metrics(args.url)
    if metrics:
        print(f"  Server QPS:      {metrics['qps']['5s']:.1f}")
        print(f"  Server Error%:   {metrics['error_rate_pct']:.2f}%")
        print(f"  Server P99:      {metrics['latency']['p99_ms']}ms")
        print(f"  Connections:     {metrics['active_connections']}")
        print(f"  Total Requests:  {metrics['total_requests']}")


if __name__ == "__main__":
    main()
