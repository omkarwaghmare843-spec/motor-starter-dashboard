/*
  Motor Starter — RX side firmware (ESP32)

  What this sketch does:
    - Receives LoRa START/STOP commands from the remote transmitter
      (ESP8266 + 2 buttons + LoRa) and drives the DOL starter's start/stop
      relays accordingly.
    - Reads the voltage sensor (LM358-based AC transformer sense circuit).
    - Polls ThingSpeak over the SIM900A GSM/GPRS module for dashboard
      commands, applies them, and reports state back — see the "ThingSpeak
      sync" section below for why ThingSpeak instead of Firebase.

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
    - SIM900A GSM/GPRS module on ESP32 hardware UART2.

  Why ThingSpeak instead of Firebase:
    Firebase's REST API requires TLS 1.2+/SNI, which SIM900A's SSL stack
    cannot reliably complete (confirmed as a real limitation on this
    hardware, not just theoretical) -- and even if it could, Firebase's
    REST API needs a real PUT to overwrite a fixed path, which SIM900A's
    AT+HTTPACTION doesn't support (only GET/POST/HEAD). ThingSpeak's
    classic write API (plain HTTP GET, e.g.
    "http://api.thingspeak.com/update?api_key=...&field1=...") was
    verified live to still work over plain, unencrypted HTTP -- exactly
    what SIM900A's AT+HTTPACTION GET can do.

  ThingSpeak channel layout (two channels, so device-write and
  dashboard-write traffic don't compete for the same per-channel rate
  limit -- ThingSpeak's free tier allows roughly one write per 15s per
  channel):

    Channel A - device state (this firmware writes, dashboard reads):
      field1 = motorStatus (0=OFF, 1=ON)
      field2 = voltage
      field3 = gsmSignal
      field4 = lastSeen (unix seconds, device's own clock via GSM network time if available)

    Channel B - commands (dashboard writes, this firmware reads + acks):
      field1 = desiredState (0=OFF, 1=ON)
      field2 = issuedAt (unix seconds)
      field3 = ack (0=pending, 1=applied by device)

  Pin assignments below are sensible ESP32 defaults where the PDF
  schematic's exact GPIO numbers weren't fully legible from the extracted
  text -- values have since been corrected to match the actual board
  during bring-up; double check before reflashing on different hardware.

  Libraries required (Arduino Library Manager):
    - LoRa (Sandeep Mistry)
*/

#include <SPI.h>
#include <LoRa.h>
#include <HardwareSerial.h>

// ---------------------------------------------------------------------------
// Pin configuration — CONFIRM/CORRECT these against your actual PCB wiring
// ---------------------------------------------------------------------------

// LoRa SX1278 (hardware SPI: default ESP32 VSPI = SCK18, MISO19, MOSI23)
#define LORA_NSS_PIN      5
#define LORA_RST_PIN      4
#define LORA_DIO0_PIN     -1     // not wired on this board -> polling only
#define LORA_FREQUENCY    433E6  // must match the transmitter's frequency

// Relay outputs (through opto + transistor stage, per schematic)
#define START_RELAY_PIN   2
#define STOP_RELAY_PIN    15
#define RELAY_ACTIVE_HIGH false   // set false if a LOW pulse is what fires the opto stage

// Voltage sense ("vtg" from the LM358 AC-sense module) — ADC1 input only
// (ADC2 pins conflict with WiFi; not used here, but keep sense inputs on ADC1)
#define VOLTAGE_SENSOR_PIN 34
#define VOLTAGE_SCALE      110.0   // calibrate: real_voltage = adc_volts * VOLTAGE_SCALE

// GSM SIM900A (hardware UART2) — confirm these against your actual wiring
#define GSM_RX_PIN 21   // ESP32 pin that receives from SIM900A TX
#define GSM_TX_PIN 22   // ESP32 pin that transmits to SIM900A RX
#define GSM_BAUD   115200   // must match the module's configured UART baud, not SIM900A's power-on default (9600)

