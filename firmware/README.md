# Geprüfte Update-Firmware für Waveshare ESP32-S3-Touch-LCD-4B

Datei: **Kuechenradio_UPDATE_0x10000.bin**

Diese Firmware wurde mit ESP-IDF 5.5.5 aus den hier enthaltenen korrigierten
Anwendungsquellen gebaut. Die vollständige Kompilierung, Host-Tests,
Konfigurationsprüfungen sowie Image-Prüfsumme und Validierungshash bestehen.
Audio- und Displaystabilität sind auf dem realen Gerät noch zu bestätigen.
Details und Diagnosewerte: [DIAGNOSE.md](../DIAGNOSE.md).

## Update mit erhaltenen WLAN-Einstellungen

1. Die BIN-Datei herunterladen und das Radio per USB-Datenkabel verbinden.
2. In Chrome oder Edge den [Espressif Web-Flasher](https://espressif.github.io/esptool-js/) öffnen.
3. **Connect** und den seriellen Anschluss des Radios auswählen.
4. **Kuechenradio_UPDATE_0x10000.bin** bei Adresse **`0x10000`** auswählen.
5. Flash Mode **DIO**, Frequency **80m**, Flash Size **16MB** verwenden.
6. **Erase all ausgeschaltet lassen**, programmieren und das Radio neu starten.

Die Datei ist ein App-Update für ein bereits bootfähiges Radio. Sie gehört
an `0x10000`, nicht an `0x0`. Das Update beschreibt die App-Partition;
bei diesem Vorgehen bleiben WLAN-Zugangsdaten, Sender und Lautstärke erhalten.

Größe: **2.024.912 Bytes**.

SHA-256:
`070d2fc8139f178613ebcedae1a63b392ec3474264e70a8746323e7d77473f1c`

[BUILD_INFO.json](BUILD_INFO.json) dokumentiert Build und Quellprüfsummen.
Das Image wurde vor dem Bereitstellungscommit erzeugt; sein Git-Versionsfeld
bezeichnet deshalb den Ausgangscommit mit lokalen Änderungen. Die
Quellprüfsummen dokumentieren den tatsächlich kompilierten Stand.
