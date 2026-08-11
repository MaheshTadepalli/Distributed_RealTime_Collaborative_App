#!/usr/bin/env python3
"""Load harness for the collaborative editor.

Each client:
  connect → init → wait for start gate → send ops one-at-a-time waiting for matching opId ack.

Requires: pip install websocket-client
"""

from __future__ import annotations

import argparse
import json
import random
import statistics
import threading
import time
import uuid
from dataclasses import dataclass, field
from typing import List

try:
    import websocket
except ImportError as exc:  # pragma: no cover
    raise SystemExit("Install dependency: pip install websocket-client") from exc


def percentile(values: List[float], q: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(q * (len(ordered) - 1)))
    return ordered[index]


@dataclass
class LoadTestResult:
    clients: int
    ops_per_client: int
    planned_ops: int
    acks: int
    elapsed_sec: float
    ops_per_sec: float
    p50_ms: float
    p95_ms: float
    p99_ms: float
    mean_ms: float
    errors: List[str] = field(default_factory=list)

    @property
    def error_rate(self) -> float:
        if self.planned_ops == 0:
            return 0.0
        return max(0, self.planned_ops - self.acks) / self.planned_ops


class ClientWorker(threading.Thread):
    def __init__(
        self,
        url: str,
        ops: int,
        latencies: List[float],
        errors: List[str],
        result_lock: threading.Lock,
        ready: threading.Event,
        start_gate: threading.Event,
        connected_count: List[int],
    ):
        super().__init__(daemon=True)
        self.url = url
        self.ops = ops
        self.latencies = latencies
        self.errors = errors
        self.result_lock = result_lock
        self.ready = ready
        self.start_gate = start_gate
        self.connected_count = connected_count
        self.revision = 0
        self.client_id = ""
        self.document = ""

    def run(self) -> None:
        ws = None
        try:
            ws = websocket.create_connection(self.url, timeout=30, ping_interval=20)
            init = json.loads(ws.recv())
            if init.get("kind") != "init":
                raise RuntimeError(f"expected init, got {init.get('kind')}")
            self.client_id = init["clientId"]
            self.revision = init["revision"]
            self.document = init["document"]

            with self.result_lock:
                self.connected_count[0] += 1
            self.ready.set()
            if not self.start_gate.wait(timeout=120):
                raise RuntimeError("timed out waiting for start gate")

            for _ in range(self.ops):
                self._send_one(ws)
        except Exception as exc:  # noqa: BLE001
            with self.result_lock:
                self.errors.append(f"{self.client_id or 'client'}: {exc}")
        finally:
            if ws is not None:
                try:
                    ws.close()
                except Exception:  # noqa: BLE001
                    pass

    def _apply(self, op: dict) -> None:
        self.revision = op["revision"]
        pos = min(op["position"], len(self.document))
        if op["type"] == "insert":
            self.document = self.document[:pos] + op.get("text", "") + self.document[pos:]
        else:
            count = op.get("count", 0)
            self.document = self.document[:pos] + self.document[pos + count :]

    def _send_one(self, ws) -> None:  # noqa: ANN001
        op_id = f"{self.client_id}-{uuid.uuid4().hex}"
        # Append-style insert keeps OT cheap under load.
        payload = {
            "kind": "op",
            "type": "insert",
            "position": len(self.document),
            "text": random.choice("abcdefghijklmnopqrstuvwxyz"),
            "count": 0,
            "baseRevision": self.revision,
            "opId": op_id,
        }
        started = time.perf_counter()
        ws.send(json.dumps(payload))

        deadline = time.time() + 30
        while time.time() < deadline:
            ws.settimeout(max(0.1, deadline - time.time()))
            try:
                raw = ws.recv()
            except websocket.WebSocketTimeoutException:
                continue
            msg = json.loads(raw)
            kind = msg.get("kind")
            if kind == "error":
                raise RuntimeError(msg.get("message", "server error"))
            if kind in ("op",):
                self._apply(msg)
                if msg.get("opId") == op_id:
                    with self.result_lock:
                        self.latencies.append((time.perf_counter() - started) * 1000.0)
                    return
            elif kind == "catchup":
                for op in msg.get("ops", []):
                    self._apply(op)
            elif kind == "snapshot":
                self.revision = msg["revision"]
                self.document = msg["document"]
        raise TimeoutError(f"no ack for {op_id}")


