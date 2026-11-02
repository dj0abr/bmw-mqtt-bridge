# 🐳 Option 2 – Docker Installation

Prebuilt Linux images are published to
[`ghcr.io/dj0abr/bmw-mqtt-bridge`](https://github.com/dj0abr/bmw-mqtt-bridge/pkgs/container/bmw-mqtt-bridge)
for `linux/amd64`, `linux/arm64`, and `linux/arm/v7`. Docker selects the matching
architecture automatically. Windows and macOS users can run these Linux images
with Docker Desktop; on Windows, use Linux-container mode and a WSL shell for the
commands and home-directory paths below.

## Install using the published image

```bash
git clone https://github.com/dj0abr/bmw-mqtt-bridge.git
cd bmw-mqtt-bridge
docker compose pull bmw-bridge
```

The bridge stores its `.env` file and all tokens on the host in
`~/.local/state/bmw-mqtt-bridge`. Compose mounts the same directory in the
container at `/app/state/bmw-mqtt-bridge`, so an existing working installation can
keep its credentials when switching between native and Docker installations.
Use the same host account for setup and runtime; token files use mode `0600`.

For a new installation, authenticate:

```bash
docker compose run --rm -it bmw-bridge ./bmw_flow.sh
```

On the first run, the script creates `.env` and opens an editor. Enter `CLIENT_ID`
and `GCID`, then run the command again. Open the displayed URL, finish browser
authorization, return to the terminal and press **ENTER**. Start the bridge after
the script confirms that the tokens were validated and saved.

Set `LOCAL_HOST` in this state-directory `.env` to an MQTT broker address
reachable **from the container**. `127.0.0.1` inside a container refers to the
container itself. Docker Desktop provides `host.docker.internal`; on Linux
Docker Engine this name may need explicit host-gateway configuration, or use the
host's reachable LAN address. For a broker in another container, use its service
name on a shared Docker network.

```bash
docker compose up -d bmw-bridge
docker compose logs -f bmw-bridge
```

## Update an existing installation

```bash
git pull --ff-only
docker compose pull bmw-bridge
docker compose up -d bmw-bridge
```

Working tokens are kept; a new login is not required for a normal update. If a
new device-code login is needed, stop the bridge before running the login script,
then start it again.

`latest` tracks successful builds of `main`. Each published image also receives
`sha-<full-commit-SHA>`. Version tags such as `1.2.3` are published when a
corresponding Git tag such as `v1.2.3` is pushed. To select a particular tag, set
`BMW_BRIDGE_IMAGE` in the shell running Compose, or in a Compose `.env` beside
`docker-compose.yml` (this is separate from the bridge's state-directory `.env`).
For an immutable image reference, use its registry digest instead of a tag:

```bash
export BMW_BRIDGE_IMAGE=ghcr.io/dj0abr/bmw-mqtt-bridge@sha256:<image-digest>
```

## Build from source

The published image is optional. Keep the same volumes and settings while adding
the source-build override:

```bash
docker compose -f docker-compose.yml -f docker-compose.build.yml build bmw-bridge
docker compose -f docker-compose.yml -f docker-compose.build.yml run --rm -it bmw-bridge ./bmw_flow.sh
docker compose -f docker-compose.yml -f docker-compose.build.yml up -d bmw-bridge
```

Skip the login command if the installation already has working tokens. Use both
`-f` arguments for later commands that should keep using the local image. Native
installation remains available as described in [Classic installation](install.md).

## Maintainer: first publication

The workflow tests native code and builds/starts images for all three platforms
on pull requests, without registry credentials or publishing. Only builds from
`main` and version tags publish images, after those checks pass. It uses the
repository's `GITHUB_TOKEN`; no separate registry secret is needed.

GitHub initially creates new container packages as **private**, even for public
repositories. After the first successful publication, open the package's
**Package settings → Change visibility → Public** so users can pull anonymously.
The workflow warns if anonymous access is unavailable.
