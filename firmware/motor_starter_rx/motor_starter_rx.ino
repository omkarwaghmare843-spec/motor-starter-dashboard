/*
  Motor Starter — RX side firmware (ESP32)

  What this sketch does:
    - Receives LoRa START/STOP commands from the remote transmitter
      (ESP8266 + 2 buttons + LoRa) and drives the DOL starter's start/stop
      relays accordingly.
    - Reads the voltage sensor (LM358-based AC transformer sense circuit).
    - Talks to Firebase Realtime Database over the A7670C 4G/LTE module for
      dashboard commands, applies them, and reports state back — see
      "Firebase over A7670C" below.

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
    - SIMCOM A7670C 4G/LTE Cat-1 module on ESP32 hardware UART2.

  Firebase over A7670C:
    Firebase's REST API requires TLS 1.2+/SNI and a real HTTP PUT to
    overwrite a fixed path. The project's original module (SIM900A, 2G) had
    neither a working TLS stack nor a PUT verb in its HTTP AT command set,
    which is why the project moved to ThingSpeak (HTTP-only) for a while.

    A7670C is a genuine 4G/LTE module with a real TLS stack (AT+CSSLCFG),
    which solves the HTTPS problem. It still only exposes GET/POST/HEAD at
    the AT+HTTPACTION layer (no native PUT) -- but Firebase's REST API
    officially documents honoring an X-HTTP-Method-Override header on a
    POST to achieve PUT (overwrite) semantics:
    https://firebase.google.com/docs/database/rest/save-data
    ("If we are making REST calls from a browser that does not support
    some of the above methods, Firebase supports the X-HTTP-Method-Override
    header.") So writes here are POST + that header, not a native PUT.

    CONFIDENCE NOTE: the exact AT+CSSLCFG parameter values below (SSL
    context index, TLS version code, and whether HTTPS is enabled via
    AT+HTTPPARA="SSLCFG",<ctxid> vs a separate flag) are based on the
    general SIMCOM A76xx/SIM7600 AT command family and have NOT been
    verified against A7670C's own AT command manual for your specific
    firmware revision. If AT+HTTPACTION fails specifically on HTTPS
    requests, check SIMCOM's "A76XX Series_HTTP(S)_Application Note" PDF
    for your module's firmware version -- this is the authoritative
    reference with a worked example, and command details are known to
    drift between SIMCOM firmware releases.

  Firebase Realtime Database schema (device/state, device/command,
  device/history) -- see the main README.md for the full schema and
  firmware contract; unchanged from the original Firebase design.

  Boot/registration behavior confirmed during bring-up:
    - Unlike SIM900A, A7670C doesn't need an explicit wait for a single
      boot banner string before it reliably answers AT -- it emits several
      startup URCs (RDY, +CPIN, *ATREADY, SIM Toolkit menu fetches, etc.)
      as a burst rather than one clean banner, so this firmware pings AT
      directly instead of watching for RDY.
    - Automatic network registration (AT+CNMP=2) can stall even when the
      operator's tower is visible in an AT+COPS=? scan (seen as
      AT+CREG? staying "0,0" indefinitely) -- forcing manual registration
      via AT+COPS=1,2,"<mcc><mnc>" resolved this immediately during
      testing. This firmware tries automatic first and falls back to a
      manual force on the configured operator if that doesn't register.
    - A genuine no-signal condition (CSQ 99,99, CREG 0,0, antenna/power
      confirmed fine) was resolved by a full power cycle rather than a
      warm AT+CFUN=1,1 restart -- if registration seems permanently stuck
      despite good antenna/power, a full power-cycle is worth trying
      before assuming a hardware fault.

  Pin assignments below were corrected to match the actual board during
  bring-up; double check before reflashing on different hardware.

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

// A7670C 4G/LTE module (hardware UART2) — confirm these against your actual wiring
#define GSM_RX_PIN 22   // ESP32 pin that receives from A7670C TX
#define GSM_TX_PIN 21   // ESP32 pin that transmits to A7670C RX
#define GSM_BAUD   115200   // must match the module's configured UART baud

// ---------------------------------------------------------------------------
// DOL starter timing
// ---------------------------------------------------------------------------

#define RELAY_PULSE_MS        700    // how long to energize start/stop relay
#define COMMAND_COOLDOWN_MS   2000   // ignore repeat commands faster than this (debounce/anti-chatter)

// ---------------------------------------------------------------------------
// Firebase configuration
// ---------------------------------------------------------------------------

static const char *APN      = "airtelgprs.com";   // Airtel India data APN
static const char *APN_USER = "";
static const char *APN_PASS = "";

// Numeric MCC/MNC for manual network registration (AT+COPS=1,2,"..."),
// used as a fallback if automatic registration (AT+CNMP=2) stalls -- seen
// during bring-up, where AT+CREG? stayed 0,0 despite the tower being
// visible in an AT+COPS=? scan, and a manual force fixed it immediately.
static const char *OPERATOR_MCC_MNC = "40490";   // Airtel India

// Firebase Realtime Database (no trailing slash). Same project used by the
// dashboard -- see src/firebase.js / .env for the matching web config.
static const char *FIREBASE_HOST = "motor-starter-4ab37-default-rtdb.firebaseio.com";

// SSL context index used for AT+CSSLCFG -- see confidence note in the file
// header. 1 is the conventional default across the SIMCOM A76xx family.
#define SSL_CTX_ID 1

#define SYNC_INTERVAL_MS      20000UL
#define GSM_CMD_TIMEOUT_MS    8000UL
#define GSM_HTTP_TIMEOUT_MS   20000UL   // HTTPS handshakes are slower than plain HTTP

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
    if (gprsReady) reportLoraTriggeredCommand("ON");
  } else if (command == CMD_STOP) {
    lastCommandMs = now;
    stopMotor();
    if (gprsReady) reportLoraTriggeredCommand("OFF");
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

// Retries plain "AT" a few times with short gaps -- covers general link
// settling/flakiness. Unlike SIM900A, A7670C doesn't need an explicit wait
// for an unsolicited boot banner before it reliably answers AT -- its own
// startup URCs (RDY, +CPIN, *ATREADY, etc.) come through as a burst rather
// than a single banner to watch for, so this just pings AT directly.
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
  Serial.println("[GSM] Modem NOT responding to AT after retries -- check GSM_BAUD, wiring, and power supply (A7670C needs a solid 2A+ supply; brownouts during TX are a common cause of exactly this symptom)");
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

  if (!registered) {
    Serial.println("[GSM] Not registered via automatic mode -- forcing manual registration on known operator...");
    gsmSendCommand("AT+COPS=1,2,\"" + String(OPERATOR_MCC_MNC) + "\"", "OK", GSM_CMD_TIMEOUT_MS * 3);

    reg = gsmSendCommand("AT+CREG?", "OK");
    registered = reg.indexOf("+CREG: 0,1") != -1 || reg.indexOf("+CREG: 0,5") != -1;
  }

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

// A7670C PDP context activation. Uses AT+CGDCONT/AT+CGACT (the standard
// 3GPP command set most 4G modules implement) rather than SIM900A/SIM800's
// older AT+SAPBR "bearer" commands, which A76xx-family modules may not
// support at all.
bool gsmAttachGprs() {
  Serial.print("[GSM] Setting up PDP context with APN \"");
  Serial.print(APN);
  Serial.println("\"...");

  gsmSendCommand("AT+CGDCONT=1,\"IP\",\"" + String(APN) + "\"");

  Serial.println("[GSM] Activating PDP context (this can take several seconds)...");
  String actResp = gsmSendCommand("AT+CGACT=1,1", "OK", GSM_CMD_TIMEOUT_MS * 2);
  if (actResp.indexOf("ERROR") != -1) {
    Serial.println("[GSM] AT+CGACT=1,1 returned ERROR -- APN may be wrong, or no data service on this SIM");
  }

  String status = gsmSendCommand("AT+CGACT?", "OK");
  bool ok = status.indexOf("+CGACT: 1,1") != -1;

  if (ok) {
    Serial.println("[GSM] PDP context active");
  } else {
    Serial.print("[GSM] PDP context NOT active, status: ");
    Serial.println(status);
  }
  return ok;
}

// Configures the SSL context used for HTTPS requests. See the confidence
// note in the file header -- these AT+CSSLCFG parameters are based on the
// general SIMCOM A76xx family and may need adjusting for your exact
// firmware revision if HTTPS requests fail while plain network/GPRS
// checks succeed.
void gsmConfigureSsl() {
  gsmSendCommand("AT+CSSLCFG=\"sslversion\"," + String(SSL_CTX_ID) + ",3");   // 3 = TLS 1.2
  gsmSendCommand("AT+CSSLCFG=\"authmode\"," + String(SSL_CTX_ID) + ",0");     // 0 = skip server cert validation
}

// HTTP(S) request via A7670C's AT+HTTPACTION. `method` is 0 (GET) or 1
// (POST). `extraHeader`, if non-empty, is sent via AT+HTTPPARA="USERDATA"
// (used for Firebase's X-HTTP-Method-Override workaround since this AT
// stack has no native PUT). `body`, if non-empty, is sent as the POST
// payload via AT+HTTPDATA.
bool gsmHttpRequest(int method, const String &url, const String &extraHeader, const String &body, String &responseOut) {
  Serial.print("[HTTP] ");
  Serial.print(method == 0 ? "GET " : "POST ");
  Serial.println(url);

  gsmSendCommand("AT+HTTPTERM");   // clear any stale session, ignore result
  gsmSendCommand("AT+HTTPINIT");
  gsmSendCommand("AT+HTTPPARA=\"CID\",1");
  gsmSendCommand("AT+HTTPPARA=\"URL\",\"" + url + "\"");

  bool isHttps = url.startsWith("https://");
  if (isHttps) {
    gsmConfigureSsl();
    gsmSendCommand("AT+HTTPPARA=\"SSLCFG\"," + String(SSL_CTX_ID));
  }

  if (extraHeader.length() > 0) {
    gsmSendCommand("AT+HTTPPARA=\"USERDATA\",\"" + extraHeader + "\\r\\n\"");
  }

  if (method == 1 && body.length() > 0) {
    // AT+HTTPDATA=<len>,<timeout> replies "DOWNLOAD" to signal it's ready
    // for the raw body bytes (not "OK" like most commands); the module
    // sends its own "OK" once it has buffered exactly <len> bytes.
    gsmSendCommand("AT+HTTPDATA=" + String(body.length()) + ",10000", "DOWNLOAD", GSM_CMD_TIMEOUT_MS);
    gsmSerial.print(body);
    unsigned long dataStart = millis();
    String dataAck;
    while (millis() - dataStart < GSM_CMD_TIMEOUT_MS) {
      while (gsmSerial.available()) dataAck += (char)gsmSerial.read();
      if (dataAck.indexOf("OK") != -1) break;
    }
    Serial.print("[GSM <] ");
    Serial.println(dataAck.length() ? dataAck : "(no ack after HTTPDATA body)");
  }

  String actionResp = gsmSendCommand("AT+HTTPACTION=" + String(method), "+HTTPACTION:", GSM_HTTP_TIMEOUT_MS);
  if (actionResp.indexOf("+HTTPACTION:") == -1) {
    Serial.println("[HTTP] No +HTTPACTION response -- request likely timed out (network down, TLS handshake failure, or server unreachable)");
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
  if (isHttps && httpStatus != 200 && httpStatus < 100) {
    Serial.println("[HTTP] Non-HTTP status code on an HTTPS request often means the TLS handshake itself failed -- see AT+CSSLCFG confidence note in file header");
  }

  // A7670C's HTTP-A stack rejects the bare "AT+HTTPREAD" (no arguments)
  // with ERROR -- unlike SIM800/SIM900, it strictly requires the
  // parameterized form AT+HTTPREAD=<start_address>,<byte_length>, where
  // start_address is a byte offset into the buffered response body (0 for
  // the beginning) and byte_length is how many bytes to read from there
  // (safe to request more than the actual remaining body -- the module
  // just returns what's left; it does not error on an oversized length).
  int contentLen = -1;
  int thirdComma = actionResp.indexOf(',', secondComma + 1);
  if (secondComma != -1) {
    contentLen = actionResp.substring(secondComma + 1, thirdComma == -1 ? actionResp.length() : thirdComma).toInt();
  }
  if (contentLen <= 0) contentLen = 1024;   // fallback if length wasn't parsed
  String readResp = gsmSendCommand("AT+HTTPREAD=0," + String(contentLen), "OK", GSM_HTTP_TIMEOUT_MS);
  responseOut = readResp;

  gsmSendCommand("AT+HTTPTERM");

  bool ok = (httpStatus == 200);
  if (!ok) {
    Serial.print("[HTTP] Request did not return 200 (got ");
    Serial.print(httpStatus);
    Serial.println(") -- check Firebase URL/auth/rules");
  }
  return ok;
}

bool gsmHttpGet(const String &url, String &responseOut) {
  return gsmHttpRequest(0, url, "", "", responseOut);
}

// POST with X-HTTP-Method-Override: PUT -- Firebase treats this exactly
// like a real PUT (full overwrite at the given path), per Firebase's own
// documented support for this header. This is how this firmware writes to
// a fixed path despite the AT stack having no native PUT method.
bool gsmHttpPutViaOverride(const String &url, const String &jsonBody, String &responseOut) {
  return gsmHttpRequest(1, url, "X-HTTP-Method-Override: PUT", jsonBody, responseOut);
}

// ---------------------------------------------------------------------------
// Firebase sync
// ---------------------------------------------------------------------------

// Very small hand-rolled JSON field extractor for Firebase's flat
// {"desiredState":"ON","ack":false,...} responses, without pulling in a
// full JSON library. Handles quoted strings, bare booleans, and numbers.
String extractJsonField(const String &json, const char *key) {
  String needle = String("\"") + key + "\":";
  int idx = json.indexOf(needle);
  if (idx == -1) return "";

  int start = idx + needle.length();
  while (start < (int)json.length() && json[start] == ' ') start++;

  if (start < (int)json.length() && json[start] == '"') {
    start++;   // skip opening quote
    int end = json.indexOf('"', start);
    if (end == -1) return "";
    return json.substring(start, end);
  }

  int end = start;
  while (end < (int)json.length() && json[end] != ',' && json[end] != '}') {
    end++;
  }
  String val = json.substring(start, end);
  val.trim();
  return val;
}

void writeDeviceState(const char *source) {
  float voltage = readVoltage();
  int signal = gsmSignalQuality();
  unsigned long nowMs = millis();   // no RTC/NTP on this board yet -- device uptime, not wall clock

  String body = "{";
  body += "\"motorStatus\":\"" + String(motorState == MOTOR_ON ? "ON" : "OFF") + "\",";
  body += "\"voltage\":" + String(voltage, 1) + ",";
  body += "\"gsmSignal\":" + String(signal) + ",";
  body += "\"lastSeen\":" + String(nowMs);
  body += "}";

  String url = "https://" + String(FIREBASE_HOST) + "/device/state.json";
  String resp;
  bool ok = gsmHttpPutViaOverride(url, body, resp);
  Serial.print("[FB] State write (");
  Serial.print(source);
  Serial.print("): ");
  Serial.println(ok ? "sent" : "FAILED");

  String historyBody = "{";
  historyBody += "\"motorStatus\":\"" + String(motorState == MOTOR_ON ? "ON" : "OFF") + "\",";
  historyBody += "\"voltage\":" + String(voltage, 1) + ",";
  historyBody += "\"source\":\"" + String(source) + "\",";
  historyBody += "\"timestamp\":" + String(nowMs);
  historyBody += "}";
  String historyUrl = "https://" + String(FIREBASE_HOST) + "/device/history.json";
  String historyResp;
  gsmHttpRequest(1, historyUrl, "", historyBody, historyResp);   // plain POST = Firebase push (new child)
}

void ackCommand() {
  Serial.println("[FB] Acking command...");
  String url = "https://" + String(FIREBASE_HOST) + "/device/command/ack.json";
  String resp;
  bool ok = gsmHttpPutViaOverride(url, "true", resp);
  Serial.println(ok ? "[FB] Ack sent" : "[FB] Ack FAILED");
}

// Called after a LoRa remote triggers a start/stop directly (bypassing the
// dashboard command queue). Writes device/command as already-applied
// (ack=true) so the dashboard doesn't try to re-send a stale command that
// no longer matches reality, then reports the resulting state.
void reportLoraTriggeredCommand(const char *desiredState) {
  String body = "{";
  body += "\"desiredState\":\"" + String(desiredState) + "\",";
  body += "\"source\":\"lora\",";
  body += "\"issuedAt\":" + String(millis()) + ",";
  body += "\"ack\":true";
  body += "}";

  String url = "https://" + String(FIREBASE_HOST) + "/device/command.json";
  String resp;
  bool ok = gsmHttpPutViaOverride(url, body, resp);
  Serial.println(ok ? "[FB] LoRa command reported" : "[FB] LoRa command report FAILED");

  writeDeviceState("lora");
}

void syncWithFirebase() {
  Serial.println("\n[FB] ---- Sync cycle starting ----");
  Serial.println("[FB] Reading device/command...");

  String url = "https://" + String(FIREBASE_HOST) + "/device/command.json";
  String resp;
  if (!gsmHttpGet(url, resp)) {
    Serial.println("[FB] Failed to read command -- skipping this sync cycle");
    return;
  }

  Serial.print("[FB] Command response: ");
  Serial.println(resp);

  String desiredState = extractJsonField(resp, "desiredState");
  String ackStr = extractJsonField(resp, "ack");

  if (desiredState.length() == 0) {
    Serial.println("[FB] Could not parse command response -- skipping this sync cycle");
    return;
  }

  bool wantOn = desiredState == "ON";
  bool ack = ackStr == "true";

  Serial.print("[FB] Parsed: desiredState=");
  Serial.print(desiredState);
  Serial.print(" ack=");
  Serial.print(ack ? "true" : "false");
  Serial.print(" currentMotorState=");
  Serial.println(motorState == MOTOR_ON ? "ON" : "OFF");

  bool applied = false;
  if (!ack && wantOn != (motorState == MOTOR_ON)) {
    Serial.println(String("[FB] Applying dashboard command: ") + (wantOn ? "ON" : "OFF"));
    if (wantOn) startMotor(); else stopMotor();
    applied = true;
  } else if (!ack) {
    Serial.println("[FB] Command already matches current motor state -- nothing to apply, just acking");
  }

  if (!ack) {
    ackCommand();
  }

  Serial.println("[FB] Reporting state back to Firebase...");
  writeDeviceState(applied ? "dashboard" : "auto");
  Serial.println("[FB] ---- Sync cycle done ----\n");
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
    syncWithFirebase();
  }
}
 
