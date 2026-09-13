# Firmware

## motor_starter_rx/

The main receiver firmware — currently **LoRa + DOL starter control only**.
GSM/Firebase sync is deliberately left out of this build: the SIM900A module
is not working on the current board, so that integration is on hold and will
be added back as a separate step once it's sorted out (see
`sim900a_https_test/` below for the still-open HTTPS question on that front).

What it does:
- Listens for LoRa packets (polled via `LoRa.parsePacket()` — no DIO0
  interrupt line is wired on this board, per the schematic, so it can't use
  an interrupt-driven receive).
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
- Reads the voltage sense line and logs it over USB serial every 5s (no
  cloud reporting yet, by design — see above).

### Pin mapping — confirm before flashing

The schematic's exact GPIO numbers weren't reliably extractable from the
PDF text layer, so the `#define`s at the top of `motor_starter_rx.ino` use
sensible ESP32 defaults (avoiding strapping pins 0/2/15 and input-only pins
34-39 for outputs). **Check these against your actual board before flashing**:

```
LORA_NSS_PIN        5
LORA_RST_PIN        27
START_RELAY_PIN     25
STOP_RELAY_PIN      26
VOLTAGE_SENSOR_PIN  34
```

LoRa SCK/MISO/MOSI use the ESP32's default hardware SPI pins (18/19/23) via
the `LoRa` library's defaults — only override these if your board doesn't
use the default VSPI pins.

### LoRa protocol

Plain text payloads: `MSTR:START` / `MSTR:STOP`. Implemented identically on
both sides — see `motor_starter_tx/` below.

### What's deliberately NOT in this build

- No GSM/SIM900A code — on hold.
- No Firebase reporting — the dashboard won't reflect LoRa-triggered
  start/stop until GSM sync is added back in.
- No debounce/anti-repeat on the TX button side needed here — the
  transmitter (`motor_starter_tx/`) already debounces at the button level,
  so this RX firmware only needs its own command-level cooldown
  (`COMMAND_COOLDOWN_MS`) as a second line of defense.

## motor_starter_tx/

The remote transmitter firmware (ESP8266) — reads the start/stop buttons and
sends `MSTR:START` / `MSTR:STOP` over LoRa to the RX unit.

### Pin mapping — requires a board rewire, not just a firmware setting

The schematic wires LoRa CLK to GPIO16. **This can't work as drawn**: GPIO16
is not one of the ESP8266's hardware SPI pins (only GPIO12/13/14 are — this
is fixed in silicon, unlike the ESP32 where SPI pins are software-selectable),
so the SPI-based LoRa library cannot use it for SCK.

The firmware instead targets this corrected mapping — **the physical LoRa
module wiring needs to move to match it**:

| Signal     | Firmware pin | Schematic originally had |
|------------|-------------|---------------------------|
| LoRa SCK   | GPIO14 (fixed HSPI SCK) | GPIO16 |
| LoRa MISO  | GPIO12 (fixed HSPI MISO) | GPIO14 |
| LoRa MOSI  | GPIO13 (fixed HSPI MOSI) | GPIO12 |
| LoRa CS    | GPIO16 | GPIO2 |
| LoRa RESET | GPIO2 | GPIO13 |
| Start button | GPIO5 | GPIO5 (unchanged) |
| Stop button  | GPIO4 | GPIO4 (unchanged) |

CS and RESET are plain GPIO toggles either way (the LoRa library doesn't use
real hardware chip-select), so moving those two is free — only SCK/MISO/MOSI
are hardware-constrained.

### Behavior

- Buttons are wired active-LOW to GND (`INPUT_PULLUP`), debounced in
  software (`DEBOUNCE_MS`, default 250ms).
- Sends one `MSTR:START`/`MSTR:STOP` packet per press (on the press edge,
  not repeated while held).
- No sleep/low-power mode yet — worth adding if this is battery-powered and
  needs long runtime between charges (deep sleep + wake-on-button-interrupt
  would be the next step, not implemented here).

## sim900a_https_test/

A one-shot diagnostic sketch — **flash this first, before any real firmware.**

It answers one question: can your specific SIM900A module complete a real
HTTPS request to Firebase? This matters because:

- Firebase's REST API requires TLS 1.2+ with SNI.
- SIM900A's SSL stack is old and widely reported (Arduino forums, GitHub
  issues) to fail against modern servers like Firebase/Google APIs — but
  firmware varies board to board, so it's worth confirming on your actual
  hardware rather than assuming.
- Separately, SIM900A's `AT+HTTPACTION` only supports GET/POST/HEAD — there's
  no PUT, and Firebase RTDB does not honor `X-HTTP-Method-Override` (its REST
  API dispatches strictly on the literal HTTP verb). So even if HTTPS works,
  writing to a fixed path (`device/state`) still needs a plan that doesn't
  rely on native PUT.

### How to run it

1. Open `sim900a_https_test.ino` and set `APN` (and `APN_USER`/`APN_PASS` if
   your SIM needs them).
2. Wire SIM900A TX → ESP32 GPIO16, SIM900A RX → ESP32 GPIO17, common ground.
   Use an adequate power supply (2A+) — brownouts during `AT+HTTPACTION` are
   a common false failure.
3. Flash it, open Serial Monitor at 115200 baud, and read the full log.
4. Read the final `VERDICT` block.

### What to do with the result

- **Plain HTTP fails too** → the problem is GPRS/APN/signal, not SSL. Fix
  that and re-run before trusting anything else.
- **Plain HTTP works, HTTPS fails** → confirms the known SIM900A limitation.
  Next step is one of:
  - Swap the GSM module for one with real TLS support (SIM800-series or
    SIM7000/SIM7600) — keeps the Firebase schema and dashboard exactly as
    already built.
  - Add a small always-on relay (e.g. a cheap VPS) that accepts plain HTTP
    from the SIM900A and forwards as authenticated HTTPS to Firebase.
- **HTTPS works** → good news, but the missing-PUT problem is still open —
  come back to that before writing the main firmware (likely solution:
  restructure `device/state` writes as POST/push + read-latest-by-query, or
  find a PATCH-equivalent via a Cloud Function endpoint you control).

The main receiver firmware (relay/LoRa/GSM polling loop) is intentionally
not written yet — it depends on which of the above paths this test points to.
