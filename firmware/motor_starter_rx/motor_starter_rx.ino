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
    which solves the HTTPS problem. Textbook SIMCOM documentation says
    AT+HTTPACTION only supports GET(0)/POST(1)/HEAD(2), which would have
    meant no native PUT -- Firebase's X-HTTP-Method-Override header (which
    Firebase's own docs confirm it honors) was tried as a workaround, in
    both a literal-text and a real-CRLF-bytes form, and BOTH failed on
    real hardware (accepted-but-ineffective, and outright AT-parser
    ERROR, respectively) -- and a live test also showed Firebase's RTDB
    REST API does not honor a query-string override either. None of that
    matters now: `AT+HTTPACTION=?` on this actual module/firmware reports
    a 0-5 range, not 0-2, and live testing confirmed method 4 is a real,
    working PUT (response echoes the sent JSON) and method 3 is DELETE
    (response is the literal text "null") -- both verified directly
    against Firebase, not assumed from documentation. Writes now use
    AT+HTTPACTION=4 directly; no header tricks needed at all.

    CONFIDENCE NOTE: the exact AT+CSSLCFG parameter values below (SSL
    context index, TLS version code, and whether HTTPS is enabled via
    AT+HTTPPARA="SSLCFG",<ctxid> vs a separate flag) are confirmed working
    on this specific module via live testing (TLS handshake succeeds,
    correct HTTP status codes returned) -- but the exact wording/param
    names could still differ on a different firmware revision. Methods 2
    and 5 in AT+HTTPACTION's reported range were not identified/tested;
    avoid relying on them without verifying the same way methods 3/4 were.

  Firebase Realtime Database schema (device/state, device/command,
  device/history) -- see the main README.md for the full schema and
  firmware contract; unchanged from the original Firebase design.

  LoRa reception during a GSM/Firebase transaction:
    A full Firebase sync cycle blocks for several seconds (multiple AT
    commands, each with multi-second timeouts) inside syncWithFirebase().
    Since LoRa.parsePacket() was previously only polled once per loop()
    iteration, any packet arriving during that window sat unread in the
    SX1278 FIFO and was overwritten/lost by the next packet -- meaning LoRa
    commands only reliably worked in the gaps between sync cycles, and were
    dropped if sent while a sync (visible in the serial log) was in
    progress. Fixed by polling LoRa from inside every GSM busy-wait loop
    (pollLoraDuringWait(), called from gsmSendCommand()'s wait and the
    AT+HTTPDATA body-ack wait). Applying the relay action is always safe to
    do immediately even mid-transaction; the Firebase report of that
    LoRa-triggered change is deferred (gsmBusy/loraReportPending) until the
    current GSM transaction finishes, since starting a second AT command
    exchange on the same UART while one is already in flight would corrupt
    both.

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

  Configurable SIM/carrier settings (config mode):
    APN and the manual-registration MCC/MNC are only correct for the SIM
    this firmware shipped with configured for (Airtel India, by default,
    below). A different carrier's SIM (e.g. Jio) generally needs a
    different APN, and if automatic registration ever stalls, a different
    MCC/MNC for the manual AT+COPS fallback -- so swapping SIMs isn't
    plug-and-play by default.

    To let a customer reconfigure this without reflashing: hold the config
    button (CONFIG_BUTTON_PIN, wired to GND, INPUT_PULLUP) down while
    powering on the board. setup() checks this pin once at boot, before any
    GSM/LoRa init, and if held LOW, enters config mode instead of normal
    operation: it starts a WiFi access point and a small web server with a
    form for APN / operator MCC-MNC / Firebase host, saves whatever is
    submitted into NVS (via Preferences), and reboots into normal mode.
    Normal boot reads these three values from NVS if present, falling back
    to the hardcoded Airtel defaults below if nothing has been configured
    yet -- so out of the box, with the button never pressed, behavior is
    unchanged from before this feature existed. Config mode fully suspends
    LoRa/relay/GSM handling; it's a dedicated setup state, not a background
    mode.

  Libraries required (Arduino Library Manager):
    - LoRa (Sandeep Mistry)
*/

#include <SPI.h>
#include <LoRa.h>
#include <HardwareSerial.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>

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
#define GSM_RX_PIN 16   // ESP32 pin that receives from A7670C TX
#define GSM_TX_PIN 17   // ESP32 pin that transmits to A7670C RX
#define GSM_BAUD   115200   // must match the module's configured UART baud

