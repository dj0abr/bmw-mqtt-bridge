# 🔒 Security Notes

- Use the official BMW CarData interfaces responsibly.
- Never publish or share your `id_token` / `refresh_token`.  
- Tokens expire automatically; the bridge refreshes them securely.  
- Keep your Mosquitto broker private or protected by authentication.  

## Authentication and token files

The device-code login and automatic refresh validate the HTTP response, token
fields and ID-token expiry before replacing any token. Failed requests and
invalid responses leave existing token files unchanged. JWT expiry is inspected
for scheduling; this is not a cryptographic signature verification.

New token files use owner-only permissions (`0600`). Run the authentication
script and the bridge as the same user, including when using systemd. Newly
created state directories use `0700`; existing directory permissions are kept.

An already working installation does not require a new device-code login just
because the software was updated. If a new login is necessary, stop the bridge
first, run `scripts/bmw_flow.sh` (or
`docker compose run --rm -it bmw-bridge ./bmw_flow.sh`), then restart the bridge.
This avoids concurrent refreshes during authorization. If the script creates
the initial `.env` file, fill in the IDs and run it again.

Each token file is replaced atomically. The rotated refresh token is saved first
so that a later file-write failure can be recovered with another refresh. The
three files are not a single filesystem transaction; check write errors and
directory permissions if saving fails.

`token_refresh_response.json` now contains only a diagnostic summary (HTTP
status, error code or expiry), with permissions `0600`. Raw token responses are
never written to this file. An old diagnostic file is replaced on the next
completed HTTP refresh response; treat older copies and backups as secrets.
