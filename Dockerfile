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

# Copy source files
WORKDIR /build
COPY ./src/bmw_mqtt_bridge.cpp .
COPY ./src/json.hpp .
COPY ./scripts/compile.sh .

# Make compile script executable and build
RUN chmod +x compile.sh && \
    ./compile.sh

# Runtime stage
FROM debian:bookworm-slim

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    libmosquitto1 \
    libcurl4 \
    ca-certificates \
    bash \
    curl \
    jq \
    && rm -rf /var/lib/apt/lists/*

# Copy the compiled binary and scripts
WORKDIR /app
COPY --from=builder ./build/bmw_mqtt_bridge .
COPY ./scripts/bmw_flow.sh .
COPY ./scripts/bmw_refresh.sh .

# Make scripts executable
RUN chmod +x bmw_flow.sh bmw_refresh.sh

# Environment variables for configuration
ENV BMW_HOST="customer.streaming-cardata.bmwgroup.com" \
    BMW_PORT=9000 \
    LOCAL_HOST="127.0.1" \
    LOCAL_PORT=1883 \
    LOCAL_PREFIX="bmw/" \
    OUT_DIR="/app/tokens" \
    REFRESH_SCRIPT="./bmw_refresh.sh"

# Create volume for persistent token storage
VOLUME ["/app/tokens"]

# Set entrypoint script
COPY ./scripts/docker-entrypoint.sh .
RUN chmod +x docker-entrypoint.sh
ENTRYPOINT [ "/bin/bash", "-l", "-c" ]
CMD ["./docker-entrypoint.sh"]