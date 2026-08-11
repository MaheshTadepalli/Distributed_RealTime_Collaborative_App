#!/usr/bin/env python3
"""Failure / recovery scenarios for the distributed collaborative editor.

Requires: pip install websocket-client requests
Docker Compose stack must be running.

Usage:
  python scripts/failure_test.py
  python scripts/failure_test.py --base http://localhost:8080
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from typing import Optional

try:
    import requests
    import websocket
except ImportError:
    raise SystemExit("Install: pip install websocket-client requests") from None


def run(cmd: list[str], check: bool = True) -> subprocess.CompletedProcess:
    print(f"$ {' '.join(cmd)}")
    return subprocess.run(cmd, capture_output=True, text=True, check=check)


def wait_ready(base: str, timeout: float = 60.0) -> float:
    """Return seconds until /readyz returns 200."""
    start = time.perf_counter()
    while time.perf_counter() - start < timeout:
        try:
            r = requests.get(f"{base}/readyz", timeout=2)
            if r.status_code == 200:
                return time.perf_counter() - start
        except requests.RequestException:
            pass
        time.sleep(0.5)
    raise TimeoutError(f"/readyz not ready within {timeout}s")


def wait_not_ready(base: str, timeout: float = 30.0) -> float:
    start = time.perf_counter()
    while time.perf_counter() - start < timeout:
        try:
            r = requests.get(f"{base}/readyz", timeout=2)
            if r.status_code != 200:
                return time.perf_counter() - start
        except requests.RequestException:
            return time.perf_counter() - start
        time.sleep(0.5)
    raise TimeoutError(f"/readyz still ready after {timeout}s")


def ws_commit(base: str, text: str = "X") -> dict:
    ws_url = base.replace("http://", "ws://").replace("https://", "wss://") + "/ws"
    result: dict = {}
    done = False

    def on_message(_ws, message: str):  # noqa: ANN001
        nonlocal done, result
        payload = json.loads(message)
        if payload.get("kind") == "init":
            op = {
                "kind": "op",
                "type": "insert",
                "position": 0,
                "text": text,
                "count": 0,
                "baseRevision": payload["revision"],
                "opId": f"failure-test-{time.time_ns()}",
            }
            _ws.send(json.dumps(op))
        elif payload.get("kind") == "op":
            result = payload
            done = True
            _ws.close()

    ws = websocket.WebSocketApp(ws_url, on_message=on_message)
    import threading

    t = threading.Thread(target=ws.run_forever, kwargs={"ping_interval": 20}, daemon=True)
    t.start()
    deadline = time.time() + 15
    while not done and time.time() < deadline:
        time.sleep(0.1)
    if not done:
        ws.close()
        raise TimeoutError("commit timed out")
    return result


def scenario_server_crash(base: str) -> dict:
    print("\n=== Scenario: server crash during editing ===")
    before = ws_commit(base, "A")
    print(f"  committed rev {before.get('revision')} before crash")

    run(["docker", "compose", "kill", "server"], check=False)
    time.sleep(1)

    # server2 should still serve traffic via nginx.
    after = ws_commit(base, "B")
    print(f"  committed rev {after.get('revision')} after server1 killed")

    recovery = run(["docker", "compose", "start", "server"], check=False)
    ready_sec = wait_ready(base, timeout=90)
    print(f"  server1 restarted, /readyz in {ready_sec:.2f}s")

    final = ws_commit(base, "C")
    return {
        "scenario": "server_crash",
        "before_revision": before.get("revision"),
        "after_crash_revision": after.get("revision"),
        "final_revision": final.get("revision"),
        "recovery_time_sec": ready_sec,
        "server_restart_ok": recovery.returncode == 0,
    }


def scenario_redis_restart(base: str) -> dict:
    print("\n=== Scenario: Redis restart ===")
    op = ws_commit(base, "R")
    print(f"  committed rev {op.get('revision')} before redis stop")

    not_ready_sec = 0.0
    try:
        run(["docker", "compose", "stop", "redis"], check=False)
        not_ready_sec = wait_not_ready(base, timeout=30)
        print(f"  /readyz failed in {not_ready_sec:.2f}s after redis stop")
    except TimeoutError:
        print("  WARNING: /readyz still returned 200 (LB may hit healthy replica cache)")

    run(["docker", "compose", "start", "redis"], check=False)
    recovery_sec = wait_ready(base, timeout=90)
    print(f"  /readyz recovered in {recovery_sec:.2f}s")

    after = ws_commit(base, "S")
    print(f"  committed rev {after.get('revision')} after redis recovery")

    return {
        "scenario": "redis_restart",
        "not_ready_sec": not_ready_sec,
        "recovery_time_sec": recovery_sec,
        "after_revision": after.get("revision"),
    }


def scenario_client_reconnect(base: str) -> dict:
    print("\n=== Scenario: client reconnect + catch-up ===")
    ws_url = base.replace("http://", "ws://").replace("https://", "wss://") + "/ws"
    state: dict = {"revision": 0, "catchup_ops": 0}

    ws = websocket.create_connection(ws_url, timeout=10)
    init = json.loads(ws.recv())
    state["revision"] = init["revision"]
    ws.close()

    # Another client commits while first is disconnected.
    remote = ws_commit(base, "Z")
    print(f"  remote commit rev {remote.get('revision')}")

    ws2 = websocket.create_connection(ws_url, timeout=10)
    init2 = json.loads(ws2.recv())
    ws2.send(json.dumps({"kind": "sync", "revision": state["revision"]}))
    catchup = json.loads(ws2.recv())
    ws2.close()

    ops = catchup.get("ops", [])
    print(f"  catch-up delivered {len(ops)} ops (kind={catchup.get('kind')})")

    return {
        "scenario": "client_reconnect",
        "from_revision": state["revision"],
        "remote_revision": remote.get("revision"),
        "catchup_kind": catchup.get("kind"),
        "catchup_ops": len(ops),
    }


def scenario_idempotency(base: str) -> dict:
    print("\n=== Scenario: duplicate opId (idempotency) ===")
    ws_url = base.replace("http://", "ws://").replace("https://", "wss://") + "/ws"
    results: list = []

    def recv_op(ws, op_id: str, timeout: float = 15.0) -> dict:  # noqa: ANN001
        deadline = time.time() + timeout
        while time.time() < deadline:
            ws.settimeout(max(0.1, deadline - time.time()))
            try:
                msg = json.loads(ws.recv())
            except Exception as exc:  # noqa: BLE001
                raise TimeoutError(f"waiting for op {op_id}: {exc}") from exc
            if msg.get("kind") == "op" and msg.get("opId") == op_id:
                return msg
            if msg.get("kind") == "error":
                raise RuntimeError(msg.get("message", "server error"))
        raise TimeoutError(f"no ack for {op_id}")

    ws = websocket.create_connection(ws_url, timeout=10)
    init = json.loads(ws.recv())
    op_id = f"idempotent-{time.time_ns()}"
    op = {
        "kind": "op",
        "type": "insert",
        "position": 0,
        "text": "I",
        "count": 0,
        "baseRevision": init["revision"],
        "opId": op_id,
    }
    ws.send(json.dumps(op))
    results.append(recv_op(ws, op_id))
    ws.send(json.dumps(op))  # duplicate retry
    results.append(recv_op(ws, op_id))
    ws.close()

    revs = [r.get("revision") for r in results]
    ok = len(revs) == 2 and revs[0] == revs[1]
    print(f"  revisions: {revs}  idempotent={ok}")
    return {"scenario": "idempotency", "revisions": revs, "idempotent": ok}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default="http://localhost:8080")
    args = parser.parse_args()

    print("Failure / recovery test suite")
    print(f"Target: {args.base}")

    # Verify stack is up.
    wait_ready(args.base, timeout=120)

    results = [
        scenario_idempotency(args.base),
        scenario_client_reconnect(args.base),
        scenario_server_crash(args.base),
        scenario_redis_restart(args.base),
    ]

    print("\n" + "=" * 60)
    print("SUMMARY")
    print("=" * 60)
    for r in results:
        print(json.dumps(r))
    print("=" * 60)

    failed = any(
        (r.get("scenario") == "idempotency" and not r.get("idempotent"))
        for r in results
    )
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