// ---------------------------------------------------------------------------
// DOL starter timing
// ---------------------------------------------------------------------------

#define RELAY_PULSE_MS        700    // how long to energize start/stop relay
#define COMMAND_COOLDOWN_MS   2000   // ignore repeat commands faster than this (debounce/anti-chatter)

// ---------------------------------------------------------------------------
// ThingSpeak configuration
// ---------------------------------------------------------------------------

static const char *APN      = "internet";   // set to your SIM's APN
static const char *APN_USER = "";
static const char *APN_PASS = "";

// Channel A — device state (this firmware writes, dashboard reads)
static const char *TS_STATE_WRITE_KEY = "EWWH8BCLUEAIKLDC";
static const char *TS_STATE_READ_KEY  = "KA9C1ZQ82DMWEII9";
#define TS_STATE_CHANNEL_ID 3492367UL

// Channel B — commands (dashboard writes, this firmware reads + acks)
static const char *TS_CMD_WRITE_KEY = "TF7MTLGD1BYMM4ST";
static const char *TS_CMD_READ_KEY  = "8BUDOKPH8PUQ14ME";
#define TS_CMD_CHANNEL_ID 3492368UL

// ThingSpeak free tier: ~1 write per 15s per channel. Poll less often than
// that so every sync cycle's writes actually land.
#define SYNC_INTERVAL_MS      30000UL
#define GSM_CMD_TIMEOUT_MS    8000UL
#define GSM_HTTP_TIMEOUT_MS   15000UL

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

HardwareSerial gsmSerial(2);   // UART2 — avoids SoftwareSerial's timing issues at 115200 baud
bool gprsReady = false;
unsigned long lastSyncMs = 0;

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
    if (gprsReady) writeDeviceState("lora");
  } else if (command == CMD_STOP) {
    lastCommandMs = now;
    stopMotor();
    if (gprsReady) writeDeviceState("lora");
  } else {
    Serial.println("[LoRa] Unrecognized command after valid prefix — ignoring");
  }
}

// ---------------------------------------------------------------------------
// GSM AT command helpers
// ---------------------------------------------------------------------------

String gsmSendCommand(const String &cmd, const char *expect = "OK", unsigned long timeoutMs = GSM_CMD_TIMEOUT_MS) {
  while (gsmSerial.available()) gsmSerial.read();   // flush stale bytes

  Serial.print("[GSM >] ");
  Serial.println(cmd);

  gsmSerial.print(cmd);
  gsmSerial.print("\r\n");

  String response;
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (gsmSerial.available()) {
      response += (char)gsmSerial.read();
    }
    if (response.indexOf(expect) != -1 || response.indexOf("ERROR") != -1) {
      break;
    }
  }

  String trimmed = response;
  trimmed.trim();
  if (trimmed.length() == 0) {
    Serial.println("[GSM <] (no response / timeout)");
  } else {
    Serial.print("[GSM <] ");
    Serial.println(trimmed);
  }

  return response;
}

// Waits for the module's unsolicited boot banner ("RDY") after power-up.
// SIM900A can take a couple seconds after RDY before it reliably answers
// plain AT commands -- sending AT immediately after boot is a common cause
// of getting silence back even though the module is otherwise fine.
bool waitForModemReady(unsigned long timeoutMs) {
  Serial.println("[GSM] Waiting for module boot banner (RDY)...");
  String buf;
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (gsmSerial.available()) {
      char c = (char)gsmSerial.read();
      buf += c;
      Serial.write(c);   // echo raw boot banner as it streams in
    }
    if (buf.indexOf("RDY") != -1) {
      Serial.println("\n[GSM] Boot banner seen (RDY)");
      return true;
    }
  }
  Serial.println("\n[GSM] No RDY banner seen within timeout — module may already be up, or not powered/wired correctly");
  return false;
}

