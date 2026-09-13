/*
  LoRa SX1278 wiring/SPI diagnostic — run this when LoRa.begin() fails.

  This does NOT use the LoRa library's begin() (which fails all-or-nothing
  and tells you nothing about *why*). Instead it talks to the SX1278 chip
  directly over SPI to read back its version register (0x42), which should
  always return 0x12 on genuine SX1278 silicon regardless of frequency,
  RESET timing, or library configuration -- if this fails, the problem is
  purely wiring/power/SPI, not firmware logic.

  How to use:
    1. Set the pins below to match your current wiring (same pins as your
       main sketch: NSS/CS, RESET, SCK, MISO, MOSI).
    2. Flash, open Serial Monitor at 115200 baud.
    3. Read the result:
       - "Version register: 0x12" -> SPI communication with the chip is
         working. If LoRa.begin() still fails in your main sketch, the
         problem is elsewhere (frequency mismatch, library pin config,
         DIO0 float causing packet issues -- NOT basic wiring).
       - "Version register: 0x00" or "0xFF" -> classic dead-bus symptoms:
         0x00 usually means MISO is stuck low (not connected, or module
         unpowered) -- 0xFF usually means MISO is stuck high (floating,
         often means NSS/CS isn't reaching the module, or nothing is
         driving the bus back to the ESP32 at all).
       - Anything else consistently -> could be a wiring swap (e.g.
         MISO/MOSI reversed) or the wrong module entirely.
*/

#include <SPI.h>

// --- Set these to match your current wiring ---
#define LORA_NSS_PIN   5
#define LORA_RST_PIN   4
#define LORA_SCK_PIN   18   // ESP32 default VSPI SCK
#define LORA_MISO_PIN  19   // ESP32 default VSPI MISO
#define LORA_MOSI_PIN  23   // ESP32 default VSPI MOSI

#define REG_VERSION 0x42

uint8_t readRegister(uint8_t addr) {
  digitalWrite(LORA_NSS_PIN, LOW);
  SPI.transfer(addr & 0x7F);       // MSB=0 for read
  uint8_t value = SPI.transfer(0x00);
  digitalWrite(LORA_NSS_PIN, HIGH);
  return value;
}

void resetModule() {
  pinMode(LORA_RST_PIN, OUTPUT);
  digitalWrite(LORA_RST_PIN, LOW);
  delay(10);
  digitalWrite(LORA_RST_PIN, HIGH);
  delay(10);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n[DIAG] SX1278 SPI diagnostic starting...");
  Serial.println("[DIAG] Pins in use:");
  Serial.print("  NSS/CS = "); Serial.println(LORA_NSS_PIN);
  Serial.print("  RESET  = "); Serial.println(LORA_RST_PIN);
  Serial.print("  SCK    = "); Serial.println(LORA_SCK_PIN);
  Serial.print("  MISO   = "); Serial.println(LORA_MISO_PIN);
  Serial.print("  MOSI   = "); Serial.println(LORA_MOSI_PIN);

  pinMode(LORA_NSS_PIN, OUTPUT);
  digitalWrite(LORA_NSS_PIN, HIGH);

  SPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_NSS_PIN);
  SPI.setFrequency(1000000);
  SPI.setDataMode(SPI_MODE0);

  resetModule();
  delay(100);

  Serial.println("\n[DIAG] Reading version register (0x42) 5 times:");
  for (int i = 0; i < 5; i++) {
    uint8_t version = readRegister(REG_VERSION);
    Serial.print("  Attempt ");
    Serial.print(i + 1);
    Serial.print(": 0x");
    if (version < 0x10) Serial.print("0");
    Serial.println(version, HEX);
    delay(200);
  }

  Serial.println();
  Serial.println("========================================");
  Serial.println("VERDICT");
  Serial.println("========================================");
  uint8_t finalRead = readRegister(REG_VERSION);
  if (finalRead == 0x12) {
    Serial.println("PASS: Chip responded with 0x12 (correct SX127x version ID).");
    Serial.println("SPI wiring to the module is correct. If the LoRa library's");
    Serial.println("begin() still fails, check LORA_FREQUENCY matches your module");
    Serial.println("(433E6 vs 868E6/915E6) and that DIO0 is connected if your");
    Serial.println("library/sketch expects an interrupt pin.");
  } else if (finalRead == 0x00) {
    Serial.println("FAIL: Read 0x00 consistently.");
    Serial.println("-> MISO is stuck LOW. Most likely causes:");
    Serial.println("   - Module not powered (check 3.3V at the module's VCC pin");
    Serial.println("     directly, not just at the source -- a bad jumper wire");
    Serial.println("     or breadboard connection is a very common culprit)");
    Serial.println("   - MISO wire not actually connected / wrong pin");
    Serial.println("   - GND not common between ESP32 and module");
  } else if (finalRead == 0xFF) {
    Serial.println("FAIL: Read 0xFF consistently.");
    Serial.println("-> MISO is floating (stuck HIGH, nothing driving it back).");
    Serial.println("   Most likely causes:");
    Serial.println("   - NSS/CS wire not connected (module never selected, so it");
    Serial.println("     never drives MISO)");
    Serial.println("   - MISO wire not connected at all (floating pin reads as");
    Serial.println("     whatever the ESP32's internal pull state is)");
  } else {
    Serial.print("FAIL: Read 0x");
    if (finalRead < 0x10) Serial.print("0");
    Serial.print(finalRead, HEX);
    Serial.println(" -- not 0x12, not 0x00, not 0xFF.");
    Serial.println("-> Possibly MISO/MOSI swapped, wrong SPI mode, or a wiring");
    Serial.println("   short. Double check each SPI wire against the pins above,");
    Serial.println("   one at a time.");
  }
}

void loop() {
  // one-shot diagnostic
}
