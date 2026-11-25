// SolarMonitor – ESP32-WROOM-32 Collector fuer Bosswerk BW-MI600 (Deye SUN600G3, Solarman-V5-Logger)
//
// Ablauf (jeder Durchlauf ist ein frischer Start aus dem Tiefschlaf):
//   * jede Minute aufwachen, ins WLAN, Holding-Register 59..112 ueber Solarman V5 (TCP 8899) lesen,
//     Messwert im RTC-Speicher ablegen, wieder schlafen (wach ca. 3 s)
//   * alle 10 min die gesammelten Werte per GitHub "repository_dispatch" hochladen; dabei auch ein
//     Lebenszeichen (Startgrund, Neustarts, WLAN-Dauer), auch wenn es keine Messwerte gibt
//   * GitHub Action haengt die Werte an docs/data/ an -> GitHub Pages Dashboard
//   * nachts (Logger 15 min nicht erreichbar) nur alle 30 min kurz pruefen
//   * Verbindung: WLAN-Signal ESP32, Signal des Logger-Hotspots, Antwortzeit Logger, Upload-Dauer
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <time.h>

#include "secrets.h"
#include "github_roots.h"

static_assert(LOGGER_SN != 0, "secrets.h: LOGGER_SN (WIFI-Nummer vom Typenschild) eintragen");
static_assert(sizeof(GH_TOKEN) > 40, "secrets.h: GH_TOKEN (fine-grained Token) eintragen");

// Der WLAN-Stick sendet zusaetzlich einen eigenen Hotspot "AP_<WIFI-Nummer>". Dessen Signal am
// Standort des ESP32 zeigt, wie gut der Stick funkt. Abweichenden Namen in secrets.h als LOGGER_AP setzen.

// ---------- Einstellungen ----------
static const uint32_t CYCLE_S           = 60;                 // Messtakt
static const uint32_t UPLOAD_S          = 10 * 60;            // Upload- und Lebenszeichen-Takt
static const uint32_t NIGHT_S           = 30 * 60;            // Pruefintervall nachts
static const int      NIGHT_AFTER_FAILS = 15;                 // 15 Abfragen ohne Logger -> Nacht
static const int      RTC_ROWS          = 60;                 // Puffer im RTC-Speicher (1 h)
static const int      ROWS_PER_UPLOAD   = 60;
static const uint32_t WIFI_TIMEOUT_MS   = 15000;
static const uint16_t REG_FIRST         = 59;
static const uint16_t REG_COUNT         = 54;                 // 59..112
static const time_t   TIME_VALID        = 1700000000;
static const char*    HOSTNAME          = "ESP32_sMonitor";  // Name im Router / in der Geraeteliste

struct Sample {
  uint32_t ts;                       // Unix-Zeit UTC
  float p, u, i, f, t;               // AC-Leistung W, Spannung V, Strom A, Frequenz Hz, Temperatur C
  float u1, i1, u2, i2;              // PV1/PV2 Spannung V, Strom A
  float eDay, eTotal;                // Ertrag heute / gesamt kWh
  int8_t   rssi;                     // WLAN-Signal ESP32 -> Router, dBm
  int8_t   ap;                       // Signal Logger-Hotspot am ESP32, dBm (0 = nicht gefunden)
  uint16_t lat;                      // Antwortzeit des Loggers, ms
};

// ---------- Zustand, bleibt im Tiefschlaf erhalten ----------
RTC_DATA_ATTR Sample   rtcBuf[RTC_ROWS];
RTC_DATA_ATTR int      rtcN = 0;
RTC_DATA_ATTR uint32_t bootCount = 0;        // Durchlaeufe seit dem Einschalten
RTC_DATA_ATTR uint32_t powerOns = 0;         // Starts, die nicht aus dem Tiefschlaf kamen
RTC_DATA_ATTR int      lastHardReset = 0;    // Grund des letzten solchen Starts (esp_reset_reason_t)
RTC_DATA_ATTR int      fails = 0;            // Abfragen ohne Logger in Folge
RTC_DATA_ATTR uint16_t lostPolls = 0;        // verlorene Abfragen seit dem letzten Upload
RTC_DATA_ATTR uint32_t lastUploadTs = 0;     // Unix-Zeit des letzten erfolgreichen Uploads
RTC_DATA_ATTR uint32_t lastUploadMs = 0;     // Dauer des letzten erfolgreichen Uploads
RTC_DATA_ATTR uint32_t lastNtpTs = 0;
RTC_DATA_ATTR int8_t   apRssi = 0;
RTC_DATA_ATTR uint32_t lastScanTs = 0;
RTC_DATA_ATTR uint8_t  seqNo = 0;
RTC_DATA_ATTR bool     night = false;
RTC_DATA_ATTR uint16_t wifiFailsInRow = 0;

