# Motor Starter Dashboard

Web dashboard to monitor and control a DOL motor starter built around an ESP32 with LoRa (SX1278) and a SIMCOM A7670C 4G/LTE module. The ESP32 has no WiFi/Ethernet — it reaches the internet only over cellular data through the A7670C — so this dashboard does **not** talk to the device directly. Instead, Firebase Realtime Database acts as the shared mailbox between the dashboard, the device, and the LoRa remote:

```
[Dashboard] <---> [Firebase Realtime DB] <---> [ESP32 over 4G/LTE] <---LoRa--- [Remote transmitter]
```

## History: why this isn't ThingSpeak anymore

The project originally used a SIM900A (2G) module, which has two hard blockers for talking to Firebase's REST API: no working TLS stack (Firebase requires TLS 1.2+/SNI, confirmed unreachable on real SIM900A hardware) and no `PUT` verb in its HTTP AT command set (only GET/POST/HEAD). That led to a stint on ThingSpeak, whose classic write API works over plain, unencrypted HTTP.

The project has since switched to a **SIMCOM A7670C** 4G/LTE module, which has a genuine TLS stack — solving the HTTPS problem — and moved back to Firebase for its more flexible schema and realtime dashboard updates (no polling needed). A7670C still only exposes GET/POST/HEAD at the AT command layer, so writes use `POST` with an `X-HTTP-Method-Override: PUT` header, which [Firebase's REST API officially documents supporting](https://firebase.google.com/docs/database/rest/save-data) as a way to achieve full-overwrite PUT semantics without a native PUT verb.

## Data flow

- The dashboard writes a **command** when you click Start/Stop.
- The ESP32 polls Firebase over the A7670C on an interval, reads the command, drives the relay, then writes back the resulting **state** (and acks the command).
- If the LoRa remote starts/stops the motor locally, the ESP32 (receiver side) updates `device/state` and `device/command` in Firebase the same way, so the dashboard reflects it automatically.
- Every state change also appends an entry to `device/history` for the event log and voltage chart.
- Since Firebase pushes updates in realtime, the dashboard reflects device state changes immediately — no polling delay (unlike the ThingSpeak-era dashboard).

## Firebase Realtime Database schema

```
device/
  state/
    motorStatus: "ON" | "OFF"
    voltage: number          // from the voltage sensor, in volts
    lastSeen: number          // device uptime in ms (no RTC/NTP on this board), updated every poll cycle
    gsmSignal: number         // CSQ reading, -1 if unknown

  command/
    desiredState: "ON" | "OFF"
    source: "dashboard" | "lora" | "auto"
    issuedAt: number          // ms, from whichever clock issued the command
    ack: boolean              // ESP32 sets true once it has applied this command

  history/
    <push-id>/
      motorStatus: "ON" | "OFF"
      voltage: number
      source: "dashboard" | "lora" | "auto"
      timestamp: number
```

### Firmware contract (for the ESP32 receiver + LoRa side)

See `firmware/motor_starter_rx/motor_starter_rx.ino` for the full implementation and `firmware/README.md` for the A7670C AT command details (including a confidence note on which parts are verified vs. best-guess pending your own hardware testing). Summary:

1. On each poll cycle (`SYNC_INTERVAL_MS`, default 20s), read `device/command`.
2. If `command.ack == false` and `command.desiredState != state.motorStatus`, drive the relay accordingly.
3. Write `device/state` and set `device/command/ack = true` (both via `POST` + `X-HTTP-Method-Override: PUT`).
4. Push a new entry under `device/history` (plain `POST`, which Firebase treats as a push/new-child) so the dashboard log/chart update.
5. When the LoRa remote triggers a local start/stop, write `device/command` as already-applied (`source: "lora"`, `ack: true`) and `device/state`/`device/history` directly.
6. The dashboard treats the device as **offline** if `lastSeen` is older than 3 minutes — adjust `OFFLINE_THRESHOLD_MS` in `src/useDevice.js` to match your actual poll interval.

## Setup

1. Create the Firebase project (Realtime Database, not Firestore) and copy your web app config.
2. Copy `.env.example` to `.env` and fill in the values from the Firebase console:
   ```
   VITE_FIREBASE_API_KEY=
   VITE_FIREBASE_AUTH_DOMAIN=
   VITE_FIREBASE_DATABASE_URL=
   VITE_FIREBASE_PROJECT_ID=
   VITE_FIREBASE_STORAGE_BUCKET=
   VITE_FIREBASE_MESSAGING_SENDER_ID=
   VITE_FIREBASE_APP_ID=
   ```
3. Install dependencies and run the dev server:
   ```
   npm install
   npm run dev
   ```
4. Deploy the database rules in `database.rules.json` via the Firebase console (Realtime Database → Rules) or the Firebase CLI.
5. Set `FIREBASE_HOST` and `APN` in `firmware/motor_starter_rx/motor_starter_rx.ino` to match your Firebase project and SIM card.

> **Security note:** `database.rules.json` currently allows open read/write to `device/*` for development convenience, since there's no auth yet. Before going live, add Firebase Authentication and restrict `.write` (at least on `device/command`) to authenticated users, and consider a device secret/token check in the firmware if cellular data costs make you want to minimize unauthorized writes triggering unwanted polls.

## Project structure

- `src/firebase.js` — Firebase app/database init from env vars.
- `src/useDevice.js` — realtime subscription to `device/state` + `device/command`, exposes `sendCommand()` and derived `isOnline`.
- `src/useHistory.js` — realtime subscription to the last N `device/history` entries.
- `src/VoltageChart.jsx` — lightweight inline SVG sparkline, no charting library dependency.
- `src/App.jsx` — dashboard layout: motor control card, voltage card + chart, event log.

## Firmware

See `firmware/README.md` for the full breakdown of each sketch (RX receiver + DOL starter control, TX remote, and the diagnostic sketches used along the way — SIM900A HTTPS testing, a LoRa wiring fault, GSM debug logging).

## Status

Dashboard and RX firmware both talk to Firebase. TX (remote) firmware is complete and tested working over LoRa. The A7670C's AT command sequence for HTTPS + custom headers (`AT+CSSLCFG`, `AT+HTTPPARA="SSLCFG"`) is based on the general SIMCOM A76xx family and has not yet been verified against this specific module/firmware on real hardware — see the confidence note in `firmware/README.md` before assuming it works untested.
