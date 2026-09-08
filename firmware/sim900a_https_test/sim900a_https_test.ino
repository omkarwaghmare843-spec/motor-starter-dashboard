/*
  SIM900A HTTPS capability test — run this BEFORE writing the real firmware.

  Purpose:
    Find out, on your actual physical SIM900A module, whether it can complete
    a real HTTPS request to Firebase's servers. SIM900A's SSL/TLS stack is
    old and widely reported to fail against modern TLS1.2+/SNI-requiring
    servers (which Firebase's are) — but firmware varies board to board, so
    this is worth 15 minutes of testing on your unit before deciding whether
    the main firmware needs a different GSM module or a relay server.

  What it does:
    1. Initializes the modem, checks network + GPRS registration.
    2. Attaches GPRS using your APN.
    3. Attempts a plain HTTP GET (to confirm GPRS/HTTP works at all — this
       should succeed regardless of the SSL question).
    4. Attempts AT+HTTPSSL=1 followed by an HTTPS GET to your Firebase
       Realtime Database URL, and prints the *real* result — not just
       whether the AT command was accepted, but the actual HTTP action
       result code and any data returned.
    5. Prints a clear PASS/FAIL verdict for the HTTPS step at the end.

  How to use:
    1. Fill in APN / APN_USER / APN_PASS for your SIM card below.
    2. Wire SIM900A TX -> ESP32 GSM_RX_PIN, SIM900A RX -> ESP32 GSM_TX_PIN,
       common ground, and make sure the SIM900A has adequate power (a 2A+
       supply — brownouts during AT+HTTPACTION are a common false-failure).
    3. Flash this sketch, open Serial Monitor at 115200 baud.
    4. Read the full log, then read the final VERDICT line.

  Interpreting the result:
    - If step 3 (plain HTTP) fails: the problem is GPRS/APN/signal, not SSL —
      fix that first, this test doesn't tell you anything about HTTPS yet.
    - If step 3 succeeds but step 4 fails: this confirms the expected
      limitation — SIM900A can reach the internet but can't complete TLS to
      Firebase. Next step is either a different GSM module (SIM800/SIM7000
      series) or a relay server.
    - If step 4 succeeds: your specific module/firmware can do it — the main
      firmware can proceed with direct HTTPS calls (though AT+HTTPACTION
      still only supports GET/POST/HEAD, so the "how do we PUT" question
      still needs solving separately).
*/

#include <SoftwareSerial.h>

#define GSM_RX_PIN   16
#define GSM_TX_PIN   17
#define GSM_BAUD     9600

static const char *APN      = "internet";   // <-- set to your SIM's APN
static const char *APN_USER = "";
static const char *APN_PASS = "";

// A path that should return a small amount of JSON (device/state.json).
static const char *FIREBASE_HOST = "motor-starter-4ab37-default-rtdb.firebaseio.com";
static const char *FIREBASE_PATH = "/device/state.json";

// A known-plain-HTTP endpoint to confirm baseline connectivity.
static const char *HTTP_TEST_HOST = "example.com";
static const char *HTTP_TEST_PATH = "/";

#define CMD_TIMEOUT_MS   8000UL
#define HTTP_TIMEOUT_MS  20000UL

SoftwareSerial gsmSerial(GSM_RX_PIN, GSM_TX_PIN);

bool httpsPassed = false;
bool httpPassed = false;

String sendAT(const String &cmd, unsigned long timeoutMs = CMD_TIMEOUT_MS) {
  while (gsmSerial.available()) gsmSerial.read();

  Serial.println("  > " + cmd);
  gsmSerial.print(cmd);
  gsmSerial.print("\r\n");

  String response;
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (gsmSerial.available()) {
      response += (char)gsmSerial.read();
    }
    if (response.indexOf("OK") != -1 || response.indexOf("ERROR") != -1) {
      // keep draining briefly in case more URC data follows (e.g. HTTPACTION)
      unsigned long drainStart = millis();
      while (millis() - drainStart < 300) {
        while (gsmSerial.available()) response += (char)gsmSerial.read();
      }
      break;
    }
  }
  response.trim();
  Serial.println("  < " + response);
  return response;
}

// Waits specifically for an unsolicited "+HTTPACTION:" result code, which
// arrives asynchronously after AT+HTTPACTION returns its initial OK.
String waitForHttpAction(unsigned long timeoutMs) {
  String response;
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (gsmSerial.available()) {
      response += (char)gsmSerial.read();
    }
    if (response.indexOf("+HTTPACTION:") != -1) {
      break;
    }
  }
  response.trim();
  Serial.println("  < " + response);
  return response;
}

void printHeader(const char *title) {
  Serial.println();
  Serial.println("========================================");
  Serial.println(title);
  Serial.println("========================================");
}

bool step1_initModem() {
  printHeader("STEP 1: Modem init + network registration");
  sendAT("AT");
  sendAT("ATE0");
  sendAT("AT+CMEE=2");

  String csq = sendAT("AT+CSQ");
  String creg = sendAT("AT+CREG?");

  bool registered = creg.indexOf("+CREG: 0,1") != -1 || creg.indexOf("+CREG: 0,5") != -1;
  Serial.println(registered ? "[OK] Registered on network" : "[FAIL] Not registered — check SIM/antenna/signal");
  return registered;
}