static uint16_t lastLatMs = 0;
static uint32_t wifiMs = 0;
static char     apName[24];

// ---------- Modbus / Solarman V5 ----------
static uint16_t crc16(const uint8_t* d, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t k = 0; k < n; k++) {
    c ^= d[k];
    for (int b = 0; b < 8; b++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
  }
  return c;
}

static uint8_t v5Checksum(const uint8_t* fr, size_t len) {   // Summe ohne Start, Checksumme, Ende
  uint8_t s = 0;
  for (size_t k = 1; k < len - 2; k++) s += fr[k];
  return s;
}

// Sucht im Empfangspuffer einen gueltigen 0x1510-Antwortrahmen und extrahiert die Register.
static bool parseResponse(const uint8_t* rx, size_t n, uint8_t seq, uint16_t qty, uint16_t* out) {
  for (size_t i = 0; i + 13 <= n; i++) {
    if (rx[i] != 0xA5) continue;
    size_t flen = (size_t)(rx[i + 1] | (rx[i + 2] << 8)) + 13;
    if (i + flen > n) return false;                       // noch nicht vollstaendig
    const uint8_t* fr = rx + i;
    if (fr[flen - 1] != 0x15 || fr[3] != 0x10 || fr[4] != 0x15) continue;  // z. B. Keep-Alive 0x4710
    if (fr[flen - 2] != v5Checksum(fr, flen) || fr[5] != seq) continue;
    const uint8_t* mb = fr + 25;
    size_t mlen = flen - 27;
    size_t need = 3 + qty * 2 + 2;
    if (mlen < need || mb[0] != 1 || mb[1] != 3 || mb[2] != qty * 2) continue;
    uint16_t c = crc16(mb, 3 + qty * 2);
    if (mb[3 + qty * 2] != (c & 0xFF) || mb[4 + qty * 2] != (c >> 8)) continue;
    for (uint16_t k = 0; k < qty; k++) out[k] = (mb[3 + 2 * k] << 8) | mb[4 + 2 * k];
    return true;
  }
  return false;
}

