FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    libpq-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY tests ./tests
COPY public ./public

RUN cmake -S . -B build -DCOLLAB_WITH_POSTGRES=ON \
    && cmake --build build --config Release \
    && ctest --test-dir build --output-on-failure

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    libpq5 \
    ca-certificates \
    curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/build/collab_server /app/collab_server
COPY public /app/public

ENV PORT=8080
EXPOSE 8080

HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=3 \
  CMD curl -fsS http://127.0.0.1:8080/healthz || exit 1

CMD ["/app/collab_server"]
