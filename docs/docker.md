# 🐳 Option 2 – Docker Installation

```bash
# Clone the repository and prepare the environment
git clone https://github.com/dj0abr/bmw-mqtt-bridge.git
cd bmw-mqtt-bridge
```

The bridge stores its .env file and all token files **on the host system** under  
`~/.local/state/bmw-mqtt-bridge`.

The Docker container automatically links to this folder,  
so host and container share the same configuration and tokens.  
You can freely switch between **Docker** and the **classic bare-metal** version without re-authenticating.

```bash
# Authenticate with BMW (creates .env automatically)
docker compose run --rm -it bmw-bridge ./bmw_flow.sh
```

If this is your first run, it will create `~/.local/state/bmw-mqtt-bridge/.env`  
and open the editor asking you to enter your CLIENT_ID and GCID (and other environment variables, if required) before continuing.  
After you’ve saved and closed the editor, run the above command again.

Open the displayed URL in your browser, log in, then return and press **ENTER**.

```bash
# Start bridge
docker compose up -d

# Logs
docker compose logs -f bmw-bridge
```

The bridge will now automatically stream BMW CarData to your local MQTT broker.