static bool readRegisters(uint16_t addr, uint16_t qty, uint16_t* out) {
  uint8_t mb[8] = {1, 3, (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(qty >> 8), (uint8_t)qty, 0, 0};
  uint16_t c = crc16(mb, 6);
  mb[6] = c & 0xFF; mb[7] = c >> 8;

  uint8_t fr[36] = {0};
  const uint32_t sn = LOGGER_SN;
  fr[0] = 0xA5; fr[1] = 15 + 8; fr[2] = 0;          // Laenge Payload
  fr[3] = 0x10; fr[4] = 0x45;                       // Control-Code 0x4510 (Request)
  fr[5] = ++seqNo; fr[6] = 0;
  fr[7] = sn; fr[8] = sn >> 8; fr[9] = sn >> 16; fr[10] = sn >> 24;
  fr[11] = 0x02;                                    // Frame-Typ; 12..25 = 0
  memcpy(fr + 26, mb, 8);
  fr[34] = v5Checksum(fr, sizeof fr);
  fr[35] = 0x15;

  WiFiClient cl;
  const uint32_t tStart = millis();
  if (!cl.connect(LOGGER_IP, 8899, 3000)) { Serial.println("Logger: keine TCP-Verbindung"); return false; }
  cl.write(fr, sizeof fr);
  uint8_t rx[512];
  size_t n = 0;
  uint32_t t0 = millis();
  bool ok = false;
  while (millis() - t0 < 6000 && n < sizeof rx) {
    int a = cl.available();
    if (a > 0) {
      n += cl.read(rx + n, min((size_t)a, sizeof rx - n));
      if ((ok = parseResponse(rx, n, fr[5], qty, out))) break;
    } else {
      delay(20);
    }
  }
  cl.stop();
  if (!ok) {
    Serial.printf("Logger: keine gueltige Antwort (%u Bytes):", (unsigned)n);
    for (size_t k = 0; k < n && k < 48; k++) Serial.printf(" %02X", rx[k]);
    Serial.println();
  }
  if (ok) lastLatMs = (uint16_t)min<uint32_t>(millis() - tStart, 60000);
  return ok;
}

static bool pollInverter(Sample& s) {
  uint16_t r[REG_COUNT];
  if (!readRegisters(REG_FIRST, REG_COUNT, r)) return false;
  auto R = [&](uint16_t reg) -> uint32_t { return r[reg - REG_FIRST]; };
  s.ts     = (uint32_t)time(nullptr);
  s.eDay   = R(60) / 10.0f;
  s.eTotal = ((R(64) << 16) | R(63)) / 10.0f;
  s.u      = R(73) / 10.0f;
  s.i      = R(76) / 10.0f;
  s.f      = R(79) / 100.0f;
  s.p      = ((R(87) << 16) | R(86)) / 10.0f;
  s.t      = ((int32_t)R(90) - 1000) / 100.0f;
  s.u1     = R(109) / 10.0f; s.i1 = R(110) / 10.0f;
  s.u2     = R(111) / 10.0f; s.i2 = R(112) / 10.0f;
  s.rssi   = (int8_t)WiFi.RSSI();
  s.ap     = apRssi;
  s.lat    = lastLatMs;
  return true;
}

// ---------- Puffer ----------
static void store(const Sample& s) {
  if (rtcN == RTC_ROWS) {                  // Puffer voll: aelteste Zeile verwerfen
    memmove(rtcBuf, rtcBuf + 1, (RTC_ROWS - 1) * sizeof(Sample));
    rtcN--;
  }
  rtcBuf[rtcN++] = s;
}

// ---------- Upload ----------
static int formatRow(const Sample& s, char* o, size_t cap) {
  return snprintf(o, cap, "%lu,%.1f,%.1f,%.1f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%d,%u",
                  (unsigned long)s.ts, s.p, s.u, s.i, s.f, s.t, s.u1, s.i1, s.u2, s.i2, s.eDay, s.eTotal,
                  s.rssi, s.ap, s.lat);
}

// Schickt bis zu `count` Zeilen (auch 0 = nur Lebenszeichen). Gibt den HTTP-Code zurueck.
static int uploadChunk(int count) {
  String body;
  body.reserve(count * 80 + 256);
  // net = Dauer des vorigen Uploads (ms), verlorene Abfragen seit dem vorigen Upload
  // st  = Durchlaeufe, Starts ohne Tiefschlaf, Grund des letzten solchen Starts, WLAN-Dauer (ms),
  //       Abfragen ohne Logger in Folge, WLAN-Signal (dBm), Nachtmodus
  char net[32], st[80];
  snprintf(net, sizeof net, "%lu,%u", (unsigned long)lastUploadMs, lostPolls);
  snprintf(st, sizeof st, "%lu,%lu,%d,%lu,%d,%d,%d", (unsigned long)bootCount, (unsigned long)powerOns,
           lastHardReset, (unsigned long)wifiMs, fails, (int)WiFi.RSSI(), night ? 1 : 0);
  body += "{\"event_type\":\"solar-data\",\"client_payload\":{\"v\":3,\"net\":\"";
  body += net;
  body += "\",\"st\":\"";
  body += st;
  body += "\",\"ip\":\"";                   // eigene IP und IP des Wechselrichter-Sticks (Heimnetz)
  body += WiFi.localIP().toString();
  body += ",";
  body += LOGGER_IP;
  body += ",";
  body += HOSTNAME;
  body += "\",\"rows\":\"";
  char line[112];
  for (int k = 0; k < count; k++) {
    formatRow(rtcBuf[k], line, sizeof line);
    if (k) body += ';';
    body += line;
  }
  body += "\"}}";

  WiFiClientSecure tls;
  tls.setCACert(GITHUB_ROOT_CAS);
  HTTPClient http;
  http.setTimeout(20000);
  if (!http.begin(tls, "https://api.github.com/repos/" GH_REPO "/dispatches")) return -1;
  http.addHeader("Authorization", "Bearer " GH_TOKEN);
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("X-GitHub-Api-Version", "2022-11-28");
  http.addHeader("User-Agent", "SolarMonitor-ESP32");
  http.addHeader("Content-Type", "application/json");
  const uint32_t t0 = millis();
  int code = http.POST(body);
  const uint32_t dt = millis() - t0;
  http.end();
  Serial.printf("Upload %d Zeilen -> HTTP %d (%lu ms)\n", count, code, (unsigned long)dt);
  if (code == 204) {
    lastUploadTs = (uint32_t)time(nullptr);
    lastUploadMs = dt;
    lostPolls = 0;
  }
  return code;
}

// Laedt alles hoch (mindestens ein Lebenszeichen). true, wenn alles angekommen ist.
static bool flush() {
  do {
    int c = min(rtcN, ROWS_PER_UPLOAD);
    if (uploadChunk(c) != 204) return false;
    memmove(rtcBuf, rtcBuf + c, (rtcN - c) * sizeof(Sample));
    rtcN -= c;
  } while (rtcN > 0);
  return true;
}

// ---------- Hotspot-Signal des Loggers ----------
static void scanLoggerAp() {
  lastScanTs = (uint32_t)time(nullptr);
  // nur nach dem Hotspot suchen (kurz, die bestehende WLAN-Verbindung bleibt erhalten)
  int n = WiFi.scanNetworks(false, true, false, 120, 0, apName);
  int8_t best = 0;
  for (int k = 0; k < n; k++)
    if (WiFi.SSID(k) == apName && (best == 0 || WiFi.RSSI(k) > best)) best = (int8_t)WiFi.RSSI(k);
  WiFi.scanDelete();
  apRssi = best;
  Serial.printf("Hotspot %s: %d dBm\n", apName, apRssi);
}

// ---------- Ablauf ----------
static bool connectWifi() {
  const uint32_t t0 = millis();
  WiFi.setHostname(HOSTNAME);              // vor mode()/begin(), sonst schickt der ESP32 "esp32-xxxxxx"
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) delay(50);
  wifiMs = millis() - t0;
  return WiFi.status() == WL_CONNECTED;
}

