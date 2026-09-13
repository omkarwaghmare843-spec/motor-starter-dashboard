# Firmware

## motor_starter_rx/

The main receiver firmware — LoRa + DOL starter control + GSM/ThingSpeak sync.

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
- Polls ThingSpeak over the SIM900A GSM/GPRS module (`SYNC_INTERVAL_MS`,
  default 30s) for dashboard commands, applies them, and reports state back.
  See "Why ThingSpeak, not Firebase" below.
- A LoRa-triggered start/stop also pushes an immediate ThingSpeak state
  update (when GPRS is up), so the dashboard reflects a remote-triggered
  start/stop without waiting for the next poll cycle.
- Reads the voltage sense line and logs it over USB serial every 5s.

### Why ThingSpeak, not Firebase

Firebase's REST API requires TLS 1.2+/SNI, which SIM900A's SSL stack cannot
reliably complete — confirmed by testing (`sim900a_https_test/`, below) and
by trying to build the original Firebase-based firmware. Even where HTTPS
worked, Firebase also needs a real `PUT` to overwrite a fixed path, and
SIM900A's `AT+HTTPACTION` only supports GET/POST/HEAD — Firebase's REST API
does not honor an `X-HTTP-Method-Override` workaround either, so that's a
dead end regardless of the TLS question.

ThingSpeak's classic write API (plain HTTP GET, e.g.
`http://api.thingspeak.com/update?api_key=...&field1=...`) was verified live
to still work over plain, unencrypted HTTP — exactly what SIM900A's
`AT+HTTPACTION=0` can do. The dashboard (`src/thingspeak.js`) and this
firmware both talk to the same two ThingSpeak channels:

```
Channel A — device state (this firmware writes, dashboard reads)
  field1 = motorStatus   (0=OFF, 1=ON)
  field2 = voltage
  field3 = gsmSignal
  field4 = lastSeen       (device uptime seconds — no RTC/NTP on this board)

Channel B — commands (dashboard writes, this firmware reads + acks)
  field1 = desiredState   (0=OFF, 1=ON)
  field2 = issuedAt       (unix seconds, from the browser's clock)
  field3 = ack            (0=pending, 1=applied by device)
```

Two channels (rather than one shared channel) so device-write and
dashboard-write traffic don't compete for ThingSpeak's free-tier rate limit
of roughly one write per 15 seconds per channel.

### Pin mapping (confirmed working on the actual board)

```
LORA_NSS_PIN        5
LORA_RST_PIN        4
START_RELAY_PIN     2
STOP_RELAY_PIN      15
RELAY_ACTIVE_HIGH   false
VOLTAGE_SENSOR_PIN  34
GSM_RX_PIN          16
GSM_TX_PIN          17
```

LoRa SCK/MISO/MOSI use the ESP32's default hardware SPI pins (18/19/23).

### ThingSpeak / GSM config

Set `APN` (and `APN_USER`/`APN_PASS` if your SIM needs them) and the four
ThingSpeak keys/channel IDs near the top of `motor_starter_rx.ino` — this
firmware needs the state channel's **write** key and the command channel's
**read + write** keys (the dashboard needs the mirror image: state read key,
command read + write keys — see the main `README.md`).

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

A one-shot diagnostic sketch that answered the question of whether this
SIM900A module can complete a real HTTPS request to Firebase. It confirmed
plain HTTP works but HTTPS does not — which is why the project moved to
ThingSpeak (plain-HTTP-compatible) instead of continuing to pursue Firebase.
Kept for reference; not part of the current firmware.

## lora_diagnostic/

A one-shot diagnostic sketch that talks to the SX1278 chip directly over
SPI (bypassing the `LoRa` library) to read back its version register (0x42,
should read `0x12` on genuine silicon). Used to debug an earlier
`LoRa.begin()` failure, which turned out to be a shorted MISO wire — not a
library or firmware issue. Kept for reference if LoRa init ever fails again.
