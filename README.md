# SolarMonitor

Rund-um-die-Uhr-Monitoring für ein Balkonkraftwerk mit **Bosswerk BW-MI600** (baugleich Deye SUN600G3)
und 2 × 410 Wp, **ohne die Hersteller-App**.

```
Wechselrichter ──WLAN──▶ ESP32-WROOM-32 ──alle 10 min, HTTPS──▶ GitHub Action ──▶ docs/data ──▶ GitHub Pages
 (Logger, Port 8899)      liest jede Minute         repository_dispatch       (Commit)         Dashboard
```

- **ESP32** fragt den WLAN-Stick des Wechselrichters lokal ab (Solarman-V5-Protokoll, TCP 8899, nur die
  Logger-Seriennummer nötig) und puffert bis zu 6 h. Verbrauch ca. 0,3–0,5 W, nachts Tiefschlaf.
- **GitHub Action** [`ingest.yml`](.github/workflows/ingest.yml) prüft die Werte und legt sie ab:
  `docs/data/days/JJJJ-MM-TT.csv` (Minutenwerte) und `docs/data/summary.json` (Tageswerte).
- **Dashboard** [`docs/index.html`](docs/index.html): Live-Tacho und animierter Energiefluss, Tagesverläufe
  (Leistung, Spannungen, Ströme, Netz, Temperatur, Ertrag), Kennlinien (P-U, U-I, Wirkungsgrad),
  Tages- und Monatserträge, Bestwerte.

## Gemessene Werte

| Spalte | Bedeutung | Register |
|---|---|---|
| `ts` | Unix-Zeit (UTC) | – |
| `p` | Einspeiseleistung AC, W | 86/87 |
| `u`, `i`, `f` | Netzspannung V, Strom A, Frequenz Hz | 73, 76, 79 |
| `t` | Temperatur °C | 90 |
| `u1`, `i1`, `u2`, `i2` | PV1/PV2 Spannung V, Strom A | 109–112 |
| `e_day`, `e_total` | Ertrag heute / gesamt, kWh | 60, 63/64 |

## Einrichtung

### 1. Feste IP für den Wechselrichter
Im Router dem WLAN-Stick (aktuell `192.168.0.155`) eine feste IP zuweisen.

### 2. GitHub-Token
GitHub → Settings → Developer settings → **Fine-grained personal access tokens** → *Generate new token*:
- Repository access: **Only select repositories → SolarMonitor**
- Permissions: **Contents: Read and write** (sonst nichts)
- Ablauf: z. B. 1 Jahr (Erinnerung setzen! Danach neu flashen bzw. per OTA aktualisieren)

### 3. Firmware flashen
Benötigt [PlatformIO](https://platformio.org/) (VS Code-Erweiterung oder `pip install platformio`).

```powershell
cd firmware
copy include\secrets.example.h include\secrets.h   # ausfüllen: WLAN, LOGGER_SN, GH_TOKEN, OTA_PASS
pio run -e esp32dev -t upload                        # ESP32 per USB angeschlossen
pio device monitor                                   # Log ansehen
```

`secrets.h` steht in `.gitignore` und wird nie committet.

Spätere Updates per WLAN: `$env:SOLAR_OTA_PASS="..."; pio run -e ota -t upload`

### 4. GitHub Pages
Repo → Settings → Pages → *Deploy from a branch* → `main` / `/docs`.

## Betrieb

- Lokaler Status des ESP32: `http://solarmonitor.local/` (JSON: letzte Messung, Puffer, letzter Upload).
- Erster Upload ca. 3 min nach dem Einschalten, danach alle 10 Minuten.
- Nachts schaltet sich der WLAN-Stick des Wechselrichters ab. Der ESP32 erkennt das nach 15 min, lädt den
  Rest hoch und schläft 30 min, dann prüft er erneut.
- Ohne ESP32 (Test oder Ersatz):
  `python tools/pc_collector.py --sn <WIFI-Nummer> --count 5 --interval 60 --dispatch wikischwemler-ops/SolarMonitor`
  (Token in `GH_TOKEN`).
- Testzeilen ohne Hardware: Actions → *Messwerte einsammeln* → *Run workflow*.

Die Daten-Commits macht die Action direkt auf `main` (automatische Messwerte).
Code-Änderungen laufen über Pull Requests mit Branch-Namen `xxx/yyy` (siehe `docs/branch-naming-setup.md`).