static void syncTime() {
  configTzTime("UTC0", "pool.ntp.org", "time.google.com");
  // nach einem Stromausfall gibt es noch keine Uhrzeit: dann bis zu 10 s warten, sonst 2 s
  const time_t before = time(nullptr);
  const int tries = before < TIME_VALID ? 40 : 8;
  for (int k = 0; k < tries; k++) {
    delay(250);
    if (time(nullptr) >= TIME_VALID && (before < TIME_VALID || sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED)) break;
  }
  if (time(nullptr) >= TIME_VALID) lastNtpTs = (uint32_t)time(nullptr);
}

[[noreturn]] static void sleepFor(uint32_t seconds) {
  if (seconds < 5) seconds = 5;
  Serial.printf("schlafe %lu s (wach %lu ms)\n", (unsigned long)seconds, (unsigned long)millis());
  Serial.flush();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
}

// Restzeit bis zum naechsten Takt, damit die Messungen im Minutenraster bleiben
static uint32_t untilNext(uint32_t period) {
  const uint32_t awake = millis() / 1000;
  return awake >= period ? 5 : period - awake;
}

void setup() {
  setCpuFrequencyMhz(80);
  Serial.begin(115200);
  bootCount++;
  const esp_reset_reason_t reason = esp_reset_reason();
  if (reason != ESP_RST_DEEPSLEEP) {        // Einschalten, Absturz, Spannungseinbruch ...
    powerOns++;
    lastHardReset = (int)reason;
  }
#ifdef LOGGER_AP
  snprintf(apName, sizeof apName, "%s", LOGGER_AP);
#else
  snprintf(apName, sizeof apName, "AP_%lu", (unsigned long)LOGGER_SN);
#endif
  Serial.printf("\nSolarMonitor #%lu, Grund %d, Puffer %d, Nacht %d\n", (unsigned long)bootCount, (int)reason, rtcN, night);

  if (!connectWifi()) {
    wifiFailsInRow++;
    Serial.printf("WLAN nicht verbunden (%u. Mal in Folge)\n", wifiFailsInRow);
    if (!night) lostPolls++;
    sleepFor(wifiFailsInRow > 5 ? 5 * 60 : CYCLE_S);  // laenger ohne WLAN: seltener versuchen
  }
  wifiFailsInRow = 0;
  Serial.printf("WLAN ok: %s (%d dBm, %lu ms)\n", WiFi.localIP().toString().c_str(), WiFi.RSSI(), (unsigned long)wifiMs);

  uint32_t now = (uint32_t)time(nullptr);
  const bool uploadDue = now < TIME_VALID || now - lastUploadTs >= UPLOAD_S;
  if (now < TIME_VALID || now - lastNtpTs >= 6 * 3600 || uploadDue) syncTime();
  now = (uint32_t)time(nullptr);

  // Hotspot-Signal alle 10 min messen (tagsueber)
  if (!night && (now - lastScanTs >= UPLOAD_S)) scanLoggerAp();

  Sample s;
  if (now >= TIME_VALID && pollInverter(s)) {
    if (night) Serial.println("Logger wieder da -> Tagbetrieb");
    night = false;
    fails = 0;
    store(s);
    Serial.printf("P=%.1f W  heute=%.1f kWh  gesamt=%.1f kWh  Logger %u ms\n", s.p, s.eDay, s.eTotal, s.lat);
  } else {
    if (now < TIME_VALID) Serial.println("Uhrzeit noch nicht synchronisiert (NTP)");
    if (!night) {
      if (lostPolls < 60000) lostPolls++;
      if (++fails >= NIGHT_AFTER_FAILS) {
        Serial.println("Logger 15 min nicht erreichbar -> Nachtbetrieb");
        night = true;
        flush();                            // Rest hochladen und Lebenszeichen "Nacht"
        sleepFor(NIGHT_S);
      }
    } else {
      sleepFor(NIGHT_S);                    // nachts: kein Upload, weiter schlafen
    }
  }

  if (now >= TIME_VALID && (uploadDue || rtcN >= RTC_ROWS - 1)) flush();
  sleepFor(untilNext(CYCLE_S));
}

void loop() {}   // wird nie erreicht: jeder Durchlauf endet im Tiefschlaf
