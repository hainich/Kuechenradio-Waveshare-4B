# Firmware installieren – Waveshare ESP32-S3-Touch-LCD-4B

**Wichtig: Für normale Updates nur die Anwendungsdatei bei Adresse `0x10000` flashen. Nur so bleiben die gespeicherten WLAN-Einstellungen (NVS) erhalten.**

## Firmware herunterladen

1. Auf [GitHub Actions](https://github.com/hainich/Kuechenradio-Waveshare-4B/actions) den neuesten erfolgreichen Build öffnen.
2. Unter **Artifacts** `Küchenradio-Update` herunterladen und entpacken.
3. Für Updates die Datei `Küchenradio.bin` auswählen.

## Update ohne Verlust der Einstellungen (Windows / Chrome / Edge)

1. Waveshare mit einem USB-Datenkabel an den USB-to-UART-Anschluss anschließen.
2. [Espressif Web-Flasher](https://espressif.github.io/esptool-js/) öffnen und **Connect**.
3. **Program:** `Küchenradio.bin`, Adresse **`0x10000`**.
4. Flash Mode **DIO**, Frequency **80m**, Flash Size **16MB**.
5. **Erase all deaktiviert lassen**. Program drücken und danach neu starten.

Der Update-Vorgang schreibt nur die App-Partition. Die NVS-Partition liegt bei `0x9000–0xEFFF` und wird dabei nicht beschrieben.

## Neuinstallation / Wiederherstellung

Nur wenn Bootloader und Partitionstabelle neu installiert werden müssen: das separate Artefakt `Küchenradio-Factory-Reset` herunterladen und `Kuechenradio_FACTORY_RESET_0x0.bin` an Adresse `0x0` flashen.

**ACHTUNG: Dieses vollständige Flash-Image überschreibt auch die NVS-Partition und löscht damit gespeicherte WLAN-Zugangsdaten, Sender und Einstellungen – selbst wenn „Erase all“ deaktiviert ist.** Das liegt daran, dass das kombinierte Image den NVS-Bereich mit `0xFF` auffüllt und der Flashvorgang die betreffenden Sektoren löscht.

## WLAN nach Neuinstallation einrichten

- WLAN `Kuechenradio-Setup` auswählen, Passwort `Kuechenradio123`.
- Im Browser `http://192.168.4.1/` öffnen, Heim-WLAN-Daten eingeben und speichern.

Die Firmware ist für das Modell **ESP32-S3-Touch-LCD-4B** vorgesehen. Bei gespeicherten WLAN-Zugangsdaten startet sie im reinen Client-Modus; nach 25 Sekunden ohne Verbindung wird das Einrichtungs-WLAN als Rückfallebene aktiviert.
