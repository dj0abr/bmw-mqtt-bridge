# 🌐 MQTT Topics and Environment Variables

### Multiple BMW Accounts and Vehicles

BMW permits one streaming connection per account (`GCID`). A single bridge
subscribes to `GCID/+` and receives messages for the vehicles available to that
account; additional vehicles do not need separate bridge instances.

For independent BMW accounts, run one bridge per account. Give each instance
its own state directory via `XDG_STATE_HOME` (including its `.env` and token
files), and use different `LOCAL_PREFIX` values to keep their status and data
topics separate. For Docker, use separate state bind mounts for the containers.

Instances can share a local MQTT broker: the default local client ID is
`bmw-bridge-<CLIENT_ID>`, independent of the topic prefix. You can override it
with `LOCAL_CLIENT_ID` if your broker requires a particular format or length;
each client ID must be unique on that broker. This setting does not change
BMW's limit of one streaming connection per `GCID`.

### Status Topic Prefix

By default, the bridge publishes its connection status to:

```
bmw/status
```

If you want a different topic prefix (for example for separate BMW accounts),
you can configure it using this environment variable in your .env file:

```
LOCAL_PREFIX=mycar/
```

The bridge will then publish:

```
mycar/status
```

and all other MQTT messages (e.g. `raw`, `vehicles`, etc.) under the same prefix.

**status:**

Reports the connection state to the BMW MQTT broker (true = connected, false = disconnected).

true is published immediately when the connection is established.

false is published only after STATUS_STABLE_DELAY seconds of continuous disconnect (default: 5).

Set STATUS_STABLE_DELAY=0 to disable the delay (instant switching).

This debounce avoids brief drops (e.g., during token refresh) from causing flicker in clients that monitor the status.

---

### Split Topics (Structured JSON Publishing)

By default, the bridge republishes BMW CarData messages exactly as received  
into a local topic of the form:

```
bmw/raw/<VIN>/<eventName>
```

To make integration easier for automation systems (like Home Assistant, Node-RED, etc.),  
you can optionally enable **split topics**, which publish each data field under its own sub-topic:

add to your `.env` file:

```
SPLIT_TOPICS=1
```

This will create additional messages like:

```
bmw/vehicles/<VIN>/fuelPercentage {"value":62.5,"unit":"%","timestamp":1739790000}
bmw/vehicles/<VIN>/range_km       {"value":420}
bmw/vehicles/<VIN>/position       {"value":{"lat":48.1,"lon":11.6},"timestamp":1739790100}
```
