# Küchenradio – Verifizierter Stand 31.10.2026

- Privates GitHub-Repository: \`hainich/Kuechenradio-Waveshare-4B\`.
- Vollständiger ESP-IDF 5.5.5 ESP32-S3-Build **erfolgreich**: [GitHub Actions Run 41225622805](https://github.com/hainich/Kuechenradio-Waveshare-4B/actions/runs/41225622805).
- Parser-Unit-Tests und separater Display-/Touch-/Audio-Testcode erfolgreich kompiliert.
- Fünf Dateien im Build-Artefakt (Bootloader, Partitionstabelle, Anwendung, ELF, Flashargumente).
- Unbekannte Kconfig-Option \`MBEDTLS_DEFAULT_CERTIFICATE_BUNDLE\` bereinigt; aktueller Workflow erzeugt zusätzlich die Gesamtdatei und verifiziert deren Bytes.
- **Kein Hardwaretest**: tatsächliche Funktion von Bildschirm, Touch, ES8311-Audio, Streaming, WLAN-Neuverbindung und Dauerbetrieb ist noch zu prüfen.

Siehe [FLASHEN.md](FLASHEN.md) und [HARDWARE_TESTPLAN.md](HARDWARE_TESTPLAN.md).
