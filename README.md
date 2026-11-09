# aranet-relay

Live readings from an [Aranet4](https://aranet.com/products/aranet4/) CO₂ monitor, viewable from anywhere.

The Aranet4 only talks Bluetooth. An ESP32 near it listens to the sensor's broadcasts and posts each new
measurement to a Cloudflare Worker, which stores it in D1. An iOS app reads it back.

```
Aranet4 ──BLE broadcast──▶ ESP32 (firmware/) ──HTTPS──▶ Cloudflare Worker + D1 (worker/) ──▶ app
```

## firmware/

PlatformIO project for an Adafruit Feather ESP32 V2 (other ESP32 boards have envs too).

- Listens passively; no pairing. Needs **Smart Home integrations** turned on in the Aranet Home app.
- Posts each new measurement (the sensor measures every 5 minutes) plus a heartbeat every minute.
- Verifies the Worker's TLS certificate against bundled root CAs (`include/root_cas.h`).
- Status light (Feather NeoPixel): green blip every 3 s = working, blue double flash = reading sent,
  red fast blinking = no Wi-Fi, sensor out of range, or posts failing.

```sh
cd firmware
python3 -m venv .venv && .venv/bin/pip install platformio
cp include/secrets.example.h include/secrets.h   # add the ingest token
./set-wifi.sh                                     # prompts for Wi-Fi name and password (2.4 GHz)
.venv/bin/pio run -t upload && .venv/bin/pio device monitor
```

If Wi-Fi fails, the serial log lists the 2.4 GHz networks the board can see.

## worker/

| Route | Token | |
| --- | --- | --- |
| `POST /ingest` | `INGEST_TOKEN` | One live reading from the bridge. Repeat broadcasts of the same measurement are stored once. |
| `POST /import` | `INGEST_TOKEN` | Bulk rows from the sensor's on-board log. |
| `GET /readings?hours=24` | `READ_TOKEN` | Latest reading, battery, bridge last-seen, and the series. |

```sh
cd worker && npm install
npx wrangler d1 create aranet                     # put the id in wrangler.toml
npx wrangler d1 execute aranet --remote --file schema.sql
npx wrangler secret put INGEST_TOKEN
npx wrangler secret put READ_TOKEN
npx wrangler deploy
```

## tools/

- `fetch.py` downloads the sensor's ~17-day on-board history over Bluetooth (needs the Mac paired once).
  `aranetctl -r -o` returns nothing on firmware v2.0.12, which is why this exists.
- `import_history.py` uploads that CSV to the Worker to backfill gaps.

## Broadcast format

Aranet4 manufacturer data (company ID `0x0702`), 22 bytes after the ID, little-endian:

| Offset | Field |
| --- | --- |
| 0 | flags (bit 5 = integrations on) |
| 8–9 | CO₂, ppm |
| 10–11 | temperature, ÷20 °C |
| 12–13 | pressure, ÷10 hPa |
| 14 | humidity, % |
| 15 | battery, % |
| 16 | status colour |
| 17–18 | measurement interval, s |
| 19–20 | seconds since measurement |