// Config-mode entry button — wired to GND, INPUT_PULLUP (pressed = LOW).
// Hold down while powering on to enter SIM/carrier config mode. Free GPIO
// (not used by LoRa/relays/voltage-sense/GSM UART).
#define CONFIG_BUTTON_PIN 21

// ---------------------------------------------------------------------------
// DOL starter timing
// ---------------------------------------------------------------------------

#define RELAY_PULSE_MS        700    // how long to energize start/stop relay
#define COMMAND_COOLDOWN_MS   2000   // ignore repeat commands faster than this (debounce/anti-chatter)

// ---------------------------------------------------------------------------
// Firebase configuration
// ---------------------------------------------------------------------------

// Defaults, used until/unless config mode saves different values to NVS.
// Do NOT change these for Airtel -- they're correct for any Airtel India
// SIM (carrier-level values, not tied to one specific SIM card).
#define DEFAULT_APN            "airtelgprs.com"   // Airtel India data APN
#define DEFAULT_OPERATOR_MCC_MNC "40490"          // Airtel India
#define DEFAULT_FIREBASE_HOST  "motor-starter-4ab37-default-rtdb.firebaseio.com"

// Loaded at boot from NVS if config mode has saved a customer's own SIM
// settings; otherwise left at the DEFAULT_* values above. Not const, since
// loadConfig() fills these in setup().
String APN = DEFAULT_APN;
String APN_USER = "";
String APN_PASS = "";

// Numeric MCC/MNC for manual network registration (AT+COPS=1,2,"..."),
// used as a fallback if automatic registration (AT+CNMP=2) stalls -- seen
// during bring-up, where AT+CREG? stayed 0,0 despite the tower being
// visible in an AT+COPS=? scan, and a manual force fixed it immediately.
String OPERATOR_MCC_MNC = DEFAULT_OPERATOR_MCC_MNC;

// Firebase Realtime Database (no trailing slash). Same project used by the
// dashboard -- see src/firebase.js / .env for the matching web config.
String FIREBASE_HOST = DEFAULT_FIREBASE_HOST;

Preferences configPrefs;

// Reads APN/MCC-MNC/Firebase host from NVS (namespace "motorcfg"), falling
// back to the DEFAULT_* values above for anything never configured -- so a
// device that has never been through config mode behaves exactly as before
// this feature existed.
void loadConfig() {
  configPrefs.begin("motorcfg", true);   // read-only
  APN = configPrefs.getString("apn", DEFAULT_APN);
  APN_USER = configPrefs.getString("apnUser", "");
  APN_PASS = configPrefs.getString("apnPass", "");
  OPERATOR_MCC_MNC = configPrefs.getString("mccMnc", DEFAULT_OPERATOR_MCC_MNC);
  FIREBASE_HOST = configPrefs.getString("fbHost", DEFAULT_FIREBASE_HOST);
  configPrefs.end();

  Serial.println("[CFG] Loaded configuration:");
  Serial.print("[CFG]   APN=");
  Serial.println(APN);
  Serial.print("[CFG]   OPERATOR_MCC_MNC=");
  Serial.println(OPERATOR_MCC_MNC);
  Serial.print("[CFG]   FIREBASE_HOST=");
  Serial.println(FIREBASE_HOST);
}

void saveConfig(const String &apn, const String &apnUser, const String &apnPass,
                const String &mccMnc, const String &fbHost) {
  configPrefs.begin("motorcfg", false);   // read-write
  if (apn.length() > 0) configPrefs.putString("apn", apn);
  configPrefs.putString("apnUser", apnUser);
  configPrefs.putString("apnPass", apnPass);
  if (mccMnc.length() > 0) configPrefs.putString("mccMnc", mccMnc);
  if (fbHost.length() > 0) configPrefs.putString("fbHost", fbHost);
  configPrefs.end();
}

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

// Set when a LoRa packet is handled while GSM is busy (mid AT-command
// transaction) -- reporting to Firebase from inside that transaction would
// corrupt it (nested AT commands on the same UART), so the relay action is
// applied immediately but the Firebase report is deferred until loop() sees
// GSM is idle again.
bool gsmBusy = false;
bool loraReportPending = false;
const char *pendingLoraState = "OFF";

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
    if (gprsReady) {
      if (gsmBusy) {
        Serial.println("[LoRa] GSM busy -- deferring Firebase report until current sync finishes");
        loraReportPending = true;
        pendingLoraState = "ON";
      } else {
        reportLoraTriggeredCommand("ON");
      }
    }
  } else if (command == CMD_STOP) {
    lastCommandMs = now;
    stopMotor();
    if (gprsReady) {
      if (gsmBusy) {
        Serial.println("[LoRa] GSM busy -- deferring Firebase report until current sync finishes");
        loraReportPending = true;
        pendingLoraState = "OFF";
      } else {
        reportLoraTriggeredCommand("OFF");
      }
    }
  } else {
    Serial.println("[LoRa] Unrecognized command after valid prefix — ignoring");
  }
}

