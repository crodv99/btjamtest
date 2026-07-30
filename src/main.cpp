#include <SPI.h>
#include <RF24.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <vector>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_wifi.h"

// ---------------------------
// Pantalla OLED
// ---------------------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---------------------------
// Pines de la matriz 1x4
// ---------------------------
//#define ROW_PIN   32
#define BTN_UP    33
#define BTN_DOWN  26
#define BTN_ENTER 17   // S3
#define BTN_BACK  4    // S4

// ---------------------------
// Módulos NRF24 (HSPI y VSPI)
// ---------------------------
SPIClass *sp = nullptr;
SPIClass *hp = nullptr;
RF24 radio(16, 15, 16000000);   // HSPI: CE=16, CS=15
RF24 radio1(22, 21, 16000000);  // VSPI: CE=22, CS=21

// ---------------------------
// Variables del jammer
// ---------------------------
int ch = 45;
int ch1 = 45;
unsigned int flag = 0;
unsigned int flagv = 0;
bool jammerActive = false;

// ---------------------------
// Estados del menú
// ---------------------------
enum State { MENU_MAIN, SCAN_WIFI, SCAN_BT, JAMMER_MENU };
State currentState = MENU_MAIN;
int menuIndex = 0;
const char *menuItems[] = {"WiFi Scanner", "BT Scanner", "Jammer BT"};
int scrollOffset = 0;
const int debounceDelay = 200;

struct WifiResult { String ssid; int rssi; };
std::vector<WifiResult> wifiNetworks;
struct BtResult { String name; String address; int rssi; };
std::vector<BtResult> btDevices;

// FIX: puntero persistente al objeto de escaneo BLE.
// El stack BLE se inicializa UNA sola vez en setup() y nunca
// se desinicializa, porque el ciclo init/deinit del controlador
// BT no es fiable en ESP-IDF (errores 258/259).
BLEScan* pBLEScan = nullptr;

// ============ PROTOTIPOS ============
void initSP();
void initHP();
void startJammer();
void stopJammer();
void jammerLoop();
void scanWiFi();
void scanBluetooth();
void drawMainMenu();
void drawWiFiList();
void drawBTList();
void drawJammerMenu();
int readButtons();

void disableWiFi();
void enableWiFi();
void initBLEOnce();
// ====================================

