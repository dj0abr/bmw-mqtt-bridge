#!/bin/bash
set -e

# Create directory for token files if it doesn't exist
mkdir -p /app/tokens

# Create symbolic links for token files in volume
ln -sf /app/tokens/id_token.txt /app/id_token.txt
ln -sf /app/tokens/refresh_token.txt /app/refresh_token.txt

# If specific command provided, execute it
if [ "$1" = "./bmw_flow.sh" ] || [ "$1" = "bash" ]; then
    exec "$@"
fi

# Check if we need initial authentication
if [ ! -f ./id_token.txt ] || [ ! -f ./refresh_token.txt ]; then
    echo "No token files found. Please run initial authentication first:"
    echo "docker-compose run --rm bmw-bridge ./bmw_flow.sh"
    exit 1
fi

# Start the bridge
exec ./bmw_mqtt_bridge
