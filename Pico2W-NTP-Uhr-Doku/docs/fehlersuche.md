# Fehlersuche

Die meisten Probleme lassen sich mit dem **seriellen Monitor** (115200 Baud, „Neue Zeile“)
eingrenzen. Beim Start gibt die Firmware die gefundenen I2C-Geräte, den erkannten Sensor, den
WLAN-Verlauf und alle Update-Schritte aus. Der Befehl `info` zeigt den aktuellen Zustand.

## Kompilieren

| Symptom | Ursache | Abhilfe |
|---|---|---|
| `variable or field 'uiEdit' declared void`, `'F' was not declared in this scope` | Ältere Version des Sketches mit einer Template-Funktion. Der Prototyp-Generator der IDE 1.8 kommt damit nicht klar. | Aktuellen Sketch verwenden; dort ersetzt `UiLock` das Template. Eigene Templates in `.ino`-Dateien vermeiden oder in eine `.h`-Datei auslagern. |
| Viele Fehler wegen doppelt definierter Funktionen | Zwei `.ino`-Dateien im Sketch-Ordner. Die IDE fügt alle `.ino` eines Ordners zu einem Programm zusammen. | Nur eine `.ino` im Ordner, Name = Ordnername |
| `Not signing the generated binary` | `openssl` wird beim Build nicht gefunden | `openssl` installieren bzw. in den Suchpfad aufnehmen; bei der Snap-Version der IDE kann die Kapselung das verhindern, dann die IDE als AppImage oder ZIP von arduino.cc verwenden |
| Keine `firmware.bin` im Sketch-Ordner | `platform.local.txt` fehlt, liegt im falschen Ordner oder es gibt keine `private.key` | Siehe [OTA: platform.local.txt](ota.md#platformlocaltxt); nach dem Kopieren die IDE neu starten |
| *Sketch → Kompilierte Binärdatei exportieren* liefert nur eine `.uf2` | So legt es der Core für die IDE 1.8 fest | `firmware.bin` kommt über `platform.local.txt` automatisch beim Kompilieren |

## Hochladen und serieller Port

| Symptom | Ursache | Abhilfe |
|---|---|---|
| „Fehler beim Öffnen des seriellen Ports '/dev/ttyS4'“ | Falscher Port: `ttyS*` sind eingebaute serielle Schnittstellen des PCs | Den Pico als `/dev/ttyACM0` wählen bzw. für den ersten Upload *UF2 Board* |
| „Permission denied: /dev/ttyACM0“ | Benutzer nicht in der Gruppe `dialout` | `sudo usermod -aG dialout $USER`, neu anmelden |
| Snap-IDE findet den Pico nicht oder kann nicht schreiben | Der Snap hat keinen Zugriff auf USB bzw. Wechseldatenträger | `sudo snap connect arduino:raw-usb` und `sudo snap connect arduino:removable-media` |
| Kein Port erscheint | Pico ohne laufende Firmware oder hängt | BOOTSEL gedrückt halten, einstecken, Port *UF2 Board* wählen |

## Display

| Symptom | Ursache | Abhilfe |
|---|---|---|
| Display bleibt schwarz, im Log kein Gerät auf 0x3C/0x3D | Verdrahtung, Versorgung, SDA/SCL vertauscht | Verdrahtung nach [hardware.md](hardware.md#verdrahtung) prüfen |
| Bild um 2 Pixel verschoben, rechts Pixelmüll | SSD1306-Treiber auf SH1106-Display oder umgekehrt | `OLED_SSD1306` umstellen |
| Display friert ein, zeigt aber weiter das letzte Bild | I2C-Bus hängt, z. B. nach Neustart mitten in einer Übertragung | Strom kurz trennen. Die aktuelle Firmware fängt das selbst ab, siehe [Firmware](firmware.md#robuster-i2c-bus) |
| Eigene Startbild-Funktion wird „ignoriert“ | Der Pico hat sich das neueste Release von GitHub geholt, das die Änderung nicht enthält | `FW_VERSION` mindestens so hoch wie das neueste Release setzen, siehe [OTA](ota.md#versionslogik) |
| Text erscheint nicht | Kein Font gesetzt, y-Koordinate zu klein (sie ist die Grundlinie, nicht die Oberkante) oder Umlaute mit einem `_tr`-Font | `setFont()` aufrufen, y mindestens so groß wie die Schrifthöhe, für Umlaute `_tf`-Font und `drawUTF8()` |

## WLAN und Uhrzeit

| Symptom | Ursache | Abhilfe |
|---|---|---|
| Display zeigt „WLAN einrichten“ | Keine Zugangsdaten im LittleFS | Serieller Monitor, Befehl `wifi` |
| Display zeigt „Kein Dateisystem!“ | Flash-Aufteilung ohne LittleFS gewählt | *Flash Size → 4MB (Sketch: 2MB, FS: 2MB)*, per USB neu hochladen, WLAN neu eingeben |
| Erster Verbindungsversuch mit Status 6 fehlgeschlagen | Kann beim ersten Versuch nach dem Einschalten vorkommen | Nichts tun, nach 15 s folgt der nächste Versuch |
| Netz wird nie gefunden | WLAN auf Kanal 12 oder 13 und *WiFi Region* auf *Worldwide* | *Werkzeuge → WiFi Region → Germany* |
| Verbunden, aber „Hole Zeit…“ bleibt stehen | NTP (UDP-Port 123) wird blockiert, z. B. in Gast- oder Firmennetzen | Anderes Netz testen oder NTP-Server des Routers in `NTP_SERVER_1` eintragen |
| Uhrzeit um eine Stunde falsch | Zeitzone geändert oder `TZ_INFO` fehlerhaft | `TZ_INFO` prüfen, siehe [Firmware](firmware.md#zeit-und-zeitzone) |

## OTA-Updates

Für eine genaue Analyse beim Kompilieren *Werkzeuge → Debug Port → Serial* einstellen. Dann
schreibt der Core zusätzlich eigene Meldungen (`[begin]`, `[Updater]`) in den seriellen Monitor.

| Symptom | Ursache | Abhilfe |
|---|---|---|
| Update-Balken steht auf 100 %, nichts passiert | Firmware mit dem alten Update-Code (`httpUpdate`), siehe [OTA](ota.md#warum-kein-httpupdate) | Strom trennen. Den aktuellen Sketch per USB aufspielen; alle Releases nur aus dem aktuellen Sketch bauen |
| Ein Update klappt, das nächste hängt wieder | Das Release wurde mit altem Code gebaut. Entscheidend ist der Code, der auf dem Pico läuft. | Wie oben |
| „Fehlercode 12“ | Signatur ungültig: Image unsigniert oder mit anderem Schlüssel signiert | `Signed binary` im Build-Log prüfen, dieselbe `private.key` verwenden |
| „kein Release gefunden“ | Kein veröffentlichtes Release, nur Entwurf oder Pre-Release, oder Repository privat | Release veröffentlichen, nicht als Pre-Release; Repository öffentlich |
| Pico findet das Release, lädt aber nicht | Tag-Nummer nicht größer als `FW_VERSION` | Tag `v` + neue Versionsnummer |
| Pico lädt dasselbe Release immer wieder | `FW_VERSION` im Release nicht erhöht | Die Firmware erkennt das selbst und überspringt das Release; neues Release mit korrekter Nummer anlegen |
| Eigene Änderungen verschwinden nach 30 s | Testbuild mit kleinerer `FW_VERSION` als das neueste Release | `FW_VERSION` erhöhen oder `UPDATE_REPO ""` |
| GitHub nimmt einen Release-Anhang nicht an („Something went really wrong“) | Leere Datei (0 Byte) | Datei prüfen. `version.txt` wird nicht mehr gebraucht; nur `firmware.bin` hochladen |
| Netzwerkport erscheint nicht in der IDE | Pico nicht im WLAN oder mDNS im Netz blockiert | Mit `info` WLAN prüfen; notfalls per USB hochladen |
| LAN-Upload bricht am Ende ab | Unsignierte `.bin` gesendet | `tools/platform.local.txt` installieren, siehe [OTA](ota.md#platformlocaltxt) |

## Sensor

| Symptom | Ursache | Abhilfe |
|---|---|---|
| „Kein BMP280/BME280 gefunden“ | Sensor nicht am I2C-Bus oder CSB auf GND (SPI-Modus) | Verdrahtung, CSB auf 3V3; im Log muss 0x76 oder 0x77 auftauchen |
| `[SENS] … unbekannte Chip-ID` | Anderer Sensor auf 0x76/0x77, z. B. BMP180 oder BME680 | Wird nicht unterstützt |
| Feuchte zeigt `-- (BMP280)` | Es ist ein BMP280, der keine Feuchte misst | BME280 verwenden (Chip-ID 0x60) |
| Temperatur einige Grad zu hoch | Eigenerwärmung durch Pico, WLAN-Chip und Display | Sensor an ein paar Zentimeter Kabel absetzen |
| Luftdruck 40–60 hPa unter dem Wetterbericht | Absoluter Druck am Standort, der Wetterbericht rechnet auf Meereshöhe um | Eigene Höhe in `ALTITUDE_M` eintragen |

## Drehgeber

| Symptom | Ursache | Abhilfe |
|---|---|---|
| Keine Reaktion | CLK oder DT nicht angeschlossen, falsche Pins in `ENC_CLK`/`ENC_DT` | Beide Signale anschließen, Konstanten prüfen |
| Zwei Rasten pro Seite nötig | Drehgeber mit zwei Signalwechseln pro Raste | `ENC_STEPS_PER_DETENT = 2` |
| Seiten wechseln falsch herum | Drehrichtung vertauscht | `ENC_REVERSE = true` oder CLK und DT tauschen |
| Seite springt beim Drehen manchmal zurück | Schlechte Kontakte, sehr starkes Prellen | Kurze Leitungen; bei Bedarf 100 nF von CLK und DT gegen GND |
