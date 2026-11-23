# Entstehung des Projekts – die Prompts

Code und Dokumentation dieses Projekts sind im Dialog mit **Claude** (Anthropic, Modell Claude Opus 5.5)
entstanden. Diese Seite hält fest, welche Prompts dazu geführt haben und was jeweils dabei herauskam.

Die Prompts stehen hier **wörtlich**, einschließlich Tippfehlern, weil genau diese Formulierungen
zum Ergebnis geführt haben. Persönliche Angaben (Dateipfade, WLAN-Name, IP-Adresse, eine private
Bemerkung) sind durch `[…]` ersetzt. Claudes Antworten sind nicht abgedruckt; ihr Ergebnis ist der
Code und die Dokumentation in diesem Repository. Zu jedem Schritt steht, welche Entscheidungen
Claude getroffen hat, ohne dass sie im Prompt verlangt waren, und welche Fehler später korrigiert
wurden. Diese Punkte sollte man kennen, wenn man den Code verändert.

## Übersicht

| # | Datum | Prompt (Kurzform) | Ergebnis |
|---|---|---|---|
| 1 | 21.11.2026 | NTP-Uhr für Pico 2 W mit 1,3"-I2C-Display | erste Version des Sketches |
| 2 | 21.11.2026 | OTA-Updates übers Internet | Signatur, LAN-Upload, Internet-Update, WLAN-Daten im LittleFS, `platform.local.txt` |
| 3 | 21.11.2026 | Anleitung Schritt für Schritt, kurz | Kurzanleitung, später Grundlage für [ota.md](ota.md) |
| 4 | 21.11.2026 | Kompilierfehler der IDE 1.8 | `UiLock` statt Template |
| 5 | 21.11.2026 | Fragen zu Dateien, Port und Build-Ausgabe | Sketch unter eigenem Namen, Port-Rechte, `firmware.bin` im Sketch-Ordner |
| 6 | 21.11.2026 | Fragen zum GitHub-Release | Version aus dem Release-Tag statt `version.txt` |
| 7 | 21.11.2026 | Update bleibt bei 100 % hängen | robuster I2C-Bus, eigener Download statt `httpUpdate` |
| 8 | 21.11.2026 | Eigenes Startbild wird ignoriert | Erklärung Versionslogik und U8g2-Grundlagen |
| 9 | 22.11.2026 | BMP280, Drehgeber, drei Bildschirme | Sensor-Treiber, Drehgeber, Klima- und Tulpen-Seite |
| 10 | 23.11.2026 | Doku für GitHub | `README.md`, `docs/`, Display-Bilder |

---

## 1. Die Uhr

> Schreibe mir für den Raspberry pico 2 W der bei mir über die Arduino IDE Programmiert wird ein
> Programm das über das Internet die Zeit holt und diese Anschsließend asuf dem Display das über I2C
> angebunden ist anzeigt. Das Display ist ein 1,3 IIC Display

**Ergebnis:** der erste Sketch: WLAN, Zeit per SNTP, Anzeige mit U8g2.