bool step2_attachGprs() {
  printHeader("STEP 2: GPRS attach");
  sendAT("AT+SAPBR=3,1,\"Contype\",\"GPRS\"");
  sendAT("AT+SAPBR=3,1,\"APN\",\"" + String(APN) + "\"");
  if (strlen(APN_USER) > 0) sendAT("AT+SAPBR=3,1,\"USER\",\"" + String(APN_USER) + "\"");
  if (strlen(APN_PASS) > 0) sendAT("AT+SAPBR=3,1,\"PWD\",\"" + String(APN_PASS) + "\"");
  sendAT("AT+SAPBR=1,1", CMD_TIMEOUT_MS * 2);
  String status = sendAT("AT+SAPBR=2,1");

  bool ok = status.indexOf("+SAPBR: 1,1") != -1;
  Serial.println(ok ? "[OK] GPRS bearer open" : "[FAIL] GPRS bearer not open — check APN/signal");
  return ok;
}

bool step3_plainHttpGet() {
  printHeader("STEP 3: Plain HTTP GET (baseline connectivity check)");
  sendAT("AT+HTTPTERM");
  sendAT("AT+HTTPINIT");
  sendAT("AT+HTTPPARA=\"CID\",1");
  sendAT("AT+HTTPPARA=\"URL\",\"http://" + String(HTTP_TEST_HOST) + String(HTTP_TEST_PATH) + "\"");

  Serial.println("  > AT+HTTPACTION=0");
  gsmSerial.print("AT+HTTPACTION=0\r\n");
  String actionResult = waitForHttpAction(HTTP_TIMEOUT_MS);

  bool ok = actionResult.indexOf(",200,") != -1;
  Serial.println(ok ? "[OK] Plain HTTP GET succeeded (got HTTP 200)" : "[FAIL] Plain HTTP GET failed: " + actionResult);

  sendAT("AT+HTTPTERM");
  return ok;
}

bool step4_httpsGet() {
  printHeader("STEP 4: HTTPS GET to Firebase (the real question)");
  sendAT("AT+HTTPTERM");
  sendAT("AT+HTTPINIT");
  sendAT("AT+HTTPPARA=\"CID\",1");

  // Enable SSL for the session — this is the command that may simply be
  // ignored/fail silently on SIM900A, or may be accepted but fail later.
  String sslResp = sendAT("AT+HTTPSSL=1");
  if (sslResp.indexOf("ERROR") != -1) {
    Serial.println("[INFO] AT+HTTPSSL=1 returned ERROR outright — this module/firmware doesn't even claim SSL support.");
  }

  sendAT("AT+HTTPPARA=\"URL\",\"https://" + String(FIREBASE_HOST) + String(FIREBASE_PATH) + "\"");

  Serial.println("  > AT+HTTPACTION=0");
  gsmSerial.print("AT+HTTPACTION=0\r\n");
  String actionResult = waitForHttpAction(HTTP_TIMEOUT_MS);

  bool ok = actionResult.indexOf(",200,") != -1;

  if (ok) {
    Serial.println("[OK] HTTPS GET to Firebase succeeded (HTTP 200) — reading body:");
    String readResp = sendAT("AT+HTTPREAD");
    Serial.println("  Body: " + readResp);
  } else {
    Serial.println("[FAIL] HTTPS GET to Firebase failed.");
    Serial.println("  Raw +HTTPACTION result: " + actionResult);
    Serial.println("  Common codes: 601=network/SSL error, 603=connect timeout, 0 or missing = no response.");
  }

  sendAT("AT+HTTPTERM");
  return ok;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\nSIM900A HTTPS capability test starting...");
  Serial.println("(This takes about 30-60 seconds. Full AT traffic is logged below.)");

  gsmSerial.begin(GSM_BAUD);
  delay(2000);

  bool registered = step1_initModem();
  if (!registered) {
    printHeader("VERDICT");
    Serial.println("Could not register on the network. Check SIM card, antenna, and signal");
    Serial.println("before this test can tell you anything about HTTPS support.");
    return;
  }

  bool gprsOk = step2_attachGprs();
  if (!gprsOk) {
    printHeader("VERDICT");
    Serial.println("GPRS did not attach. Check your APN setting and signal strength");
    Serial.println("before this test can tell you anything about HTTPS support.");
    return;
  }

  httpPassed = step3_plainHttpGet();
  httpsPassed = step4_httpsGet();

  printHeader("VERDICT");
  if (!httpPassed) {
    Serial.println("Plain HTTP failed too, so this run is inconclusive about SSL specifically.");
    Serial.println("Check GPRS/APN/signal and re-run before trusting the HTTPS result above.");
  } else if (httpsPassed) {
    Serial.println("PASS: This SIM900A module CAN complete HTTPS to Firebase.");
    Serial.println("-> Proceed with direct HTTPS calls in the main firmware.");
    Serial.println("   (Note: AT+HTTPACTION still only supports GET/POST/HEAD, no PUT --");
    Serial.println("   that problem is separate and still needs a plan.)");
  } else {
    Serial.println("FAIL: Plain HTTP worked, but HTTPS to Firebase did not.");
    Serial.println("-> This matches the commonly reported SIM900A limitation (old TLS stack,");
    Serial.println("   likely missing SNI support that Firebase's servers require).");
    Serial.println("-> Recommended next step: switch to a module with real TLS support");
    Serial.println("   (e.g. SIM800-series or SIM7000/SIM7600), or add a relay server.");
  }
}

void loop() {
  // Nothing to do — this is a one-shot diagnostic. Reset the board to re-run.
}
