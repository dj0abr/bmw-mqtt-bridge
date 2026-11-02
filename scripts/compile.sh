#!/bin/bash
# compile.sh – build bmw_mqtt_bridge inside src directory
set -euo pipefail

# get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
SRC_DIR="$ROOT_DIR/src"

cd "$SRC_DIR" || exit 1

echo "Compiling bmw_mqtt_bridge..."
MOSQUITTO_FLAGS="$(pkg-config --cflags --libs libmosquitto)"
# pkg-config emits separate compiler/linker arguments.
# shellcheck disable=SC2086
g++ -std=c++17 -O2 -pthread \
  bmw_mqtt_bridge.cpp -o bmw_mqtt_bridge \
  $MOSQUITTO_FLAGS -lcurl

echo "✅ Build successful: $SRC_DIR/bmw_mqtt_bridge"
