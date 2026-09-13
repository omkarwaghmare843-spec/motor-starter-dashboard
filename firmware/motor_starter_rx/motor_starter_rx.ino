/*
  Motor Starter — RX side firmware (ESP32)

  Scope of this sketch (GSM/Firebase intentionally left out for now — the
  SIM900A module is currently non-functional on this board; that part will
  be added back once the GSM path is sorted out separately):

    - Receive LoRa START/STOP commands from the remote transmitter
      (ESP8266 + 2 buttons + LoRa, schematic to follow) and drive the DOL
      starter's start/stop relays accordingly.
    - Read the voltage sensor (LM358-based AC transformer sense circuit)
      and log it over USB serial.

  Hardware, per Schematic_motor_starter_tx (RX side sheet):
    - ESP32-WROOM-32D
    - LoRa SX1278 (RA-02) on SPI, RESET on a dedicated GPIO, DIO0/1/2/3 not
      wired to the ESP32 (no interrupt pin available) -> firmware polls
      LoRa.parsePacket() instead of using an interrupt.
    - Two independent relay channels, each opto-isolated (PC817MB) and
      driven through a 2N2222A transistor stage:
        "start" -> Q2 -> PC817MB(U7) -> RELAY-SPST(U9) -> CN2 (AC)
        "stop"  -> Q1 -> PC817MB(U4) -> RELAY-SPST(U3) -> CN3 (AC)
      This is a standard self-latching DOL starter: start/stop are
      momentary pulses, not held signals -- the starter's own auxiliary
      contact holds the contactor in after a start pulse, and a stop pulse
      breaks that latch.
    - Voltage sense ("vtg") from an LM358-based AC transformer sense module
      into an ESP32 ADC input.

  Pin assignments below are sensible ESP32 defaults (avoiding strapping
  pins 0/2/15 and input-only pins 34-39 for anything that needs to be an
  output) — the PDF schematic's exact GPIO numbers were not fully legible
  from the extracted text, so these are marked clearly for you to correct
  to match your actual board.

  Libraries required (Arduino Library Manager):
    - LoRa (Sandeep Mistry)
*/

#include <SPI.h>
#include <LoRa.h>

// ---------------------------------------------------------------------------
// Pin configuration — CONFIRM/CORRECT these against your actual PCB wiring
// ---------------------------------------------------------------------------

// LoRa SX1278 (hardware SPI: default ESP32 VSPI = SCK18, MISO19, MOSI23)
#define LORA_NSS_PIN      5
#define LORA_RST_PIN      4
#define LORA_DIO0_PIN     -1     // not wired on this board -> polling only
#define LORA_FREQUENCY    433E6  // must match the transmitter's frequency

// Relay outputs (through opto + transistor stage, per schematic)
#define START_RELAY_PIN   25
#define STOP_RELAY_PIN    26
#define RELAY_ACTIVE_HIGH true   // set false if a LOW pulse is what fires the opto stage

// Voltage sense ("vtg" from the LM358 AC-sense module) — ADC1 input only
// (ADC2 pins conflict with WiFi; not used here, but keep sense inputs on ADC1)
#define VOLTAGE_SENSOR_PIN 34
#define VOLTAGE_SCALE      110.0   // calibrate: real_voltage = adc_volts * VOLTAGE_SCALE

// ---------------------------------------------------------------------------
// DOL starter timing
// ---------------------------------------------------------------------------

#define RELAY_PULSE_MS        700    // how long to energize start/stop relay
#define COMMAND_COOLDOWN_MS   2000   // ignore repeat commands faster than this (debounce/anti-chatter)

// ---------------------------------------------------------------------------
// LoRa packet protocol
// ---------------------------------------------------------------------------

// Expected payloads: "MSTR:START" / "MSTR:STOP"
// The prefix guards against acting on stray packets from other 433MHz
// devices sharing the band -- the transmitter must send this exact prefix.
static const char *PACKET_PREFIX = "MSTR:";
static const char *CMD_START     = "START";
static const char *CMD_STOP      = "STOP";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum MotorState { MOTOR_OFF, MOTOR_ON };
MotorState motorState = MOTOR_OFF;

