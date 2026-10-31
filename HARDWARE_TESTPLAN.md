# Hardware-Abnahmeplan – erst nach erfolgreichem Firmware-Build

Dieser Plan beschreibt Tests auf dem **Waveshare ESP32-S3-Touch-LCD-4B**. Keine der folgenden Prüfungen wurde bisher auf realer Hardware durchgeführt.

| Nr. | Test | Erwartetes Ergebnis | Status |
|---:|---|---|---|
| 1 | Neustart mit leerem NVS | WLAN `Kuechenradio-Setup` und Webkonfiguration erscheinen | offen |
| 2 | WLAN einrichten | Verbindung, Uhr synchronisiert, Anzeige bleibt bedienbar | offen |
| 3 | Erstes Abspielen | SRF 1 Ostschweiz über 8-Ohm-Lautsprecher hörbar | offen |
| 4 | Sechs Tasten | Sender wechseln ohne Absturz; jeweils Ton | offen |
| 5 | Lautstärke 0–100 | Ton regelbar, 0 stumm; Einstellung nach Neustart erhalten | offen |
| 6 | Pause / Start | Pause ohne Ton; Start mit zuletzt gewähltem Sender | offen |
| 7 | WLAN-Router 30 s aus | Verbindung wird automatisch wieder aufgenommen | offen |
| 8 | WLAN-Kennwort absichtlich falsch | Setup-WLAN wird nach rund 45 s sichtbar | offen |
| 9 | 2 h Dauerbetrieb | Keine Abstürze, keine deutlichen Audioaussetzer | offen |
| 10 | 24 h Dauerbetrieb | Keine Neustarts, keine UI-Hänger, kein Heap-Einbruch | offen |

**Abbruchkriterien:** Bootschleife, dauerhafter Audioausfall, unzuverlässiger Touch oder Überhitzung.

## Buildabnahme

1. GitHub Action `ESP32-S3 Firmware bauen` beendet sich mit Status `success`.
2. Vier Firmware-Dateien und Flashargumente liegen im Artefakt vor.
3. Das ESP32-S3-Image wird aus dem **tatsächlich erzeugten** Build übernommen, nicht aus einer Demo oder einem alten Archiv.
4. Flashen erst nach Sichtung des Buildprotokolls.