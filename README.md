# 🚗 BMW CarData → MQTT Bridge

After BMW disabled access for *bimmerconnected*, an alternative solution was required.  
The best working replacement is the new **BMW CarData MQTT streaming interface**,  
which this bridge connects to and forwards to your local MQTT broker.

A small C++ bridge that connects **BMW ConnectedDrive CarData (MQTT)** to your **local Mosquitto MQTT broker**.  
It authenticates via the official BMW OAuth2 device flow and continuously republishes vehicle telemetry in real time.

---

## 🧩 Features

- MQTT v5 protocol with reason codes and reconnect logic  
- Automatic **token refresh** using BMW’s `refresh_token`  
- Local **watchdog & reconnect** if the BMW broker drops the connection  
- Publishes a small JSON status message (`bmw/status`) showing online/offline state  
- Lightweight: depends only on `libmosquitto`, `libcurl`, and `nlohmann/json` (header-only)  
- Runs perfectly on Debian, Ubuntu, or Raspberry Pi OS  

---

## 🧱 Project Structure

```
bmw-mqtt-bridge/
│
├── bmw_flow.sh          # Start OAuth2 device flow, get first tokens
├── bmw_refresh.sh       # Refresh id_token using refresh_token
├── bmw_mqtt_bridge.cpp  # Main bridge application (C++)
├── compile.sh           # Simple build script (g++)
├── install_deps.sh      # Installs required dependencies via apt
├── json.hpp             # nlohmann/json header (MIT license)
├── LICENSE              # MIT license
└── README.md            # This file
```

---

## ⚙️ Requirements

Tested on:
- Debian 12 / Ubuntu 22.04+ / Raspberry Pi OS Bookworm
- libmosquitto ≥ 2.0
- libcurl ≥ 7.74
- g++ ≥ 10

---

## 📥 Download and Preparation

```bash
# Clone the repository
git clone https://github.com/dj0abr/bmw-mqtt-bridge.git

# Make scripts executable
chmod +x install_deps.sh compile.sh bmw_flow.sh bmw_refresh.sh

# Install dependencies
./install_deps.sh
```

---

## 🚗 Get Your BMW IDs

1. Go to the [MyBMW website](https://www.bmw-connecteddrive.com/)  
   (You should already have an account and your car should be registered.)
2. Navigate to **Personal Data → My Vehicles → CarData**  
3. Click on **"Create Client ID"**  
   ⚠️ *Do **not** click on "Authenticate Vehicle"*
4. Copy the **Client ID** and insert it into:
   - `bmw_flow.sh`
   - `bmw_refresh.sh`
   - `bmw_mqtt_bridge.cpp`
5. Scroll down to **CARDATA STREAM → Show Connection Details**
6. Copy the **USERNAME** and insert it into `bmw_mqtt_bridge.cpp` as **GCID**

---

## ⚙️ Compile the Program

```bash
./compile.sh
```

---

## 🔑 Get Your First BMW Token

```bash
./bmw_flow.sh
```

1. A URL will be displayed in the terminal.  
2. Copy it into your browser and log in with your BMW account.  
3. Once the login is successful, return to the terminal and press **ENTER**.  
4. The tokens will be written to disk.

⚠️ **Note:**  
The BMW token expires after **1 hour** and becomes invalid.  
If the `bmw_mqtt_bridge` is not used for more than 1 hour,  
you will need to log in again manually by running:

```bash
./bmw_flow.sh
```

---

## 🚀 Start the Bridge

```bash
./bmw_mqtt_bridge
```

---

## 🐳 Docker

### Prerequisites

1. Clone repository and change directory:
```bash
git clone https://github.com/dj0abr/bmw-mqtt-bridge.git
cd bmw-mqtt-bridge
```

2. Configure environment variables:
```bash
# Create .env file from example
cp .env.example .env

# Edit .env file with your values
nano .env
```

### Build and Run Container

1. Build container image:
```bash
docker compose build
```

2. Perform BMW authentication:
```bash
docker compose run --rm bmw-bridge ./bmw_flow.sh
```
   - Open the displayed URL in your browser
   - Log in with your BMW account
   - Return to terminal and press ENTER

3. Start bridge:
```bash
docker compose up -d
```

---

## 📝 Notes

- Your **local MQTT server** must run on the **same PC** as this program.  
- If it runs on a different PC, edit the IP address in  
  `bmw_mqtt_bridge.cpp` — look for the line:

```cpp
#define LOCAL_HOST "127.0.0.1"
```

---

## 🔒 Security Notes

- BMW CarData is a private API — use responsibly.  
- Never publish or share your `id_token` / `refresh_token`.  
- Tokens expire automatically; the bridge refreshes them securely.  
- Keep your Mosquitto broker private or protected by authentication.  

---

## 🧾 License

MIT License  
Copyright (c) 2025 Kurt, DJ0ABR

This project also includes [`nlohmann/json`](https://github.com/nlohmann/json)  
licensed under the MIT License.

---

## ☕ Credits

Developed by **Kurt**  
Contributions, pull requests, and improvements are welcome!
