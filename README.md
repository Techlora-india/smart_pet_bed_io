# Smart Pet Bed ESP32 firmware

## Build configuration

Copy `include/secrets.example.h` to `include/secrets.h` and set:

- `CLOUDBASE_SYNC_URL`: exact HTTPS HTTP-trigger URL shown after deploying
  `deviceSync` in the CloudBase console.
- `DEVICE_SYNC_API_KEY`: device-only key accepted by `deviceSync`.
- `CLOUDBASE_CA_CERT`: optional PEM trust-anchor override. The checked-in
  DigiCert Global Root G2 certificate is used when this is empty.

`include/secrets.h` is ignored by Git. Do not use the Flutter publishable key
or a PostgreSQL password in the firmware.

## Provisioning protocol

1. On first boot, after an unsuccessful Wi-Fi connection, or after holding
   GPIO0 for ten seconds, connect to `karuna_luxe_<last-six-station-MAC>`.
   The station MAC is read from ESP-IDF hardware MAC data with an eFuse
   fallback, formatted as 12 uppercase hex characters, and printed at boot.
2. `GET http://192.168.4.1/` returns the device ID, MAC address, AP name,
   provisioning state, sensor readiness, and protocol version. The firmware
   and app reject all-zero/all-`F` sentinel identities. `GET /scan` returns
   `{ "networks": [{ "ssid": "...", "rssi": -52, "secure": true }] }`.
3. The app posts a JSON body to `POST /provision`:

```json
{"ssid":"home-wifi","password":"...","userId":"cloudbase-user-id"}
```

The device validates and saves the complete network configuration to NVS as
one record, then responds with HTTP 202 and
`{"status":"saved","restartInMs":1500}`. It restarts after the response has
been delivered. The app advances to its cloud-connection step, guides the user
back to an internet-connected network, registers the MAC under the signed-in
account, and confirms the first telemetry update. Hidden SSIDs and open
networks are supported. The ESP32 only supports 2.4 GHz Wi-Fi.

If a configured network is unavailable, the bed keeps its setup AP available
while retrying the saved credentials. After connection recovery, the AP stays
available for a 60-second grace period and remains open while a setup client is
connected; it closes when the network is stable and the grace period expires.
An unconfigured device keeps setup AP active. A prolonged outage opens the AP
so the user can update Wi-Fi details in the app.

At startup the firmware scans both I2C buses and logs the responding
addresses. It probes TMP117 addresses `0x48`–`0x4B` on SDA 25/SCL 32 and retries
every ten seconds if no valid sensor is available. Heating remains disabled
while the TMP117 is missing or returning invalid readings.

After connecting to home Wi-Fi, the bed posts telemetry every three seconds
and receives the latest desired command from `deviceSync`. The firmware
synchronizes its clock with NTP before making HTTPS requests so certificate
validity checks use the correct time.

## Provisioning and sync diagnostics

Open the PlatformIO serial monitor at 115200 baud:

```powershell
pio device monitor -b 115200
```

Serial logs identify `/`, `/scan`, and `/provision` requests; credential
validation and NVS save outcomes; scheduled restart; home Wi-Fi retries; and
time/TLS/CloudBase sync readiness and results. A successful cloud check-in
logs `[SYNC] Cloud update OK`. Passwords, account IDs, API keys, and request
bodies are never printed.

The heater is forced off on boot, invalid/stale sensor readings,
over-temperature, Wi-Fi loss, stale cloud responses, and cool mode. Legacy OTA
is disabled by default until its transport is migrated to validated TLS.

The temporary setup AP currently has no WPA2 passphrase and the device API key
is embedded in firmware. This meets the initial provisioning protocol but is
not a sufficient commercial anti-tamper design. A production rollout should
add a per-device setup credential (for example, printed QR/label) and a unique
per-device API key or request signature, plus ESP32 flash/NVS encryption and a
hardware thermal cutoff independent of firmware.