def run_load_test(
    url: str,
    clients: int,
    ops_per_client: int,
    burst: bool = False,  # kept for CLI compatibility; ops are sequential per client
    retry_duplicate: bool = False,
    timeout: int = 300,
) -> LoadTestResult:
    del burst, retry_duplicate  # unused in sync harness
    latencies: List[float] = []
    errors: List[str] = []
    result_lock = threading.Lock()
    start_gate = threading.Event()
    connected_count = [0]

    workers = []
    for i in range(clients):
        ready = threading.Event()
        worker = ClientWorker(
            url, ops_per_client, latencies, errors, result_lock, ready, start_gate, connected_count
        )
        workers.append((worker, ready))
        worker.start()
        # Stagger connects so nginx/accept queues don't drop sockets.
        if clients >= 50 and i % 10 == 9:
            time.sleep(0.05)

    # Wait until all clients connected (or timeout).
    deadline = time.time() + min(120, 5 + clients * 0.2)
    while time.time() < deadline:
        with result_lock:
            n = connected_count[0]
        if n >= clients:
            break
        time.sleep(0.05)

    with result_lock:
        connected = connected_count[0]
    if connected < clients:
        errors.append(f"only {connected}/{clients} clients connected before start")

    started = time.perf_counter()
    start_gate.set()
    for worker, _ in workers:
        worker.join(timeout=timeout)
    elapsed = time.perf_counter() - started

    with result_lock:
        lat_copy = list(latencies)
        err_copy = list(errors)

    planned = clients * ops_per_client
    return LoadTestResult(
        clients=clients,
        ops_per_client=ops_per_client,
        planned_ops=planned,
        acks=len(lat_copy),
        elapsed_sec=elapsed,
        ops_per_sec=len(lat_copy) / elapsed if elapsed else 0.0,
        p50_ms=percentile(lat_copy, 0.50),
        p95_ms=percentile(lat_copy, 0.95),
        p99_ms=percentile(lat_copy, 0.99),
        mean_ms=statistics.mean(lat_copy) if lat_copy else 0.0,
        errors=err_copy,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="Collaborative editor load test")
    parser.add_argument("--url", default="ws://localhost:8080/ws")
    parser.add_argument("--clients", type=int, default=10)
    parser.add_argument("--ops", type=int, default=20, help="ops per client")
    parser.add_argument("--burst", action="store_true", help="ignored (compat)")
    parser.add_argument("--retry-duplicate", action="store_true", help="ignored (compat)")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    result = run_load_test(
        url=args.url,
        clients=args.clients,
        ops_per_client=args.ops,
    )

    if args.json:
        print(
            json.dumps(
                {
                    "clients": result.clients,
                    "planned_ops": result.planned_ops,
                    "acks": result.acks,
                    "elapsed_sec": result.elapsed_sec,
                    "ops_per_sec": result.ops_per_sec,
                    "p50_ms": result.p50_ms,
                    "p95_ms": result.p95_ms,
                    "p99_ms": result.p99_ms,
                    "error_rate": result.error_rate,
                    "errors": result.errors[:20],
                },
                indent=2,
            )
        )
        return

    print("=== Load test results ===")
    print(f"clients:           {result.clients}")
    print(f"ops planned:       {result.planned_ops}")
    print(f"acks received:     {result.acks}")
    print(f"elapsed_sec:       {result.elapsed_sec:.3f}")
    print(f"ops_per_sec:       {result.ops_per_sec:.2f}")
    print(f"latency_p50_ms:    {result.p50_ms:.2f}")
    print(f"latency_p95_ms:    {result.p95_ms:.2f}")
    print(f"latency_p99_ms:    {result.p99_ms:.2f}")
    print(f"latency_mean_ms:   {result.mean_ms:.2f}")
    print(f"error_rate:        {result.error_rate * 100:.2f}%")
    print(f"errors:            {len(result.errors)}")
    for err in result.errors[:10]:
        print(f"  - {err}")


if __name__ == "__main__":
    main()
