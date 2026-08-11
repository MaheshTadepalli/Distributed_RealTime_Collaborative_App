#!/usr/bin/env python3
"""Benchmark harness – runs load tests at 10/50/100/500 concurrent clients.

Usage:
  python scripts/benchmark.py --url ws://localhost:8080/ws
  python scripts/benchmark.py --url ws://localhost:8080/ws --ops 10 --clients 10 50 100 500
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

# Allow importing load_test from same directory.
sys.path.insert(0, str(Path(__file__).parent))
from load_test import run_load_test  # noqa: E402


def main() -> None:
    parser = argparse.ArgumentParser(description="Run collaborative editor benchmarks")
    parser.add_argument("--url", default="ws://localhost:8080/ws")
    parser.add_argument("--ops", type=int, default=10, help="ops per client")
    parser.add_argument(
        "--clients",
        type=int,
        nargs="+",
        default=[10, 50, 100, 500],
        help="concurrent client counts to test",
    )
    parser.add_argument("--json", action="store_true", help="emit JSON results")
    parser.add_argument("--burst", action="store_true", help="no inter-op delay (max throughput)")
    args = parser.parse_args()

    results = []
    print("=" * 72)
    print("COLLABORATIVE EDITOR BENCHMARK")
    print(f"url={args.url}  ops/client={args.ops}")
    print("=" * 72)

    for n in args.clients:
        print(f"\n--- {n} clients ---")
        result = run_load_test(
            url=args.url,
            clients=n,
            ops_per_client=args.ops,
            burst=args.burst,
            timeout=300 if n >= 100 else 180,
        )
        results.append(result)
        print(f"  ops/sec:     {result.ops_per_sec:.2f}")
        print(f"  p50 latency: {result.p50_ms:.2f} ms")
        print(f"  p95 latency: {result.p95_ms:.2f} ms")
        print(f"  p99 latency: {result.p99_ms:.2f} ms")
        print(f"  error rate:  {result.error_rate * 100:.2f}%")
        print(f"  acks:        {result.acks}/{result.planned_ops}")
        time.sleep(2)

    print("\n" + "=" * 72)
    print(f"{'Clients':>8} {'Ops/s':>10} {'P50':>8} {'P95':>8} {'P99':>8} {'Err%':>8}")
    print("-" * 72)
    for r in results:
        print(
            f"{r.clients:>8} {r.ops_per_sec:>10.2f} "
            f"{r.p50_ms:>7.2f} {r.p95_ms:>7.2f} {r.p99_ms:>7.2f} "
            f"{r.error_rate * 100:>7.2f}"
        )
    print("=" * 72)

    if args.json:
        import dataclasses

        print(json.dumps([dataclasses.asdict(r) for r in results], indent=2))


if __name__ == "__main__":
    main()
