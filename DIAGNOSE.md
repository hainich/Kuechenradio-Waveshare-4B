# Audioaussetzer und RGB-Bildverschiebungen

Ausgangscode: `f3a697354246e7e4dbe51017311e6c4c7fe139ae` (#47).
Hardwarebeobachtungen: technische Übergabe vom 1. November 2026, mit #36
(`2481cffed58a`) als Wiederherstellungsreferenz. Die Beobachtungen beweisen
keine einzelne Ursache. Insbesondere fehlt ein serielles Protokoll der
Neustartschleife. Diese Änderungen sind erst nach einem Gerätetest als
Hardwarekorrektur bestätigt.

## Nachprüfbare Befunde und gezielte Änderungen

1. **Bandbreite und CPU-Reserve:** Die Projektdefaults legten weder CPU- noch
   PSRAM-Takt fest. ESP-IDF 5.5.5 wählt für ESP32-S3 dann **160 MHz CPU und
   40 MHz PSRAM**, auch bei Octal-PSRAM. Framebuffer, Code und konstante Daten
   teilen sich den PSRAM-Bus. Die Defaults verlangen nun die vom Board
   unterstützten **240 MHz CPU / 80 MHz PSRAM**. Das erhöht die Reserve, ist
   aber noch kein Beweis, dass damit alle Aussetzer verschwinden.
2. **RGB-Neustart in jedem Bild:** `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y` erzwingt
   in IDF 5.5.5 einen DMA-Neustart bei jedem VSYNC. Der Treiber warnt ausdrücklich,
   dass ein verspäteter Interrupt dabei selbst ein verschobenes Bild erzeugen
   kann. Die Option ist nun aus. Die automatische Wiederherstellung bei einem
   unvollständigen Bounce-Frame bleibt im Treiber aktiv.
3. **Ungeprüfte Neustarttaste:** Die EXIO4-Eingangskonfiguration und automatische
   Neustartabfrage entfallen. Eine Zuordnung zur physischen Taste ist nicht
   belegt. Der Resetgrund wird einmal beim Start ausgegeben. Das ist eine
   Rücknahme eines unbestätigten Hardwareeingriffs, keine bewiesene Erklärung
   der schwarzen Anzeige oder USB-Neustartschleife.
4. **HTTP-/Decoderfehler:** Unvollständige HTTP-Header werden nun als Fehler
   behandelt, statt nach einer bereits gelesenen Statuszeile mit dem Body
   fortzufahren. Decoderfortschritt berücksichtigt auch ausgegebenes PCM ohne
   gleichzeitig verbrauchte neue Eingabebytes. Temporäre HTTP-Lesepausen werden
   auch im RAM-bedingten direkten Audiopfad behandelt.

Einzelner RGB-Framebuffer, 20-Zeilen-Bounce-Puffer, ST7701-Initialisierung,
LVGL ohne feste Kernzuordnung, SRAM-Audio-FIFO und die bisherigen
Audio-Taskprioritäten bleiben erhalten. Double-Buffering und die bereits
erfolglos getestete LVGL-Kernbindung werden nicht erneut eingeführt.
Die Änderungen an `sdkconfig.defaults`, `main/main.c` und dem Audiopfad können
unabhängig voneinander zurückgenommen werden.

## Warum Display und Audio gleichzeitig gestört werden können

Der BSP verwendet RGB565 mit 480 × 480 Pixeln und einen PSRAM-Framebuffer.
Die 16-MHz-RGB-Timings ergeben mit den Austastintervallen ungefähr 60 Hz.
Allein das Nachfüllen der SRAM-Bounce-Puffer liest etwa **28 MB/s** Bilddaten
aus dem PSRAM. Ein 20-Zeilen-Puffer muss während des aktiven Bildteils ungefähr
alle **650 µs** nachgefüllt werden. Codeausführung aus PSRAM und LVGL-Zeichnen
beanspruchen denselben Bus zusätzlich.

Bounce-Puffer helfen dem LCD-DMA, verlagern die Arbeit aber auf CPU und
Interrupts. ESP-IDF dokumentiert ausdrücklich höhere CPU-Last und mögliche
Bildverschiebungen bei gleichzeitigen PSRAM-Kopien. Die BSP-I2S-Konfiguration
verwendet standardmäßig 6 DMA-Deskriptoren mit je 240 Frames: nur rund
**30–33 ms** PCM-Reserve bei 48/44,1 kHz. Ein voller komprimierter FIFO schützt
nicht vor einem verspäteten Decoder-/I2S-Aufruf. Damit ist eine gemeinsame
Ressourcenursache plausibel; ob sie am Gerät tatsächlich dominiert, bleibt
zu messen. Insbesondere war Bounce-Höhe 20 bereits BSP-Default: Ihre spätere
explizite Aufnahme allein erklärt keine Regression.

## Ein Gerätetest mit verwertbarer Diagnose

Nach erfolgreichem Build die **Update-BIN bei `0x10000`** schreiben, ohne
vollständiges Löschen; siehe [FLASHEN.md](FLASHEN.md). Bootloader,
Partitionstabelle und NVS bleiben dabei erhalten. Ein Factory-Image gehört
an `0x0` und überschreibt die gespeicherte Konfiguration.

Für einen vergleichbaren Test denselben MP3-Sender und dieselbe Lautstärke
wie bei #36 verwenden: zunächst normale Wiedergabe, anschließend einige
Touch-/Lautstärkeänderungen. Das serielle Protokoll vom Start bis über eine
hörbare Störung hinaus erfassen. Die Firmware protokolliert alle 30 Sekunden
zwei kompakte Diagnosezeilen statt Meldungen pro Audioframe:

| Messwert | Aussage und Grenze |
|---|---|
| FIFO-Minimum, Unterlaufzähler, längster HTTP-Read, Timeouts | Leerer FIFO zusammen mit langen Reads deutet auf Empfangsprobleme; ein langer Read allein beweist keinen hörbaren Ausfall. |
| Maximale Decoderzeit | Misst Aufrufdauer einschließlich Präemption, keine reine Decoder-CPU-Zeit. |
| Maximale I2S-Schreibdauer, langsame Writes | Warten bis zur Dauer des eingereichten PCM ist normal; auffällig ist zusätzliche Wartezeit. |
| Größte Lücke zwischen PCM-Schreibaufrufen | Lücken jenseits der DMA-Reserve erklären Stille trotz gefülltem komprimiertem FIFO; sie enthalten auch HTTP-/Decoder-/Scheduling-Pausen. |
| Größter interner freier Block | Zeigt RAM-Druck; ein einzelner Wert ersetzt keine Speicheranalyse. |
| Resetgrund beim Start | Unterscheidet z. B. Software-Reset, Watchdog und Brownout; ein vollständiger Panic-Backtrace liefert mehr Information. |

Maxima stammen aus demselben 30-Sekunden-Fenster, aber nicht zwingend vom
gleichen Ereignis. Im direkten Notpfad ohne FIFO sind FIFO-/HTTP-Metriken
nicht verfügbar. Versorgungsspannung, tatsächliche DMA-Unterläufe und
RGB-Signalqualität werden damit nicht elektrisch gemessen.

Bei unveränderten Störungen zuerst dieses eine Protokoll auswerten.
Weitere Taskbindungen, größere Puffer oder geänderte Paneltimings sollten
erst aus den Messwerten folgen. Eine Softwareabfrage der PWR-Taste bleibt
bis zur Prüfung des konkreten Schaltplans zurückgestellt.

## Quellen

- [ESP-IDF 5.5.5: CPU-Default](https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_system/port/soc/esp32s3/Kconfig.cpu)
- [ESP-IDF 5.5.5: PSRAM-Takt](https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_psram/esp32s3/Kconfig.spiram)
- [RGB-Treiber: `lcd_rgb_restart_trans`, erzwungener und bedingter Neustart](https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_lcd/rgb/esp_lcd_panel_rgb.c#L1142-L1166)
- [ESP-IDF: Bounce-Buffer-Einschränkungen](https://github.com/espressif/esp-idf/blob/v5.5.5/docs/en/api-reference/peripherals/lcd/rgb_lcd.rst)
- [Waveshare-BSP 2.0.0: Quellcommit der Registry-Komponente](https://github.com/waveshareteam/Waveshare-ESP32-components/blob/9daffb7168b3c093a330da700988a22092294eef/bsp/esp32_s3_touch_lcd_4b/esp32_s3_touch_lcd_4b.c)
- [I2S-Standardwerte in ESP-IDF 5.5.5](https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_driver_i2s/include/driver/i2s_common.h)
- [Audio-Codec 2.5.0: offizielles Decoderbeispiel](https://github.com/espressif/esp-adf-libs/blob/a791bb959b2c53ca3db87131ef8f3b6c500511f2/esp_audio_codec/test_apps/audio_codec_test/main/simple_decoder_test.c)

Die BSP-Befunde wurden nach der Netzwerkfreigabe an der tatsächlich geladenen
Registry-Komponente 2.0.0 bestätigt. `dependencies.lock` hält die aufgelösten
Versionen und Komponentenprüfsummen fest, darunter Audio Codec 2.5.0,
Codec Dev 1.5.11, ST7701 2.0.3, LVGL Port 2.9.0 und LVGL 9.3.0.

## Lokaler Prüfstand dieser Änderung

Alle drei Host-Testsuiten bestehen: URL-Parser, Ringpuffer und vier neue Fälle
am tatsächlichen Audiocode mit simulierten SDK-Schnittstellen. Geprüft werden
zwischengespeicherte PCM-Ausgabe, temporäres HTTP-EAGAIN, ein Decoder ohne
Fortschritt und unvollständige HTTP-Header nach Status 200. Der bisherige Code
scheitert an der Regression für zwischengespeicherte PCM-Ausgabe.
Die Tests simulieren den direkten Audiopfad; nebenläufiger FIFO-/DMA-Betrieb
und reale Hardware sind damit nicht geprüft.

Die ESP-IDF-5.5.5-Toolchain ist lokal installiert; Espressifs unverändertes
`hello_world` wurde für ESP32-S3 einschließlich Bootloader und App erfolgreich
kompiliert und gelinkt. Das wiederholte Einrichtungsskript und alle drei
Host-Testsuiten liefen erfolgreich. Dieser Beispielbuild ist keine
Küchenradio-Firmware.

Nach Veröffentlichung der Netzwerkfreigabe wurde der **vollständige
Küchenradio-Build mit ESP-IDF 5.5.5 erfolgreich abgeschlossen**. Die wirksame
Konfiguration bestätigt CPU 240 MHz, PSRAM 80 MHz, einen RGB-Framebuffer,
beide PSRAM-XIP-Optionen und den deaktivierten erzwungenen VSYNC-Neustart.
Alle Konfigurations- und Audioarchitekturprüfungen aus dem CI-Workflow bestehen.
Die App belegt 2.024.912 Bytes; die 5-MiB-App-Partition hat 61 % Reserve.
`esptool image_info` bestätigt gültige Image-Prüfsumme und Validierungshash.
Update-Datei und verifiziertes kombiniertes Factory-Image wurden aus diesem
Build erzeugt. Die Update-Datei ist bytegleich mit der App-BIN des Builds.

Ein bereits vorhandener Konfigurationseintrag bleibt wirkungslos:
`CONFIG_LWIP_TCP_RCV_BUF_DEFAULT` ist in ESP-IDF 5.5.5 kein gültiges Symbol.
Die übrigen TCP-Einstellungen wurden nicht als Ersatz für Laufzeitmessungen
behandelt. Es gab keine weiteren Compilerwarnungen im vollständigen Build.

**Noch offen:** Start, Audio, Display, Touch und Versorgungsspannung auf dem
realen Board. Der erfolgreiche Build bestätigt keine aussetzerfreie Wiedergabe.
