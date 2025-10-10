#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# bmw_refresh.sh
#
# refresh an ID-token
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
#
# Purpose:
#   Refresh BMW OAuth tokens using a refresh_token.
#   Writes fresh: id_token.txt (→ MQTT password), refresh_token.txt, access_token.txt
#
# Usage:
#   ./bmw_refresh.sh
#
# Requirements:
#   bash, curl, jq, openssl (optional; only if you enable WRITE_ID_EXP=1)
#
# Environment overrides (optional):
#   CLIENT_ID              : your BMW CarData client ID (GUID)
#   TOKEN_ENDPOINT         : default https://customer.bmwgroup.com/gcdm/oauth/token
#   REFRESH_TOKEN_FILE     : default ./refresh_token.txt
#   OUT_DIR                : default current directory
#   DEBUG                  : set to 1 to write token_refresh_response.json
#   WRITE_ID_EXP           : set to 1 to also write id_token.exp (Unix epoch) next to id_token.txt
#
# Security:
#   - Files are written with umask 077 (owner rw only).
#   - Do NOT commit real tokens/IDs to Git repositories.

set -euo pipefail

# ---------- Config ----------
: "${CLIENT_ID:=12345678-abcd-ef12-3456-789012345678}"  # Use default only if CLIENT_ID is unset or empty
TOKEN_ENDPOINT="https://customer.bmwgroup.com/gcdm/oauth/token"
: "${REFRESH_TOKEN_FILE:="./refresh_token.txt"}"  # Use default only if REFRESH_TOKEN_FILE is unset or empty
: "${OUT_DIR:="$."}"  # Use default only if OUT_DIR is unset or empty
DEBUG="0"
WRITE_ID_EXP="0"

# ---------- Pre-flight ----------
need() { command -v "$1" >/dev/null 2>&1 || { echo "✖ Missing dependency: $1" >&2; exit 1; }; }
need curl
need jq

if [[ "${CLIENT_ID}" == "12345678-abcd-ef12-3456-789012345678" ]]; then
  echo "⚠ CLIENT_ID is still the placeholder. Export your real CLIENT_ID or edit this script." >&2
fi

if [[ ! -s "$REFRESH_TOKEN_FILE" ]]; then
  echo "✖ Missing refresh token file: $REFRESH_TOKEN_FILE" >&2
  exit 1
fi

umask 077

mkdir -p "$OUT_DIR"

REFRESH_TOKEN="$(tr -d '\r\n' < "$REFRESH_TOKEN_FILE")"
if [[ -z "$REFRESH_TOKEN" ]]; then
  echo "✖ Refresh token is empty in $REFRESH_TOKEN_FILE" >&2
  exit 1
fi

# ---------- Request ----------
# Capture HTTP code separately to robustly detect non-200 responses.
HTTP_CODE=0
RESP="$(
  curl -sS -w '\n%{http_code}' "$TOKEN_ENDPOINT" \
    -H "Content-Type: application/x-www-form-urlencoded" \
    --data-urlencode grant_type=refresh_token \
    --data-urlencode refresh_token="$REFRESH_TOKEN" \
    --data-urlencode client_id="$CLIENT_ID"
)"
HTTP_CODE="$(tail -n1 <<<"$RESP")"
JSON="$(sed '$d' <<<"$RESP")"

[[ "$DEBUG" == "1" ]] && echo "$JSON" | jq . > "$OUT_DIR/token_refresh_response.json"

if [[ "$HTTP_CODE" != "200" ]]; then
  echo "✖ HTTP $HTTP_CODE from token endpoint" >&2
  echo "$JSON" | jq . >&2 || echo "$JSON" >&2
  exit 1
fi

ERR="$(jq -r '.error // empty' <<<"$JSON")"
if [[ -n "$ERR" ]]; then
  DESC="$(jq -r '.error_description // ""' <<<"$JSON")"
  echo "✖ Refresh failed: $ERR ($DESC)" >&2
  echo "$JSON" | jq . >&2 || true
  exit 1
fi

# ---------- Extract ----------
ID_TOKEN="$(jq -r '.id_token // empty' <<<"$JSON" | tr -d '\r\n')"
NEW_REFRESH="$(jq -r '.refresh_token // empty' <<<"$JSON" | tr -d '\r\n')"
ACCESS_TOKEN="$(jq -r '.access_token // empty' <<<"$JSON" | tr -d '\r\n')"

if [[ -z "$ID_TOKEN" || -z "$NEW_REFRESH" ]]; then
  echo "✖ Response missing id_token or refresh_token" >&2
  echo "$JSON" | jq . >&2 || true
  exit 1
fi

# ---------- Atomic write ----------
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

printf '%s' "$ID_TOKEN"     > "$tmpdir/id_token.txt"
printf '%s' "$NEW_REFRESH"  > "$tmpdir/refresh_token.txt"
[[ -n "$ACCESS_TOKEN" ]] && printf '%s' "$ACCESS_TOKEN" > "$tmpdir/access_token.txt" || true

# Optional: write id_token.exp (Unix epoch) for consumers that want it
if [[ "$WRITE_ID_EXP" == "1" ]]; then
  # Decode JWT payload (base64url) and extract .exp using jq.
  # POSIX-friendly base64url decode with padding:
  PAYLOAD="$(cut -d '.' -f2 <<<"$ID_TOKEN" | tr '_-' '/+' | awk '{l=4-(length%4); if(l<4) printf "%s", $0 substr("====",1,l); else printf "%s",$0}')"
  if command -v base64 >/dev/null 2>&1; then
    EXP="$(printf '%s' "$PAYLOAD" | base64 -d 2>/dev/null | jq -r '.exp // empty')"
    [[ -n "$EXP" ]] && printf '%s\n' "$EXP" > "$tmpdir/id_token.exp" || true
  fi
fi

mv "$tmpdir/id_token.txt"      "$OUT_DIR/id_token.txt"
mv "$tmpdir/refresh_token.txt" "$OUT_DIR/refresh_token.txt"
[[ -f "$tmpdir/access_token.txt" ]] && mv "$tmpdir/access_token.txt" "$OUT_DIR/access_token.txt" || true
[[ -f "$tmpdir/id_token.exp"   ]] && mv "$tmpdir/id_token.exp"      "$OUT_DIR/id_token.exp"      || true

trap - EXIT
rmdir "$tmpdir" 2>/dev/null || true

echo "✔ Tokens refreshed:"
ls -l "$OUT_DIR"/id_token.txt "$OUT_DIR"/refresh_token.txt 2>/dev/null || true
[[ -f "$OUT_DIR/access_token.txt" ]] && ls -l "$OUT_DIR/access_token.txt" || true
[[ -f "$OUT_DIR/id_token.exp"   ]] && echo "  (wrote id_token.exp for convenience)" || true
