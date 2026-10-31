# Installationsanleitung – Waveshare ESP32-S3-Touch-LCD-4B

**Nur für das exakte Modell mit „4B“!** Die Firmware ist unter ESP-IDF 5.5.5 erfolgreich kompiliert, aber noch nicht auf einem physischen Waveshare getestet.

## Windows / Chrome oder Edge
1. Im [GitHub-Actions-Verlauf](https://github.com/hainich/Kuechenradio-Waveshare-4B/actions) den neuesten erfolgreichen (grünen) Workflow öffnen und unter **Artifacts** das Paket **Kuechenradio-Waveshare-4B-Firmware** herunterladen.
2. ZIP entpacken. Datei **Kuechenradio_4B_Flash_0x0.bin** bereitlegen.
3. Waveshare mit einem USB-**Datenkabel** verbinden.
4. Offiziellen [Espressif-Web-Flasher](https://espressif.github.io/esptool-js/) in Chrome oder Edge öffnen, COM-Port auswählen, **Connect**.
5. Unter **Program** die Binärdatei eintragen, Adresse **0x0**. Flash Mode **DIO**, Frequency **80m**, Flash Size **16MB**. **Erase all** nicht aktivieren, wenn vorhandene NVS-Einstellungen erhalten bleiben sollen.
6. **Program** drücken, Erfolgsmeldung prüfen, Gerät neu starten. Falls es nicht erkannt wird, beim Verbinden ggf. BOOT gedrückt halten.

Alternative für Nutzer mit Python und \`esptool\`:

\`\`\`powershell
py -m pip install esptool
py -m esptool --chip esp32s3 --port COM5 --baud 460800 write-flash 0x0 Kuechenradio_4B_Flash_0x0.bin
\`\`\`

**COM5 ist nur ein Beispiel.** Den tatsächlich angezeigten COM-Port verwenden.

## Ersteinrichtung
- WLAN **Kuechenradio-Setup** (Passwort **Kuechenradio123**) verbinden.
- **http://192.168.4.1/** öffnen; Heim-WLAN und MP3-Sender speichern.
- Lautsprecher: **8 Ohm / 2 Watt**, an der vorgesehenen Waveshare-Buchse.
- Bei Problemen zuerst den seriellen Start-/Fehlerlog ansehen und die Tests in [HARDWARE_TESTPLAN.md](HARDWARE_TESTPLAN.md) durchgehen.

Die Gesamtdatei enthält Bootloader, Partitionstabelle und Anwendung an den vom ESP-IDF-Build vorgegebenen Adressen. Sie wird bei jedem Build bytegenau gegen die Einzeldateien geprüft.
