# Motor Starter Dashboard

Web dashboard to monitor and control a DOL motor starter built around an ESP32 with LoRa (SX1278) and GSM (SIM900A). The ESP32 has no WiFi/Ethernet — it reaches the internet only over GPRS through the SIM900A — so this dashboard does **not** talk to the device directly. Instead, Firebase Realtime Database acts as the shared mailbox between the dashboard, the device, and the LoRa remote:

```
[Dashboard] <---> [Firebase Realtime DB] <---> [ESP32 over GSM/GPRS] <---LoRa--- [Remote transmitter]
```

## Data flow

- The dashboard writes a **command** when you click Start/Stop.
- The ESP32 polls Firebase over GPRS on an interval, reads the command, drives the relay, then writes back the resulting **state** (and acks the command).
- If the LoRa remote starts/stops the motor locally, the ESP32 (receiver side) updates `device/state` and `device/command` in Firebase the same way, so the dashboard reflects it automatically.
- Every state change also appends an entry to `device/history` for the event log and voltage chart.

## Firebase Realtime Database schema

```
device/
  state/
    motorStatus: "ON" | "OFF"
    voltage: number          // from the voltage sensor, in volts
    lastSeen: number          // Date.now() in ms, updated every poll cycle
    gsmSignal: number         // optional, CSQ or similar, -1 if unknown

  command/
    desiredState: "ON" | "OFF"
    source: "dashboard" | "lora" | "auto"
    issuedAt: number          // Date.now() in ms
    ack: boolean              // ESP32 sets true once it has applied this command

  history/
    <push-id>/
      motorStatus: "ON" | "OFF"
      voltage: number
      source: "dashboard" | "lora" | "auto"
      timestamp: number
```

### Firmware contract (for the ESP32 receiver + LoRa side)

1. On each GPRS poll cycle, read `device/command`.
2. If `command.ack == false` and `command.desiredState != state.motorStatus`, drive the relay accordingly.
3. After applying, write `device/state` (`motorStatus`, `voltage`, `lastSeen: now`) and set `device/command/ack = true`.
4. Push a new entry under `device/history` with the same `motorStatus`/`voltage`/`source`/`timestamp` so the dashboard log/chart update.
5. When the LoRa remote triggers a local start/stop, treat it the same way but set `source: "lora"` and write directly to `state`/`history` (no need to go through `command` since it's already been applied locally) — just also update `command.desiredState`/`ack: true` so the dashboard stays in sync and doesn't try to re-send a stale command.
6. The dashboard treats the device as **offline** if `lastSeen` is older than 3 minutes — adjust `OFFLINE_THRESHOLD_MS` in `src/useDevice.js` to match your actual GPRS poll interval.

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

> **Security note:** `database.rules.json` currently allows open read/write to `device/*` for development convenience, since there's no auth yet. Before going live, add Firebase Authentication and restrict `.write` (at least on `device/command`) to authenticated users, and consider a device secret/token check in the firmware if SIM900A data costs make you want to minimize unauthorized writes triggering unwanted polls.

## Project structure

- `src/firebase.js` — Firebase app/database init from env vars.
- `src/useDevice.js` — realtime subscription to `device/state` + `device/command`, exposes `sendCommand()` and derived `isOnline`.
- `src/useHistory.js` — realtime subscription to the last N `device/history` entries.
- `src/VoltageChart.jsx` — lightweight inline SVG sparkline, no charting library dependency.
- `src/App.jsx` — dashboard layout: motor control card, voltage card + chart, event log.

## Status

UI and Firebase data layer are ready. Pending: your Firebase project credentials (`.env`) and the ESP32 receiver firmware that implements the contract above.
