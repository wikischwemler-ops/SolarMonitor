// SolarMonitor – ESP32-WROOM-32 Collector fuer Bosswerk BW-MI600 (Deye SUN600G3, Solarman-V5-Logger)
//
// Ablauf:
//   * jede Minute Holding-Register 59..112 ueber Solarman V5 (TCP 8899) lesen
//   * Werte im RAM puffern, alle 10 min per GitHub "repository_dispatch" hochladen
//   * GitHub Action haengt die Werte an docs/data/ an -> GitHub Pages Dashboard
//   * nachts (Logger aus) Tiefschlaf, alle 30 min kurz pruefen
//   * lokaler Status: http://solarmonitor.local/   Update per WLAN: ArduinoOTA
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <time.h>

#include "secrets.h"
#include "github_roots.h"

static_assert(LOGGER_SN != 0, "secrets.h: LOGGER_SN (WIFI-Nummer vom Typenschild) eintragen");
static_assert(sizeof(GH_TOKEN) > 40, "secrets.h: GH_TOKEN (fine-grained Token) eintragen");

// ---------- Einstellungen ----------
static const uint32_t POLL_MS           = 60UL * 1000;        // Abfrageintervall
static const uint32_t FLUSH_MS          = 10UL * 60 * 1000;   // Upload-Intervall (oeffentliches Repo: Actions kostenlos)
static const uint32_t FIRST_FLUSH_MS    = 3UL * 60 * 1000;    // erster Upload kurz nach Start
static const int      MAX_ROWS          = 360;                // Puffer: 6 h falls GitHub nicht erreichbar
static const int      ROWS_PER_UPLOAD   = 120;
static const int      NIGHT_AFTER_FAILS = 15;                 // 15 min kein Logger -> Nacht
static const int      FAILS_AFTER_WAKE  = 3;                  // nach Aufwachen: 3 Versuche
static const uint64_t NIGHT_SLEEP_US    = 30ULL * 60 * 1000000;
static const uint16_t REG_FIRST         = 59;
static const uint16_t REG_COUNT         = 54;                 // 59..112

RTC_DATA_ATTR uint32_t bootCount = 0;
RTC_DATA_ATTR bool     wokeFromNight = false;

struct Sample {
  uint32_t ts;                       // Unix-Zeit UTC
  float p, u, i, f, t;               // AC-Leistung W, Spannung V, Strom A, Frequenz Hz, Temperatur C
  float u1, i1, u2, i2;              // PV1/PV2 Spannung V, Strom A
  float eDay, eTotal;                // Ertrag heute / gesamt kWh
};

static Sample   buf[MAX_ROWS];
static int      nRows = 0;
static Sample   last{};
static bool     haveLast = false;
static int      fails = 0;
static uint32_t lastPoll = 0, lastFlush = 0;
static int      lastUploadCode = 0;
static uint32_t lastUploadTs = 0;
static uint8_t  seqNo = 0;
static uint32_t wifiLostSince = 0;
static WebServer server(80);

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
  if (!cl.connect(LOGGER_IP, 8899, 3000)) return false;
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
  return true;
}

// ---------- Upload ----------
static int formatRow(const Sample& s, char* o, size_t cap) {
  return snprintf(o, cap, "%lu,%.1f,%.1f,%.1f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f",
                  (unsigned long)s.ts, s.p, s.u, s.i, s.f, s.t, s.u1, s.i1, s.u2, s.i2, s.eDay, s.eTotal);
}

static bool uploadChunk(int count) {
  String body;
  body.reserve(count * 64 + 128);
  body += "{\"event_type\":\"solar-data\",\"client_payload\":{\"v\":1,\"rows\":\"";
  char line[96];
  for (int k = 0; k < count; k++) {
    formatRow(buf[k], line, sizeof line);
    if (k) body += ';';
    body += line;
  }
  body += "\"}}";

  WiFiClientSecure tls;
  tls.setCACert(GITHUB_ROOT_CAS);
  HTTPClient http;
  http.setTimeout(20000);
  if (!http.begin(tls, "https://api.github.com/repos/" GH_REPO "/dispatches")) return false;
  http.addHeader("Authorization", "Bearer " GH_TOKEN);
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("X-GitHub-Api-Version", "2022-11-28");
  http.addHeader("User-Agent", "SolarMonitor-ESP32");
  http.addHeader("Content-Type", "application/json");
  lastUploadCode = http.POST(body);
  http.end();
  if (lastUploadCode == 204) lastUploadTs = (uint32_t)time(nullptr);
  Serial.printf("Upload %d Zeilen -> HTTP %d\n", count, lastUploadCode);
  return lastUploadCode == 204;
}

