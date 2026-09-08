# Firmware

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
