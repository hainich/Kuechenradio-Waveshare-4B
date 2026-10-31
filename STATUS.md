# Projektstatus – 31.10.2026

## Repository
Privates Repository: hainich/Kuechenradio-Waveshare-4B

## Quellcode
ESP-IDF 5.5.5, ESP32-S3, offizieller Waveshare-4B-BSP (2.0.0),
LVGL 9.3.0 und quelloffener MP3-Decoder minimp3.
Die 20 Kern-Dateien wurden via GitHub Actions importiert.
Parser-Test im CI erfolgreich.

## Builds
GitHub Actions:
- ESP32-S3 Küchenradio Firmware (main)
- Waveshare 4B Display Touch Audio Bring-up (hardware-bringup)

Der ESP-IDF-Build und der anschließende Hardwaretest sind getrennt zu beurteilen.
Ein grüner Build ist kein Beweis für korrekte Audio-, Touch- und Streaming-Funktion
auf echter Hardware.

## Einschränkungen
MP3-Streams; kein Spotify, kein AAC/HLS. Bei Problemen mit Streamadressen
über die Webkonfiguration alternative direkte MP3-Streams eintragen.
