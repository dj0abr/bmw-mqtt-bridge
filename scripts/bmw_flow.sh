#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# bmw_flow.sh
#
# Obtain a new BMW CarData OAuth2 device code and generate id_token / refresh_token.
# Requires manual user authorization via the provided BMW login URL.
#
# Copyright (c) 2025 Kurt, DJ0ABR
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
# -----------------------------------------------------------------------------
# bmw_flow.sh
#
# Purpose:
#   Run the BMW OAuth 2.0 Device Authorization Grant (with PKCE) to obtain:
#     - access_token
#     - id_token           (→ use as MQTT password for BMW Streaming MQTT)
#     - refresh_token      (→ use to refresh tokens later)
#
# Usage:
#   ./bmw_flow.sh
#
# Requirements:
#   - bash, curl, jq, openssl
#
# Behavior:
#   - On the first run, this script creates the directory:
#       ~/.local/state/bmw-mqtt-bridge
#   - If no .env file exists, it will be created automatically with placeholders
#     and opened in your text editor (nano by default).
#   - Enter your real CLIENT_ID and GCID, save, exit the editor,
#     and then rerun the script.
#
# Outputs (stored in ~/.local/state/bmw-mqtt-bridge, permissions 600):
#   access_token.txt
#   id_token.txt
#   refresh_token.txt
#
# Notes:
#   - This script is interactive: it prints a verification URL you must open
#     in a browser and confirm login/consent.
#   - Polls the token endpoint respecting "interval" and "slow_down".
#   - id_token is a JWT that contains the 'exp' claim used by the bridge.
#
# Security:
#   - Files are written with owner-only permissions (rw-------).
#   - Do NOT commit real tokens/IDs to Git repositories.
# -----------------------------------------------------------------------------


set -euo pipefail
umask 077

# ---------- Fixed locations (no path overrides) ----------
STATE_BASE="${XDG_STATE_HOME:-$HOME/.local/state}"
OUT_DIR="$STATE_BASE/bmw-mqtt-bridge"
ENV_FILE="$OUT_DIR/.env"

# Ensure token dir exists
mkdir -p -m 700 "$OUT_DIR"

# ---------- Bootstrap .env if missing ----------
if [[ ! -f "$ENV_FILE" ]]; then
  cat >"$ENV_FILE" <<'EOF'
# BMW CarData Credentials
CLIENT_ID=11111111-1111-1111-1111-111111111111
GCID=11111111-1111-1111-1111-111111111111

# Optional MQTT Configuration
#LOCAL_HOST=mosquitto
#LOCAL_PORT=1883
#LOCAL_PREFIX=bmw/
#LOCAL_USER=username
#LOCAL_PASSWORD=password
EOF
  if ! [ -t 0 ]; then
    echo "Non-interactive mode detected."
    echo "Run with an interactive TTY:"
    echo "  docker compose run --rm -it bmw-bridge ./bmw_flow.sh"
    exit 1
  fi
  echo "Created $ENV_FILE"
  echo "Please enter your real CLIENT_ID and GCID into $ENV_FILE."
  "${EDITOR:-nano}" "$ENV_FILE" || true
  echo "Re-run this script after updating $ENV_FILE."
  exit 1
fi

# ---------- Load .env from fixed location ----------
set -a
# shellcheck disable=SC1090
. "$ENV_FILE"
set +a

# ---------- Validate CLIENT_ID / GCID ----------
if [[ -z "${CLIENT_ID:-}" || "$CLIENT_ID" == "11111111-1111-1111-1111-111111111111" ]]; then
  if ! [ -t 0 ]; then
    echo "Non-interactive mode detected."
    echo "Run with an interactive TTY:"
    echo "  docker compose run --rm -it bmw-bridge ./bmw_flow.sh"
    exit 1
  fi
  echo "✖ CLIENT_ID is missing or still the placeholder in $ENV_FILE." >&2
  "${EDITOR:-nano}" "$ENV_FILE" || true
  echo "Re-run this script after updating $ENV_FILE."
  exit 1
fi

if [[ -z "${GCID:-}" || "$GCID" == "11111111-1111-1111-1111-111111111111" ]]; then
  if ! [ -t 0 ]; then
    echo "Non-interactive mode detected."
    echo "Run with an interactive TTY:"
    echo "  docker compose run --rm -it bmw-bridge ./bmw_flow.sh"
    exit 1
  fi
  echo "✖ GCID is missing or still the placeholder in $ENV_FILE." >&2
  "${EDITOR:-nano}" "$ENV_FILE" || true
  echo "Re-run this script after updating $ENV_FILE."
  exit 1
