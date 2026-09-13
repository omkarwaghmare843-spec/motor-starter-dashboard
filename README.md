# Motor Starter Dashboard

Web dashboard to monitor and control a DOL motor starter built around an ESP32 with LoRa (SX1278) and GSM (SIM900A). The ESP32 has no WiFi/Ethernet — it reaches the internet only over GPRS through the SIM900A — so this dashboard does **not** talk to the device directly. Instead, ThingSpeak acts as the shared mailbox between the dashboard, the device, and the LoRa remote:

```
[Dashboard] <---> [ThingSpeak] <---> [ESP32 over GSM/GPRS] <---LoRa--- [Remote transmitter]
```

## Why ThingSpeak, not Firebase

Firebase's REST API requires TLS 1.2+/SNI, which the SIM900A's SSL stack cannot reliably complete — confirmed on real hardware, not just in theory. Even where SIM900A's HTTPS worked at all, Firebase also needs a genuine `PUT` to overwrite a fixed path, and SIM900A's `AT+HTTPACTION` only supports GET/POST/HEAD. ThingSpeak's classic write API (`http://api.thingspeak.com/update?api_key=...&field1=...`) was verified live to still work over plain, unencrypted HTTP — exactly what `AT+HTTPACTION=0` (GET) can do.

The tradeoff: ThingSpeak is a numeric-fields-per-channel model (up to 8 fields per channel), not Firebase's flexible JSON tree, and free-tier channels are rate-limited to roughly one write per 15 seconds.

## Data flow

- The dashboard writes a **command** to the command channel when you click Start/Stop.
- The ESP32 polls the command channel over GPRS on an interval, applies it, writes the resulting **state** to the state channel, and acks the command.
- If the LoRa remote starts/stops the motor locally, the ESP32 pushes an immediate state update (tagged as applied) so the dashboard reflects it without waiting for the next poll.
- The state channel's own entry history (ThingSpeak stores every write with a timestamp) doubles as the event log and voltage chart — no separate history channel needed.

## ThingSpeak channel layout

Two channels, so device-write and dashboard-write traffic don't compete for the same per-channel rate limit:

```
Channel A — device state (ESP32 writes, dashboard reads)
  field1 = motorStatus   (0=OFF, 1=ON)
  field2 = voltage
  field3 = gsmSignal
  field4 = lastSeen       (device uptime seconds — no RTC/NTP on this board yet)

Channel B — commands (dashboard writes, ESP32 reads + acks)
  field1 = desiredState   (0=OFF, 1=ON)
  field2 = issuedAt       (unix seconds, from the browser's clock)
  field3 = ack            (0=pending, 1=applied by device)
```

### Firmware contract (for the ESP32 receiver + LoRa side)

See `firmware/motor_starter_rx/motor_starter_rx.ino` for the full implementation. Summary:

1. On each poll cycle (`SYNC_INTERVAL_MS`, default 30s — must stay above ThingSpeak's ~15s per-channel rate limit), GET Channel B's last entry.
2. If `ack == 0` and `desiredState != current motorStatus`, apply it (pulse the relevant relay).
3. Write Channel A with the resulting `motorStatus`/`voltage`/`gsmSignal`/`lastSeen`, and ack Channel B (`field3=1`).
4. When the LoRa remote triggers a local start/stop, apply it immediately and push a Channel A update tagged as already-applied — no need to go through Channel B since it was already actioned locally.
5. The dashboard treats the device as **offline** if the state channel's last entry (`created_at`, ThingSpeak's own server-side receive timestamp) is older than 3 minutes — adjust `OFFLINE_THRESHOLD_MS` in `src/useDevice.js` to match your actual GPRS poll interval.

## Setup

1. Create a free ThingSpeak account and two channels as described above (Channel A: 4 fields for state, Channel B: 3 fields for commands).
2. Copy `.env.example` to `.env` and fill in the values from each channel's **API Keys** tab:
   ```
   VITE_TS_STATE_CHANNEL_ID=
   VITE_TS_STATE_READ_KEY=
   VITE_TS_CMD_CHANNEL_ID=
   VITE_TS_CMD_READ_KEY=
   VITE_TS_CMD_WRITE_KEY=
   ```
   Note: the dashboard only needs the state channel's **read** key and the command channel's **read + write** keys — it never needs the state channel's write key, since only the device writes state.
3. Install dependencies and run the dev server:
   ```
   npm install
   npm run dev
   ```
4. Set the same channel IDs/keys in the ESP32 firmware (`firmware/motor_starter_rx/motor_starter_rx.ino`) — it needs the state channel's **write** key and the command channel's **read + write** keys (the mirror image of the dashboard's needs).

> **Security note:** the command channel's write key is embedded client-side in the dashboard's JS bundle (visible in browser devtools), so anyone who finds it could replay start/stop commands directly against ThingSpeak, bypassing the dashboard's UI. This is an accepted tradeoff for now since there's no dashboard authentication yet either — revisit both together before this goes into unattended production use.

## Project structure

- `src/thingspeak.js` — ThingSpeak REST client: fetch latest state, fetch state history, fetch/send commands.
- `src/useDevice.js` — polls state + command every ~16s, exposes `sendCommand()` and derived `isOnline`.
- `src/useHistory.js` — polls the state channel's last N entries every ~30s for the event log and voltage chart.
- `src/VoltageChart.jsx` — lightweight inline SVG sparkline, no charting library dependency.
- `src/App.jsx` — dashboard layout: motor control card, voltage card + chart, event log.

## Firmware

See `firmware/README.md` for the full breakdown of each sketch (RX receiver + DOL starter control, TX remote, and the diagnostic sketches used to debug the SIM900A HTTPS question and a LoRa wiring fault along the way).

## Status

Dashboard and RX firmware both talk to ThingSpeak and are believed complete pending your own end-to-end hardware test (GSM polling cycle observed live, dashboard reflecting a real device report). TX (remote) firmware is complete and tested working over LoRa.
