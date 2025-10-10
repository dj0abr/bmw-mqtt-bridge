#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# install_deps.sh
#
# Installs all dependencies required for the BMW CarData → MQTT bridge project.
# Supports Debian and Ubuntu systems (including Raspberry Pi OS).
#
# Components installed:
#   - build-essential  (compiler & linker tools)
#   - libmosquitto-dev (MQTT client library)
#   - libcurl4-openssl-dev (for HTTPS token refresh)
#   - jq (JSON parsing in shell scripts)
#   - openssl (PKCE + secure randoms)
#   - nlohmann-json3-dev (JSON header-only library for C++)
#
# Optional but recommended:
#   - mosquitto (local MQTT broker)
#   - mosquitto-clients (for testing: mosquitto_sub/pub)
#
# Copyright (c) 2025 Kurt
# Licensed under the MIT License.
# -----------------------------------------------------------------------------
set -euo pipefail

echo "🔍 Updating package lists..."
sudo apt-get update -y

echo "📦 Installing required packages..."
sudo apt-get install -y \
    build-essential \
    libmosquitto-dev \
    libcurl4-openssl-dev \
    jq \
    openssl \
    nlohmann-json3-dev \
    mosquitto \
    mosquitto-clients

echo
echo "✅ All dependencies installed successfully!"
echo "You can now compile the bridge with ./compile or your own build script."