void setup() {
  Serial.begin(115200);

  Wire.begin(25, 27);
  if(!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("Fallo OLED");
    while(1);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  //pinMode(ROW_PIN, OUTPUT);
  //digitalWrite(ROW_PIN, HIGH);
  pinMode(BTN_UP, INPUT_PULLDOWN);
  pinMode(BTN_DOWN, INPUT_PULLDOWN);
  pinMode(BTN_ENTER, INPUT_PULLDOWN);
  pinMode(BTN_BACK, INPUT_PULLDOWN);

  // FIX: inicializar BLE UNA sola vez, al arranque.
  // El jammer usa los NRF24 externos, no el radio interno,
  // así que no hay conflicto en dejar BLE siempre activo.
  initBLEOnce();

  // WiFi sí soporta ciclo init/deinit; lo dejamos apagado al inicio
  disableWiFi();

  initSP();
  initHP();

  drawMainMenu();
}

void loop() {
  int btn = readButtons();

  if (jammerActive) {
    if (btn == BTN_BACK) {
      stopJammer();
      jammerActive = false;
      drawJammerMenu();
    } else {
      jammerLoop();
    }
    return;
  }

  if (currentState == JAMMER_MENU) {
    if (btn == BTN_ENTER) {
      disableWiFi();   // solo WiFi; BLE no se toca
      startJammer();
      jammerActive = true;
      drawJammerMenu();
    } else if (btn == BTN_BACK) {
      currentState = MENU_MAIN;
      drawMainMenu();
    }
    delay(10);
    return;
  }

  switch (currentState) {
    case MENU_MAIN:
      if (btn == BTN_UP) {
        menuIndex = (menuIndex - 1 + 3) % 3;
        drawMainMenu();
      } else if (btn == BTN_DOWN) {
        menuIndex = (menuIndex + 1) % 3;
        drawMainMenu();
      } else if (btn == BTN_ENTER) {
        if (menuIndex == 0) {
          enableWiFi();
          scanWiFi();
          disableWiFi();
          currentState = SCAN_WIFI;
          scrollOffset = 0;
          drawWiFiList();
        } else if (menuIndex == 1) {
          // FIX: BLE ya está inicializado desde setup();
          // solo se escanea directamente, sin enable/disable.
          scanBluetooth();
          currentState = SCAN_BT;
          scrollOffset = 0;
          drawBTList();
        } else if (menuIndex == 2) {
          disableWiFi();
          currentState = JAMMER_MENU;
          drawJammerMenu();
        }
      }
      break;

    case SCAN_WIFI:
      if (btn == BTN_UP && scrollOffset > 0) {
        scrollOffset--;
        drawWiFiList();
      } else if (btn == BTN_DOWN && scrollOffset < (int)wifiNetworks.size() - 1) {
        scrollOffset++;
        drawWiFiList();
      } else if (btn == BTN_BACK) {
        currentState = MENU_MAIN;
        drawMainMenu();
      } else if (btn == BTN_ENTER) {
        enableWiFi();
        scanWiFi();
        disableWiFi();
        scrollOffset = 0;
        drawWiFiList();
      }
      break;

    case SCAN_BT:
      if (btn == BTN_UP && scrollOffset > 0) {
        scrollOffset--;
        drawBTList();
      } else if (btn == BTN_DOWN && scrollOffset < (int)btDevices.size() - 1) {
        scrollOffset++;
        drawBTList();
      } else if (btn == BTN_BACK) {
        currentState = MENU_MAIN;
        drawMainMenu();
      } else if (btn == BTN_ENTER) {
        scanBluetooth();
        scrollOffset = 0;
        drawBTList();
      }
      break;

    case JAMMER_MENU:
      break;
  }

  delay(10);
}

// ================== GESTIÓN DE RADIOS INTERNOS ==================

// FIX: BLE se inicializa una única vez y queda residente.
void initBLEOnce() {
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(false);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  Serial.println("BLE stack listo (residente)");
}

void disableWiFi() {
  esp_wifi_disconnect();
  esp_wifi_stop();
  esp_wifi_deinit();
}

void enableWiFi() {
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_start();
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  WiFi.mode(WIFI_STA);
  delay(100);
}
// ================================================================

int readButtons() {
  static unsigned long lastPress = 0;
  if (millis() - lastPress < debounceDelay) return -1;

  int btn = -1;
  if (digitalRead(BTN_UP) == HIGH)         btn = BTN_UP;
  else if (digitalRead(BTN_DOWN) == HIGH)  btn = BTN_DOWN;
  else if (digitalRead(BTN_ENTER) == HIGH) btn = BTN_ENTER;
  else if (digitalRead(BTN_BACK) == HIGH)  btn = BTN_BACK;

  if (btn != -1) lastPress = millis();
  return btn;
}

void initSP() {
  sp = new SPIClass(VSPI);
  sp->begin();
  if (radio1.begin(sp)) {
    Serial.println("SP (VSPI) OK");
    radio1.setAutoAck(false);
    radio1.stopListening();
    radio1.setRetries(0, 0);
    radio1.setPALevel(RF24_PA_MAX, true);
    radio1.setDataRate(RF24_2MBPS);
    radio1.setCRCLength(RF24_CRC_DISABLED);
    radio1.printPrettyDetails();
    radio1.startConstCarrier(RF24_PA_MAX, ch1);
  } else {
    Serial.println("SP (VSPI) error");
  }
}

void initHP() {
  hp = new SPIClass(HSPI);
  hp->begin();
  if (radio.begin(hp)) {
    Serial.println("HP (HSPI) OK");
    radio.setAutoAck(false);
    radio.stopListening();
    radio.setRetries(0, 0);
    radio.setPALevel(RF24_PA_MAX, true);
    radio.setDataRate(RF24_2MBPS);
    radio.setCRCLength(RF24_CRC_DISABLED);
    radio.printPrettyDetails();
    radio.startConstCarrier(RF24_PA_MAX, ch);
  } else {
    Serial.println("HP (HSPI) error");
  }
}

void startJammer() {
  radio.powerUp();
  radio1.powerUp();
  delay(5);

  radio.setPALevel(RF24_PA_MAX, true);
  radio.startConstCarrier(RF24_PA_MAX, ch);
  radio1.setPALevel(RF24_PA_MAX, true);
  radio1.startConstCarrier(RF24_PA_MAX, ch1);
}

void stopJammer() {
  radio.stopConstCarrier();
  radio1.stopConstCarrier();
  radio.powerDown();
  radio1.powerDown();
  radio.setPALevel(RF24_PA_MAX, true);
  radio1.setPALevel(RF24_PA_MAX, true);
}

void jammerLoop() {
  if (flag == 0) ch += 2; else ch -= 2;
  if (ch > 79 && flag == 0) flag = 1;
  else if (ch < 2 && flag == 1) flag = 0;

  if (flagv == 0) ch1 += 4; else ch1 -= 4;
  if (ch1 > 79 && flagv == 0) flagv = 1;
  else if (ch1 < 2 && flagv == 1) flagv = 0;

  radio.setChannel(ch);
  radio1.setChannel(ch1);
}

void scanWiFi() {
  wifiNetworks.clear();
  int n = WiFi.scanNetworks(false, true);
  for (int i = 0; i < n; i++) {
    WifiResult r;
    r.ssid = WiFi.SSID(i);
    r.rssi = WiFi.RSSI(i);
    wifiNetworks.push_back(r);
  }
  WiFi.scanDelete();
}

void scanBluetooth() {
  // FIX: reutilizar el puntero persistente del stack residente
  if (pBLEScan == nullptr) return;

  BLEScanResults results = pBLEScan->start(3, false);

  btDevices.clear();
  for (int i = 0; i < results.getCount(); i++) {
    BLEAdvertisedDevice dev = results.getDevice(i);
    BtResult r;
    r.name = dev.haveName() ? dev.getName().c_str() : "Sin nombre";
    r.address = dev.getAddress().toString().c_str();
    r.rssi = dev.getRSSI();
    btDevices.push_back(r);
  }
  pBLEScan->clearResults();
}

void drawMainMenu() {
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("== MENU PRINCIPAL ==");
  for (int i = 0; i < 3; i++) {
    if (i == menuIndex) display.print("> ");
    else display.print("  ");
    display.println(menuItems[i]);
  }
  display.display();
}

void drawWiFiList() {
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("Redes WiFi:");
  int maxLines = 5;
  int start = scrollOffset;
  for (int i = 0; i < maxLines; i++) {
    if (start + i >= (int)wifiNetworks.size()) break;
    WifiResult &r = wifiNetworks[start + i];
    display.print(r.ssid);
    display.print(" (");
    display.print(r.rssi);
    display.println(")");
  }
  if (wifiNetworks.empty()) display.println("No encontradas");
  display.println("S3 rescan | S4 back");
  display.display();
}

void drawBTList() {
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("Dispositivos BT:");
  int maxLines = 5;
  int start = scrollOffset;
  for (int i = 0; i < maxLines; i++) {
    if (start + i >= (int)btDevices.size()) break;
    BtResult &r = btDevices[start + i];
    display.print(r.name);
    display.print(" ");
    display.println(r.rssi);
  }
  if (btDevices.empty()) display.println("No encontrados");
  display.println("S3 rescan | S4 back");
  display.display();
}

void drawJammerMenu() {
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("   JAMMER BT");
  display.println("");
  display.print("Estado: ");
  display.println(jammerActive ? "ACTIVO" : "Inactivo");
  display.println("");
  display.println("S3: Activar");
  display.println("S4: Desactivar/Volver");
  display.display();
}