// Retries plain "AT" a few times with short gaps -- covers both the
// just-after-RDY settling time and general link flakiness.
bool pingModem(int attempts = 5) {
  for (int i = 0; i < attempts; i++) {
    Serial.print("[GSM] AT ping attempt ");
    Serial.print(i + 1);
    Serial.print("/");
    Serial.println(attempts);
    String resp = gsmSendCommand("AT", "OK", 2000);
    if (resp.indexOf("OK") != -1) {
      Serial.println("[GSM] Modem responding to AT");
      return true;
    }
    delay(500);
  }
  Serial.println("[GSM] Modem NOT responding to AT after retries -- check GSM_BAUD, wiring, and power supply (SIM900A needs a solid 2A+ 3.7-4.2V supply; brownouts during TX are a common cause of exactly this symptom)");
  return false;
}

bool checkSimPresent() {
  String resp = gsmSendCommand("AT+CPIN?", "OK", 5000);
  if (resp.indexOf("+CPIN: READY") != -1) {
    Serial.println("[GSM] SIM detected and ready (+CPIN: READY)");
    return true;
  }
  if (resp.indexOf("NOT INSERTED") != -1) {
    Serial.println("[GSM] *** SIM NOT DETECTED *** -- check SIM seating/orientation, then power-cycle the module (not just reset)");
    return false;
  }
  if (resp.indexOf("+CPIN:") != -1) {
    Serial.print("[GSM] SIM present but not ready: ");
    Serial.println(resp);
    return false;
  }
  Serial.println("[GSM] Could not read SIM status (no response to AT+CPIN?)");
  return false;
}

bool gsmInitModem() {
  Serial.println("[GSM] Initializing modem...");

  waitForModemReady(5000);   // don't hard-fail on this -- module might already be past boot

  if (!pingModem()) {
    return false;
  }

  gsmSendCommand("ATE0");
  gsmSendCommand("AT+CMEE=2");

  if (!checkSimPresent()) {
    return false;   // no point checking network registration without a SIM
  }

  String signalResp = gsmSendCommand("AT+CSQ", "OK");
  Serial.print("[GSM] Signal quality raw: ");
  Serial.println(signalResp);

  String reg = gsmSendCommand("AT+CREG?", "OK");
  bool registered = reg.indexOf("+CREG: 0,1") != -1 || reg.indexOf("+CREG: 0,5") != -1;
  Serial.println(registered ? "[GSM] Registered on network" : "[GSM] Not registered on network yet (check antenna/signal/SIM activation)");
  return registered;
}

int gsmSignalQuality() {
  String resp = gsmSendCommand("AT+CSQ", "OK");
  int idx = resp.indexOf("+CSQ:");
  if (idx == -1) return -1;
  int comma = resp.indexOf(',', idx);
  if (comma == -1) return -1;
  String rssiStr = resp.substring(idx + 6, comma);
  rssiStr.trim();
  int rssi = rssiStr.toInt();
  return (rssi == 99) ? -1 : rssi;
}

bool gsmAttachGprs() {
  Serial.print("[GSM] Attaching GPRS with APN \"");
  Serial.print(APN);
  Serial.println("\"...");

  gsmSendCommand("AT+SAPBR=3,1,\"Contype\",\"GPRS\"");
  gsmSendCommand("AT+SAPBR=3,1,\"APN\",\"" + String(APN) + "\"");
  if (strlen(APN_USER) > 0) gsmSendCommand("AT+SAPBR=3,1,\"USER\",\"" + String(APN_USER) + "\"");
  if (strlen(APN_PASS) > 0) gsmSendCommand("AT+SAPBR=3,1,\"PWD\",\"" + String(APN_PASS) + "\"");

  Serial.println("[GSM] Opening GPRS bearer (this can take several seconds)...");
  String openResp = gsmSendCommand("AT+SAPBR=1,1", "OK", GSM_CMD_TIMEOUT_MS * 2);
  if (openResp.indexOf("ERROR") != -1) {
    Serial.println("[GSM] AT+SAPBR=1,1 returned ERROR -- APN may be wrong, or no data service on this SIM");
  }

  String status = gsmSendCommand("AT+SAPBR=2,1", "OK");
  bool ok = status.indexOf("+SAPBR: 1,1") != -1;

  if (ok) {
    Serial.println("[GSM] GPRS bearer open");
  } else {
    Serial.print("[GSM] GPRS bearer NOT open, status: ");
    Serial.println(status);
  }
  return ok;
}

