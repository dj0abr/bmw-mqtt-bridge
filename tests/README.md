# Offline regression tests

From the repository root:

```bash
python3 -B -m unittest discover -s tests -v
```

Requires Python 3, Bash, curl, jq, OpenSSL, g++, pkg-config, Mosquitto (the broker), and the existing
libmosquitto/libcurl development dependencies. No Python packages are needed.

The tests use temporary state directories and artificial tokens. Device-flow
HTTP requests are intercepted by a fake curl executable; the C++ refresh tests
replace libcurl transport functions at link time. The MQTT tests start a private
Mosquitto broker on a temporary loopback port and run two bridge processes with
separate state directories. No BMW account, external network access, existing
broker or real token files are used.

Coverage includes HTTP/network errors, gateway `fault` objects, incomplete JSON,
wrong types, `null` tokens, malformed/expired JWTs, successful token rotation,
PKCE request parameters, OAuth polling and `slow_down`, owner-only permissions,
redacted diagnostics, and preservation of existing credentials on bad responses.
The build-script tests also check that compiler and pkg-config failures return
nonzero exit codes, and that compiler/linker flags are passed separately.
The MQTT tests verify that distinct BMW client IDs stay connected to the same
local broker with both shared and separate topic prefixes, that an empty
`LOCAL_CLIENT_ID` uses the default, and that explicit overrides are respected.
