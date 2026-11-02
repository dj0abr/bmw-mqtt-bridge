# ⚙️ Installation

You can install and run the bridge either **natively (bare metal)** or via **Docker**.

---

### 🧩 Option 1 – Classic Installation (without Docker)

For direct use on Debian, Ubuntu or Raspberry Pi OS.

```bash
# Clone the repository
git clone https://github.com/dj0abr/bmw-mqtt-bridge.git
cd bmw-mqtt-bridge

# Make scripts executable
chmod +x scripts/*.sh

# Install dependencies
./scripts/install_deps.sh

# Compile
./scripts/compile.sh

# Run OAuth2 Device Flow (creates .env automatically)
./scripts/bmw_flow.sh
```

If this is your first run, the script will:

- create ~/.local/state/bmw-mqtt-bridge/
- open nano with a new .env file  
→ insert your CLIENT_ID and GCID there, then save and exit.

Run `./scripts/bmw_flow.sh` again after saving the initial configuration.

After that, open the displayed URL in your browser, log in,  
then return to the terminal and press ENTER.

# Then start the bridge:

```bash
./src/bmw_mqtt_bridge
```
