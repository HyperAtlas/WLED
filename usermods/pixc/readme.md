# PixC usermod

Bridges WLED to the PixC cloud (`pixc-mqtt`) over MQTT: applies cloud commands, reports
live state + estimated power every 30s, and handles OTA triggers.

## Enable

1. Set the **PixC API base** in WLED config (Settings → Usermods → `pixc` → `api_base`),
   e.g. `http://10.0.0.5:8080` (LAN) or `https://api.pixc.app`. The device does NOT hardcode the
   broker — on WiFi connect it calls `GET {api_base}/api/v1/provision?mac={escapedMac}`, gets the
   MQTT broker host/port, sets WLED's MQTT server, and connects. Broker can rotate server-side.
   (During pairing the app can also write `api_base` via WLED's config API.)
2. Add `pixc` to `custom_usermods` in `platformio_override.ini`:

   ```ini
   [env:esp32dev]
   custom_usermods = pixc
   ```

   (space-separate to combine with others, e.g. `custom_usermods = pixc audioreactive`)
3. Build & flash with PlatformIO.

## Topic contract

Device MAC = WLED `escapedMac` (12 hex chars). **Register the device in PixC pairing with
this exact value as `device_mac`.**

| Direction | Topic | Payload |
|---|---|---|
| sub | `pixc/d/{mac}/cmd` | `{power,brightness(0-100),r,g,b,w,effect}` |
| sub | `pixc/d/{mac}/ota` | `{job_id,url,sig_url,version,sha256}` |
| pub | `pixc/d/{mac}/state` | `{power,brightness,r,g,b,w,effect,watts}` (30s + on change) |
| pub | `pixc/d/{mac}/status` | `"online"` (retained) on connect |
| pub | `pixc/d/{mac}/ota/progress` | `{job_id,status,percent}` |

Presence is TTL-based: `pixc-mqtt` refreshes a 65s Redis key on each `/state` message.

## Follow-ups (marked TODO in code)

- **OTA signature verification**: download `sig_url` and verify the Ed25519 signature over the
  binary against a baked-in public key before flashing. Currently flashes over TLS without
  signature verification (`setInsecure()`), which is NOT production-safe.
- Pin the `firmware.pixc.app` TLS certificate instead of `setInsecure()`.
- Effect map covers solid/breeze/sunrise/aurora/pulse; extend `effectToFx`/`fxToEffect` as needed.
