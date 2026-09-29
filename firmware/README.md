# Firmware

## motor_starter_rx/

The main receiver firmware — LoRa + DOL starter control + GSM/Firebase sync.

What it does:
- Listens for LoRa packets (polled via `LoRa.parsePacket()` — no DIO0
  interrupt line is wired on this board, so it can't use an
  interrupt-driven receive).
- Validates a `MSTR:` prefix on the payload before acting on it, to reject
  stray packets from other 433MHz devices sharing the band — a real safety
  consideration since this controls a physical motor starter. Recognized
  commands: `MSTR:START`, `MSTR:STOP`.
- Drives the two relay channels (start/stop) as **momentary pulses**
  (`RELAY_PULSE_MS`, default 700ms), matching a standard self-latching DOL
  starter: the starter's own auxiliary contact holds the contactor in after
  a start pulse, and a stop pulse breaks that latch — the ESP32 does not
  hold either relay energized continuously.
- Applies a cooldown (`COMMAND_COOLDOWN_MS`) so a burst of repeated/duplicate
  LoRa packets can't rapid-fire the relays.
- Polls Firebase over the A7670C 4G/LTE module (`SYNC_INTERVAL_MS`, default
  20s) for dashboard commands, applies them, and reports state back. See
  "Firebase over A7670C" below.
- A LoRa-triggered start/stop also pushes an immediate Firebase update (when
  the module is online), so the dashboard reflects a remote-triggered
  start/stop without waiting for the next poll cycle.
- Reads the voltage sense line and logs it over USB serial every 5s.

### Firebase over A7670C

The project's original module (SIM900A, 2G) had two hard blockers for
talking to Firebase's REST API directly: no working TLS stack (Firebase
needs TLS 1.2+/SNI), and no `PUT` verb in its HTTP AT command set (only
GET/POST/HEAD). Those blockers led to a stint on ThingSpeak (plain-HTTP
compatible, still in git history if useful) before switching to A7670C.

A7670C is a genuine 4G/LTE module with a real TLS stack (`AT+CSSLCFG`),
solving the HTTPS problem. It still only exposes GET/POST/HEAD at the
`AT+HTTPACTION` layer — but Firebase's REST API
[officially documents](https://firebase.google.com/docs/database/rest/save-data)
honoring an `X-HTTP-Method-Override: PUT` header on a `POST` to get full
overwrite (PUT) semantics. So every "write" in this firmware is actually a
`POST` with that header, not a native PUT — and Firebase treats it exactly
like a PUT per its own docs.

**Confidence note:** the exact `AT+CSSLCFG` parameter values (SSL context
index, TLS version code, and whether HTTPS binds via
`AT+HTTPPARA="SSLCFG",<ctxid>` or a separate flag) are based on the general
SIMCOM A76xx/SIM7600 AT command family and have **not** been verified
against A7670C's own manual for your specific firmware revision. If HTTPS
requests fail specifically (while plain AT commands/network registration
work fine), check SIMCOM's "A76XX Series_HTTP(S)_Application Note" PDF for
your firmware version — command details are known to drift between SIMCOM
firmware releases. GPRS/PDP context activation also uses the more standard
`AT+CGDCONT`/`AT+CGACT` (3GPP) commands rather than SIM800/900's
`AT+SAPBR`, since A76xx-family modules may not implement the latter at all.

Firebase Realtime Database schema (`device/state`, `device/command`,
`device/history`) is unchanged from the original design — see the main
`README.md` for the full schema and firmware contract.

### Pin mapping (confirmed working on the actual board)

```
LORA_NSS_PIN        5
LORA_RST_PIN        4
START_RELAY_PIN     2
STOP_RELAY_PIN      15
RELAY_ACTIVE_HIGH   false
VOLTAGE_SENSOR_PIN  34
GSM_RX_PIN          21
GSM_TX_PIN          22
GSM_BAUD            115200
```

LoRa SCK/MISO/MOSI use the ESP32's default hardware SPI pins (18/19/23). GSM
uses hardware UART2 (not `SoftwareSerial`, which is unreliable at 115200
baud on ESP32).

### Firebase / GSM config

Set `APN` (Airtel India: `airtelgprs.com`) and `FIREBASE_HOST` near the top
of `motor_starter_rx.ino` to match your Firebase project (same project the
dashboard uses — see `src/firebase.js` / `.env` in the project root). No API
keys needed here since Firebase's REST API is reached directly by host +
path, unlike ThingSpeak's per-channel keys.

### LoRa protocol

Plain text payloads: `MSTR:START` / `MSTR:STOP`. Implemented identically on
both sides — see `motor_starter_tx/` below.

## motor_starter_tx/

The remote transmitter firmware (ESP8266) — reads the start/stop buttons and
sends `MSTR:START` / `MSTR:STOP` over LoRa to the RX unit. Tested working
over the air against `motor_starter_rx`.

### Pin mapping (confirmed working)

The schematic originally wired LoRa CLK to GPIO16, which isn't one of the
ESP8266's hardware SPI pins (only GPIO12/13/14 are — fixed in silicon,
unlike the ESP32 where SPI pins are software-selectable), so the SPI-based
LoRa library couldn't use it for SCK. The board's SPI lines were corrected
to the actual hardware HSPI pins:

```
LoRa SCK    GPIO14  (fixed HSPI SCK)
LoRa MISO   GPIO12  (fixed HSPI MISO)
LoRa MOSI   GPIO13  (fixed HSPI MOSI)
LoRa CS     GPIO2
LoRa RESET  GPIO16
Start button GPIO5
Stop button  GPIO4
```

CS and RESET are plain GPIO toggles either way (the LoRa library doesn't use
real hardware chip-select), so those two are free to place anywhere;
SCK/MISO/MOSI are the hardware-constrained ones.

### Behavior

- Buttons are wired active-LOW to GND (`INPUT_PULLUP`), debounced in
  software (`DEBOUNCE_MS`, default 250ms).
- Sends one `MSTR:START`/`MSTR:STOP` packet per press (on the press edge,
  not repeated while held).
- No sleep/low-power mode yet — worth adding if this is battery-powered and
  needs long runtime between charges (deep sleep + wake-on-button-interrupt
  would be the next step, not implemented here).

## sim900a_https_test/

A one-shot diagnostic sketch from when the project used a SIM900A (2G)
module. It answered the question of whether SIM900A could complete a real
HTTPS request to Firebase. It confirmed plain HTTP works but HTTPS does not
— which is why the project moved to ThingSpeak (plain-HTTP-compatible) for a
while, before switching hardware to the A7670C (which has a real TLS stack)
and moving back to Firebase. Kept for reference; not applicable to A7670C
and not part of the current firmware.

## lora_diagnostic/

A one-shot diagnostic sketch that talks to the SX1278 chip directly over
SPI (bypassing the `LoRa` library) to read back its version register (0x42,
should read `0x12` on genuine silicon). Used to debug an earlier
`LoRa.begin()` failure, which turned out to be a shorted MISO wire — not a
library or firmware issue. Kept for reference if LoRa init ever fails again.