static bool flushBuffer() {
  lastFlush = millis();
  while (nRows > 0) {
    int c = min(nRows, ROWS_PER_UPLOAD);
    if (!uploadChunk(c)) return false;
    memmove(buf, buf + c, (nRows - c) * sizeof(Sample));
    nRows -= c;
  }
  return true;
}

static void store(const Sample& s) {
  if (nRows == MAX_ROWS) {                 // Puffer voll: aelteste Zeile verwerfen
    memmove(buf, buf + 1, (MAX_ROWS - 1) * sizeof(Sample));
    nRows--;
  }
  buf[nRows++] = s;
}

// ---------- Lokaler Status ----------
static void handleStatus() {
  char row[96] = "";
  if (haveLast) formatRow(last, row, sizeof row);
  char o[512];
  snprintf(o, sizeof o,
           "{\"last\":\"%s\",\"columns\":\"ts,p,u,i,f,t,u1,i1,u2,i2,e_day,e_total\","
           "\"buffered\":%d,\"fails\":%d,\"upload_http\":%d,\"upload_ts\":%lu,"
           "\"uptime_s\":%lu,\"boots\":%lu,\"rssi\":%d,\"heap\":%u}",
           row, nRows, fails, lastUploadCode, (unsigned long)lastUploadTs,
           (unsigned long)(millis() / 1000), (unsigned long)bootCount, WiFi.RSSI(), ESP.getFreeHeap());
  server.send(200, "application/json", o);
}

// ---------- Nacht ----------
static void goToSleep() {
  Serial.println("Logger nicht erreichbar -> Tiefschlaf 30 min");
  if (nRows) flushBuffer();
  wokeFromNight = true;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_timer_wakeup(NIGHT_SLEEP_US);
  esp_deep_sleep_start();
}

static bool connectWifi(uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("solarmonitor");
  WiFi.setSleep(true);                     // Modem-Sleep: spart Strom zwischen den Abfragen
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(250);
  return WiFi.status() == WL_CONNECTED;
}

void setup() {
  setCpuFrequencyMhz(80);
  Serial.begin(115200);
  bootCount++;
  Serial.printf("\nSolarMonitor Start #%lu (Nacht-Wake: %d)\n", (unsigned long)bootCount, wokeFromNight);

  if (!connectWifi(30000)) {
    Serial.println("WLAN nicht verbunden -> 5 min schlafen");
    esp_sleep_enable_timer_wakeup(5ULL * 60 * 1000000);
    esp_deep_sleep_start();
  }
  Serial.printf("WLAN ok: %s\n", WiFi.localIP().toString().c_str());

  configTzTime("UTC0", "pool.ntp.org", "time.google.com");
  for (int k = 0; k < 40 && time(nullptr) < 1700000000; k++) delay(250);

  ArduinoOTA.setHostname("solarmonitor");
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.begin();
  server.on("/", handleStatus);
  server.begin();

  fails = wokeFromNight ? NIGHT_AFTER_FAILS - FAILS_AFTER_WAKE : 0;
  wokeFromNight = false;
  lastPoll = millis() - POLL_MS;                     // sofort abfragen
  lastFlush = millis() - FLUSH_MS + FIRST_FLUSH_MS;  // erster Upload nach 3 min
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();

  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiLostSince) wifiLostSince = millis();
    if (millis() - wifiLostSince > 5UL * 60 * 1000) ESP.restart();
  } else {
    wifiLostSince = 0;
  }

  if (millis() - lastPoll >= POLL_MS) {
    lastPoll = millis();
    Sample s;
    if (time(nullptr) > 1700000000 && pollInverter(s)) {
      fails = 0;
      last = s; haveLast = true;
      store(s);
      Serial.printf("P=%.1f W  heute=%.1f kWh  gesamt=%.1f kWh\n", s.p, s.eDay, s.eTotal);
    } else if (++fails >= NIGHT_AFTER_FAILS) {
      goToSleep();
    }
  }

  if (nRows && millis() - lastFlush >= FLUSH_MS) flushBuffer();
  delay(10);
}