// Plain HTTP GET via SIM900A's AT+HTTPACTION=0 (ThingSpeak's classic API
// works over unencrypted HTTP, so no AT+HTTPSSL is used here at all).
bool gsmHttpGet(const String &url, String &responseOut) {
  Serial.print("[HTTP] GET ");
  Serial.println(url);

  gsmSendCommand("AT+HTTPTERM");   // clear any stale session, ignore result
  gsmSendCommand("AT+HTTPINIT");
  gsmSendCommand("AT+HTTPPARA=\"CID\",1");
  gsmSendCommand("AT+HTTPPARA=\"URL\",\"" + url + "\"");

  String actionResp = gsmSendCommand("AT+HTTPACTION=0", "+HTTPACTION:", GSM_HTTP_TIMEOUT_MS);
  if (actionResp.indexOf("+HTTPACTION:") == -1) {
    Serial.println("[HTTP] No +HTTPACTION response -- request likely timed out (GPRS down, or server unreachable)");
    gsmSendCommand("AT+HTTPTERM");
    responseOut = "";
    return false;
  }

  int httpStatus = -1;
  int actionIdx = actionResp.indexOf("+HTTPACTION:");
  int firstComma = actionResp.indexOf(',', actionIdx);
  int secondComma = actionResp.indexOf(',', firstComma + 1);
  if (firstComma != -1 && secondComma != -1) {
    httpStatus = actionResp.substring(firstComma + 1, secondComma).toInt();
  }
  Serial.print("[HTTP] Status code: ");
  Serial.println(httpStatus);

  String readResp = gsmSendCommand("AT+HTTPREAD", "OK", GSM_HTTP_TIMEOUT_MS);
  responseOut = readResp;

  gsmSendCommand("AT+HTTPTERM");

  bool ok = (httpStatus == 200);
  if (!ok) {
    Serial.print("[HTTP] Request did not return 200 (got ");
    Serial.print(httpStatus);
    Serial.println(") -- check API key/channel ID/URL");
  }
  return ok;
}

// ---------------------------------------------------------------------------
// ThingSpeak sync
// ---------------------------------------------------------------------------

// Very small hand-rolled JSON field extractor — good enough for ThingSpeak's
// flat {"field1":"0","field2":"123.4",...} responses without pulling in a
// full JSON library.
float extractJsonField(const String &json, const char *key) {
  String needle = String("\"") + key + "\":\"";
  int idx = json.indexOf(needle);
  if (idx == -1) {
    needle = String("\"") + key + "\":";   // numeric (unquoted) fallback
    idx = json.indexOf(needle);
    if (idx == -1) return NAN;
  }
  int start = idx + needle.length();
  int end = start;
  while (end < (int)json.length() && json[end] != '"' && json[end] != ',' && json[end] != '}') {
    end++;
  }
  return json.substring(start, end).toFloat();
}

void writeDeviceState(const char *source) {
  float voltage = readVoltage();
  int signal = gsmSignalQuality();
  unsigned long nowSec = millis() / 1000;   // no RTC/NTP on this board yet — relative uptime, not wall clock

  String url = "http://api.thingspeak.com/update?api_key=" + String(TS_STATE_WRITE_KEY) +
               "&field1=" + String(motorState == MOTOR_ON ? 1 : 0) +
               "&field2=" + String(voltage, 1) +
               "&field3=" + String(signal) +
               "&field4=" + String(nowSec);

  String resp;
  bool ok = gsmHttpGet(url, resp);
  Serial.print("[TS] State write (" );
  Serial.print(source);
  Serial.print("): ");
  Serial.println(ok ? "sent" : "FAILED");
}

