# Offline authentication regression tests

From the repository root:

```bash
python3 -B -m unittest discover -s tests -v
```

Requires Python 3, Bash, curl, jq, OpenSSL, g++, pkg-config and the existing
libmosquitto/libcurl development dependencies. No Python packages are needed.

The tests use temporary state directories and artificial tokens. Device-flow
HTTP requests are intercepted by a fake curl executable; the C++ refresh tests
replace libcurl transport functions at link time. No BMW account, broker,
network access or real token files are used.

Coverage includes HTTP/network errors, gateway `fault` objects, incomplete JSON,
wrong types, `null` tokens, malformed/expired JWTs, successful token rotation,
PKCE request parameters, OAuth polling and `slow_down`, owner-only permissions,
redacted diagnostics, and preservation of existing credentials on bad responses.