// ---------------------------------------------------------------------------
// GSM AT command helpers
// ---------------------------------------------------------------------------

// Checks for and handles one LoRa packet, if present, without blocking.
// Safe to call from inside any GSM busy-wait loop.
void pollLoraDuringWait() {
  int packetSize = LoRa.parsePacket();
  if (packetSize > 0) {
    handleLoraPacket(packetSize);
  }
}

// Every blocking GSM wait below also calls pollLoraDuringWait() so a
// remote packet arriving mid-HTTP-transaction (which can block for many
// seconds across AT+HTTPACTION/AT+HTTPDATA/etc.) still gets caught instead
// of sitting unread in the SX1278 FIFO until it's overwritten by the next
// packet and lost. Without this, LoRa only "worked" in the gaps between
// sync cycles -- exactly the symptom of needing to spam the remote button
// while a sync was in progress.
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
    pollLoraDuringWait();
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

// Days since 1970-01-01 for a UTC proleptic-Gregorian date, via Howard
// Hinnant's well-known "days_from_civil" algorithm -- correct across leap
// years (including century years like 2000) without a lookup table.
// Avoids relying on ESP32 Arduino core's mktime()/timegm(), which depend on
// TZ environment state and aren't guaranteed reliably available/correct
// across core versions for a plain UTC epoch conversion.
static int32_t daysFromCivil(int year, int month, int day) {
  int y = year;
  if (month <= 2) y -= 1;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

// Global wall-clock reference, established once via NTP: unixMsAtSync is
// the real epoch time (ms) at the moment millis() read msAtSync. Later
// wall-clock time is unixMsAtSync + (millis() - msAtSync) -- the standard
// pattern for tracking real time on a device with no RTC.
int64_t unixMsAtSync = 0;
unsigned long msAtSync = 0;
bool timeSynced = false;

int64_t currentUnixMs() {
  if (!timeSynced) return 0;
  return unixMsAtSync + (int64_t)(millis() - msAtSync);
}

// Arduino's String() has no int64_t/long long overload (ESP32's `long` is
// only 32-bit, too small for a Unix ms timestamp) -- format manually.
String int64ToString(int64_t value) {
  if (value == 0) return "0";
  bool negative = value < 0;
  uint64_t v = negative ? (uint64_t)(-value) : (uint64_t)value;
  char buf[21];   // max int64 digits (19) + sign + null
  int i = 20;
  buf[i] = '\0';
  while (v > 0) {
    buf[--i] = '0' + (v % 10);
    v /= 10;
  }
  if (negative) buf[--i] = '-';
  return String(&buf[i]);
}

// Syncs the module's clock to a real NTP server (UTC, no timezone offset
// -- this firmware only needs a correct epoch value, not local time) over
// the already-active PDP context, then reads it back via AT+CCLK? and
// converts to a Unix ms timestamp for use as lastSeen/timestamp fields.
bool gsmSyncTime() {
  Serial.println("[GSM] Syncing time via NTP...");
  gsmSendCommand("AT+CNTP=\"pool.ntp.org\",0");
  gsmSendCommand("AT+CNTP", "+CNTP:", GSM_CMD_TIMEOUT_MS * 2);

  String clk = gsmSendCommand("AT+CCLK?", "OK", 5000);
  int idx = clk.indexOf("+CCLK: \"");
  if (idx == -1) {
    Serial.println("[GSM] Could not read AT+CCLK? -- time sync failed");
    return false;
  }
  int start = idx + 8;   // length of "+CCLK: \""
  // Expected: yy/MM/dd,hh:mm:ss+zz
  if (clk.length() < (unsigned)(start + 17)) {
    Serial.println("[GSM] AT+CCLK? response too short to parse -- time sync failed");
    return false;
  }
  int yy = clk.substring(start, start + 2).toInt();
  int month = clk.substring(start + 3, start + 5).toInt();
  int day = clk.substring(start + 6, start + 8).toInt();
  int hh = clk.substring(start + 9, start + 11).toInt();
  int mi = clk.substring(start + 12, start + 14).toInt();
  int ss = clk.substring(start + 15, start + 17).toInt();

  if (yy < 0 || yy >= 100 || month < 1 || month > 12 || day < 1 || day > 31) {
    Serial.println("[GSM] AT+CCLK? response out of range -- time sync failed");
    return false;
  }

  // Standard AT+CCLK 2-digit year convention: 00-79 -> 2000-2079,
  // 80-99 -> 1980-1999. A module that hasn't actually synced (still on
  // its power-on default clock) commonly reads back as 1970/1980-ish or
  // similar placeholder dates -- reject anything not plausibly "now" so a
  // failed sync doesn't get treated as a real timestamp.
  int year = yy < 80 ? 2000 + yy : 1900 + yy;
  if (year < 2024) {
    Serial.print("[GSM] AT+CCLK? returned an implausible/unsynced clock (year ");
    Serial.print(year);
    Serial.println(") -- time sync failed");
    return false;
  }

  int32_t days = daysFromCivil(year, month, day);
  int64_t utcSeconds = (int64_t)days * 86400LL + hh * 3600LL + mi * 60LL + ss;

  unixMsAtSync = utcSeconds * 1000LL;
  msAtSync = millis();
  timeSynced = true;

  Serial.print("[GSM] Time synced: ");
  Serial.print(year);
  Serial.print(clk.substring(start + 2, start + 17));
  Serial.print(" UTC (unix ms: ");
  Serial.print((long)(unixMsAtSync / 1000));
  Serial.println("...)");
  return true;
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

// HTTP(S) request via A7670C's AT+HTTPACTION. Confirmed by live testing
// against this exact module/firmware (AT+HTTPACTION=? reports a 0-5 range,
// wider than the textbook SIMCOM GET/POST/HEAD trio):
//   0 = GET
//   1 = POST   (Firebase: push, creates a new child with a random key)
//   3 = DELETE (Firebase: removes the path; response body is the literal
//               text "null", matching Firebase's documented DELETE response)
//   4 = PUT    (Firebase: real overwrite; response body echoes back the
//               exact JSON that was sent -- this is what device/state and
//               device/command now use instead of the abandoned
//               X-HTTP-Method-Override header approach, which turned out
//               not to work on this module in either form tried, and
//               which a live test also showed Firebase's RTDB REST API
//               does not honor via header OR query-string override)
// Methods 2 and 5 were not identified/tested -- avoid relying on them.
// `body`, if non-empty, is sent as the request payload via AT+HTTPDATA
// (used for POST and PUT; GET/DELETE pass an empty body).
bool gsmHttpRequest(int method, const String &url, const String &body, String &responseOut) {
  static const char *methodNames[] = { "GET", "POST", "HEAD", "DELETE", "PUT", "?" };
  Serial.print("[HTTP] ");
  Serial.print(methodNames[method >= 0 && method <= 5 ? method : 5]);
  Serial.print(" ");
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

  if ((method == 1 || method == 4) && body.length() > 0) {
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
      pollLoraDuringWait();
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
  return gsmHttpRequest(0, url, "", responseOut);
}

// Real PUT (method 4) -- full overwrite at the given path. Confirmed by
// live testing against this exact module/firmware: the response body
// echoes back the sent JSON, matching Firebase's documented PUT response
// (as opposed to POST's {"name": "-pushId"} or DELETE's "null").
bool gsmHttpPut(const String &url, const String &jsonBody, String &responseOut) {
  return gsmHttpRequest(4, url, jsonBody, responseOut);
}

bool gsmHttpPost(const String &url, const String &jsonBody, String &responseOut) {
  return gsmHttpRequest(1, url, jsonBody, responseOut);
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

  int64_t nowMs = currentUnixMs();
  if (nowMs == 0) {
    // Time never successfully synced (e.g. NTP failed at boot) -- fall
    // back to device uptime so lastSeen still moves forward, though the
    // dashboard's offline check (which compares against Date.now()) will
    // incorrectly read this device as offline until a sync succeeds.
    Serial.println("[FB] Warning: time not synced, using device uptime for lastSeen/timestamp");
    nowMs = (int64_t)millis();
  }
  String nowMsStr = int64ToString(nowMs);

  String body = "{";
  body += "\"motorStatus\":\"" + String(motorState == MOTOR_ON ? "ON" : "OFF") + "\",";
  body += "\"voltage\":" + String(voltage, 1) + ",";
  body += "\"gsmSignal\":" + String(signal) + ",";
  body += "\"lastSeen\":" + nowMsStr;
  body += "}";

  String url = "https://" + String(FIREBASE_HOST) + "/device/state.json";
  String resp;
  bool ok = gsmHttpPut(url, body, resp);
  Serial.print("[FB] State write (");
  Serial.print(source);
  Serial.print("): ");
  Serial.println(ok ? "sent" : "FAILED");

  String historyBody = "{";
  historyBody += "\"motorStatus\":\"" + String(motorState == MOTOR_ON ? "ON" : "OFF") + "\",";
  historyBody += "\"voltage\":" + String(voltage, 1) + ",";
  historyBody += "\"source\":\"" + String(source) + "\",";
  historyBody += "\"timestamp\":" + nowMsStr;
  historyBody += "}";
  String historyUrl = "https://" + String(FIREBASE_HOST) + "/device/history.json";
  String historyResp;
  gsmHttpPost(historyUrl, historyBody, historyResp);   // push (new child) -- history is meant to accumulate
}

void ackCommand() {
  Serial.println("[FB] Acking command...");
  String url = "https://" + String(FIREBASE_HOST) + "/device/command/ack.json";
  String resp;
  bool ok = gsmHttpPut(url, "true", resp);
  Serial.println(ok ? "[FB] Ack sent" : "[FB] Ack FAILED");
}

// Called after a LoRa remote triggers a start/stop directly (bypassing the
// dashboard command queue). Writes device/command as already-applied
// (ack=true) so the dashboard doesn't try to re-send a stale command that
// no longer matches reality, then reports the resulting state.
void reportLoraTriggeredCommand(const char *desiredState) {
  int64_t nowMs = currentUnixMs();
  if (nowMs == 0) nowMs = (int64_t)millis();   // time not synced yet -- see writeDeviceState note

  String body = "{";
  body += "\"desiredState\":\"" + String(desiredState) + "\",";
  body += "\"source\":\"lora\",";
  body += "\"issuedAt\":" + int64ToString(nowMs) + ",";
  body += "\"ack\":true";
  body += "}";

  String url = "https://" + String(FIREBASE_HOST) + "/device/command.json";
  String resp;
  bool ok = gsmHttpPut(url, body, resp);
  Serial.println(ok ? "[FB] LoRa command reported" : "[FB] LoRa command report FAILED");

  writeDeviceState("lora");
}

void syncWithFirebaseInner() {
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

// Thin wrapper ensuring gsmBusy is always cleared (even on an early return
// inside syncWithFirebaseInner), and flushing any LoRa-triggered Firebase
// report that got deferred while this sync cycle was in progress.
void syncWithFirebase() {
  gsmBusy = true;
  syncWithFirebaseInner();
  gsmBusy = false;

  if (loraReportPending) {
    loraReportPending = false;
    Serial.print("[FB] Flushing deferred LoRa report: ");
    Serial.println(pendingLoraState);
    reportLoraTriggeredCommand(pendingLoraState);
  }
}

// ---------------------------------------------------------------------------
// Config mode — WiFi AP + web form for SIM/carrier settings
// ---------------------------------------------------------------------------

static const char *CONFIG_AP_SSID = "MotorStarter-Setup";
static const char *CONFIG_AP_PASSWORD = "configure123";   // WPA2, 8+ chars required

WebServer configServer(80);

const char *CONFIG_PAGE_HTML =
  "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>Motor Starter Setup</title>"
  "<style>body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px}"
  "label{display:block;margin-top:12px;font-weight:bold}"
  "input{width:100%;padding:8px;box-sizing:border-box;font-size:16px}"
  "button{margin-top:20px;padding:10px 20px;font-size:16px}</style></head><body>"
  "<h2>Motor Starter — SIM/Carrier Setup</h2>"
  "<form method='POST' action='/save'>"
  "<label>APN</label><input name='apn' value='%APN%' required>"
  "<label>APN Username (optional)</label><input name='apnUser' value='%APNUSER%'>"
  "<label>APN Password (optional)</label><input name='apnPass' value='%APNPASS%'>"
  "<label>Operator MCC+MNC (e.g. 40490 for Airtel India, 40570 for Jio)</label>"
  "<input name='mccMnc' value='%MCCMNC%' required>"
  "<label>Firebase Realtime Database host (no https://, no trailing slash)</label>"
  "<input name='fbHost' value='%FBHOST%' required>"
  "<button type='submit'>Save &amp; Reboot</button>"
  "</form></body></html>";

String buildConfigPage() {
  String page = CONFIG_PAGE_HTML;
  page.replace("%APN%", APN);
  page.replace("%APNUSER%", APN_USER);
  page.replace("%APNPASS%", APN_PASS);
  page.replace("%MCCMNC%", OPERATOR_MCC_MNC);
  page.replace("%FBHOST%", FIREBASE_HOST);
  return page;
}

void handleConfigRoot() {
  configServer.send(200, "text/html", buildConfigPage());
}

void handleConfigSave() {
  String apn = configServer.arg("apn");
  String apnUser = configServer.arg("apnUser");
  String apnPass = configServer.arg("apnPass");
  String mccMnc = configServer.arg("mccMnc");
  String fbHost = configServer.arg("fbHost");

  saveConfig(apn, apnUser, apnPass, mccMnc, fbHost);

  configServer.send(200, "text/html",
    "<!DOCTYPE html><html><body style='font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px'>"
    "<h2>Saved</h2><p>Configuration saved. The device is rebooting into normal operation now.</p>"
    "</body></html>");

  Serial.println("[CFG] Configuration saved via web form -- rebooting into normal mode");
  delay(500);   // let the response flush before restarting
  ESP.restart();
}

// Enters config mode: starts a WiFi AP + web server for SIM/carrier setup
// and never returns (loops forever handling web requests) until the device
// is rebooted, either by the save handler or a manual power cycle. Called
// from setup() only when CONFIG_BUTTON_PIN is held LOW at boot -- normal
// LoRa/relay/GSM operation is fully suspended in this mode.
void enterConfigMode() {
  Serial.println("[CFG] Config button held at boot -- entering config mode");
  Serial.println("[CFG] Normal operation (LoRa/relay/GSM) is suspended while in this mode");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(CONFIG_AP_SSID, CONFIG_AP_PASSWORD);
  IPAddress ip = WiFi.softAPIP();

  Serial.print("[CFG] AP started: SSID=\"");
  Serial.print(CONFIG_AP_SSID);
  Serial.print("\" password=\"");
  Serial.print(CONFIG_AP_PASSWORD);
  Serial.println("\"");
  Serial.print("[CFG] Connect to that WiFi network, then open http://");
  Serial.print(ip);
  Serial.println("/ in a browser to configure");

  configServer.on("/", HTTP_GET, handleConfigRoot);
  configServer.on("/save", HTTP_POST, handleConfigSave);
  configServer.begin();

  while (true) {
    configServer.handleClient();
  }
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[BOOT] Motor Starter RX firmware starting...");
  Serial.print("[BOOT] Free heap at boot: ");
  Serial.println(ESP.getFreeHeap());

  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);

  loadConfig();

  if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
    enterConfigMode();   // never returns
  }

  pinMode(START_RELAY_PIN, OUTPUT);
  pinMode(STOP_RELAY_PIN, OUTPUT);
  relayWrite(START_RELAY_PIN, false);   // fail-safe: both relays de-energized at boot
  relayWrite(STOP_RELAY_PIN, false);

  pinMode(VOLTAGE_SENSOR_PIN, INPUT);

  setupLora();

  Serial.print("[BOOT] Free heap before gsmSerial.begin(): ");
  Serial.println(ESP.getFreeHeap());
  gsmSerial.begin(GSM_BAUD, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
  Serial.println("[BOOT] gsmSerial.begin() returned");
  gprsReady = gsmInitModem() && gsmAttachGprs();
  if (!gprsReady) {
    Serial.println("[BOOT] GPRS not ready yet — will keep retrying in main loop");
  } else if (!gsmSyncTime()) {
    Serial.println("[BOOT] Time sync failed -- lastSeen/timestamp will use device uptime until a later sync succeeds");
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
    gsmBusy = true;
    gprsReady = gsmInitModem() && gsmAttachGprs();
    if (gprsReady && !timeSynced) gsmSyncTime();
    gsmBusy = false;
    if (loraReportPending && gprsReady) {
      loraReportPending = false;
      reportLoraTriggeredCommand(pendingLoraState);
    }
    delay(2000);
    return;
  }

  if (!timeSynced) {
    gsmBusy = true;
    gsmSyncTime();   // retry until it succeeds -- lastSeen is wrong until then
    gsmBusy = false;
    if (loraReportPending) {
      loraReportPending = false;
      reportLoraTriggeredCommand(pendingLoraState);
    }
  }

  if (now - lastSyncMs >= SYNC_INTERVAL_MS) {
    lastSyncMs = now;
    syncWithFirebase();
  }
}
 
