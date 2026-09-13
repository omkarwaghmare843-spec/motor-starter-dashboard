/*
  Motor Starter — TX side firmware (ESP8266 remote)

  Scope: reads the start/stop push buttons and transmits the corresponding
  command over LoRa to the RX unit (see ../motor_starter_rx), which pulses
  the DOL starter's relays.

  Hardware, per Schematic_motor_starter_tx (ESP8266-12E/ESP-12E):
    - Start button (SW3) on GPIO5, pulls to GND when pressed.
    - Stop button (SW4) on GPIO4, pulls to GND when pressed.
    - LoRa SX1278 (RA-02) module.

  IMPORTANT — pin mapping differs from the raw schematic:
    The schematic's silkscreen wiring has LoRa CLK on GPIO16. GPIO16 has no
    hardware SPI capability on the ESP8266 (only GPIO12/13/14 are the fixed
    HSPI pins — this is not configurable in software, unlike ESP32), so the
    standard SPI-based LoRa library cannot use GPIO16 as SCK.

    This firmware instead assumes the LoRa module's SPI lines are wired to
    the ESP8266's actual hardware HSPI pins, with CS and RESET on plain
    GPIO toggles (the LoRa library treats both as manual GPIOs anyway, not
    true hardware signals) confirmed working on the actual board as:

        LoRa SCK   -> GPIO14  (fixed HSPI SCK,  was GPIO16 on schematic)
        LoRa MISO  -> GPIO12  (fixed HSPI MISO, matches schematic)
        LoRa MOSI  -> GPIO13  (fixed HSPI MOSI, was GPIO12 on schematic)
        LoRa CS    -> GPIO2   (was GPIO2 on schematic)
        LoRa RESET -> GPIO16  (was GPIO13 on schematic)

    Verified working over the air against motor_starter_rx — if you're
    building a new board from scratch, wire it to match the table above.

  Libraries required (Arduino Library Manager):
    - LoRa (Sandeep Mistry)
*/

#include <SPI.h>
#include <LoRa.h>

// ---------------------------------------------------------------------------
// Pin configuration — see rewiring note above
// ---------------------------------------------------------------------------

#define START_BUTTON_PIN  5    // GPIO5, INPUT_PULLUP, active LOW
#define STOP_BUTTON_PIN   4    // GPIO4, INPUT_PULLUP, active LOW

#define LORA_NSS_PIN      2    // CS
#define LORA_RST_PIN      16   // RESET
#define LORA_DIO0_PIN     -1   // not used on TX side (no receive needed)
#define LORA_FREQUENCY    433E6   // must match the RX unit's frequency

// ---------------------------------------------------------------------------
// Protocol — must match motor_starter_rx.ino exactly
// ---------------------------------------------------------------------------

static const char *PACKET_PREFIX = "MSTR:";
static const char *CMD_START     = "START";
static const char *CMD_STOP      = "STOP";

// ---------------------------------------------------------------------------
// Button debounce
// ---------------------------------------------------------------------------

#define DEBOUNCE_MS 250

struct Button {
  uint8_t pin;
  const char *command;
  bool lastReading;
  bool stableState;
  unsigned long lastChangeMs;
};

Button startButton = { START_BUTTON_PIN, CMD_START, HIGH, HIGH, 0 };
Button stopButton  = { STOP_BUTTON_PIN,  CMD_STOP,  HIGH, HIGH, 0 };

// ---------------------------------------------------------------------------
// LoRa
// ---------------------------------------------------------------------------

void setupLora() {
  LoRa.setPins(LORA_NSS_PIN, LORA_RST_PIN, LORA_DIO0_PIN);

  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("[LoRa] Init FAILED — check wiring/frequency. Halting.");
    while (true) {
      delay(1000);
    }
  }
  Serial.println("[LoRa] Ready");
}

void sendCommand(const char *command) {
  String packet = String(PACKET_PREFIX) + command;
  Serial.print("[LoRa] Sending: ");
  Serial.println(packet);

  LoRa.beginPacket();
  LoRa.print(packet);
  LoRa.endPacket();
}

// ---------------------------------------------------------------------------
// Button handling — sends one command per press (on the press edge only,
// not while held), with simple time-based debounce.
// ---------------------------------------------------------------------------

void pollButton(Button &btn) {
  bool reading = digitalRead(btn.pin);   // active LOW (INPUT_PULLUP)
  unsigned long now = millis();

  if (reading != btn.lastReading) {
    btn.lastChangeMs = now;
    btn.lastReading = reading;
  }

  if ((now - btn.lastChangeMs) > DEBOUNCE_MS && reading != btn.stableState) {
    btn.stableState = reading;

    if (btn.stableState == LOW) {   // pressed
      Serial.print("[BUTTON] ");
      Serial.print(btn.command);
      Serial.println(" pressed");
      sendCommand(btn.command);
    }
  }
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[BOOT] Motor Starter TX (remote) firmware starting...");

  pinMode(START_BUTTON_PIN, INPUT_PULLUP);
  pinMode(STOP_BUTTON_PIN, INPUT_PULLUP);

  setupLora();
  Serial.println("[BOOT] Ready — press Start or Stop to send a command.");
}

void loop() {
  pollButton(startButton);
  pollButton(stopButton);
}