unsigned long lastCommandMs = 0;
unsigned long lastVoltageLogMs = 0;
#define VOLTAGE_LOG_INTERVAL_MS 5000

// ---------------------------------------------------------------------------
// Relay helpers
// ---------------------------------------------------------------------------

void relayWrite(int pin, bool energize) {
  bool level = RELAY_ACTIVE_HIGH ? energize : !energize;
  digitalWrite(pin, level ? HIGH : LOW);
}

void pulseRelay(int pin, const char *label) {
  Serial.print("[RELAY] Pulsing ");
  Serial.print(label);
  Serial.print(" for ");
  Serial.print(RELAY_PULSE_MS);
  Serial.println("ms");

  relayWrite(pin, true);
  delay(RELAY_PULSE_MS);
  relayWrite(pin, false);
}

// ---------------------------------------------------------------------------
// DOL starter logic
// ---------------------------------------------------------------------------

void startMotor() {
  if (motorState == MOTOR_ON) {
    Serial.println("[DOL] Start requested but motor already ON — ignoring");
    return;
  }
  Serial.println("[DOL] Starting motor");
  pulseRelay(START_RELAY_PIN, "START");
  motorState = MOTOR_ON;
}

void stopMotor() {
  if (motorState == MOTOR_OFF) {
    Serial.println("[DOL] Stop requested but motor already OFF — ignoring");
    return;
  }
  Serial.println("[DOL] Stopping motor");
  pulseRelay(STOP_RELAY_PIN, "STOP");
  motorState = MOTOR_OFF;
}

// ---------------------------------------------------------------------------
// Voltage sensing
// ---------------------------------------------------------------------------

float readVoltage() {
  int raw = analogRead(VOLTAGE_SENSOR_PIN);   // 0-4095 on ESP32 ADC
  float adcVolts = (raw / 4095.0) * 3.3;
  return adcVolts * VOLTAGE_SCALE;
}

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
  Serial.println("[LoRa] Ready, listening for commands...");
}

void handleLoraPacket(int packetSize) {
  if (packetSize == 0) return;

  String message;
  message.reserve(packetSize);
  while (LoRa.available()) {
    message += (char)LoRa.read();
  }
  message.trim();

  int rssi = LoRa.packetRssi();
  Serial.print("[LoRa] Received (rssi=");
  Serial.print(rssi);
  Serial.print("): ");
  Serial.println(message);

  if (!message.startsWith(PACKET_PREFIX)) {
    Serial.println("[LoRa] Missing/unrecognized prefix — ignoring packet");
    return;
  }

  String command = message.substring(strlen(PACKET_PREFIX));
  command.trim();

  unsigned long now = millis();
  if (now - lastCommandMs < COMMAND_COOLDOWN_MS) {
    Serial.println("[LoRa] Command arrived during cooldown — ignoring (debounce)");
    return;
  }

  if (command == CMD_START) {
    lastCommandMs = now;
    startMotor();
  } else if (command == CMD_STOP) {
    lastCommandMs = now;
    stopMotor();
  } else {
    Serial.println("[LoRa] Unrecognized command after valid prefix — ignoring");
  }
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[BOOT] Motor Starter RX firmware starting...");
  Serial.println("[BOOT] GSM/Firebase sync is disabled in this build (SIM900A on hold).");

  pinMode(START_RELAY_PIN, OUTPUT);
  pinMode(STOP_RELAY_PIN, OUTPUT);
  relayWrite(START_RELAY_PIN, false);   // fail-safe: both relays de-energized at boot
  relayWrite(STOP_RELAY_PIN, false);

  pinMode(VOLTAGE_SENSOR_PIN, INPUT);

  setupLora();
}

void loop() {
  int packetSize = LoRa.parsePacket();
  if (packetSize > 0) {
    handleLoraPacket(packetSize);
  }

  unsigned long now = millis();
  if (now - lastVoltageLogMs >= VOLTAGE_LOG_INTERVAL_MS) {
    lastVoltageLogMs = now;
    Serial.print("[SENSE] Voltage: ");
    Serial.print(readVoltage(), 1);
    Serial.print("V  |  Motor: ");
    Serial.println(motorState == MOTOR_ON ? "ON" : "OFF");
  }
}