**Wie die Vorgabe hinterfragt wurde:** „1,3 IIC Display“ nennt keinen Controller. Claude ist davon
ausgegangen, dass es ein **SH1106** ist, wie bei fast allen 1,3"-Modulen, und hat die Umstellung
auf SSD1306 als Schalter eingebaut. Viele Anleitungen verwenden für diese Displays den
SSD1306-Treiber und bekommen dann ein verschobenes Bild. Siehe [hardware.md](hardware.md#display-sh1106-statt-ssd1306).

**Entscheidungen von Claude:**
- **arduino-pico-Core** von Earle Philhower, weil nur er den Pico 2 W in der Arduino-IDE unterstützt.
- **SNTP von lwIP direkt** statt `NTP.begin()` des Cores. Im Quelltext des Cores fiel auf, dass
  `NTP.begin()` den Servernamen nur einmal auflöst und bei einem DNS-Fehler trotzdem als
  gestartet gilt.
- **Zeitzone MEZ/MESZ mit automatischer Sommerzeit** über einen POSIX-TZ-String.
- **Display auf dem zweiten Kern**, damit die Uhr während eines WLAN-Reconnects weitertickt.
  `WiFi.begin()` blockiert bis zu etwa 30 s.

**Prüfung:** mit arduino-cli für den Pico 2 W kompiliert; die Displayausgabe mit den echten
U8g2-Fonts am PC gerendert, um Überlappungen auszuschließen.

## 2. OTA-Updates

> Ich würde gerne Over the Air Updates realisieren schreibe mir passenden Code dafür das ich den
> Raspberry nicht immer an den PC anschließen muss sondern das übers Internet machen kann

**Ergebnis:** LAN-Upload aus der IDE (ArduinoOTA), Internet-Update aus GitHub-Releases,
signierte Firmware, WLAN-Zugangsdaten im LittleFS mit seriellen Befehlen zur Einrichtung,
`tools/platform.local.txt`.

**Wie die Vorgabe hinterfragt wurde:**
- „Übers Internet“ hätte man auch mit einer Portfreigabe für den Upload-Dienst der IDE lösen
  können. Der ist aber nur mit MD5 geschützt und für das lokale Netz gedacht. Claude hat stattdessen
  ein **Pull-Verfahren** gebaut: Der Pico holt sich Updates selbst, im Router muss nichts
  freigegeben werden.
- Eine öffentlich abgelegte Firmware enthält alles, was im Sketch steht, **auch das
  WLAN-Passwort** im Klartext. Deshalb stehen die Zugangsdaten seitdem im LittleFS des Pico statt
  im Programm.

**Entscheidungen von Claude:**
- **RSA-Signatur** für beide Update-Wege. Dadurch ist es vertretbar, die HTTPS-Verbindung ohne
  Zertifikatsprüfung aufzubauen.
- **`platform.local.txt`**, weil der Netzwerk-Upload der IDE die unsignierte `.bin` schickt. Das
  hat Claude in der `platform.txt` des Cores nachgesehen und den Ersatz mit echten Schlüsseln
  getestet: Die `.bin` ist danach signiert, die Signatur mit openssl verifiziert, und ohne
  Schlüssel bleibt alles unverändert.
- **Schutz vor Update-Schleifen** über `/ota_target.txt`.
- **Fortschrittsbalken** auf dem Display.

**Fehler, später korrigiert:**
- Die erste OTA-Version enthielt eine Template-Funktion, an der die IDE 1.8 scheiterte (Schritt 4).
  Claudes Testbuild hatte den Prototyp-Generator der IDE nicht mit ausgeführt.
- Die Anleitung nannte einen `build/`-Ordner mit einer `.bin` nach dem Exportieren. Die IDE 1.8
  legt dort aber nur eine `.uf2` ab (Schritt 5).
- Die Version stand zunächst in einer eigenen `version.txt` im Release (Schritt 6).
- Das Internet-Update nutzte `httpUpdate` des Cores und blieb nach dem Download hängen (Schritt 7).

## 3. Kurzanleitung

> Kannst du nochmal die Anleitung was zu tun ist sauber schritt für schritt aufschreiben, […] kann
> gerade keine langen texte lesen

**Ergebnis:** die Einrichtung in kurzen nummerierten Schritten: einmal einrichten, Update im
Heimnetz, Update übers Internet. In ausführlicher Form steht das jetzt im README und in
[ota.md](ota.md#ein-release-veröffentlichen).

## 4. Kompilierfehler

> ```
> Arduino: 1.8.19 (Linux), Board: "Raspberry Pi Pico 2W, 4MB (Sketch: 2MB, FS: 2MB), […]"
>
> Enabling binary signing
> wlan-zeit:259:13: error: variable or field 'uiEdit' declared void
> […]
> ```

**Ergebnis:** `UiLock` (RAII-Sperre) statt der Template-Funktion `uiEdit`.

**Ursache:** Die Arduino-IDE erzeugt für jede Funktion in einer `.ino` automatisch einen
Prototyp. Bei Templates lässt sie die Zeile `template <typename F>` weg, der Prototyp verweist dann
auf einen unbekannten Typ `F`.

**Prüfung:** Claude hat den Prototyp-Generator der IDE (Arduinos ctags-Variante) aus dem Quelltext
gebaut, den Fehler damit exakt nachgestellt und die Korrektur mit demselben Werkzeug kompiliert.
Seitdem wird jeder Stand so geprüft.

## 5. Dateien, Port und Build-Ausgabe

> warum immer zwei .ino datein

> Fehler beim Öffnen des seriellen Ports '/dev/ttyS4'. Schauen Sie in die Dokumentation unter
> http://playground.arduino.cc/Linux/All#Permission

> es ist keine bin und es ist gibt auch kein build ordner sondern es ist eine .uf2 datei im
> projektordner

**Ergebnis:**
- Zwei `.ino`-Dateien: Claude hatte den Sketch unter einem anderen Namen geliefert als dem des
  Sketch-Ordners. Die IDE fügt alle `.ino` eines Ordners zusammen. Seitdem heißt die Datei
  `wlan-zeit.ino`.
- `/dev/ttyS4` ist eine serielle Schnittstelle des PCs, nicht der Pico. Dazu die Rechte für die
  Gruppe `dialout` und die Freigaben für die Snap-Version der IDE.
- Die IDE 1.8 exportiert laut `platform.txt` des Cores nur die `.uf2`. `platform.local.txt` legt
  seitdem bei jedem signierten Build `firmware.bin` direkt in den Sketch-Ordner.

## 6. GitHub-Release

> also auf github muss die firmware.bin und die version.txt datei ist das richtig

> […] Das verstehe ich nicht mein Projektverzeichhnis hat nichmal soo viele datein

> was war mit den schlüsseln?

> hat er sich das automatisch geholt also die public.key, weil ich niergends die datei angfügt

> ich seh kein realse

> bei der txt datei kann man nicht im realse uploaden irgendwie lässt er nur bin zu

> Nein Github kann keine .txt datein obwohl was drin steht

> Wann holt er sich das immer von Github also die neuste bin

**Ergebnis:**
- Erklärung, dass ein Release ein eigener Bereich mit Anhängen ist und kein Ordner im Repository,
  und dass ein leeres Repository erst eine Datei braucht, bevor es Releases gibt.
- Die Versionsnummer kommt seitdem aus dem **Release-Tag** (`v6` → 6) über die GitHub-API. Ins
  Release gehört nur noch `firmware.bin`.

**Wie das entschieden wurde:** Claude hielt die Upload-Probleme zuerst für eine leere Datei. Statt
weiter zu suchen, wurde `version.txt` ganz abgeschafft. Das spart einen Schritt pro Release und
schließt Abweichungen zwischen Tag und Datei aus.

## 7. Update bleibt hängen

> Der Update Blaken ist seit einer minute auf 100% nichts passiert was jetzt

> check nochmal dein code, weil er bekommts ja offensichtlich gedownloaded

> Also er hängt bei signatur prüfen, müssen die zwei keys wirklich nur im Projektordner liegen oder
> muss denen noch was passierne

> ```
> Pico2W_NTP_Uhr v3, Signatur aktiv
> […]
> [OTA]  neuestes Release: Tag "v4"
> [OTA]  installiert v3, verfuegbar v4
> [OTA]  Lade https://github.com/Neon153/Pico2W-NTP-Uhr/releases/latest/download/firmware.bin
> [OTA]  Download komplett, pruefe Signatur ...
> ```

> Es hat geklappt was hast du jetzt geändert

> Es hat genau einmal funktioniert jetzt wollte ich testweise nach dem erfolgreichen uodate nochmal
> eins anstoßen jetzt hängts schon wieder

**Ergebnis:**
- **Robuster I2C-Bus:** längerer Timeout mit Bus-Reset, Display vor jedem Neustart anhalten,
  blockierten Bus beim Start freigeben. Siehe [firmware.md](firmware.md#robuster-i2c-bus).
- **Eigener Download** (`downloadFirmware()`) statt `httpUpdate`: Verbindung vor der
  Signaturprüfung schließen, jeden Schritt protokollieren. Siehe
  [ota.md](ota.md#warum-kein-httpupdate).

**Wie die Ursache eingegrenzt wurde:** Das Log zeigte, dass der Pico bereits v3 lief, obwohl das
Update auf v3 vorher „gehangen“ hatte. Die Prüfung war also durchgelaufen, und der Fehler lag bei
oder nach der Signaturprüfung. Das „hängt schon wieder“ im letzten Prompt kam daher, dass Release
v4 noch mit dem alten Code gebaut war: Für ein Update zählt der Code, der auf dem Pico läuft.

**Offen:** Die genaue Ursache des Hängers ist nicht nachgewiesen. Die Erklärung mit dem
Thunk-Stack ist die wahrscheinlichste, aber nicht gemessen.

## 8. Eigenes Startbild

> ```cpp
> void setup1() {
>   […]
>   u8g2.clearBuffer();
>   startScreen(u8g2);
>   u8g2.sendBuffer();
>   delay(500);
> ```
>
> Warum wird meine funktion komplett ignoriert

> brauch man sonst noch was oder ist der font das einzige

**Ergebnis:** drei mögliche Ursachen. Die wahrscheinlichste: Der Testbuild hatte eine kleinere
`FW_VERSION` als das neueste Release, und der Pico hat sich 30 s nach dem Start das Release ohne
Startbild geholt. Dazu die U8g2-Grundlagen: Font setzen, y-Koordinate ist die Grundlinie, Umlaute
nur mit `_tf`-Fonts, XBM-Format für Bilder. Das Startbild ist seitdem fester Teil des Sketches.

## 9. Sensor, Drehgeber und drei Bildschirme

> Ich hab jetzt noch ein BMP280 hinzugefügt, und ein Poti mit CLK, SW, DT als Pins ich hab SW und
> DT an einen ADC Pin vom Pico. Ich möchte das durch drehen des Potis zwischen ein Bildschirm
> Uhrzeit und ein Bildschirm Temperatur, Luftfeuchtigkeit, und druck. Und noch ein dritter
> Bildschirm der eine Tuple anzeigt als Grafik

> Der Code für OTA ist noch drin oder

**Ergebnis:** eigener Treiber für BMP280/BME280, Drehgeber per Interrupt, drei Seiten mit
Seitenpunkten, Tulpen-Grafik.

**Wie die Vorgabe hinterfragt wurde:**
- Ein Bauteil mit CLK, DT und SW ist kein Potentiometer, sondern ein **Drehgeber**. Er liefert
  zwei digitale Signale, deshalb muss auch CLK angeschlossen sein. Die ADC-Pins funktionieren
  trotzdem, weil sie auch normale GPIOs sind.
- Der **BMP280 misst keine Luftfeuchte**. Die Firmware liest deshalb die Chip-ID und zeigt die
  Feuchte nur bei einem BME280 an.
- „Tuple“ hat Claude als **Tulpe** gelesen.

**Entscheidungen von Claude:**
- **Eigener Sensor-Treiber** statt einer Bibliothek, damit BMP280 und BME280 ohne zusätzliche
  Installation funktionieren. Die Umrechnungsformeln aus dem Datenblatt wurden am PC gegen das
  Rechenbeispiel im Datenblatt getestet.
- **Sensor und Drehgeber auf Kern 1**, damit nur ein Kern den I2C-Bus benutzt.
- **Quadratur-Tabelle und Hysterese** gegen Prellen beim Drehgeber.
- **`ALTITUDE_M`** für den Luftdruck auf Meereshöhe, weil der absolute Druck sonst nicht zum
  Wetterbericht passt.
- **Tulpen-Grafik** aus Ellipsen, Dreiecken und Linien, in zwei Runden am PC gerendert und
  nachgebessert.

**Offen:** Sensor, Drehgeber und die neuen Seiten sind noch nicht auf der Hardware bestätigt.

## 10. Diese Dokumentation

> Schreibe mir bitte eine Doku für Github orientiere dich dabei an vorherigen Projekte

**Ergebnis:** `README.md`, die Dokumente in `docs/` und die Display-Bilder. Aufbau und Stil folgen
dem Projekt [Arduino-ADC-Plotter](https://github.com/Neon153/arduino-adc-plotter): README mit
Funktionen, Systemdiagramm, Projektstruktur, Schnellstart und Projektstatus, ausführliche
Dokumente in `docs/` und diese Prompt-Seite.

**Entscheidungen von Claude:**
- Die Display-Bilder sind mit dem Zeichencode der Firmware am PC gerendert und als solche
  gekennzeichnet, keine Fotos.
- Im Projektstatus steht getrennt, was auf der Hardware getestet ist und was nur kompiliert.
- Alle Werte in der Doku (Speicherbedarf, Registerwerte, Fehlercodes, Dateinamen) sind gegen den
  Sketch, den Build und den Quelltext des Cores abgeglichen; alle internen Links automatisch
  geprüft.

---

## Erfahrungen

- **Fehlermeldungen und Logs vollständig einfügen hat am schnellsten geholfen.** Am Log in
  Schritt 7 ließ sich ablesen, dass der Pico bereits v3 lief. Daraus folgte, wo der Fehler liegen
  musste.
- **Nachhaken lohnt sich.** „check nochmal dein code“ hat zu zwei echten Fehlern im Display-Code
  geführt, die mit dem eigentlichen Hänger nur indirekt zu tun hatten.
- **Ein Testbuild ist nur so gut wie seine Werkzeuge.** Claudes erster Build lief ohne den
  Prototyp-Generator der IDE und hat den Template-Fehler deshalb nicht gefunden. Seitdem wird mit
  genau dem Werkzeug geprüft, das auch die IDE benutzt.
- **Genaue Bauteilnamen helfen.** „Poti“ und „BMP280“ hätten zu falschem Code geführt, wenn man sie
  wörtlich genommen hätte: ein Analogeingang und eine Feuchteanzeige, die nie einen Wert zeigt.
- **Bei OTA zählt der Code auf dem Gerät.** Ein Fehler im Update-Code lässt sich nicht mit einem
  Update beheben, das genau dieser Code herunterladen muss. Dafür braucht es einmal USB.

## Mögliche nächste Prompts

- Ergebnisse vom echten Aufbau zurückmelden: Sensorwerte im Vergleich zu einem anderen Thermometer,
  Verhalten des Drehgebers, mehrere Updates hintereinander. Die Dokumentation daran anpassen lassen.
- Den Taster des Drehgebers belegen, z. B. „zurück zur Uhr“ oder Display aus.
- Das Display nachts dimmen und die Anzeige leicht verschieben, um Einbrennen zu vermeiden.
- Eine vierte Seite mit dem Luftdruckverlauf der letzten 24 Stunden.
- Releases mit GitHub Actions bauen. Dafür müsste `private.key` als Secret bei GitHub liegen; das
  sollte man vorher abwägen.
- Eine Lizenz festlegen.
