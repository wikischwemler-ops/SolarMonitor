#pragma once
// Kopieren nach secrets.h und ausfuellen. secrets.h wird NIE committet (.gitignore).

#define WIFI_SSID     "MeinWLAN"
#define WIFI_PASS     "geheim"

// WLAN-Stick des Wechselrichters (feste IP im Router vergeben!)
#define LOGGER_IP     "192.168.0.155"
#define LOGGER_SN     0UL            // "WIFI-Nummer" vom Typenschild, z. B. 41xxxxxxxxUL

// Fine-grained Personal Access Token, NUR fuer das Repo SolarMonitor,
// Berechtigung "Contents: Read and write". Laufzeit z. B. 1 Jahr.
#define GH_TOKEN      "github_pat_..."
#define GH_REPO       "wikischwemler-ops/SolarMonitor"

// Passwort fuer Firmware-Updates per WLAN (ArduinoOTA)
#define OTA_PASS      "anderes-geheimnis"
