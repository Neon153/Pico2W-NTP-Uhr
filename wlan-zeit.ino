/*
  Pico2W_NTP_Uhr.ino  (mit OTA-Updates)
  -------------------------------------
  NTP-Uhr auf einem 1,3"-OLED (SH1106, 128x64, I2C) mit zwei Update-Wegen
  ohne USB-Kabel:

    1) LAN / Arduino IDE (ArduinoOTA)
       Werkzeuge -> Port -> Netzwerkports -> "pico2w-uhr at 192.168.x.y"
       und normal hochladen. PC und Pico müssen im selben Netz sein.

    2) Internet / GitHub Releases (Pull-Prinzip)
       Der Pico fragt nach dem Verbinden und dann alle 6 h über die GitHub-API
       das neueste Release ab und liest die Versionsnummer aus dem Tag
       (v2 -> 2). Ist sie größer als FW_VERSION, lädt er firmware.bin aus
       diesem Release, prüft die Signatur und startet neu.
       Ins Release gehört nur firmware.bin, der Tag heißt v<FW_VERSION>.
       Der Pico baut die Verbindung selbst auf: keine Portfreigabe im Router.

  Sicherheit:
    - Firmware wird mit RSA-2048 signiert (private.key/public.key im
      Sketch-Ordner, Signatur erzeugt der Core automatisch beim Kompilieren).
      Der Pico nimmt über beide Wege nur Images an, die mit DEINEM
      privaten Schlüssel signiert sind.
    - WLAN-Zugangsdaten und OTA-Passwort stehen NICHT im Programm, sondern
      im LittleFS des Pico. Das Firmware-Image kann deshalb öffentlich auf
      GitHub liegen, ohne dein WLAN-Passwort zu verraten.

  Arduino-IDE-Einstellungen:
    Board:       Raspberry Pi Pico 2W (Core von Earle F. Philhower)
    Flash Size:  4MB (Sketch: 2MB, FS: 2MB)   <- PFLICHT für OTA
    WiFi Region: Germany
    Bibliothek:  U8g2

  Ersteinrichtung (einmalig per USB):
    Sketch per USB hochladen, seriellen Monitor öffnen (115200, "Neue Zeile"),
    "wifi" eingeben, SSID und Passwort eingeben. Befehle: "help".

  Verdrahtung (alle Module an 3V3, nicht an 5 V: die Pull-ups auf den
  Modulen hängen an deren VCC, und die GPIOs dürfen nur 3,3 V sehen):
    OLED SH1106:      VCC->3V3(OUT) Pin 36, GND->Pin 38, SDA->GP4 Pin 6, SCL->GP5 Pin 7
    BMP280/BME280:    am selben I2C-Bus: VCC->3V3, GND, SDA->GP4, SCL->GP5
                      SDO an GND = Adresse 0x76, an 3V3 = 0x77 (wird automatisch erkannt)
    Drehgeber KY-040: +->3V3, GND, CLK->GP28 Pin 34, DT->GP27 Pin 32, SW->GP26 Pin 31
                      (ADC-Pins funktionieren auch als normale digitale Eingänge)

  Bedienung: Drehgeber drehen wechselt die Seite: Uhr -> Klima -> Tulpe.

  Aufteilung auf die Kerne:
    Core 0: WLAN, SNTP, OTA, serielle Befehle      (darf blockieren)
    Core 1: Display, Sensor, Drehgeber, I2C-Bus     (tickt immer weiter)
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Updater.h>
#include <StackThunk.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <Updater_Signing.h>      // vom Core erzeugt: ARDUINO_SIGNING = 1, wenn public.key im Sketch-Ordner liegt
#include <Wire.h>
#include <U8g2lib.h>
#include <time.h>
#include <math.h>
#include <pico/mutex.h>
#include <hardware/gpio.h>
#include <lwip/apps/sntp.h>

// ============================ Konfiguration ============================

// Vor JEDEM Release um 1 erhöhen. Der Release-Tag muss v<FW_VERSION> heißen.
// Testbuilds, die du per LAN hochlädst, brauchen eine Nummer >= dem neuesten
// GitHub-Release, sonst holt sich der Pico nach 30 s das Release zurück.
#define FW_VERSION 9

static const char *HOSTNAME = "pico2w-uhr";           // mDNS-Name, erscheint als Netzwerkport in der IDE

// GitHub-Repository für Internet-Updates ("" = Internet-Updates aus).
// Muss öffentlich sein: Release-Assets privater Repos brauchen einen Token,
// und der gehört nicht in eine Firmware.
#define UPDATE_REPO "Neon153/Pico2W-NTP-Uhr"
static const uint32_t UPDATE_INTERVAL_MS    = 6UL * 60UL * 60UL * 1000UL;   // alle 6 h
static const uint32_t UPDATE_FIRST_DELAY_MS = 30UL * 1000UL;                // 30 s nach WLAN-Verbindung

static const char *NTP_SERVER_1 = "de.pool.ntp.org";
static const char *NTP_SERVER_2 = "ptbtime1.ptb.de";
static const char *TZ_INFO      = "CET-1CEST,M3.5.0,M10.5.0/3";   // MEZ/MESZ

static const uint8_t  PIN_SDA = 0;
static const uint8_t  PIN_SCL = 1;
static const uint32_t I2C_CLOCK_HZ = 400000;
#define OLED_SSD1306 0                                    // 0 = SH1106 (1,3"), 1 = SSD1306

// Drehgeber (KY-040). Die Pins an deine Verdrahtung anpassen.
static const uint8_t ENC_CLK = 2;
static const uint8_t ENC_DT  = 27;
static const uint8_t ENC_SW  = 26;                        // Taster, derzeit ohne Funktion
static const int     ENC_STEPS_PER_DETENT = 4;            // Flanken pro Raste; braucht es 2 Rasten pro Seite: auf 2 setzen
static const bool    ENC_REVERSE = false;                 // Drehrichtung umkehren

// Höhe über Meeresspiegel in m. 0 = absoluter Luftdruck am Standort.
// Wetterberichte zeigen den auf Meereshöhe umgerechneten Druck; mit deiner
// Höhe hier sind die Werte vergleichbar (pro 100 m etwa 12 hPa Unterschied).
static const float   ALTITUDE_M = 0.0f;

static const uint32_t WIFI_TIMEOUT_MS = 10000;
static const uint32_t WIFI_RETRY_MS   = 15000;
static const time_t   VALID_EPOCH     = 1700000000;      // ab hier gilt die Uhrzeit als gesetzt

// ======================================================================

static const bool  INTERNET_UPDATE = sizeof(UPDATE_REPO) > 1;
static const char *RELEASE_API_URL = "https://api.github.com/repos/" UPDATE_REPO "/releases/latest";
static const char *FIRMWARE_URL    = "https://github.com/" UPDATE_REPO "/releases/latest/download/firmware.bin";

static const char *CONFIG_PATH   = "/config.txt";
static const char *CONFIG_TMP    = "/config.tmp";
static const char *OTA_MARK_PATH = "/ota_target.txt";

#if OLED_SSD1306
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
#else
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
#endif

// ==== RENDER BEGIN ====  (reine Zeichenfunktionen, kein Hardwarezugriff)

enum NetState : uint8_t { NET_NO_CONFIG, NET_CONNECTING, NET_CONNECTED, NET_DISCONNECTED };
enum UpdState : uint8_t { UPD_NONE, UPD_RUNNING, UPD_DONE, UPD_FAILED };

// Alles, was das Display braucht. Core 0 schreibt, Core 1 kopiert (beides unter Mutex).
struct UiState {
  uint8_t  netState;
  int8_t   rssi;
  bool     ntpOk;          // letzte NTP-Abfrage beantwortet
  bool     fsOk;           // LittleFS vorhanden
  uint32_t attempts;       // WLAN-Verbindungsversuche
  uint32_t fwVersion;
  uint8_t  updState;
  uint8_t  updPercent;
  bool     updInternet;    // Quelle: true = GitHub, false = Arduino IDE (LAN)
  int      updError;
  char     ssid[33];
};

// Messwerte des BMP280/BME280 (nur Core 1)
struct Climate {
  bool  present;           // Sensor gefunden
  bool  valid;             // letzte Messung gültig
  bool  hasHum;            // BME280: mit Luftfeuchte
  float tempC;
  float humPct;
  float pressHpa;          // bei ALTITUDE_M > 0 auf Meereshöhe umgerechnet
};

static const uint8_t PAGE_COUNT = 3;   // 0 = Uhr, 1 = Klima, 2 = Tulpe

static const char *const WOCHENTAG[7] = { "So", "Mo", "Di", "Mi", "Do", "Fr", "Sa" };

// Zahl mit einer Nachkommastelle ohne printf-Float-Unterstützung, z. B. "-0.3"
static void fmtTenths(char *buf, size_t n, float v) {
  long t = lroundf(v * 10.0f);
  long a = labs(t);
  snprintf(buf, n, "%s%ld.%ld", (t < 0) ? "-" : "", a / 10, a % 10);
}

// WLAN-Symbol, 11x9 Pixel, linke obere Ecke (x, y)
static void drawWifiIcon(U8G2 &d, int x, int y, uint8_t netState, int8_t rssi) {
  if (netState != NET_CONNECTED) {
    d.drawLine(x + 1, y,     x + 9, y + 8);
    d.drawLine(x + 9, y,     x + 1, y + 8);
    return;
  }
  int bars = (rssi >= -55) ? 4 : (rssi >= -67) ? 3 : (rssi >= -78) ? 2 : (rssi >= -89) ? 1 : 0;
  for (int i = 0; i < 4; i++) {
    int h  = 3 + i * 2;
    int bx = x + i * 3;
    if (i < bars) {
      d.drawBox(bx, y + 9 - h, 2, h);
    } else {
      d.drawHLine(bx, y + 8, 2);
    }
  }
}

// Seitenanzeige oben in der Mitte: gefüllter Punkt = aktuelle Seite
static void drawPageDots(U8G2 &d, int page) {
  for (int i = 0; i < PAGE_COUNT; i++) {
    int x = 92 + i * 7;
    if (i == page) {
      d.drawDisc(x, 4, 2);
    } else {
      d.drawCircle(x, 4, 2);
    }
  }
}

// page < 0: keine Seitenpunkte (Update-Anzeige)
static void drawHeader(U8G2 &d, const char *title, const UiState &s, int page) {
  d.setFont(u8g2_font_helvB10_tr);
  d.drawStr(0, 11, title);
  if (page >= 0) {
    drawPageDots(d, page);
  }
  drawWifiIcon(d, 117, 1, s.netState, s.rssi);
  d.drawHLine(0, 15, d.getDisplayWidth());
}

// Startbild für ca. 1,5 s nach dem Einschalten. Eigene Version einfach hier einsetzen.
static void startScreen(U8G2 &d) {
  char buf[24];
  d.setFont(u8g2_font_helvB14_tr);
  const char *title = "NTP-Uhr";
  d.drawStr((d.getDisplayWidth() - d.getStrWidth(title)) / 2, 30, title);
  d.setFont(u8g2_font_6x10_tr);
  snprintf(buf, sizeof buf, "Firmware v%d", FW_VERSION);
  d.drawStr((d.getDisplayWidth() - d.getStrWidth(buf)) / 2, 48, buf);
}

static void drawClockScreen(U8G2 &d, const struct tm &t, const UiState &s, int page) {
  char buf[32];

  d.setFont(u8g2_font_6x10_tr);
  snprintf(buf, sizeof buf, "%s %02d.%02d.%04d",
           WOCHENTAG[t.tm_wday], t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
  d.drawStr(0, 8, buf);
  drawPageDots(d, page);
  drawWifiIcon(d, 117, 0, s.netState, s.rssi);

  // HH:MM groß, Sekunden klein; zentriert über feste Breiten -> kein Springen
  const int GAP = 3;
  d.setFont(u8g2_font_logisoso32_tn);
  int wHM = d.getStrWidth("00:00");
  d.setFont(u8g2_font_logisoso16_tn);
  int wS  = d.getStrWidth("00");
  int x   = (d.getDisplayWidth() - (wHM + GAP + wS)) / 2;
  const int BASE = 48;

  d.setFont(u8g2_font_logisoso32_tn);
  snprintf(buf, sizeof buf, "%02d:%02d", t.tm_hour, t.tm_min);
  d.drawStr(x, BASE, buf);
  d.setFont(u8g2_font_logisoso16_tn);
  snprintf(buf, sizeof buf, "%02d", t.tm_sec);
  d.drawStr(x + wHM + GAP, BASE, buf);

  // Fußzeile: Zeitzone | Firmware-Version | NTP-Status
  d.setFont(u8g2_font_5x8_tr);
  d.drawStr(0, 63, (t.tm_isdst > 0) ? "MESZ" : "MEZ");
  snprintf(buf, sizeof buf, "v%lu", (unsigned long)s.fwVersion);
  d.drawStr((d.getDisplayWidth() - d.getStrWidth(buf)) / 2, 63, buf);
  const char *ntp = s.ntpOk ? "NTP ok" : "NTP --";
  d.drawStr(d.getDisplayWidth() - d.getStrWidth(ntp), 63, ntp);
}

// Bis zum ersten Zeitabgleich bzw. bei fehlender Einrichtung
static void drawStatusScreen(U8G2 &d, const UiState &s, uint32_t tick, int page) {
  static const char *const DOTS[4] = { "", ".", "..", "..." };
  char buf[40];

  drawHeader(d, "NTP-Uhr", s, page);
  d.setFont(u8g2_font_6x10_tr);

  if (!s.fsOk) {
    d.drawStr(0, 28, "Kein Dateisystem!");
    d.drawStr(0, 40, "Werkzeuge>Flash Size");
    d.drawStr(0, 52, "4MB (FS: 2MB) waehlen");
  } else if (s.netState == NET_NO_CONFIG) {
    d.drawStr(0, 28, "WLAN einrichten:");
    d.drawStr(0, 40, "USB, Serial Monitor,");
    d.drawStr(0, 52, "Befehl: wifi");
  } else {
    switch (s.netState) {
      case NET_CONNECTING: snprintf(buf, sizeof buf, "WLAN verbinden%s", DOTS[tick % 4]); break;
      case NET_CONNECTED:  snprintf(buf, sizeof buf, "WLAN verbunden");                   break;
      default:             snprintf(buf, sizeof buf, "WLAN getrennt");                    break;
    }
    d.drawStr(0, 28, buf);
    snprintf(buf, sizeof buf, "%.21s", s.ssid);
    d.drawStr(0, 40, buf);
    if (s.netState == NET_CONNECTED) {
      snprintf(buf, sizeof buf, "Hole Zeit%s", DOTS[tick % 4]);
    } else {
      snprintf(buf, sizeof buf, "Versuch %lu", (unsigned long)s.attempts);
    }
    d.drawStr(0, 52, buf);
  }

  d.setFont(u8g2_font_5x8_tr);
  snprintf(buf, sizeof buf, "v%lu", (unsigned long)s.fwVersion);
  d.drawStr(d.getDisplayWidth() - d.getStrWidth(buf), 63, buf);
}

static void drawClimateScreen(U8G2 &d, const Climate &c, const UiState &s, int page) {
  char buf[24];
  const int W = d.getDisplayWidth();
  drawHeader(d, "Klima", s, page);

  if (!c.present) {
    d.setFont(u8g2_font_6x10_tr);
    d.drawStr(0, 30, "Kein BMP280/BME280");
    d.drawStr(0, 42, "gefunden (I2C 0x76");
    d.drawStr(0, 54, "oder 0x77)");
    return;
  }

  // Temperatur groß, Gradzeichen als Kreis (braucht keinen Font mit Sonderzeichen)
  d.setFont(u8g2_font_logisoso22_tn);
  if (c.valid) {
    fmtTenths(buf, sizeof buf, c.tempC);
  } else {
    snprintf(buf, sizeof buf, "--");
  }
  int wT = d.getStrWidth(buf);
  d.setFont(u8g2_font_helvB12_tr);
  int wC = d.getStrWidth("C");
  int x  = (W - (wT + 9 + wC)) / 2;
  d.setFont(u8g2_font_logisoso22_tn);
  d.drawStr(x, 41, buf);
  d.drawCircle(x + wT + 4, 22, 2);
  d.setFont(u8g2_font_helvB12_tr);
  d.drawStr(x + wT + 8, 31, "C");

  // Feuchte und Druck: Bezeichnung links, Wert rechtsbündig
  d.setFont(u8g2_font_6x10_tr);
  d.drawStr(0, 53, "Feuchte");
  if (!c.hasHum) {
    snprintf(buf, sizeof buf, "-- (BMP280)");
  } else if (c.valid) {
    snprintf(buf, sizeof buf, "%ld %%", lroundf(c.humPct));
  } else {
    snprintf(buf, sizeof buf, "--");
  }
  d.drawStr(W - d.getStrWidth(buf), 53, buf);

  d.drawStr(0, 63, "Druck");
  if (c.valid) {
    fmtTenths(buf, sizeof buf, c.pressHpa);
    strncat(buf, " hPa", sizeof buf - strlen(buf) - 1);
  } else {
    snprintf(buf, sizeof buf, "--");
  }
  d.drawStr(W - d.getStrWidth(buf), 63, buf);
}

// Eine Tulpe. cx = Mitte, by = Unterkante der Blüte, gy = Boden, w = halbe Blütenbreite.
// leafDir = +1/-1: Seite des langen Blatts
static void drawTulip(U8G2 &d, int cx, int by, int gy, int w, int leafDir) {
  const int h  = w + w / 3;                    // halbe Kelchhöhe
  const int cy = by - h;                       // Mitte des Kelchs
  const int stemLen = gy - by;
  const int ld = leafDir;

  // Stiel
  d.drawBox(cx - 1, by - 2, 2, stemLen + 2);

  // Blätter: breit am Boden, spitz nach oben, je aus zwei Dreiecken
  int lLen = stemLen * 5 / 6;
  int sLen = stemLen * 1 / 2;
  d.drawTriangle(cx, gy, cx + ld * (w + 2), gy - lLen, cx + ld * 2, gy - lLen / 2);
  d.drawTriangle(cx, gy, cx + ld * (w + 2), gy - lLen, cx + ld * (w * 2 / 3 + 3), gy - lLen / 5);
  d.drawTriangle(cx, gy, cx - ld * (w - 1), gy - sLen, cx - ld * 2, gy - sLen / 2);
  d.drawTriangle(cx, gy, cx - ld * (w - 1), gy - sLen, cx - ld * (w / 2 + 2), gy - sLen / 6);

  // Kelch als Ellipse, oben mit drei Spitzen
  d.drawFilledEllipse(cx, cy, w, h, U8G2_DRAW_ALL);
  d.drawTriangle(cx - w / 3, cy - h + 2, cx + w / 3, cy - h + 2, cx, cy - h - w / 2);           // Mitte
  d.drawTriangle(cx - w, cy - 1, cx - w / 3, cy - h + 1, cx - w - 1, cy - h - w / 4);          // links
  d.drawTriangle(cx + w, cy - 1, cx + w / 3, cy - h + 1, cx + w + 1, cy - h - w / 4);          // rechts

  // Kerben zwischen den Spitzen und Kanten der Blütenblätter (schwarz)
  d.setDrawColor(0);
  d.drawTriangle(cx - w / 3 - 2, cy - h - 1, cx - w / 3 + 1, cy - h - 1, cx - w / 3, cy - h + 3);
  d.drawTriangle(cx + w / 3 - 1, cy - h - 1, cx + w / 3 + 2, cy - h - 1, cx + w / 3, cy - h + 3);
  d.drawLine(cx - w / 3, cy - h + 3, cx - w / 4, by - 3);
  d.drawLine(cx + w / 3, cy - h + 3, cx + w / 4, by - 3);
  d.setDrawColor(1);
}

static void drawTulipScreen(U8G2 &d, int page) {
  drawPageDots(d, page);
  const int GROUND = 61;
  drawTulip(d, 34, 42, GROUND, 7, -1);
  drawTulip(d, 94, 40, GROUND, 7, +1);
  drawTulip(d, 64, 32, GROUND, 10, +1);
  d.drawHLine(8, GROUND + 1, d.getDisplayWidth() - 16);
  d.drawHLine(16, GROUND + 2, d.getDisplayWidth() - 32);
}

static void drawUpdateScreen(U8G2 &d, const UiState &s) {
  char buf[32];
  drawHeader(d, s.updInternet ? "Update (Web)" : "Update (LAN)", s, -1);
  d.setFont(u8g2_font_6x10_tr);

  switch (s.updState) {
    case UPD_RUNNING: {
      const int W = d.getDisplayWidth();
      d.drawFrame(0, 22, W, 12);
      d.drawBox(2, 24, (W - 4) * s.updPercent / 100, 8);
      snprintf(buf, sizeof buf, "%u %%", s.updPercent);
      d.drawStr((W - d.getStrWidth(buf)) / 2, 46, buf);
      d.drawStr(0, 60, "Nicht ausschalten");
      break;
    }
    case UPD_DONE:
      d.drawStr(0, 32, "Erfolgreich.");
      d.drawStr(0, 46, "Neustart...");
      break;
    default:
      d.drawStr(0, 30, "Fehlgeschlagen");
      snprintf(buf, sizeof buf, "Fehlercode %d", s.updError);
      d.drawStr(0, 43, buf);
      d.drawStr(0, 56, "Details: Serial");
      break;
  }
}

// ==== RENDER END ====

// ======================= Gemeinsamer Zustand ==========================

auto_init_mutex(g_uiMutex);
static UiState g_ui = {};

// RAII-Sperre für g_ui. Bewusst kein Template: Der Prototyp-Generator der
// Arduino IDE 1.8 erzeugt für Templates in .ino-Dateien kaputte Prototypen.
struct UiLock {
  UiLock()  { mutex_enter_blocking(&g_uiMutex); }
  ~UiLock() { mutex_exit(&g_uiMutex); }
};

static UiState uiSnapshot() {
  mutex_enter_blocking(&g_uiMutex);
  UiState s = g_ui;
  mutex_exit(&g_uiMutex);
  return s;
}

// ===================== Core 0: Konfiguration ==========================

struct Config {
  String ssid;
  String pass;
  String otaPw;
};

static Config   g_cfg;
static bool     g_fsOk             = false;
static uint32_t g_blockedVersion   = 0;      // Release, das nach dem Update nicht die erwartete Version lieferte
static uint32_t g_attempts         = 0;
static bool     g_forceReconnect   = false;
static bool     g_lanOtaActive     = false;
static bool     g_updCheckArmed    = false;
static uint32_t g_updCheckAt       = 0;
static uint32_t g_updFailedUntil   = 0;

// Display vor Neustarts anhalten: Ein Reset mitten in einer I2C-Übertragung
// kann das Display hängen lassen (es hält SDA auf Low). Dann zeigt es nach dem
// Neustart weiter das alte Bild, obwohl die neue Firmware längst läuft.
static volatile bool g_displayHold = false;   // Core 0 -> Core 1: nichts mehr senden
static volatile bool g_displayHeld = false;   // Core 1 -> Core 0: Bus ist frei

static void holdDisplay() {
  g_displayHold = true;
  uint32_t t0 = millis();
  while (!g_displayHeld && millis() - t0 < 1000) {
    delay(1);
  }
}

static void safeReboot() {
  holdDisplay();
  LittleFS.end();
  rp2040.reboot();
}

static bool loadConfig() {
  g_cfg = Config();
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) {
    return false;
  }
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.endsWith("\r")) {
      line.remove(line.length() - 1);
    }
    int eq = line.indexOf('=');
    if (eq <= 0) {
      continue;
    }
    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);          // nicht trimmen: Leerzeichen im Passwort sind erlaubt
    if (key == "ssid")       g_cfg.ssid  = val;
    else if (key == "pass")  g_cfg.pass  = val;
    else if (key == "otapw") g_cfg.otaPw = val;
  }
  f.close();
  return g_cfg.ssid.length() > 0;
}

static bool saveConfig() {
  File f = LittleFS.open(CONFIG_TMP, "w");
  if (!f) {
    return false;
  }
  f.printf("ssid=%s\npass=%s\notapw=%s\n", g_cfg.ssid.c_str(), g_cfg.pass.c_str(), g_cfg.otaPw.c_str());
  f.close();
  LittleFS.remove(CONFIG_PATH);
  return LittleFS.rename(CONFIG_TMP, CONFIG_PATH);
}

static void publishConfig() {
  UiLock lock;
  strncpy(g_ui.ssid, g_cfg.ssid.c_str(), sizeof g_ui.ssid - 1);
  g_ui.ssid[sizeof g_ui.ssid - 1] = '\0';
  if (g_cfg.ssid.length() == 0) {
    g_ui.netState = NET_NO_CONFIG;
  }
}

// Schutz gegen Update-Schleifen: Wurde auf Version N aktualisiert, die neue
// Firmware meldet aber eine kleinere FW_VERSION (Version im Sketch nicht
// erhöht), wird N nicht erneut installiert.
static void checkOtaMark() {
  File f = LittleFS.open(OTA_MARK_PATH, "r");
  if (!f) {
    return;
  }
  uint32_t target = f.readString().toInt();
  f.close();
  if (target > FW_VERSION) {
    g_blockedVersion = target;
    Serial.printf("[OTA]  WARNUNG: Update auf v%lu geladen, laufende Firmware meldet aber v%d.\n"
                  "       FW_VERSION im Release nicht erhoeht? v%lu wird nicht erneut geladen.\n",
                  (unsigned long)target, FW_VERSION, (unsigned long)target);
  } else {
    LittleFS.remove(OTA_MARK_PATH);
    Serial.printf("[OTA]  Update auf v%d erfolgreich\n", FW_VERSION);
  }
}

static void writeOtaMark(uint32_t version) {
  File f = LittleFS.open(OTA_MARK_PATH, "w");
  if (f) {
    f.printf("%lu\n", (unsigned long)version);
    f.close();
  }
}

// ======================== Core 0: Netzwerk ============================

static void startSntp() {
  sntp_stop();
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, NTP_SERVER_1);
  sntp_setservername(1, NTP_SERVER_2);
  sntp_init();
}

static void startLanOta() {
  if (g_lanOtaActive) {
    ArduinoOTA.end();                              // nach Reconnect mDNS neu anmelden
  }
  ArduinoOTA.setHostname(HOSTNAME);                // wirkt nur beim ersten Aufruf
  if (g_cfg.otaPw.length()) {
    ArduinoOTA.setPassword(g_cfg.otaPw.c_str());   // wirkt nur beim ersten Aufruf -> nach Änderung Neustart
  }
  ArduinoOTA.begin();
  g_lanOtaActive = true;
  Serial.printf("[OTA]  LAN-Upload bereit: %s.local, Port 2040, %s\n",
                HOSTNAME, g_cfg.otaPw.length() ? "mit Passwort" : "ohne Passwort");
}

static void connectWifi() {
  g_attempts++;
  {
    UiLock lock;
    g_ui.netState = NET_CONNECTING;
    g_ui.attempts = g_attempts;
  }
  Serial.printf("[WLAN] Verbinde mit \"%s\" (Versuch %lu)\n", g_cfg.ssid.c_str(), (unsigned long)g_attempts);

  if (g_cfg.pass.length()) {
    WiFi.begin(g_cfg.ssid.c_str(), g_cfg.pass.c_str());   // blockiert bis verbunden oder Timeout
  } else {
    WiFi.begin(g_cfg.ssid.c_str());                       // offenes Netz
  }

  if (WiFi.status() != WL_CONNECTED) {
    { UiLock lock; g_ui.netState = NET_DISCONNECTED; }
    Serial.printf("[WLAN] fehlgeschlagen (Status %d), neuer Versuch in %lu s\n",
                  WiFi.status(), (unsigned long)(WIFI_RETRY_MS / 1000));
  }
}

static void handleNetwork() {
  static bool     wasConnected = false;
  static bool     triedOnce    = false;
  static uint32_t lastAttempt  = 0;
  static uint32_t lastStats    = 0;

  if (g_cfg.ssid.length() == 0) {
    return;
  }

  bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected && !wasConnected) {
    Serial.printf("[WLAN] verbunden, IP %s, RSSI %ld dBm\n",
                  WiFi.localIP().toString().c_str(), (long)WiFi.RSSI());
    startSntp();
    startLanOta();
    g_updCheckArmed = true;
    g_updCheckAt    = millis() + UPDATE_FIRST_DELAY_MS;
    { UiLock lock; g_ui.netState = NET_CONNECTED; }
  } else if (!connected && wasConnected) {
    Serial.println("[WLAN] Verbindung verloren");
    {
      UiLock lock;
      g_ui.netState = NET_DISCONNECTED;
      g_ui.ntpOk    = false;
    }
    lastAttempt = millis();
  }
  wasConnected = connected;

  if (connected) {
    if (millis() - lastStats >= 1000) {
      lastStats = millis();
      int8_t rssi = (int8_t)WiFi.RSSI();
      bool   ntp  = ((sntp_getreachability(0) | sntp_getreachability(1)) & 0x01) != 0;
      {
        UiLock lock;
        g_ui.rssi  = rssi;
        g_ui.ntpOk = ntp;
      }
    }
  } else if (g_forceReconnect || !triedOnce || (millis() - lastAttempt >= WIFI_RETRY_MS)) {
    triedOnce        = true;
    g_forceReconnect = false;
    connectWifi();
    lastAttempt = millis();
  }
}

// ===================== Core 0: Internet-Update ========================

// Liefert die Versionsnummer aus dem Tag des neuesten Releases
// ("v2" -> 2, "v2.1" -> 2), <= 0 bei Fehler oder ohne Release.
static long fetchRemoteVersion() {
  // TLS ohne Zertifikatsprüfung ist hier vertretbar: Ob die Firmware echt
  // ist, entscheidet die RSA-Signatur, nicht die Transportverschlüsselung.
  // Ein Angreifer im Netz kann höchstens ein Update verhindern oder eine
  // ältere, von dir signierte Firmware unterschieben.
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.setUserAgent(HOSTNAME);                     // GitHub-API lehnt Anfragen ohne User-Agent ab
  if (!http.begin(client, RELEASE_API_URL)) {
    return -1;
  }
  http.addHeader("Accept", "application/vnd.github+json");

  long version = -1;
  int  code    = http.GET();
  if (code == HTTP_CODE_OK) {
    String json = http.getString();
    int key = json.indexOf("\"tag_name\"");
    int q1  = (key >= 0) ? json.indexOf('"', json.indexOf(':', key) + 1) : -1;
    int q2  = (q1 >= 0) ? json.indexOf('"', q1 + 1) : -1;
    if (q2 > q1 && q1 >= 0) {
      String tag = json.substring(q1 + 1, q2);
      Serial.printf("[OTA]  neuestes Release: Tag \"%s\"\n", tag.c_str());
      if (tag.startsWith("v") || tag.startsWith("V")) {
        tag.remove(0, 1);
      }
      version = tag.toInt();
    } else {
      Serial.println("[OTA]  Antwort ohne tag_name");
    }
  } else if (code == HTTP_CODE_NOT_FOUND) {
    Serial.println("[OTA]  kein Release gefunden (noch keins veroeffentlicht oder Repo privat)");
  } else {
    Serial.printf("[OTA]  GitHub-API: HTTP %d %s\n", code, code < 0 ? http.errorToString(code).c_str() : "");
  }
  http.end();
  return version;
}

static void markUpdateFailed(int err) {
  {
    UiLock lock;
    g_ui.updState = UPD_FAILED;
    g_ui.updError = err;
  }
  g_updFailedUntil = millis() + 15000;
}

// Lädt firmware.bin und legt sie für den OTA-Bootloader im LittleFS ab.
// Bewusst ohne HTTPUpdate: Die TLS-Verbindung wird VOR der Signaturprüfung
// geschlossen, und jeder Schritt landet im seriellen Monitor.
static bool downloadFirmware(int &err) {
  err = 0;
  WiFiClientSecure client;
  client.setInsecure();                            // Echtheit prüft die Signatur, siehe fetchRemoteVersion()
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);   // GitHub leitet auf das CDN um
  http.setTimeout(15000);
  http.setUserAgent(HOSTNAME);
  if (!http.begin(client, FIRMWARE_URL)) {
    err = -1;
    return false;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[OTA]  firmware.bin: HTTP %d %s\n", code, code < 0 ? http.errorToString(code).c_str() : "");
    http.end();
    err = code;
    return false;
  }
  int len = http.getSize();
  Serial.printf("[OTA]  firmware.bin: %d Bytes\n", len);
  if (len <= 0) {
    http.end();
    err = -2;
    return false;
  }

  Update.onProgress([](size_t done, size_t total) {
    uint8_t pct = total ? (uint8_t)((uint64_t)done * 100 / total) : 0;
    UiLock lock;
    g_ui.updPercent = pct;
  });
  if (!Update.begin(len)) {
    err = Update.getError();
    Serial.printf("[OTA]  Update.begin fehlgeschlagen, Fehler %d\n", err);
    http.end();
    Update.onProgress(nullptr);
    return false;
  }

  size_t written = Update.writeStream(*http.getStreamPtr());
  http.end();
  client.stop();
  Update.onProgress(nullptr);
  Serial.printf("[OTA]  %u von %d Bytes gespeichert, Verbindung geschlossen\n", (unsigned)written, len);
  Serial.printf("[OTA]  Thunk-Stack bisher max. %lu von 6400 Bytes\n", (unsigned long)stack_thunk_get_max_usage());
  if (written != (size_t)len) {
    err = Update.getError() ? Update.getError() : -3;
    Update.end();                                  // aufräumen, Datei schließen
    return false;
  }

  Serial.println("[OTA]  pruefe Signatur ...");
  Serial.flush();
  bool ok = Update.end();
  err = Update.getError();
  Serial.printf("[OTA]  Signaturpruefung %s (Fehler %d)\n", ok ? "ok, Update bereitgestellt" : "FEHLGESCHLAGEN", err);
  Serial.flush();
  return ok;
}

static void checkInternetUpdate() {
  Serial.printf("[OTA]  Pruefe %s\n", RELEASE_API_URL);
  long remote = fetchRemoteVersion();
  if (remote <= 0) {
    Serial.println("[OTA]  keine gueltige Versionsnummer erhalten");
    return;
  }
  Serial.printf("[OTA]  installiert v%d, verfuegbar v%ld\n", FW_VERSION, remote);
  if (remote <= FW_VERSION) {
    return;
  }
  if ((uint32_t)remote == g_blockedVersion) {
    Serial.printf("[OTA]  v%ld uebersprungen (lieferte nach letztem Update falsche FW_VERSION)\n", remote);
    return;
  }

  Serial.printf("[OTA]  Lade %s\n", FIRMWARE_URL);
  {
    UiLock lock;
    g_ui.updState    = UPD_RUNNING;
    g_ui.updInternet = true;
    g_ui.updPercent  = 0;
  }

  int err = 0;
  if (downloadFirmware(err)) {
    Serial.println("[OTA]  Neustart, der Bootloader kopiert jetzt die neue Firmware (ca. 10 s)");
    Serial.flush();
    writeOtaMark((uint32_t)remote);                // erst jetzt: Stromausfall beim Download blockiert nichts
    { UiLock lock; g_ui.updState = UPD_DONE; }
    delay(1500);                                   // Display zeigt "Neustart..."
    safeReboot();
  }

  Serial.printf("[OTA]  Update fehlgeschlagen, Fehler %d\n", err);
  markUpdateFailed(err);
}

static void handleUpdateCheck() {
  if (g_updFailedUntil && (int32_t)(millis() - g_updFailedUntil) >= 0) {
    g_updFailedUntil = 0;
    { UiLock lock; g_ui.updState = UPD_NONE; }
  }
  if (!INTERNET_UPDATE || !g_updCheckArmed || (int32_t)(millis() - g_updCheckAt) < 0) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return;                                        // wird beim nächsten Verbinden neu angesetzt
  }
  g_updCheckAt = millis() + UPDATE_INTERVAL_MS;
  checkInternetUpdate();
}

// ===================== Core 0: serielle Befehle =======================

enum InputMode : uint8_t { IN_CMD, IN_SSID, IN_PASS, IN_OTAPW };
static InputMode g_inMode = IN_CMD;
static String    g_newSsid;

static void printHelp() {
  Serial.println(F("Befehle:\n"
                   "  wifi    WLAN-Zugangsdaten setzen\n"
                   "  otapw   Passwort fuer LAN-Upload aus der IDE setzen (leer = keins)\n"
                   "  update  sofort nach Internet-Update suchen\n"
                   "  info    Status anzeigen\n"
                   "  reboot  Neustart"));
}

static void printInfo() {
  FSInfo fs = {};
  if (g_fsOk) {
    LittleFS.info(fs);
  }
  extern uint8_t __flash_binary_end;
  uint32_t imageSize = (uint32_t)&__flash_binary_end - XIP_BASE;

  Serial.printf("Firmware   v%d, gebaut %s %s\n", FW_VERSION, __DATE__, __TIME__);
  Serial.printf("Signatur   %s\n", ARDUINO_SIGNING ? "aktiv (nur signierte Updates)"
                                                   : "AUS - jedes Image wird akzeptiert!");
  Serial.printf("Image      %lu KB, LittleFS %lu KB (belegt %lu KB)\n",
                (unsigned long)(imageSize / 1024), (unsigned long)(fs.totalBytes / 1024),
                (unsigned long)(fs.usedBytes / 1024));
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WLAN       \"%s\", IP %s, RSSI %ld dBm\n", g_cfg.ssid.c_str(),
                  WiFi.localIP().toString().c_str(), (long)WiFi.RSSI());
  } else {
    Serial.printf("WLAN       %s\n", g_cfg.ssid.length() ? "nicht verbunden" : "nicht eingerichtet");
  }
  time_t now = time(nullptr);
  if (now >= VALID_EPOCH) {
    struct tm t;
    char buf[40];
    localtime_r(&now, &t);
    strftime(buf, sizeof buf, "%d.%m.%Y %H:%M:%S %Z", &t);
    Serial.printf("Zeit       %s\n", buf);
  } else {
    Serial.println("Zeit       noch nicht synchronisiert");
  }
  Serial.printf("LAN-OTA    %s.local:2040, %s\n", HOSTNAME, g_cfg.otaPw.length() ? "mit Passwort" : "ohne Passwort");
  if (INTERNET_UPDATE) {
    if (g_updCheckArmed) {
      Serial.printf("Web-OTA    %s, naechste Pruefung in %ld min\n", UPDATE_REPO,
                    (long)((int32_t)(g_updCheckAt - millis()) / 60000));
    } else {
      Serial.printf("Web-OTA    %s, Pruefung nach WLAN-Verbindung\n", UPDATE_REPO);
    }
  } else {
    Serial.println("Web-OTA    aus");
  }
}

static void processLine(const char *line) {
  switch (g_inMode) {
    case IN_SSID:
      if (!*line) {
        Serial.println("Abgebrochen.");
        g_inMode = IN_CMD;
      } else if (strlen(line) > 32) {
        Serial.print("SSID ist laenger als 32 Zeichen. SSID: ");
      } else {
        g_newSsid = line;
        Serial.println(g_newSsid);
        Serial.print("Passwort (leer = offenes Netz): ");
        g_inMode = IN_PASS;
      }
      return;

    case IN_PASS: {
      size_t n = strlen(line);
      if (n > 0 && (n < 8 || n > 63)) {
        Serial.print("\nWPA-Passwoerter haben 8 bis 63 Zeichen. Passwort: ");
        return;
      }
      g_cfg.ssid = g_newSsid;
      g_cfg.pass = line;
      g_inMode   = IN_CMD;
      Serial.println(n ? "********" : "(keins)");
      Serial.println(saveConfig() ? "Gespeichert. Verbinde neu ..." : "FEHLER beim Speichern!");
      publishConfig();
      WiFi.disconnect();
      g_forceReconnect = true;
      return;
    }

    case IN_OTAPW:
      g_cfg.otaPw = line;
      g_inMode    = IN_CMD;
      Serial.println(saveConfig() ? "Gespeichert. Neustart, damit es wirkt ..." : "FEHLER beim Speichern!");
      delay(300);
      safeReboot();
      return;

    case IN_CMD:
      break;
  }

  String cmd = line;
  cmd.trim();
  cmd.toLowerCase();
  if (cmd.length() == 0) {
    return;
  }
  if ((cmd == "wifi" || cmd == "otapw") && !g_fsOk) {
    Serial.println("Kein LittleFS: Werkzeuge -> Flash Size -> 4MB (Sketch: 2MB, FS: 2MB) und per USB neu flashen.");
  } else if (cmd == "wifi") {
    Serial.print("SSID: ");
    g_inMode = IN_SSID;
  } else if (cmd == "otapw") {
    Serial.print("OTA-Passwort (leer = keins): ");
    g_inMode = IN_OTAPW;
  } else if (cmd == "update") {
    if (!INTERNET_UPDATE) {
      Serial.println("Internet-Update ist aus (UPDATE_REPO leer).");
    } else {
      g_updCheckArmed = true;
      g_updCheckAt    = millis();
    }
  } else if (cmd == "info") {
    printInfo();
  } else if (cmd == "reboot") {
    safeReboot();
  } else {
    printHelp();
  }
}

static void handleSerial() {
  static char   line[100];
  static size_t len    = 0;
  static bool   prevCR = false;

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' && prevCR) {                     // CRLF als ein Zeilenende
      prevCR = false;
      continue;
    }
    prevCR = (c == '\r');
    if (c == '\r' || c == '\n') {
      line[len] = '\0';
      len = 0;
      processLine(line);
    } else if (len < sizeof line - 1) {
      line[len++] = c;
    }
  }
}

// =========================== Core 0: Ablauf ===========================

void setup() {
  Serial.begin(115200);
  {
    UiLock lock;
    g_ui.fwVersion = FW_VERSION;
    g_ui.netState  = NET_NO_CONFIG;
  }

  g_fsOk = LittleFS.begin();                       // formatiert beim allerersten Start automatisch
  if (g_fsOk) {
    loadConfig();
    checkOtaMark();
  }
  publishConfig();
  {
    UiLock lock;
    g_ui.fsOk = g_fsOk;
    if (g_cfg.ssid.length()) {
      g_ui.netState = NET_CONNECTING;
    }
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setTimeout(WIFI_TIMEOUT_MS);

  // LAN-Upload aus der Arduino IDE
  ArduinoOTA.onStart([]() {
    Serial.println("[OTA]  LAN-Upload gestartet");
    {
      UiLock lock;
      g_ui.updState    = UPD_RUNNING;
      g_ui.updInternet = false;
      g_ui.updPercent  = 0;
    }
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    uint8_t pct = total ? (uint8_t)((uint64_t)done * 100 / total) : 0;
    { UiLock lock; g_ui.updPercent = pct; }
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("[OTA]  LAN-Upload fertig, Neustart");
    { UiLock lock; g_ui.updState = UPD_DONE; }
    delay(1000);
    holdDisplay();                                 // ArduinoOTA startet danach selbst neu
  });
  ArduinoOTA.onError([](ota_error_t e) {
    // OTA_END_ERROR bei aktiver Signatur meist: Image unsigniert oder mit anderem Schlüssel signiert
    Serial.printf("[OTA]  LAN-Upload fehlgeschlagen, Fehler %d%s\n", (int)e,
                  (e == OTA_END_ERROR && ARDUINO_SIGNING) ? " (Signatur?)" : "");
    markUpdateFailed((int)e);
  });

  delay(1500);                                     // Zeit, den seriellen Monitor zu öffnen
  Serial.printf("\nPico2W_NTP_Uhr v%d, Signatur %s\n", FW_VERSION, ARDUINO_SIGNING ? "aktiv" : "AUS");
  if (!g_fsOk) {
    Serial.println("FEHLER: kein LittleFS. Werkzeuge -> Flash Size -> 4MB (Sketch: 2MB, FS: 2MB)");
  } else if (g_cfg.ssid.length() == 0) {
    Serial.println("Keine WLAN-Daten gespeichert. Eingabe: wifi");
  }
  printHelp();
}

void loop() {
  handleSerial();
  handleNetwork();
  if (g_lanOtaActive) {
    ArduinoOTA.handle();                           // blockiert während eines LAN-Uploads
  }
  handleUpdateCheck();                             // blockiert während eines Internet-Updates
  delay(10);
}

// ============================ Core 1 ==================================

static void scanI2C(bool &found3C, bool &found3D) {
  found3C = found3D = false;
  Wire.begin();
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[I2C]  Geraet auf 0x%02X\n", a);
      if (a == 0x3C) found3C = true;
      if (a == 0x3D) found3D = true;
    }
  }
}

// Gibt den Bus frei, falls das Display nach einem Reset mitten in einer
// Übertragung SDA noch auf Low hält: bis zu 9 Takte auf SCL, dann ein STOP.
// Open-Drain nachgebildet: Low = als Ausgang treiben, High = loslassen (Pull-up).
static void i2cBusRecover() {
  pinMode(PIN_SDA, INPUT_PULLUP);
  pinMode(PIN_SCL, INPUT_PULLUP);
  delayMicroseconds(10);
  if (digitalRead(PIN_SDA) == LOW) {
    Serial.println("[I2C]  SDA haengt auf Low, gebe Bus frei");
  }
  for (int i = 0; i < 9 && digitalRead(PIN_SDA) == LOW; i++) {
    pinMode(PIN_SCL, OUTPUT);
    digitalWrite(PIN_SCL, LOW);
    delayMicroseconds(5);
    pinMode(PIN_SCL, INPUT_PULLUP);
    delayMicroseconds(5);
  }
  pinMode(PIN_SDA, OUTPUT);                         // STOP: SDA Low -> High bei SCL High
  digitalWrite(PIN_SDA, LOW);
  delayMicroseconds(5);
  pinMode(PIN_SDA, INPUT_PULLUP);
  delayMicroseconds(5);
}

// ===================== Core 1: BMP280 / BME280 ========================
// Eigener Treiber statt Bibliothek: Er erkennt beide Chips an der ID und
// nutzt dieselben Kompensationsformeln wie das Bosch-Datenblatt.

// ==== BME MATH BEGIN ====  (Kompensationsformeln aus dem Bosch-Datenblatt, Integer-Variante)

struct BmeCalib {
  uint16_t T1;
  int16_t  T2, T3;
  uint16_t P1;
  int16_t  P2, P3, P4, P5, P6, P7, P8, P9;
  uint8_t  H1, H3;
  int16_t  H2, H4, H5;
  int8_t   H6;
};

static BmeCalib g_cal;
static int32_t  g_tFine;

// Temperatur in 0,01 °C
static int32_t bmeCompT(int32_t adc) {
  int32_t var1 = ((((adc >> 3) - ((int32_t)g_cal.T1 << 1))) * ((int32_t)g_cal.T2)) >> 11;
  int32_t var2 = (((((adc >> 4) - ((int32_t)g_cal.T1)) * ((adc >> 4) - ((int32_t)g_cal.T1))) >> 12) *
                  ((int32_t)g_cal.T3)) >> 14;
  g_tFine = var1 + var2;
  return (g_tFine * 5 + 128) >> 8;
}

// Druck in Pa * 256 (Q24.8), 0 bei ungültiger Kalibrierung
static uint32_t bmeCompP(int32_t adc) {
  int64_t var1 = ((int64_t)g_tFine) - 128000;
  int64_t var2 = var1 * var1 * (int64_t)g_cal.P6;
  var2 = var2 + ((var1 * (int64_t)g_cal.P5) << 17);
  var2 = var2 + (((int64_t)g_cal.P4) << 35);
  var1 = ((var1 * var1 * (int64_t)g_cal.P3) >> 8) + ((var1 * (int64_t)g_cal.P2) << 12);
  var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)g_cal.P1) >> 33;
  if (var1 == 0) {
    return 0;
  }
  int64_t p = 1048576 - adc;
  p = (((p << 31) - var2) * 3125) / var1;
  var1 = (((int64_t)g_cal.P9) * (p >> 13) * (p >> 13)) >> 25;
  var2 = (((int64_t)g_cal.P8) * p) >> 19;
  p = ((p + var1 + var2) >> 8) + (((int64_t)g_cal.P7) << 4);
  return (uint32_t)p;
}

// Relative Feuchte in %RH * 1024 (Q22.10), nur BME280
static uint32_t bmeCompH(int32_t adc) {
  int32_t v = g_tFine - ((int32_t)76800);
  v = (((((adc << 14) - (((int32_t)g_cal.H4) << 20) - (((int32_t)g_cal.H5) * v)) + ((int32_t)16384)) >> 15) *
       (((((((v * ((int32_t)g_cal.H6)) >> 10) * (((v * ((int32_t)g_cal.H3)) >> 11) + ((int32_t)32768))) >> 10) +
          ((int32_t)2097152)) * ((int32_t)g_cal.H2) + 8192) >> 14));
  v = v - (((((v >> 15) * (v >> 15)) >> 7) * ((int32_t)g_cal.H1)) >> 4);
  v = (v < 0) ? 0 : v;
  v = (v > 419430400) ? 419430400 : v;
  return (uint32_t)(v >> 12);
}

// Rohdaten-Bytes ab Register 0xF7 (6 bzw. 8 Bytes) in Werte umrechnen
static void bmeConvert(const uint8_t *b, bool withHum, float &tempC, float &pressPa, float &humPct) {
  int32_t adcP = ((int32_t)b[0] << 12) | ((int32_t)b[1] << 4) | (b[2] >> 4);
  int32_t adcT = ((int32_t)b[3] << 12) | ((int32_t)b[4] << 4) | (b[5] >> 4);
  tempC   = bmeCompT(adcT) / 100.0f;
  pressPa = bmeCompP(adcP) / 256.0f;
  humPct  = -1.0f;
  if (withHum) {
    int32_t adcH = ((int32_t)b[6] << 8) | b[7];
    humPct = bmeCompH(adcH) / 1024.0f;
  }
}

// ==== BME MATH END ====

static const uint8_t CHIP_ID_BMP280 = 0x58;
static const uint8_t CHIP_ID_BME280 = 0x60;
static uint8_t g_bmeAddr = 0;                      // 0 = kein Sensor
static bool    g_bmeHum  = false;

static bool bmeWriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(g_bmeAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool bmeReadRegs(uint8_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(g_bmeAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(g_bmeAddr, n) != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    buf[i] = (uint8_t)Wire.read();
  }
  return true;
}

static uint16_t le16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// Sucht den Sensor auf 0x76 und 0x77, liest die Kalibrierung und startet den Messbetrieb.
static bool bmeBegin(bool verbose) {
  static const uint8_t ADDRS[2] = { 0x76, 0x77 };
  for (uint8_t addr : ADDRS) {
    g_bmeAddr = addr;
    uint8_t id = 0;
    if (!bmeReadRegs(0xD0, &id, 1)) {
      continue;
    }
    if (id != CHIP_ID_BMP280 && id != CHIP_ID_BME280) {
      if (verbose) {
        Serial.printf("[SENS] 0x%02X: unbekannte Chip-ID 0x%02X\n", addr, id);
      }
      continue;
    }
    g_bmeHum = (id == CHIP_ID_BME280);

    bmeWriteReg(0xE0, 0xB6);                       // Soft-Reset
    delay(5);
    for (int i = 0; i < 20; i++) {                 // warten, bis die Kalibrierung geladen ist
      uint8_t st = 1;
      if (bmeReadRegs(0xF3, &st, 1) && !(st & 0x01)) {
        break;
      }
      delay(2);
    }

    uint8_t c[24];
    if (!bmeReadRegs(0x88, c, sizeof c)) {
      continue;
    }
    g_cal.T1 = le16(c + 0);
    g_cal.T2 = (int16_t)le16(c + 2);
    g_cal.T3 = (int16_t)le16(c + 4);
    g_cal.P1 = le16(c + 6);
    g_cal.P2 = (int16_t)le16(c + 8);
    g_cal.P3 = (int16_t)le16(c + 10);
    g_cal.P4 = (int16_t)le16(c + 12);
    g_cal.P5 = (int16_t)le16(c + 14);
    g_cal.P6 = (int16_t)le16(c + 16);
    g_cal.P7 = (int16_t)le16(c + 18);
    g_cal.P8 = (int16_t)le16(c + 20);
    g_cal.P9 = (int16_t)le16(c + 22);

    if (g_bmeHum) {
      uint8_t h1 = 0;
      uint8_t h[7];
      if (!bmeReadRegs(0xA1, &h1, 1) || !bmeReadRegs(0xE1, h, sizeof h)) {
        continue;
      }
      g_cal.H1 = h1;
      g_cal.H2 = (int16_t)le16(h + 0);
      g_cal.H3 = h[2];
      g_cal.H4 = (int16_t)((int16_t)(int8_t)h[3] * 16 | (h[4] & 0x0F));
      g_cal.H5 = (int16_t)((int16_t)(int8_t)h[5] * 16 | (h[4] >> 4));
      g_cal.H6 = (int8_t)h[6];
      bmeWriteReg(0xF2, 0x01);                     // Feuchte 1x (wirkt erst mit ctrl_meas)
    }
    bmeWriteReg(0xF5, 0x90);                       // Standby 500 ms, IIR-Filter 16
    bmeWriteReg(0xF4, 0x57);                       // Temperatur 2x, Druck 16x, Normalbetrieb
    Serial.printf("[SENS] %s auf 0x%02X gefunden\n", g_bmeHum ? "BME280 (mit Feuchte)" : "BMP280 (ohne Feuchte)", addr);
    return true;
  }
  g_bmeAddr = 0;
  if (verbose) {
    Serial.println("[SENS] kein BMP280/BME280 auf 0x76/0x77");
  }
  return false;
}

static void bmeMeasure(Climate &c) {
  uint8_t b[8];
  if (!bmeReadRegs(0xF7, b, g_bmeHum ? 8 : 6)) {
    c.present = false;                             // Sensor weg: später neu suchen
    c.valid   = false;
    g_bmeAddr = 0;
    return;
  }
  if (b[0] == 0x80 && b[1] == 0x00 && (b[2] >> 4) == 0) {
    c.valid = false;                               // Rücksetzwert: noch keine Messung fertig
    return;
  }
  float t, p, h;
  bmeConvert(b, g_bmeHum, t, p, h);
  if (ALTITUDE_M > 0.0f) {
    p = p / powf(1.0f - ALTITUDE_M / 44330.0f, 5.255f);
  }
  c.present  = true;
  c.valid    = true;
  c.hasHum   = g_bmeHum;
  c.tempC    = t;
  c.pressHpa = p / 100.0f;
  c.humPct   = h;
}

// ======================== Core 1: Drehgeber ===========================
// Quadratur-Auswertung per Interrupt. Die Tabelle lässt nur gültige
// Gray-Code-Übergänge zählen; Prellen erzeugt +1/-1-Paare, die sich aufheben.

static volatile int32_t g_encCount = 0;
static volatile uint8_t g_encState = 0;

static void encIsr() {
  static const int8_t STEP[16] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };
  uint32_t in = gpio_get_all();
  uint8_t  s  = (uint8_t)((((in >> ENC_CLK) & 1u) << 1) | ((in >> ENC_DT) & 1u));
  g_encCount = g_encCount + STEP[(g_encState << 2) | s];
  g_encState = s;
}

static void encBegin() {
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT,  INPUT_PULLUP);
  pinMode(ENC_SW,  INPUT_PULLUP);
  g_encState = (uint8_t)((digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT));
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), encIsr, CHANGE);   // Interrupt läuft auf Core 1
  attachInterrupt(digitalPinToInterrupt(ENC_DT),  encIsr, CHANGE);
}

// Liefert die Rasten seit dem letzten Aufruf. Eine Seite weiter erst nach
// einer vollen Raste, kleines Zittern um die Rastposition zählt nicht.
static int encTakeDetents() {
  static int32_t ref = 0;
  int32_t c = ENC_REVERSE ? -g_encCount : g_encCount;
  int detents = 0;
  while (c - ref >= ENC_STEPS_PER_DETENT) {
    ref += ENC_STEPS_PER_DETENT;
    detents++;
  }
  while (ref - c >= ENC_STEPS_PER_DETENT) {
    ref -= ENC_STEPS_PER_DETENT;
    detents--;
  }
  return detents;
}

// ============================ Core 1: Ablauf ==========================

void setup1() {
  setenv("TZ", TZ_INFO, 1);
  tzset();

  i2cBusRecover();
  Wire.setSDA(PIN_SDA);
  Wire.setSCL(PIN_SCL);
  Wire.setClock(I2C_CLOCK_HZ);
  // Core 0 hält diesen Kern bei jedem Flash-Schreibvorgang an (OTA-Download,
  // LittleFS). Ein Sektor-Löschen dauert bis zu einige 100 ms. Der Standard-
  // Timeout von 25 ms bricht dabei laufende Übertragungen ab und lässt den Bus
  // ohne Reset hängen -> eingefrorenes Display. Daher: länger warten und bei
  // einem Timeout den Bus zurücksetzen.
  Wire.setTimeout(1000, true);

  bool found3C, found3D;
  scanI2C(found3C, found3D);
  if (!found3C && found3D) {
    u8g2.setI2CAddress(0x3D << 1);
  }

  u8g2.setBusClock(I2C_CLOCK_HZ);
  u8g2.begin();
  u8g2.setContrast(160);
  u8g2.clearBuffer();
  startScreen(u8g2);
  u8g2.sendBuffer();

  bmeBegin(true);                                  // I2C gehört Core 1: Sensor und Display hier
  encBegin();
  delay(1500);                                     // Startbild stehen lassen
}

void loop1() {
  enum Screen : uint8_t { SCR_CLOCK, SCR_STATUS, SCR_UPDATE, SCR_CLIMATE, SCR_TULIP, SCR_NONE };
  static Screen   lastScreen   = SCR_NONE;
  static time_t   lastShown    = 0;
  static uint32_t lastFrameMs  = 0;
  static uint32_t lastSensorMs = 0;
  static uint32_t sensorTicks  = 0;
  static int      page         = 0;                // 0 = Uhr, 1 = Klima, 2 = Tulpe
  static Climate  climate      = {};

  if (g_displayHold) {                             // Neustart steht bevor: Bus frei lassen
    g_displayHeld = true;
    delay(50);
    return;
  }

  // Zwei Lesungen schließen einen halb aktualisierten 64-Bit-Zeitwert aus,
  // falls Core 0 genau jetzt per NTP stellt.
  time_t a   = time(nullptr);
  time_t now = time(nullptr);
  if (now < a || now - a > 1) {
    delay(2);
    return;
  }
  uint32_t ms = millis();

  // Drehgeber: eine Raste = eine Seite weiter/zurück, im Kreis
  int det = encTakeDetents();
  if (det != 0) {
    page = ((page + det) % PAGE_COUNT + PAGE_COUNT) % PAGE_COUNT;
  }

  // Sensor einmal pro Sekunde lesen; fehlt er, alle 5 s neu suchen
  if (ms - lastSensorMs >= 1000) {
    lastSensorMs = ms;
    sensorTicks++;
    if (g_bmeAddr == 0 && sensorTicks % 5 == 0) {
      climate.present = bmeBegin(false);
    }
    if (g_bmeAddr != 0) {
      bmeMeasure(climate);
    }
  }

  UiState s      = uiSnapshot();
  Screen  screen = (s.updState != UPD_NONE)              ? SCR_UPDATE
                 : (page == 1)                            ? SCR_CLIMATE
                 : (page == 2)                            ? SCR_TULIP
                 : (!s.fsOk || now < VALID_EPOCH)         ? SCR_STATUS
                                                          : SCR_CLOCK;
  bool changed = (screen != lastScreen);
  lastScreen = screen;

  if (screen == SCR_STATUS || screen == SCR_UPDATE) {
    if (changed || ms - lastFrameMs >= (screen == SCR_UPDATE ? 150u : 300u)) {
      lastFrameMs = ms;
      u8g2.clearBuffer();
      if (screen == SCR_UPDATE) {
        drawUpdateScreen(u8g2, s);
      } else {
        drawStatusScreen(u8g2, s, ms / 300, page);
      }
      u8g2.sendBuffer();
    }
  } else if (changed || now != lastShown) {        // Uhr, Klima, Tulpe: einmal pro Sekunde
    lastShown = now;
    u8g2.clearBuffer();
    if (screen == SCR_CLOCK) {
      struct tm t;
      localtime_r(&now, &t);
      drawClockScreen(u8g2, t, s, page);
    } else if (screen == SCR_CLIMATE) {
      drawClimateScreen(u8g2, climate, s, page);
    } else {
      drawTulipScreen(u8g2, page);
    }
    u8g2.sendBuffer();
  }

  delay(10);
}
