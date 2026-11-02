# -----------------------------------------------------------------------------
# Build stage
FROM debian:bookworm-slim AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    g++ \
    make \
    libmosquitto-dev \
    libcurl4-openssl-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY ./src ./src
COPY ./scripts ./scripts
RUN chmod +x ./scripts/compile.sh && bash ./scripts/compile.sh

# -----------------------------------------------------------------------------
# Runtime stage
FROM debian:bookworm-slim

# Install runtime dependencies only (lean but includes nano for interactive setup)
RUN apt-get update && apt-get install -y \
    libmosquitto1 \
    libcurl4 \
    ca-certificates \
    bash \
    curl \
    jq \
    openssl \
    nano \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy compiled binary
COPY --from=builder /build/src/bmw_mqtt_bridge /app/bmw_mqtt_bridge

# Copy scripts
COPY ./scripts/bmw_flow.sh .
COPY ./scripts/docker-entrypoint.sh .

RUN chmod +x /app/bmw_mqtt_bridge /app/bmw_flow.sh /app/docker-entrypoint.sh || true

# Default environment
ENV XDG_STATE_HOME=/app/state \
    BMW_HOST=customer.streaming-cardata.bmwgroup.com \
    BMW_PORT=9000 \
    LOCAL_HOST=host.docker.internal \
    LOCAL_PORT=1883 \
    LOCAL_PREFIX=bmw/

# Persist token/config directory
VOLUME ["/app/state"]

COPY ./scripts/docker-entrypoint.sh /app/docker-entrypoint.sh
RUN chmod +x /app/docker-entrypoint.sh

ENTRYPOINT ["/app/docker-entrypoint.sh"]
CMD ["/app/bmw_mqtt_bridge"]

