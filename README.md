# Real-Time Collaborative Application

Google Docs-style collaborative editor with a C++ Operational Transformation core, now packaged as a horizontally scalable distributed system.

```
                 Load Balancer (nginx / K8s Service)
                /      |      \
           C++ Server  C++ Server  C++ Server
                \      |      /
                     Redis
                 (state + Pub/Sub)
                       |
                   PostgreSQL
                   (persistence)

            Prometheus → Grafana
            Docker / Kubernetes
```

## What stayed the same

- The OT engine in `src/ot.hpp` (transform rules, apply, document session).
- Browser pending-op rebasing in `public/app.js`.

## What is new

1. **Redis shared state + Pub/Sub** – document content, revision, history, and cross-server fan-out.
2. **PostgreSQL persistence** – documents, current revision, operation history.
3. **Multiple server replicas** behind a load balancer.
4. **Reconnect/recovery** – client sends `{"kind":"sync","revision":N}`; server returns catch-up ops or a snapshot.
5. **Health/readiness/metrics** – `/healthz`, `/readyz`, `/metrics` (Prometheus).
6. **Docker Compose + Kubernetes manifests + GitHub Actions CI**.
7. **Load/failure harness** – `scripts/load_test.py`.

## Quick start (local, in-memory)

Same as before – no Redis/Postgres required:

```powershell
New-Item -ItemType Directory -Force -Path build-manual | Out-Null
C:\MinGW\bin\g++.exe -std=c++17 -O2 -Wall -Wextra -Isrc src\main.cpp -lws2_32 -o build-manual\collab_server.exe
.\build-manual\collab_server.exe 8090
```

Open http://localhost:8090 in two browser windows.

## Distributed stack (Docker Compose)

```bash
docker compose up --build
```

- Editor + LB: http://localhost:8080
- Prometheus: http://localhost:9090
- Grafana: http://localhost:3000 (admin / admin)

Compose runs **two C++ replicas**, Redis, PostgreSQL, nginx, Prometheus, and Grafana.

## Environment variables

| Variable | Default | Purpose |
|----------|---------|---------|
| `PORT` | `8080` | HTTP/WebSocket listen port |
| `REDIS_HOST` | empty | Enable Redis shared state when set |
| `REDIS_PORT` | `6379` | Redis port |
| `DATABASE_URL` | empty | Enable PostgreSQL when set (`postgres://...`) |
| `DOCUMENT_ID` | `default` | Logical document key |

## Tests

```powershell
C:\MinGW\bin\g++.exe -std=c++17 -O2 -Wall -Wextra -Isrc tests\ot_tests.cpp -o build-manual\ot_tests.exe
.\build-manual\ot_tests.exe
```

Or with CMake:

```bash
cmake -S . -B build -DCOLLAB_WITH_POSTGRES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Load / failure testing

```powershell
pip install websocket-client requests
python scripts/benchmark.py --url ws://localhost:8080/ws --ops 10
python scripts/failure_test.py --base http://localhost:8080
```

Single-size load run:

```powershell
python scripts/load_test.py --url ws://localhost:8080/ws --clients 50 --ops 10
```

### Benchmark results (Docker Compose, 2 replicas + Redis + PostgreSQL)

Measured with `scripts/benchmark.py --ops 10` (10 ops per client, one shared document):

| Clients | Ops/s | P50 (ms) | P95 (ms) | P99 (ms) | Error % | ACKs |
|--------:|------:|---------:|---------:|---------:|--------:|------|
| 10 | 286.61 | 30.25 | 52.06 | 55.62 | 0.00 | 100/100 |
| 50 | 248.74 | 188.34 | 265.93 | 345.41 | 0.00 | 500/500 |
| 100 | 126.01 | 802.98 | 1008.68 | 1080.41 | 0.00 | 1000/1000 |
| 500 | 24.07 | 21055.00 | 25567.21 | 28096.95 | 0.98 | 4951/5000 |

**Notes**

- 10–50 clients is the realistic operating band for one document.
- 100 clients stays correct (0% errors) but latency rises under the global Redis commit lock.
- 500 clients is a saturation stress test: commits serialize on one document, so latency grows and a small fraction of ops can time out.
- Throughput does not scale linearly with client count on a single document; more waiters mostly increase queue time.

### Failure / recovery results

`scripts/failure_test.py` against the Compose stack:

| Scenario | Result |
|----------|--------|
| Duplicate `opId` (idempotency) | Same revision returned twice |
| Client reconnect | Catch-up ops delivered from client revision |
| Server crash (`docker compose kill server`) | Edits continue via `server2`; restart recovers |
| Redis restart | `/readyz` goes not-ready, then recovers; commits resume |

Suggested manual checks:

- Kill one `server` container and confirm editing continues via the other replica.
- Restart Redis and confirm `/readyz` fails then recovers.
- Disconnect a browser tab and reconnect – client should catch up from its revision.

## Kubernetes

```bash
docker build -t collab-server:latest .
kubectl apply -f deploy/k8s/collab.yaml
```

The manifest deploys 3 server replicas, Redis, PostgreSQL, readiness/liveness probes, and a LoadBalancer service.

## Key files

| Path | Role |
|------|------|
| `src/ot.hpp` | OT transform + apply (unchanged core) |
| `src/redis_store.hpp` | Shared state, lock, Pub/Sub |
| `src/postgres.hpp` | Durable documents + op history |
| `src/main.cpp` | WebSocket/HTTP server, health, metrics |
| `public/app.js` | Client sync + reconnect recovery |
| `docker-compose.yml` | Full local distributed stack |
| `deploy/k8s/collab.yaml` | Kubernetes deployment |
| `scripts/load_test.py` | Concurrent client load harness |
| `scripts/benchmark.py` | 10/50/100/500 client benchmark runner |
| `scripts/failure_test.py` | Crash / Redis / reconnect / idempotency checks |
| `.github/workflows/ci.yml` | Build, test, Docker image |
