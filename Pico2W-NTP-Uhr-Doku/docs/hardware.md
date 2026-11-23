# Hardware

## Benötigte Teile

- Raspberry Pi Pico 2 W (RP2350 mit WLAN-Chip CYW43439)
- 1,3"-OLED, 128 × 64 Pixel, I2C, vier Anschlüsse (VCC, GND, SCL, SDA)
- BMP280- oder BME280-Modul mit I2C
- Drehgeber-Modul KY-040 (Anschlüsse CLK, DT, SW, +, GND)
- Steckbrett oder Jumperkabel, USB-Kabel

## Verdrahtung

Display und Sensor hängen am **selben I2C-Bus**. Beide haben unterschiedliche Adressen, deshalb
reichen zwei Leitungen für beide.

| Modul | Anschluss | Pico 2 W | Pin |
|---|---|---|---|
| OLED | VCC | 3V3(OUT) | 36 |
| OLED | GND | GND | 38 |
| OLED | SDA | GP4 (I2C0 SDA) | 6 |
| OLED | SCL | GP5 (I2C0 SCL) | 7 |
| BMP280/BME280 | VCC | 3V3(OUT) | 36 |
| BMP280/BME280 | GND | GND | 38 |
| BMP280/BME280 | SDA | GP4 | 6 |
| BMP280/BME280 | SCL | GP5 | 7 |
| BMP280/BME280 | SDO | GND → Adresse 0x76, 3V3 → 0x77 | – |
| BMP280/BME280 | CSB | 3V3 oder offen (wählt I2C, viele Module haben dafür einen Pull-up) | – |
| Drehgeber | + | 3V3(OUT) | 36 |
| Drehgeber | GND | GND | 38 |
| Drehgeber | CLK | GP28 | 34 |
| Drehgeber | DT | GP27 | 32 |
| Drehgeber | SW | GP26 | 31 |

Andere Pins für den Drehgeber stellt man oben im Sketch ein (`ENC_CLK`, `ENC_DT`, `ENC_SW`), siehe
[Firmware](firmware.md#konfiguration). Die I2C-Pins stehen in `PIN_SDA` und `PIN_SCL`; sie müssen zu
I2C0 passen (GP0/1, 4/5, 8/9, 12/13, 16/17 oder 20/21).

### Alles an 3,3 V

Alle Module bekommen **3V3**, nicht 5 V (VBUS/VSYS). Die Pull-up-Widerstände auf den Modulen
hängen an deren VCC. An 5 V würden SDA, SCL, CLK und DT auf 5 V hochgezogen, die GPIOs des Pico sind
aber für 3,3 V ausgelegt. Der Strombedarf aller drei Module zusammen liegt weit unter dem, was der
3V3-Ausgang des Pico liefert.

### Belegte und freie Pins

| Pins | Verwendung |
|---|---|
| GP4, GP5 | I2C0 für Display und Sensor |
| GP26, GP27, GP28 | Drehgeber (diese Pins können auch ADC, hier digital genutzt) |
| GP23, GP24, GP25, GP29 | intern mit dem WLAN-Chip verbunden, nicht verwenden |
| alle übrigen GPIOs | frei |

## Display: SH1106 statt SSD1306

Fast alle 1,3"-OLEDs haben einen **SH1106** als Controller, die kleineren 0,96"-Module dagegen
einen SSD1306. Beide sehen gleich aus und sprechen ein ähnliches Protokoll, der SH1106 hat aber
einen internen Speicher von 132 statt 128 Spalten. Wer ein SH1106-Display mit einem
SSD1306-Treiber anspricht, sieht ein um 2 Pixel verschobenes Bild und rechts einen Streifen
Pixelmüll.

Die Firmware nutzt die Bibliothek U8g2 mit SH1106-Treiber. Hat ein Modul doch einen SSD1306, setzt
man im Sketch `#define OLED_SSD1306 1`.

Die I2C-Adresse (0x3C oder 0x3D) erkennt die Firmware beim Start selbst. Im seriellen Monitor steht
dazu eine Liste aller gefundenen Geräte:

```
[I2C]  Geraet auf 0x3C
[I2C]  Geraet auf 0x76
```

## Sensor: BMP280 oder BME280

Die beiden Bosch-Sensoren sehen auf den üblichen Breakout-Platinen gleich aus und werden oft unter
falschem Namen verkauft. Der Unterschied:

| | BMP280 | BME280 |
|---|---|---|
| Temperatur | ja | ja |
| Luftdruck | ja | ja |
| **Luftfeuchte** | **nein** | ja |
| Chip-ID (Register 0xD0) | 0x58 | 0x60 |

Die Firmware liest die Chip-ID aus und stellt sich darauf ein. Mit einem BMP280 zeigt die
Klima-Seite bei der Feuchte `-- (BMP280)`. Welcher Chip erkannt wurde, steht beim Start im seriellen
Monitor:

```
[SENS] BMP280 (ohne Feuchte) auf 0x76 gefunden
```

### Platzierung

Der Sensor misst die Temperatur **an seinem eigenen Gehäuse**. Pico, WLAN-Chip, Spannungsregler und
Display erwärmen ihre Umgebung um einige Grad. Für brauchbare Raumtemperaturen gehört der Sensor
an ein paar Zentimeter Kabel, weg von der Platine und nicht über dem Display.

### Luftdruck und Höhe

Der Sensor misst den **absoluten Luftdruck am Standort**. Wetterberichte zeigen dagegen den auf
Meereshöhe umgerechneten Druck. Pro 100 m Höhe sinkt der Luftdruck um etwa 12 hPa; auf 450 m zeigt
der Sensor deshalb rund 50 hPa weniger als der Wetterbericht.

Mit der eigenen Höhe in `ALTITUDE_M` rechnet die Firmware auf Meereshöhe um, siehe
[Firmware](firmware.md#sensor-bmp280--bme280).

## Drehgeber KY-040

Das Modul sieht aus wie ein Potentiometer, ist aber ein **Drehgeber** (Inkrementalgeber). Er
liefert keinen Analogwert, sondern zwei digitale Rechtecksignale, CLK und DT, die um eine
Viertelperiode gegeneinander versetzt sind. Aus der Reihenfolge der Flanken ergibt sich die
Drehrichtung. Deshalb müssen **beide** Signale angeschlossen sein.

- **Rasten:** Die meisten KY-040 erzeugen pro Raste vier Signalwechsel, manche nur zwei. Muss man
  zweimal klicken, um eine Seite weiterzukommen, setzt man `ENC_STEPS_PER_DETENT` auf `2`.
- **Richtung:** Läuft die Seitenfolge falsch herum, setzt man `ENC_REVERSE` auf `true` oder
  vertauscht CLK und DT.
- **Taster (SW):** schaltet beim Drücken gegen GND. Auf vielen Modulen fehlt der Pull-up dafür;
  die Firmware schaltet den internen Pull-up ein. Der Taster hat derzeit keine Funktion.
- **ADC-Pins:** GP26 bis GP28 sind normale GPIOs mit zusätzlicher ADC-Funktion. Für den Drehgeber
  werden sie digital gelesen.
