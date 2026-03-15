/*
 * Interaktivni mapa CR s webovym portalem pro snadne nastaveni Wi-Fi site 
 * a zobrazeni ruznych dat - teplota, vlhkost, tlak, prasnost a meteoradar na mape.
 *
 * Po prvnim zapnuti ESP32 vytvori Access Point s SSID Laskakit-MapaCR, pripojte se na tuto WiFi a ve
 * vasem webovem prohlizeci zadejte IP adresu 192.168.4.1, tam nastavte SSID a heslo vasi domaci Wi-Fi site,
 * Po restartu a pripojeni do vasi Wi-Fi site si mapa nacte vybrana data a zobrazi. Upravu zobrazeni dat, jasu nebo
 * intervalu aktualizace muzete provest pres webovy prohlizec kam napisete IP adresu zarizeni.
 * Tu zjistite napriklad z vaseho routeru (jakou IP adresu router mape pridelil) nebo z Terminalu (Putty, YAT, Arduino)
 * rychlost 115 200 Bd
 *
 *  Knihovny:
 * https://arduinojson.org/
 * https://github.com/adafruit/Adafruit_NeoPixel
 *
 * Kompilace
 * Tools -> Board -> ESP32 Dev Module
 *
 * Hardware: https://www.laskakit.cz/laskakit-interaktivni-mapa-cr-ws2812b/
 *
 * Laskakit (2025)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <Update.h>
#include "version.h"
#include "html_templates.h"
#include "mapa_png.h"

#define LEDS_COUNT 72     // počet LED diod na mapě
#define LEDS_PIN 25       // pin pro připojení LED pásku
#define PRAHA_LED 26      // LED pro Prahu (LED č. 27, 0-indexed)

Preferences preferences;  // pro ukládání nastavení
WebServer server(80);    // webový server na portu 80

Adafruit_NeoPixel strip(LEDS_COUNT, LEDS_PIN, NEO_GRB + NEO_KHZ800); // LED pás konfigurace

// Klíče pro uložení konfigurace do Preferences
const char* KEY_SSID = "ssid";
const char* KEY_PASS = "password";
const char* KEY_PARAM = "param";
const char* KEY_BRIGHT = "brightness";
const char* KEY_UPDATE = "update";
const char* KEY_NIGHT_START = "nightStart";
const char* KEY_NIGHT_END = "nightEnd";
const char* KEY_NIGHT_BRIGHT = "nightBright";

// URL pro získání JSON dat
const char* JSON_URL_MAIN = "http://cdn.tmep.cz/app/export/okresy-cr-vse.json";
const char* JSON_URL_METEORADAR = "http://tmep.cz/app/export/okresy-srazky-laskakit.json";

// Proměnné pro uložené hodnoty
char storedSSID[32] = "";
char storedPassword[64] = "";
char selectedParameter[16] = "h1";  // výchozí parametr - teplota
int brightness = 50;                // jas LED
unsigned long timerDelay = 60000;  // interval aktualizace v ms
int nightStart = 22;               // hodina začátku nočního režimu
int nightEnd = 6;                  // hodina konce nočního režimu
int nightBrightness = 0;           // jas v nočním režimu (0=vypnuto)

float TMEPDistrictValues[77];       // pole hodnot pro jednotlivé okresy

// Mapování indexů okresů na pozice LED diod
int TMEPDistrictPosition[72] = {
  9, 11, 12, 8, 10, 13, 6, 15, 7, 5, 3, 14, 16, 67, 66, 4, 2, 24, 17, 1, 68, 18, 65, 64, 0,
  25, 76, 20, 69, 19, 27, 23, 73, 70, 21, 29, 28, 59, 22, 71, 61, 63, 30, 72, 31, 26, 48,
  46, 33, 39, 58, 49, 51, 47, 57, 40, 32, 35, 56, 38, 55, 34, 45, 41, 50, 36, 54, 52, 37,
  44, 53, 43
};

unsigned long lastTime = 0;  // čas poslední aktualizace
bool wifiConnected = false;  // připojeno k WiFi
bool firstTime = false;      // příznak pro první spuštění

bool errorState = false;     // chybový stav (WiFi/data)
unsigned long lastBlink = 0; // čas posledního bliknutí
bool blinkOn = false;        // stav blikání LED

// Struktura pro statistiky hodnot a odpovídající barvy LED
struct StatColor {
  float minValue;
  float avgValue;
  float maxValue;
  uint32_t minColor;
  uint32_t avgColor;
  uint32_t maxColor;
};

StatColor cachedStats = {0, 0, 0, 0, 0, 0};
bool statsValid = false;

void setupWiFi();
void startAP();
void fetchAndDisplayData();
void handleRoot();
void handleSave();
void handleNotFound();
void handleMapImage();
void handleApiProxy(const char* url);
void handleApiOkresy();
void handleApiSrazky();
void handleApiStats();
void handleApiConfig();
void handleApiInfo();
void handleWifiScan();
void registerRoutes();
void handleOtaUploadData();
void handleOtaUploadFinish();
bool isNightMode();

// Float verze Arduino map() pro přesné mapování desetinných hodnot
float mapFloat(float x, float in_min, float in_max, float out_min, float out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// Pomocná funkce mapující hodnotu na barvu dle odstínu (Hue) LED
int mapValueToHue(float val, float minVal, float maxVal) {
  if (maxVal == minVal) return 85;  // střední barva, když všechny hodnoty stejné
  float hue = mapFloat(val, minVal, maxVal, 170.0, 0.0);
  if (hue < 0.0) hue = 0.0;
  if (hue > 170.0) hue = 170.0;
  return (int)hue;
}

// Převod 32bitové barvy RGB na hex string pro HTML stylování
String colorToHex(uint32_t c) {
  uint8_t r = (c >> 16) & 0xFF;
  uint8_t g = (c >> 8) & 0xFF;
  uint8_t b = c & 0xFF;
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
  return String(buf);
}

// Vypočítá min, průměr a max hodnotu z pole JSON dat a příslušné barvy LED
StatColor calculateStatsAndColors(JsonArray data, const char* param) {
  float minV = 1e6;
  float maxV = -1e6;
  float sumV = 0;
  int count = 0;

  // Jeden průchod pro zjištění min, max a součtu
  for (JsonObject obj : data) {
    if (obj[param].is<float>()) {
      float val = obj[param];
      if (val < minV) minV = val;
      if (val > maxV) maxV = val;
      sumV += val;
      count++;
    }
  }
  float avgV = (count > 0) ? (sumV / count) : 0.0;

  // Převedení hodnot na barvu ve škále LED (HSV)
  uint32_t minCol = strip.ColorHSV(mapValueToHue(minV, minV, maxV) * 256);
  uint32_t avgCol = strip.ColorHSV(mapValueToHue(avgV, minV, maxV) * 256);
  uint32_t maxCol = strip.ColorHSV(mapValueToHue(maxV, minV, maxV) * 256);

  return StatColor{minV, avgV, maxV, minCol, avgCol, maxCol};
}

// Zjistí, zda je aktuálně noční režim na základě NTP času
bool isNightMode() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) return false;  // nemáme čas → není noční režim
  int hour = timeinfo.tm_hour;
  if (nightStart > nightEnd) {
    // přes půlnoc (např. 22-6)
    return (hour >= nightStart || hour < nightEnd);
  } else if (nightStart < nightEnd) {
    // v rámci dne (např. 13-15)
    return (hour >= nightStart && hour < nightEnd);
  }
  return false;  // start == end → noční režim vypnut
}

void setup() {
  Serial.begin(115200);
  delay(10);

  // Inicializace preferencí pro uložení nastavení
  preferences.begin("LaskaKit", false);

  // Načtení uložených hodnot z paměti
  preferences.getString(KEY_SSID, "").toCharArray(storedSSID, sizeof(storedSSID));
  preferences.getString(KEY_PASS, "").toCharArray(storedPassword, sizeof(storedPassword));
  String param = preferences.getString(KEY_PARAM, "h1");
  param.toCharArray(selectedParameter, sizeof(selectedParameter));
  brightness = preferences.getInt(KEY_BRIGHT, 50);
  timerDelay = preferences.getInt(KEY_UPDATE, 60) * 1000UL;
  nightStart = preferences.getInt(KEY_NIGHT_START, 22);
  nightEnd = preferences.getInt(KEY_NIGHT_END, 6);
  nightBrightness = preferences.getInt(KEY_NIGHT_BRIGHT, 0);

  // Inicializace LED pásku
  strip.begin();
  strip.setBrightness(brightness);
  strip.show();

  // Připojení k WiFi nebo spuštění AP podle uložených dat
  if (strlen(storedSSID) > 0) {
    setupWiFi();
  } else {
    startAP();
  }
}

void loop() {
  server.handleClient();

  // Blikání LED pro Prahu při chybovém stavu
  if (errorState && (millis() - lastBlink > 500)) {
    lastBlink = millis();
    blinkOn = !blinkOn;
    strip.setPixelColor(PRAHA_LED, blinkOn ? strip.Color(255, 0, 0) : 0);
    strip.show();
  }

  // Detekce výpadku WiFi a automatické znovupřipojení
  if (wifiConnected && WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi odpojeno, pokouším se znovu připojit...");
    wifiConnected = false;
    errorState = true;
    WiFi.reconnect();
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("Znovu připojeno! IP: %s\n", WiFi.localIP().toString().c_str());
      wifiConnected = true;
      errorState = false;
    } else {
      Serial.println("Reconnect selhal, zkusím příště.");
      lastTime = millis();
      return;
    }
  }

  // Pravidelné aktualizace dat a zobrazení
  if (wifiConnected) {
    bool night = isNightMode();

    if (night && nightBrightness == 0) {
      // Noční režim s úplným vypnutím LED
      if (firstTime) {
        // Pokud LEDs svítily, zhasnout
        strip.clear();
        strip.show();
        firstTime = false;  // resetujeme, aby se po skončení noci znovu načetla data
      }
      lastTime = millis();  // posouváme timer, aby se po probuzení hned aktualizovalo
    } else {
      // Nastavení jasu podle režimu
      int activeBrightness = (night && nightBrightness > 0) ? nightBrightness : brightness;
      strip.setBrightness(activeBrightness);
      strip.show();

      if ((millis() - lastTime) > timerDelay || !firstTime) {
        fetchAndDisplayData();
        lastTime = millis();
        firstTime = true;
      }
    }
  }
}

void registerRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/mapa.png", HTTP_GET, handleMapImage);
  server.on("/api/okresy", HTTP_GET, handleApiOkresy);
  server.on("/api/srazky", HTTP_GET, handleApiSrazky);
  server.on("/api/stats", HTTP_GET, handleApiStats);
  server.on("/api/config", HTTP_GET, handleApiConfig);
  server.on("/api/config", HTTP_POST, handleApiConfig);
  server.on("/api/info", HTTP_GET, handleApiInfo);
  server.on("/api/wifi-scan", HTTP_GET, handleWifiScan);
  server.on("/api/ota-update", HTTP_POST, handleOtaUploadFinish, handleOtaUploadData);
  server.onNotFound(handleNotFound);
  server.begin();
}

void handleMapImage() {
  server.send_P(200, "image/png", (const char*)MAPA_PNG, MAPA_PNG_LEN);
}

void handleApiProxy(const char* url) {
  WiFiClient client;
  HTTPClient http;
  http.begin(client, url);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    server.send(200, "application/json", payload);
  } else {
    server.send(502, "text/plain", "TMEP fetch failed");
  }
  http.end();
}

void handleApiOkresy() {
  handleApiProxy(JSON_URL_MAIN);
}

void handleApiSrazky() {
  handleApiProxy(JSON_URL_METEORADAR);
}

void handleApiStats() {
  String json = "{\"valid\":" + String(statsValid ? "true" : "false");
  if (statsValid) {
    json += ",\"min\":" + String(cachedStats.minValue, 1);
    json += ",\"avg\":" + String(cachedStats.avgValue, 1);
    json += ",\"max\":" + String(cachedStats.maxValue, 1);
  }
  json += "}";
  server.send(200, "application/json", json);
}

void handleApiConfig() {
  if (server.method() == HTTP_POST) {
    handleSave();
    return;
  }
  String json = "{\"ssid\":\"" + String(storedSSID) + "\"";
  json += ",\"param\":\"" + String(selectedParameter) + "\"";
  json += ",\"brightness\":" + String(brightness);
  json += ",\"update\":" + String(timerDelay / 1000);
  json += ",\"nightStart\":" + String(nightStart);
  json += ",\"nightEnd\":" + String(nightEnd);
  json += ",\"nightBright\":" + String(nightBrightness);
  json += "}";
  server.send(200, "application/json", json);
}

void handleWifiScan() {
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\""
          + ",\"rssi\":" + String(WiFi.RSSI(i))
          + ",\"ch\":" + String(WiFi.channel(i))
          + ",\"enc\":" + String(WiFi.encryptionType(i))
          + "}";
  }
  json += "]";
  WiFi.scanDelete();
  server.send(200, "application/json", json);
}

void handleApiInfo() {
  String ip = WiFi.getMode() == WIFI_AP ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  String mac = WiFi.macAddress();
  int rssi = WiFi.RSSI();
  uint32_t heap = ESP.getFreeHeap();
  unsigned long uptime = millis() / 1000;
  String json = "{\"ip\":\"" + ip + "\"";
  json += ",\"mac\":\"" + mac + "\"";
  json += ",\"rssi\":" + String(rssi);
  json += ",\"heap\":" + String(heap);
  json += ",\"uptime\":" + String(uptime);
  json += ",\"version\":\"" + String(FW_VERSION) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void setupWiFi() {
  Serial.printf("Připojuji se k WiFi: '%s'\n", storedSSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(storedSSID, storedPassword);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Připojeno! IP adresa: %s\n", WiFi.localIP().toString().c_str());
    wifiConnected = true;
    // NTP synchronizace času pro noční režim (timezone Praha CET/CEST)
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com");
    Serial.println("NTP čas synchronizován (CET/CEST)");
  } else {
    Serial.println("Nepodařilo se připojit! Spouštím AP...");
    wifiConnected = false;
    errorState = true;
    startAP();
    return;
  }

  registerRoutes();
}

void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("LaskaKit-MapaCR");
  Serial.print("IP adresa AP: ");
  Serial.println(WiFi.softAPIP());

  wifiConnected = false;
  registerRoutes();

  strip.clear();
  strip.show();
}

void handleRoot() {
  server.send_P(200, "text/html; charset=UTF-8", HTML_CONFIG);
}

void handleSave() {
  // Uložení nastavení z webového formuláře
  if (server.hasArg("ssid") && server.hasArg("param") && server.hasArg("brightness") && server.hasArg("update")) {
    String newSSID = server.arg("ssid");
    String newPassword = server.arg("password");
    String newParam = server.arg("param");
    int newBrightness = constrain(server.arg("brightness").toInt(), 0, 255);
    int newUpdate = constrain(server.arg("update").toInt(), 10, 3600);

    bool restartNeeded = false;

    // Restart je potřeba pokud se změní SSID nebo heslo WiFi
    if (newSSID != String(storedSSID)) {
      restartNeeded = true;
      preferences.putString(KEY_SSID, newSSID);
      newSSID.toCharArray(storedSSID, sizeof(storedSSID));
    }

    if (newPassword.length() > 0) {
      restartNeeded = true;
      preferences.putString(KEY_PASS, newPassword);
      newPassword.toCharArray(storedPassword, sizeof(storedPassword));
    }

    // Pokud se změní parametr, uloží se a aktualizují se LED ihned
    if (newParam != String(selectedParameter)) {
      preferences.putString(KEY_PARAM, newParam);
      newParam.toCharArray(selectedParameter, sizeof(selectedParameter));
      fetchAndDisplayData();
    }

    // Změna jasu LED bez restartu
    if (newBrightness != brightness) {
      preferences.putInt(KEY_BRIGHT, newBrightness);
      brightness = newBrightness;
      int activeBright = isNightMode() ? nightBrightness : brightness;
      strip.setBrightness(activeBright);
      strip.show();
    }

    // Změna intervalu aktualizace bez restartu
    if (newUpdate != (timerDelay / 1000)) {
      preferences.putInt(KEY_UPDATE, newUpdate);
      timerDelay = newUpdate * 1000UL;
    }

    // Noční režim – uložení nastavení bez restartu
    if (server.hasArg("nightStart")) {
      int ns = constrain(server.arg("nightStart").toInt(), 0, 23);
      if (ns != nightStart) {
        preferences.putInt(KEY_NIGHT_START, ns);
        nightStart = ns;
      }
    }
    if (server.hasArg("nightEnd")) {
      int ne = constrain(server.arg("nightEnd").toInt(), 0, 23);
      if (ne != nightEnd) {
        preferences.putInt(KEY_NIGHT_END, ne);
        nightEnd = ne;
      }
    }
    if (server.hasArg("nightBright")) {
      int nb = constrain(server.arg("nightBright").toInt(), 0, 255);
      if (nb != nightBrightness) {
        preferences.putInt(KEY_NIGHT_BRIGHT, nb);
        nightBrightness = nb;
      }
    }

    // Odeslání JSON odpovědi, pokud je potřeba restart, za 2 sekundy restart
    if (restartNeeded) {
      server.send(200, "application/json", "{\"restart\":true}");
      delay(2000);
      ESP.restart();
    } else {
      server.send(200, "application/json", "{\"restart\":false}");
    }
  } else {
    server.send(400, "text/plain; charset=UTF-8", "Chybějící parametry.");
  }
}

void handleNotFound() {
  // Vypsání chyby při požadavku na neznámou stránku
  server.send(404, "text/plain; charset=UTF-8", "Stránka nenalezena");
}

void handleOtaUploadData() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("OTA: zahájení '%s'\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("OTA: úspěch, %u B\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
}

void handleOtaUploadFinish() {
  bool ok = !Update.hasError();
  server.send(200, "application/json",
    ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Update failed\"}");
  if (ok) {
    delay(1000);
    ESP.restart();
  }
}

void fetchAndDisplayData() {
  String currentURL = JSON_URL_MAIN;
  if (strcmp(selectedParameter, "meteoradar") == 0) {
    currentURL = JSON_URL_METEORADAR;
  }

  WiFiClient client;
  HTTPClient http;
  http.begin(client, currentURL);
  int httpResponseCode = http.GET();

  if (httpResponseCode != 200) {
    Serial.printf("HTTP GET selhal, kód: %d\n", httpResponseCode);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.print(F("Chyba parsování JSON: "));
    Serial.println(error.f_str());
    return;
  }

  if (strcmp(selectedParameter, "meteoradar") == 0) {
    Serial.println("Zobrazuji meteoradar");
    strip.clear();
    statsValid = false;
    JsonArray mesta = doc["seznam"].as<JsonArray>();
    for (JsonObject mesto : mesta) {
      int id = mesto["id"];
      if (id < 0 || id >= LEDS_COUNT) continue;
      int r = mesto["r"];
      int g = mesto["g"];
      int b = mesto["b"];

#ifdef jednoducheZobrazeni
      if ((r > g) && (r > b))
        strip.setPixelColor(id, strip.Color(r, 0, 0));
      else if ((g > r) && (g > b))
        strip.setPixelColor(id, strip.Color(0, g, 0));
      else if ((b > r) && (b > g))
        strip.setPixelColor(id, strip.Color(0, 0, b));
#else
      strip.setPixelColor(id, strip.Color(r, g, b));
#endif
    }
    strip.show();
  } else {
    JsonArray arr = doc.as<JsonArray>();
    float maxValue = -999999;
    float minValue = 999999;

    for (JsonObject item : arr) {
      int districtIndex = item["id"].as<int>() - 1;
      if (districtIndex < 0 || districtIndex >= 77) continue;
      if (item[selectedParameter].is<float>()) {
        float val = item[selectedParameter];
        TMEPDistrictValues[districtIndex] = val;
        if (val < minValue) minValue = val;
        if (val > maxValue) maxValue = val;
      } else {
        TMEPDistrictValues[districtIndex] = 0.0;
      }
    }

    for (int LED = 0; LED < LEDS_COUNT; LED++) {
      int districtIdx = TMEPDistrictPosition[LED];
      float val = TMEPDistrictValues[districtIdx];
      int hue = mapValueToHue(val, minValue, maxValue);
      strip.setPixelColor(LED, strip.ColorHSV(hue * 256));
    }
    strip.show();

    // Cachování statistik pro zobrazení na webové stránce
    cachedStats = calculateStatsAndColors(arr, selectedParameter);
    statsValid = true;
  }
}