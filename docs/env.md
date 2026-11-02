# ⚙️ Environment Variables (.env)

This page lists **all environment variables** the bridge reads at runtime and how they behave.  
Values are loaded from the `.env` file located in the **token directory** (see below). Quotes in `.env` are supported.

---

## 📄 Token & .env Location (fixed)

The program loads `.env` from the **token directory** created by `scripts/bmw_flow.sh`:

- Default:  
  `$HOME/.local/state/bmw-mqtt-bridge/.env`
- If `$XDG_STATE_HOME` is set:  
  `${XDG_STATE_HOME}/bmw-mqtt-bridge/.env`

---

## 🌐 BMW CarData Broker

| Variable    | Type | Default                                        | Required | Description |
|-------------|------|-------------------------------------------------|----------|-------------|
| `CLIENT_ID` | str  | *(none)*                                       | **Yes**  | BMW CarData **Client ID** (GUID) from the MyBMW portal. Placeholder values are rejected. |
| `GCID`      | str  | *(none)*                                       | **Yes**  | BMW **GCID / username** for the MQTT broker (from “Show Connection Details”). Placeholder values are rejected. |
| `BMW_HOST`  | str  | `customer.streaming-cardata.bmwgroup.com`      | No       | BMW CarData MQTT hostname. |
| `BMW_PORT`  | int  | `9000`                                          | No       | BMW CarData MQTT port. |

Validation on startup:
- If `CLIENT_ID` or `GCID` are missing/placeholder → the program exits with an error.

---

## 🏠 Local MQTT Broker (Mosquitto)

| Variable         | Type | Default     | Required | Description |
|------------------|------|-------------|----------|-------------|
| `LOCAL_HOST`     | str  | `127.0.0.1` | No       | Host/IP of your local MQTT broker. |
| `LOCAL_PORT`     | int  | `1883`      | No       | Port of your local MQTT broker. |
| `LOCAL_CLIENT_ID` | str | `bmw-bridge-<CLIENT_ID>` | No | MQTT client ID on the local broker. An empty value uses the default. Explicit overrides must be unique on that broker. Independent of `LOCAL_PREFIX`. |
| `LOCAL_USER`     | str  | *(empty)*   | No       | Username for local broker authentication (optional). |
| `LOCAL_PASSWORD` | str  | *(empty)*   | No       | Password for local broker authentication (optional). |

## 🧭 Topic Prefix & Status Topic

| Variable       | Type | Default | Required | Description |
|----------------|------|---------|----------|-------------|
| `LOCAL_PREFIX` | str  | `bmw/`  | No       | Topic prefix for all republished topics. If empty, the program falls back to `bmw/`. A trailing slash is **enforced** automatically. |
| `STATUS_STABLE_DELAY` | int  | 5  | No       | delay time for bmw connection state true->false: anti flickering during token refresh |

## ✂️ Split Topics

| Variable        | Type | Default | Required | Description |
|-----------------|------|---------|----------|-------------|
| `SPLIT_TOPICS`  | int  | `0`     | No       | `0` = disabled, `1` = enabled. When enabled, JSON payloads are parsed and individual fields are republished under `vehicles/<VIN>/<propertyName>`. |

## 🔁 Retained Messages

| Variable       | Type | Default | Required | Description |
|----------------|------|---------|----------|-------------|
| `MQTT_RETAIN`  | int  | `0`     | No       | `0` = do not retain (default), `1` = retain republished topics. Affects **RAW**, **Legacy**, and **Split** topics. The **status topic** is always retained regardless of this setting. |
