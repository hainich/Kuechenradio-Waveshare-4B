# Projektstatus: Firmware-Build erfolgreich

[Einfach flashen](FLASHEN.md) · [GitHub Actions](https://github.com/hainich/Kuechenradio-Waveshare-4B/actions)

# Küchenradio – Waveshare ESP32-S3-Touch-LCD-4B

**Eigenständiger ESP-IDF-Quellcode, ohne AtlasCube-Bibliotheken**

Zielhardware: **Waveshare ESP32-S3-Touch-LCD-4B**, quadratisches **480 × 480 Pixel** LCD, **ST7701** / **GT911**, **ES8311** mit 8-Ohm-2-Watt-Lautsprecheranschluss. Achtung: Dies ist das **4B**, nicht das **Touch-LCD-4** (ohne B).

## Funktionen des vorliegenden Programms

- 6 Direktwahltasten auf dem Touchscreen, Sendername, WLAN-/Wiedergabestatus, Uhr, Lautstärke und Pause/Start
- MP3-Streaming über den vollständig offenen minimp3-Decoder (CC0); HTTP und HTTPS
- Speicherung der WLAN-Einstellungen, Sender und Lautstärke im Flash (NVS)
- Automatische WLAN-Neuverbindung und Stream-Neustart nach Unterbrechungen
- WLAN-Einrichtungsnetz und einfache lokale Weboberfläche zum Verwalten der 6 Sender
- Vorinstallierte Stream-Adressen: SRF 1 Regionaljournal Ostschweiz, SRF 3, Swiss Pop, SRF 2 Kultur, Swiss Jazz, Radio Paradise
- Hardwaretreiber für LCD, Touch und Verstärker über das veröffentlichte Waveshare-BSP

## Inbetriebnahme (nach erfolgreichem Build und Flash)

1. Einen **8 Ω / 2 W** Lautsprecher an die **MX1.25-2-Pin-Lautsprecherbuchse** des Waveshare-Boards anschließen. Der Lautsprecher ist nicht im Lieferumfang des Codes.
2. Display mit USB-C versorgen. Ersteinrichtung: Auf dem Handy/PC WLAN **`Kuechenradio-Setup`**, Passwort **`Kuechenradio123`** auswählen.
3. Browser: **http://192.168.4.1/** öffnen. WLAN-Namen und -Passwort eingeben, gewünschte Streams anpassen, **Speichern und neu starten** drücken.
4. Nach dem Neustart spielt der zuletzt gewählte Sender automatisch, sofern zuvor nicht pausiert wurde. Nach der erfolgreichen WLAN-Einrichtung ist das Setup-WLAN normalerweise ausgeschaltet. Die Konfiguration ist über die IP-Adresse im Heimnetz erreichbar. Falls das gespeicherte WLAN 45 Sekunden nach einem Neustart nicht erreichbar ist, wird das Setup-WLAN als Wiederherstellungsweg aktiviert.

**Sicherheit:** Das Setup-WLAN verwendet absichtlich ein im Quellcode dokumentiertes Standardpasswort. Für dauerhafte Nutzung bitte `RADIO_AP_PASSWORD` in `main/radio_config.h` ändern und neu bauen. Die Web-Konfiguration hat keine zusätzliche Anmeldung; nur in vertrauenswürdigen Netzwerken betreiben. Das Setup-WLAN ist beim ersten Start aktiv, danach nur als Wiederherstellungsmodus bei 45 Sekunden fehlgeschlagener WLAN-Verbindung.

## Build

Benötigt: **ESP-IDF 5.5.x**, Internetzugang zum erstmaligen Herunterladen des Waveshare-BSP und des offenen `minimp3.h`-Decoders, ESP32-S3-Xtensa-Compiler. Eine separate Installation von AtlasCube oder ESP-ADF ist **nicht** erforderlich.

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Unter Windows: ESP-IDF-5.5-Konsole starten und `build_windows.bat` ausführen; anschließend `idf.py -p COMx flash monitor`.

Das Projekt enthält außerdem einen **GitHub-Actions-Buildworkflow**, mit dem die Firmware nach dem Hochladen des Projektordners automatisch kompiliert werden kann. Der komplette Firmware-Build wurde am 31.10.2026 auf GitHub Actions erfolgreich durchgeführt. Die Hardwarefunktion ist noch nicht getestet. Siehe [FLASHEN.md](FLASHEN.md). Die verwendete LVGL-Version ist fest auf **9.3.0** gepinnt; die getestete API-Konstante `LV_LABEL_LONG_MODE_DOTS` wird verwendet.

## Projektstruktur

- `main/radio_config.*`: Sender, WLAN, NVS
- `main/radio_wifi.*`: WLAN-Verbindung und Setup-AP
- `main/radio_web.*`: lokale Weboberfläche
- `main/radio_ui.*`: LVGL-Touchoberfläche
- `main/radio_stream.*`: HTTP/HTTPS-Streaming, minimp3-Decoder, Stereo-zu-Mono, I²S/ES8311
- `main/url_codec.*`: Formulardecodierung, HTML-Escaping
- `main/main.c`: Systemstart
- `sdkconfig.defaults`, `partitions.csv`, `main/idf_component.yml`: ESP-IDF-Projekteinstellungen
- `tests/run.sh`: portable lokale Unit-Tests der Formularfunktionen

## Externe Quellcodebasis und Lizenzen

1. Waveshare BSP: https://github.com/waveshareteam/Waveshare-ESP32-components/tree/master/bsp/esp32_s3_touch_lcd_4b – Display, Touch, Audio; ESP Component Registry `waveshare/esp32_s3_touch_lcd_4b` v2.0.0.
2. minimp3 (CC0 1.0): https://github.com/lieff/minimp3 – vollständig einsehbarer MP3-Decoder in einer C-Headerdatei; wird beim ersten Build als **Quellcode** geholt und kompiliert, keine vorübersetzte Codec-Bibliothek.
3. ESP-IDF: https://github.com/espressif/esp-idf – WLAN, HTTP, NVS, TLS und FreeRTOS.
4. LVGL: https://github.com/lvgl/lvgl – Benutzeroberfläche, über BSP-Abhängigkeit eingebunden.
5. Offizielle SRG-Streams: https://www.broadcast.ch/fileadmin/kundendaten/Dokumente/Internet_Streaming/links_for_streaming_internetradio_de_fr_it_01.pdf
6. Radio Paradise: https://legacy.radioparadise.com/rp3.php?file=links&name=Listen

## Teststatus und Einschränkungen

Der hardwareseitige Abnahmeplan steht in `HARDWARE_TESTPLAN.md`.

- Die **kompletten selbst geschriebenen Anwendungsquelltexte** und die Baukonfiguration sind vorhanden.
- Der **portable Formularparser einschließlich Prozentkodierung, HTML-Escaping und Grenzfällen wurde mit gcc und `-Wall -Wextra -Werror` erfolgreich getestet** (`tests/run.sh`).
- **GitHub Actions hat den vollständigen ESP-IDF-Build erfolgreich ausgeführt und die Firmwaredateien als Artefakt bereitgestellt. Der Hardwaretest ist weiterhin offen.**
- Direkt-Stream-URLs für MP3 sind vorgesehen; AAC ist bewusst nicht eingebaut, damit keine proprietären Codec-Bibliotheken benötigt werden. **HLS (`.m3u8`), Playlists (`.m3u`/`.pls`), DRM-Streams, Podcasts und Bluetooth-Audio** sind nicht implementiert.
- Die interne Audioausgabe ist mono. Bei Stereo-Streams werden beide Kanäle gemittelt.
- Die Anzeige von Metadaten (Interpret/Titel via ICY) ist nicht integriert; es erscheint der Sendername.
- Bei bestimmten Streamservern können zusätzliche HTTP-Header oder nicht standardkonforme Antworten Anpassungen erfordern.

**Nächster technischer Meilenstein:** Flashen gemäß [FLASHEN.md](FLASHEN.md) und Hardware-Abnahme.

**Quelloffenheit:** Die komplette selbst entwickelte Radio-Anwendung samt MP3-Decoder ist offen. ESP-IDF/WLAN und die Hardware-Treiber von Waveshare bleiben Hersteller-Komponenten; diese können ihrerseits herstellerspezifische Firmware-Blobs enthalten. „Vollständig quelloffen“ bezieht sich hier auf die Radiologik und den Audio-Decoder, nicht auf jeden Chip-/SDK-Firmwarebestandteil.