fi

# ---------- Static OAuth config ----------
DEVICE_ENDPOINT="https://customer.bmwgroup.com/gcdm/oauth/device/code"
TOKEN_ENDPOINT="https://customer.bmwgroup.com/gcdm/oauth/token"
SCOPES="authenticate_user openid cardata:streaming:read"

# ---------- Pre-flight checks ----------
need() { command -v "$1" >/dev/null 2>&1 || { echo "✖ Missing dependency: $1" >&2; exit 1; }; }
need curl
need jq
need openssl

# Keep HTTP responses and staged tokens private, and remove them on every exit.
AUTH_TMP_DIR="$(mktemp -d "$OUT_DIR/.auth.XXXXXX")"
trap 'rm -rf -- "$AUTH_TMP_DIR"' EXIT
RESPONSE_FILE="$AUTH_TMP_DIR/response.json"
HTTP_CODE=""

request_json() {
  if ! HTTP_CODE="$(curl -sS --connect-timeout 10 --max-time 20 \
    --output "$RESPONSE_FILE" --write-out '%{http_code}' "$@")"; then
    echo "✖ BMW request failed (network/TLS/timeout). Existing tokens were not changed." >&2
    return 1
  fi
  if [[ ! "$HTTP_CODE" =~ ^[0-9]{3}$ ]] || \
     ! jq -e 'type == "object"' "$RESPONSE_FILE" >/dev/null 2>&1; then
    echo "✖ BMW returned an invalid JSON response (HTTP $HTTP_CODE). Existing tokens were not changed." >&2
    return 1
  fi
}

# Extract only a bounded error code; raw responses may contain credentials.
error_code() {
  jq -r 'try ((.error // .fault.detail.errorcode // "") |
    if type == "string" and test("^[A-Za-z0-9_.-]{1,128}$") then . else "" end) catch ""' \
    "$RESPONSE_FILE"
}

report_error() {
  local code
  code="$(error_code)"
  echo "✖ $1 (HTTP $HTTP_CODE${code:+, $code}). Existing tokens were not changed." >&2
}

# ---------- PKCE (per run): 96 unreserved characters, without line breaks ----------
CODE_VERIFIER="$(openssl rand -hex 48)"
CODE_CHALLENGE="$(printf '%s' "$CODE_VERIFIER" \
  | openssl dgst -sha256 -binary \
  | openssl base64 -A \
  | tr '+/' '-_' \
  | tr -d '=')"

echo "1) Requesting device code…"
request_json \
  -H "Accept: application/json" \
  -H "Content-Type: application/x-www-form-urlencoded" \
  --data-urlencode client_id="$CLIENT_ID" \
  --data-urlencode response_type="device_code" \
  --data-urlencode scope="$SCOPES" \
  --data-urlencode code_challenge="$CODE_CHALLENGE" \
  --data-urlencode code_challenge_method="S256" \
  "$DEVICE_ENDPOINT"

if [[ "$HTTP_CODE" != 200 ]] || ! jq -e '
  def positive_integer: type == "number" and . == floor and . > 0 and . < 2147483647;
  (.error == null) and (.fault == null) and
  (.device_code | type == "string" and length > 0 and . != "null") and
  ((if .interval == null then 5 else .interval end) | positive_integer) and
  (.expires_in | positive_integer)
' "$RESPONSE_FILE" >/dev/null 2>&1; then
  report_error "Failed to obtain a valid device code"
  exit 1
fi

DEVICE_CODE="$(jq -r '.device_code' "$RESPONSE_FILE")"
VERIFY_URL="$(jq -r '
  if (.verification_uri_complete | type == "string" and length > 0) then
    .verification_uri_complete
  elif (.verification_uri | type == "string" and length > 0) and
       (.user_code | type == "string" and length > 0) then
    .verification_uri + "?user_code=" + (.user_code | @uri)
  else "" end' "$RESPONSE_FILE")"
