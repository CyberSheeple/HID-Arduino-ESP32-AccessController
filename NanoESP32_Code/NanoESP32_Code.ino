/*
  HID RFID Door Simulator (Arduino Nano ESP32)

  Same silkscreen wiring as the classic Nano sketch, with the ESP32
  Wi-Fi, web console, and REST API. GPIO is 3.3 V only. Power the
  SSD1306 from the board 3.3 V pin so A4/A5 are not pulled up to 5 V.

  Pins (Arduino names; GPIO in parentheses):
    Wiegand D0 (green)  D2 (GPIO 5)
    Wiegand D1 (white)  D3 (GPIO 6)
    Beeper (yellow)     A3 (GPIO 4), idle high
    Door strike         D7 (GPIO 10)
    OLED SDA / SCL      A4 / A5 (GPIO 11 / GPIO 12)

  Cards, the admin UID, the scan log, and saved Wi-Fi settings live on
  the factory FAT partition (ffat at 0x610000). The web page is compiled
  in from data/index.html, because DFU upload only replaces the app.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <FFat.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wiegand.h>
#include "index_html.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

#define WIEGAND_D0_PIN D2
#define WIEGAND_D1_PIN D3
#define BEEPER_PIN A3
#define SIGNAL_PIN D7

// --- Wi-Fi and Static IP Configuration ---
// Put your SSID in here below.
String userSSID = "YourSSID";
// Put your password for your SSID below.
String userPassword = "YourWifiAddress";
bool useHardcodedIP = true; // Set to true to use static IP below
IPAddress hardcodedIP(192, 168, 1, 100);
IPAddress hardcodedGW(192, 168, 1, 1);
IPAddress hardcodedSN(255, 255, 255, 0);

const char* ap_ssid = "RFID-Door-Sim";
const char* ap_password = "12345678";

WebServer server(80);
WIEGAND wg;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define MAX_CARDS 20
const char* cardsFile = "/cards.txt";
const char* adminFile = "/admin.txt";
const char* logFile = "/scanlog.txt";
const char* wifiFile = "/wifi.txt";

unsigned long authorizedCards[MAX_CARDS];
uint8_t cardCount = 0;

#define MODE_READ 1
#define MODE_DOOR 2
#define MODE_ADD 3
#define MODE_REMOVE 4

uint8_t currentMode = MODE_READ;
unsigned long lastCardData = 0;
uint8_t lastBitLength = 0;
uint8_t lastFacilityCode = 0;
uint16_t lastCardNumber = 0;
unsigned long adminUID = 0;

#define MODE_CARD_READ 0x1E6DC032
#define MODE_CARD_DOOR 0x1E6D9861
#define MODE_CARD_ADD 0x1E6DFCA0
#define MODE_CARD_REMOVE 0x1E6DE636

const unsigned long configCards[4] = {
  MODE_CARD_READ,
  MODE_CARD_DOOR,
  MODE_CARD_ADD,
  MODE_CARD_REMOVE
};

unsigned long parseCard(String line) {
  return strtoul(line.c_str(), NULL, 10);
}

void beep(unsigned long ms) {
  digitalWrite(BEEPER_PIN, LOW);
  delay(ms);
  digitalWrite(BEEPER_PIN, HIGH);
}

void loadAuthorizedCards() {
  cardCount = 0;
  if (!FFat.exists(cardsFile)) return;

  File file = FFat.open(cardsFile, "r");
  if (!file) return;
  while (file.available() && cardCount < MAX_CARDS) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    authorizedCards[cardCount++] = parseCard(line);
  }
  file.close();
}

void saveAuthorizedCards() {
  File file = FFat.open(cardsFile, "w");
  if (!file) return;
  for (uint8_t i = 0; i < cardCount; i++) {
    file.println(String(authorizedCards[i]));
  }
  file.close();
}

bool isAuthorized(unsigned long cardData) {
  for (uint8_t i = 0; i < cardCount; i++) {
    if (authorizedCards[i] == cardData) return true;
  }
  return false;
}

void addCard(unsigned long cardData) {
  if (cardCount < MAX_CARDS && !isAuthorized(cardData)) {
    authorizedCards[cardCount++] = cardData;
    saveAuthorizedCards();
  }
}

void removeCard(unsigned long cardData) {
  for (uint8_t i = 0; i < cardCount; i++) {
    if (authorizedCards[i] == cardData) {
      for (uint8_t j = i; j < cardCount - 1; j++) {
        authorizedCards[j] = authorizedCards[j + 1];
      }
      cardCount--;
      saveAuthorizedCards();
      break;
    }
  }
}

String modeName(uint8_t mode) {
  switch (mode) {
    case MODE_READ: return "READ MODE";
    case MODE_DOOR: return "DOOR MODE";
    case MODE_ADD: return "ADD MODE";
    case MODE_REMOVE: return "DEL MODE";
    default: return "UNKNOWN";
  }
}

String getCardJSON() {
  String json = "{";
  json += "\"mode\":\"" + modeName(currentMode) + "\",";
  json += "\"card\":\"0x" + String(lastCardData, HEX) + "\",";
  json += "\"fc\":\"" + String(lastFacilityCode) + "\",";
  json += "\"id\":\"" + String(lastCardNumber) + "\"";
  json += "}";
  return json;
}

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  server.send(200, "application/json", getCardJSON());
}

void displayScanPrompt() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.println(modeName(currentMode));
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.println("Scan Card to Begin...");
  display.display();
}

void saveAdminUID(unsigned long uid) {
  File f = FFat.open(adminFile, "w");
  if (!f) return;
  f.println(String(uid));
  f.close();
  adminUID = uid;
}

unsigned long loadAdminUID() {
  if (!FFat.exists(adminFile)) return 0;
  File f = FFat.open(adminFile, "r");
  if (!f) return 0;
  String line = f.readStringUntil('\n');
  f.close();
  line.trim();
  if (line.length() == 0) return 0;
  return strtoul(line.c_str(), NULL, 10);
}

void displayEnrollPrompt() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("ADMIN ENROLL");
  display.setCursor(0, 20);
  display.println("Scan Admin Card...");
  display.display();
}

void enrollAdminIfNeeded() {
  if (adminUID != 0UL) return;
  displayEnrollPrompt();
  while (adminUID == 0UL) {
    if (!wg.available()) {
      delay(10);
      continue;
    }
    unsigned long cardData = wg.getCode();
    saveAdminUID(cardData);
    beep(200);
    display.clearDisplay();
    display.setTextSize(2);
    display.setCursor(8, 0);
    display.println("ADMIN");
    display.setTextSize(1);
    display.setCursor(0, 24);
    display.println("Admin Card Saved");
    display.setCursor(0, 40);
    display.print("UID: 0x");
    display.println(adminUID, HEX);
    display.display();
    delay(1500);
    break;
  }
}

void handleSetMode() {
  if (server.hasArg("admin")) {
    unsigned long adminCard = strtoul(server.arg("admin").c_str(), NULL, 10);
    if (adminUID != 0UL && adminCard == adminUID) {
      currentMode++;
      if (currentMode > MODE_REMOVE) currentMode = MODE_READ;
      displayScanPrompt();
      server.send(200, "text/plain", "Admin card cycled mode.");
      return;
    }
    File f = FFat.open(adminFile, "w");
    if (f) {
      f.println(String(adminCard));
      f.close();
      adminUID = adminCard;
      server.send(200, "text/plain", "Admin card saved.");
    } else {
      server.send(500, "text/plain", "Failed to save admin card.");
    }
    return;
  }

  if (server.hasArg("m")) {
    uint8_t mode = server.arg("m").toInt();
    if (mode >= MODE_READ && mode <= MODE_REMOVE) {
      currentMode = mode;
    }
  } else if (server.hasArg("scan")) {
    unsigned long scannedCard = strtoul(server.arg("scan").c_str(), NULL, 16);
    for (uint8_t i = 0; i < 4; i++) {
      if (scannedCard == configCards[i]) {
        currentMode = i + 1;
        break;
      }
    }
  }
  displayScanPrompt();
  server.send(200, "text/plain", "Mode Set");
}

void pulseDoor() {
  digitalWrite(SIGNAL_PIN, HIGH);
  delay(5000);
  digitalWrite(SIGNAL_PIN, LOW);
}

void handleTriggerDoor() {
  pulseDoor();
  server.send(200, "text/plain", "Triggered");
}

void handleApiStatus() {
  server.send(200, "application/json", getCardJSON());
}

void handleApiCardsGet() {
  String json = "[";
  for (uint8_t i = 0; i < cardCount; i++) {
    json += String(authorizedCards[i]);
    if (i < cardCount - 1) json += ",";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleApiCardsPost() {
  if (!server.hasArg("card")) {
    server.send(400, "application/json", "{\"error\":\"Missing card\"}");
    return;
  }
  unsigned long cardData = strtoul(server.arg("card").c_str(), NULL, 10);
  addCard(cardData);
  server.send(200, "application/json", "{\"success\":true}");
}

void handleApiCardsDelete() {
  if (!server.hasArg("card")) {
    server.send(400, "application/json", "{\"error\":\"Missing card\"}");
    return;
  }
  unsigned long cardData = strtoul(server.arg("card").c_str(), NULL, 10);
  removeCard(cardData);
  server.send(200, "application/json", "{\"success\":true}");
}

void handleApiTrigger() {
  pulseDoor();
  server.send(200, "application/json", "{\"success\":true}");
}

void handleApiModePost() {
  if (!server.hasArg("mode")) {
    server.send(400, "application/json", "{\"error\":\"Missing mode\"}");
    return;
  }
  uint8_t mode = server.arg("mode").toInt();
  if (mode >= MODE_READ && mode <= MODE_REMOVE) {
    currentMode = mode;
    displayScanPrompt();
    server.send(200, "application/json", "{\"success\":true}");
  } else {
    server.send(400, "application/json", "{\"error\":\"Invalid mode\"}");
  }
}

String getTimestamp() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char buf[64];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
           t->tm_hour, t->tm_min, t->tm_sec);
  return String(buf);
}

void appendLogEntry(unsigned long raw, uint8_t fc, uint16_t id, String type) {
  File log = FFat.open(logFile, "a");
  if (log) {
    log.printf("%s | RAW: %lu | FC: %u | ID: %u | TYPE: %s\n",
               getTimestamp().c_str(), raw, fc, id, type.c_str());
    log.close();
  }
}

void handleLogFile() {
  File log = FFat.open(logFile, "r");
  if (!log) {
    server.send(200, "text/plain", "Log file not found.");
    return;
  }
  server.streamFile(log, "text/plain");
  log.close();
}

void handleApiLogGet() {
  File log = FFat.open(logFile, "r");
  if (!log) {
    server.send(200, "application/json", "{\"error\":\"Log file not found.\"}");
    return;
  }
  String logData;
  while (log.available()) {
    logData += log.readStringUntil('\n') + "\\n";
  }
  log.close();
  server.send(200, "text/plain", logData);
}

void handleClearLog() {
  FFat.remove(logFile);
  server.send(200, "text/plain", "Log cleared.");
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/mode", handleSetMode);
  server.on("/trigger", handleTriggerDoor);
  server.on("/log", handleLogFile);
  server.on("/clearlog", HTTP_POST, handleClearLog);
  server.on("/wifi", HTTP_POST, []() {
    if (server.hasArg("ssid") && server.hasArg("pass")) {
      String newSSID = server.arg("ssid");
      String newPASS = server.arg("pass");
      String newIP = server.hasArg("ip") ? server.arg("ip") : "";
      String newGW = server.hasArg("gw") ? server.arg("gw") : "";
      String newSN = server.hasArg("subnet") ? server.arg("subnet") : "";

      File config = FFat.open(wifiFile, "w");
      if (!config) {
        server.send(500, "text/plain", "Failed to save Wi-Fi settings.");
        return;
      }
      config.println(newSSID);
      config.println(newPASS);
      config.println(newIP);
      config.println(newGW);
      config.println(newSN);
      config.close();

      server.send(200, "text/plain", "Wi-Fi settings saved. Please reboot.");
    } else {
      server.send(400, "text/plain", "Missing SSID or Password");
    }
  });

  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/cards", HTTP_GET, handleApiCardsGet);
  server.on("/api/cards", HTTP_POST, handleApiCardsPost);
  server.on("/api/cards", HTTP_DELETE, handleApiCardsDelete);
  server.on("/api/trigger", HTTP_POST, handleApiTrigger);
  server.on("/api/mode", HTTP_POST, handleApiModePost);
  server.on("/api/log", HTTP_GET, handleApiLogGet);

  server.begin();
}

void connectWiFi() {
  WiFi.mode(WIFI_AP_STA);
  String ssidVal = userSSID;
  String passVal = userPassword;
  IPAddress local_IP, gateway, subnet;

  if (useHardcodedIP) {
    WiFi.config(hardcodedIP, hardcodedGW, hardcodedSN);
  } else if (FFat.exists(wifiFile)) {
    File f = FFat.open(wifiFile, "r");
    if (f) {
      ssidVal = f.readStringUntil('\n');
      passVal = f.readStringUntil('\n');
      String ip = f.readStringUntil('\n');
      String gw = f.readStringUntil('\n');
      String sn = f.readStringUntil('\n');
      ssidVal.trim(); passVal.trim();
      ip.trim(); gw.trim(); sn.trim();
      f.close();

      if (ip.length() && gw.length() && sn.length()) {
        if (local_IP.fromString(ip) && gateway.fromString(gw) && subnet.fromString(sn)) {
          WiFi.config(local_IP, gateway, subnet);
        }
      }
    }
  }

  WiFi.begin(ssidVal.c_str(), passVal.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected to WiFi");
    Serial.println(WiFi.localIP());
  } else {
    WiFi.softAP(ap_ssid, ap_password);
    Serial.println("Started Access Point");
    Serial.println(WiFi.softAPIP());
  }
}

void displayFatError() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("FAT MOUNT FAILED");
  display.setCursor(0, 16);
  display.println("Factory ffat partition");
  display.setCursor(0, 28);
  display.println("did not mount.");
  display.display();
}

void setup() {
  Serial.begin(115200);
  Wire.begin(SDA, SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("OLED failed");
    while (true) delay(1000);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (!FFat.begin(true)) {
    Serial.println("FAT mount failed");
    displayFatError();
    while (true) delay(1000);
  }

  pinMode(BEEPER_PIN, OUTPUT);
  digitalWrite(BEEPER_PIN, HIGH);
  pinMode(SIGNAL_PIN, OUTPUT);
  digitalWrite(SIGNAL_PIN, LOW);

  wg.begin(WIEGAND_D0_PIN, WIEGAND_D1_PIN);
  loadAuthorizedCards();
  adminUID = loadAdminUID();
  enrollAdminIfNeeded();
  displayScanPrompt();

  connectWiFi();
  configTime(0, 0, "pool.ntp.org");
  setupWebServer();
}

void loop() {
  server.handleClient();

  if (!wg.available()) return;

  unsigned long cardData = wg.getCode();
  if (adminUID != 0UL && cardData == adminUID) {
    currentMode++;
    if (currentMode > MODE_REMOVE) currentMode = MODE_READ;
    displayScanPrompt();
    beep(100);
    Serial.print("Admin card scanned. Mode -> ");
    Serial.println(modeName(currentMode));
    return;
  }

  for (uint8_t i = 0; i < 4; i++) {
    if (cardData == configCards[i]) {
      currentMode = i + 1;
      displayScanPrompt();
      beep(100);
      Serial.print("Config card set mode -> ");
      Serial.println(modeName(currentMode));
      return;
    }
  }

  lastCardData = cardData;
  lastBitLength = 0;
  unsigned long data = lastCardData;
  while (data) {
    lastBitLength++;
    data >>= 1;
  }
  lastFacilityCode = (lastCardData >> 16) & 0xFF;
  lastCardNumber = lastCardData & 0xFFFF;

  beep(50);

  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.println(modeName(currentMode));

  display.setTextSize(1);
  display.setCursor(0, 20);
  display.print("Card: 0x");
  display.println(lastCardData, HEX);

  display.setCursor(0, 30);
  display.print("Facility: ");
  display.println(lastFacilityCode);

  display.setCursor(0, 40);
  display.print("ID: ");
  display.println(lastCardNumber);

  String cardType = (wg.getWiegandType() == 26) ? "Wiegand26" : "Unknown";

  switch (currentMode) {
    case MODE_READ:
      Serial.printf("Card: 0x%08lX\n", lastCardData);
      break;

    case MODE_DOOR: {
      bool authorized = isAuthorized(lastCardData);
      display.setTextSize(2);
      display.setCursor(0, 50);
      if (authorized) {
        display.println("AUTHORIZED");
        display.display();
        pulseDoor();
      } else {
        display.println("NO ENTRY");
        display.display();
      }
      appendLogEntry(lastCardData, lastFacilityCode, lastCardNumber, cardType);
      break;
    }

    case MODE_ADD:
      addCard(lastCardData);
      break;
    case MODE_REMOVE:
      removeCard(lastCardData);
      break;
  }

  if (currentMode != MODE_DOOR) {
    display.display();
    appendLogEntry(lastCardData, lastFacilityCode, lastCardNumber, cardType);
  }
}