void ackCommand(int desiredState, unsigned long issuedAt) {
  Serial.println("[TS] Acking command on Channel B...");
  String url = "http://api.thingspeak.com/update?api_key=" + String(TS_CMD_WRITE_KEY) +
               "&field1=" + String(desiredState) +
               "&field2=" + String(issuedAt) +
               "&field3=1";
  String resp;
  bool ok = gsmHttpGet(url, resp);
  Serial.println(ok ? "[TS] Ack sent" : "[TS] Ack FAILED");
}

void syncWithThingSpeak() {
  Serial.println("\n[TS] ---- Sync cycle starting ----");
  Serial.println("[TS] Reading command channel...");

  String url = "http://api.thingspeak.com/channels/" + String(TS_CMD_CHANNEL_ID) +
               "/feeds/last.json?api_key=" + String(TS_CMD_READ_KEY);

  String resp;
  if (!gsmHttpGet(url, resp)) {
    Serial.println("[TS] Failed to read command channel -- skipping this sync cycle");
    return;
  }

  Serial.print("[TS] Command channel response: ");
  Serial.println(resp);

  float desiredStateF = extractJsonField(resp, "field1");
  float issuedAtF     = extractJsonField(resp, "field2");
  float ackF          = extractJsonField(resp, "field3");

  if (isnan(desiredStateF) || isnan(ackF)) {
    Serial.println("[TS] Could not parse command response -- skipping this sync cycle");
    return;
  }

  bool wantOn = desiredStateF >= 1;
  bool ack = ackF >= 1;
  unsigned long issuedAt = isnan(issuedAtF) ? 0 : (unsigned long)issuedAtF;

  Serial.print("[TS] Parsed: desiredState=");
  Serial.print(wantOn ? "ON" : "OFF");
  Serial.print(" ack=");
  Serial.print(ack ? "true" : "false");
  Serial.print(" currentMotorState=");
  Serial.println(motorState == MOTOR_ON ? "ON" : "OFF");

  bool applied = false;
  if (!ack && wantOn != (motorState == MOTOR_ON)) {
    Serial.println(String("[TS] Applying dashboard command: ") + (wantOn ? "ON" : "OFF"));
    if (wantOn) startMotor(); else stopMotor();
    applied = true;
  } else if (!ack) {
    Serial.println("[TS] Command already matches current motor state -- nothing to apply, just acking");
  }

  if (!ack) {
    ackCommand(wantOn ? 1 : 0, issuedAt);
  }

  Serial.println("[TS] Reporting state back to Channel A...");
  writeDeviceState(applied ? "dashboard" : "poll");
  Serial.println("[TS] ---- Sync cycle done ----\n");
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[BOOT] Motor Starter RX firmware starting...");

  pinMode(START_RELAY_PIN, OUTPUT);
  pinMode(STOP_RELAY_PIN, OUTPUT);
  relayWrite(START_RELAY_PIN, false);   // fail-safe: both relays de-energized at boot
  relayWrite(STOP_RELAY_PIN, false);

  pinMode(VOLTAGE_SENSOR_PIN, INPUT);

  setupLora();

  gsmSerial.begin(GSM_BAUD, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
  gprsReady = gsmInitModem() && gsmAttachGprs();
  if (!gprsReady) {
    Serial.println("[BOOT] GPRS not ready yet — will keep retrying in main loop");
  }
  lastSyncMs = millis() - SYNC_INTERVAL_MS;   // force an immediate first sync
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

  if (!gprsReady) {
    gprsReady = gsmInitModem() && gsmAttachGprs();
    delay(2000);
    return;
  }

  if (now - lastSyncMs >= SYNC_INTERVAL_MS) {
    lastSyncMs = now;
    syncWithThingSpeak();
  }
}
 