if [[ "$VERIFY_URL" != https://* ]]; then
  report_error "Missing or invalid verification URL"
  exit 1
fi
INTERVAL="$(jq -r '.interval // 5' "$RESPONSE_FILE")"
EXPIRES_IN="$(jq -r '.expires_in' "$RESPONSE_FILE")"
# Include browser authorization and HTTP request time in the device-code lifetime.
DEADLINE=$((SECONDS + EXPIRES_IN))

echo
echo "2) Open the following URL in your browser and complete login/consent:"
echo "   $VERIFY_URL"
echo
trap 'echo; echo "Aborted by user."; exit 1' INT
read -r -p "Press Enter once you see 'Anmeldung erfolgreich / Login successful'… " _
trap - INT

echo
echo "3) Polling token endpoint every ${INTERVAL}s…"

while (( SECONDS < DEADLINE )); do
  request_json \
    -H "Content-Type: application/x-www-form-urlencoded" \
    --data-urlencode grant_type="urn:ietf:params:oauth:grant-type:device_code" \
    --data-urlencode device_code="$DEVICE_CODE" \
    --data-urlencode client_id="$CLIENT_ID" \
    --data-urlencode code_verifier="$CODE_VERIFIER" \
    "$TOKEN_ENDPOINT"

  ERR="$(error_code)"
  if [[ "$ERR" == authorization_pending || "$ERR" == slow_down ]]; then
    if [[ "$HTTP_CODE" != 200 && "$HTTP_CODE" != 400 ]]; then
      report_error "Token endpoint rejected the request"
      exit 1
    fi
    if [[ "$ERR" == slow_down ]]; then
      INTERVAL=$((INTERVAL+5))
      echo "Server says 'slow_down' → new interval ${INTERVAL}s"
    fi
    LEFT=$((DEADLINE-SECONDS))
    if (( LEFT <= 0 )); then break; fi
    WAIT="$INTERVAL"
    if (( WAIT > LEFT )); then WAIT="$LEFT"; fi
    printf '…waiting for user confirmation (remaining ~%ss)\n' "$LEFT"
    sleep "$WAIT"
    continue
  fi

  if [[ "$HTTP_CODE" != 200 ]] || ! jq -e '
    def token: type == "string" and length > 0 and . != "null" and (test("\\s") | not);
    (.error == null) and (.fault == null) and
    (.access_token | token) and (.id_token | token) and (.refresh_token | token) and
    (.expires_in | type == "number" and . == floor and . > 0) and
    (.id_token | test("^[A-Za-z0-9_-]+\\.[A-Za-z0-9_-]+\\.[A-Za-z0-9_-]+$"))
  ' "$RESPONSE_FILE" >/dev/null 2>&1; then
    report_error "Token response is incomplete, invalid, or contains an OAuth/gateway error"
    if [[ "$ERR" == invalid_grant || "$ERR" == access_denied || "$ERR" == expired_token || "$ERR" == expired_device_code ]]; then
      echo "Please restart this script and complete a new browser authorization." >&2
    fi
    exit 1
  fi

  if ! TOKEN_EXP="$(jq -er --argjson now "$(date +%s)" '
    .id_token | split(".")[1] | gsub("-"; "+") | gsub("_"; "/") |
    @base64d | fromjson | .exp |
    select(type == "number" and . == floor and . > $now)
  ' "$RESPONSE_FILE" 2>/dev/null)"; then
    report_error "ID token has no valid future expiry"
    exit 1
  fi

  # Stage the entire validated response before replacing any token. Each mv is
  # atomic on this filesystem. Save the rotated refresh token first for recovery.
  for name in access_token id_token refresh_token; do
    jq -er --arg name "$name" '.[$name]' "$RESPONSE_FILE" > "$AUTH_TMP_DIR/$name.txt"
    chmod 0600 "$AUTH_TMP_DIR/$name.txt"
  done
  for name in refresh_token id_token access_token; do
    if ! mv -fT -- "$AUTH_TMP_DIR/$name.txt" "$OUT_DIR/$name.txt"; then
      echo "✖ Could not save $name.txt. Check permissions in $OUT_DIR." >&2
      exit 1
    fi
  done

  echo
  echo "✔ Tokens validated and saved in: $OUT_DIR (permissions 0600)"
  echo "   ID token expires at Unix timestamp $TOKEN_EXP"
  exit 0
done

echo
echo "✖ Device code expired. Restart this script to obtain a new code; existing tokens were not changed." >&2
exit 1
