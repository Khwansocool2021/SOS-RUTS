#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <mbedtls/base64.h>
#include <time.h>
#include <sys/time.h>
#include <queue>
#include <vector>
#include <mutex>
#include <esp_task_wdt.h>

#define MY_NODE_ID "B"
#define WDT_TIMEOUT 8

#define SCK 5
#define MISO 19
#define MOSI 27
#define SS 18
#define RST 14
#define DIO0 26
#define BAND 923E6
#define BAT_ADC_PIN 35

#define OLED_SDA 21
#define OLED_SCL 22
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Preferences preferences;

const char* DEFAULT_LOCATION_B = "7.0090,100.4770";
String currentLocationB;

int hbCounterB = 0;
int relayedPacketsCount = 0;
int lastRssi = 0;
float lastSnr = 0.0;

int oledPage = 0;
unsigned long lastOledSwitch = 0;
unsigned long lastOledRender = 0;
bool oledReady = false;

std::queue<String> txQueue;
std::mutex txMutex;

unsigned long lastHeartbeat = 0;
unsigned long heartbeatInterval = 40000;

std::vector<String> seenMsgIDs;
bool isDuplicate(String msgId) {
  for (const auto& id : seenMsgIDs) {
    if (id == msgId) return true;
  }
  if (seenMsgIDs.size() >= 60) seenMsgIDs.erase(seenMsgIDs.begin());
  seenMsgIDs.push_back(msgId);
  return false;
}

std::vector<String> splitString(const String &str, char delim) {
  std::vector<String> tokens;
  int start = 0;
  int end = str.indexOf(delim);
  while (end != -1) {
    tokens.push_back(str.substring(start, end));
    start = end + 1;
    end = str.indexOf(delim, start);
  }
  tokens.push_back(str.substring(start));
  return tokens;
}

uint16_t calculateCRC16(const String &str) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < str.length(); i++) {
    crc ^= (uint8_t)str[i];
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;
      else crc >>= 1;
    }
  }
  return crc;
}

void setupWatchdog() {
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = WDT_TIMEOUT * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_reconfigure(&twdt_config);
  esp_task_wdt_add(NULL);
#else
  esp_task_wdt_init(WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);
#endif
}

void performReboot() {
  if (oledReady) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(15, 25);
    display.println("REBOOTING NODE B...");
    display.display();
  }
  LoRa.sleep();
  delay(200);
  esp_task_wdt_delete(NULL);
  esp_restart();
  while (true) { yield(); }
}

void loadLocationConfig() {
  preferences.begin("node_cfg", true);
  currentLocationB = preferences.getString("gps_loc", DEFAULT_LOCATION_B);
  preferences.end();
}

void configureLoRaRadio() {
  LoRa.setTxPower(20);            
  LoRa.setSpreadingFactor(9);     
  LoRa.setSignalBandwidth(125E3); 
  LoRa.setCodingRate4(5);         
  LoRa.setPreambleLength(12);     
  LoRa.setGain(0);                
}

void sendLoRaRaw(const String &packet) {
  uint16_t crc = calculateCRC16(packet);
  String finalPacket = packet + "|" + String(crc, HEX);
  
  LoRa.beginPacket();
  LoRa.print(finalPacket);
  LoRa.endPacket();
}

// ==================== [ปรับแต่งระบบวัดแบตเตอรี่: 100% = 3.90V ลดทีละ 5%] ====================
float getAccurateBatteryVoltage() {
  uint32_t mvSum = 0;
  for (int i = 0; i < 20; i++) {
    mvSum += analogReadMilliVolts(BAT_ADC_PIN);
    delayMicroseconds(100);
  }
  float mvAvg = mvSum / 20.0f;
  float volts = (mvAvg / 1000.0f) * 2.0f;

  if (volts > 4.25f) volts = 4.25f;
  if (volts < 2.50f) volts = 2.50f;
  return volts;
}

int calculateBatteryPercentage(float volts) {
  if (volts >= 3.90f) return 100;
  if (volts <= 3.00f) return 0;

  float rawPct = ((volts - 3.00f) / (3.90f - 3.00f)) * 100.0f;
  if (rawPct > 100.0f) rawPct = 100.0f;
  if (rawPct < 0.0f) rawPct = 0.0f;

  int steppedPct = ((int)(rawPct + 2.5f) / 5) * 5;
  if (steppedPct > 100) steppedPct = 100;
  if (steppedPct < 0) steppedPct = 0;
  return steppedPct;
}

void syncTimeFromEpoch(unsigned long epoch) {
  if (epoch > 1700000000) {
    struct timeval tv = { (time_t)epoch, 0 };
    settimeofday(&tv, NULL);
    setenv("TZ", "ICT-7", 1);
    tzset();
  }
}

String getFormattedTime() {
  time_t now; time(&now);
  struct tm timeinfo; localtime_r(&now, &timeinfo);
  if (timeinfo.tm_year < 120) return "00:00:00";
  char buf[20]; strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
  return String(buf);
}

String getFormattedDate() {
  time_t now; time(&now);
  struct tm timeinfo; localtime_r(&now, &timeinfo);
  if (timeinfo.tm_year < 120) return "--/--/----";
  char buf[20]; strftime(buf, sizeof(buf), "%d/%m/%Y", &timeinfo);
  return String(buf);
}

String getFormattedUptime() {
  unsigned long sec = millis() / 1000;
  int d = sec / 86400; int h = (sec % 86400) / 3600;
  int m = (sec % 3600) / 60; int s = sec % 60;
  char buf[30];
  if (d > 0) snprintf(buf, sizeof(buf), "%dd %02dh %02dm %02ds", d, h, m, s);
  else snprintf(buf, sizeof(buf), "%02dh %02dm %02ds", h, m, s);
  return String(buf);
}

void initOLED() {
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setTimeOut(1000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    oledReady = false;
  } else {
    oledReady = true;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 20);
    display.println("BOOTING NODE B...");
    display.display();
  }
}

void updateOLED() {
  if (!oledReady) return;
  display.clearDisplay();
  display.fillRect(0, 0, 128, 14, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(4, 3);
  display.print("NODE B: REPEATER");
  display.setTextColor(SSD1306_WHITE);
  display.drawFastHLine(0, 14, 128, SSD1306_WHITE);

  float vB = getAccurateBatteryVoltage();
  int pctB = calculateBatteryPercentage(vB);

  if (oledPage == 0) {
    display.setCursor(0, 17); display.print("DATE: "); display.print(getFormattedDate());
    display.setTextSize(2); display.setCursor(16, 27); display.print(getFormattedTime());
    display.drawFastHLine(0, 44, 128, SSD1306_WHITE);
    display.setTextSize(1); display.setCursor(0, 47); display.print("UP  : "); display.print(getFormattedUptime());
    display.setCursor(0, 56); display.printf("BAT : %3d%% (%.2fV)", pctB, vB);
  } else {
    display.setTextSize(1);
    display.setCursor(0, 17); display.print("ROLE: MESH REPEATER");
    display.setCursor(0, 27); display.printf("Relayed : %d pkts", relayedPacketsCount);
    display.setCursor(0, 37); display.printf("Last RSSI: %d dBm", lastRssi);
    display.drawFastHLine(0, 46, 128, SSD1306_WHITE);
    display.setCursor(0, 49); display.printf("BAT : %3d%% (%.2fV)", pctB, vB);
    display.setCursor(0, 57); display.print("MESH : ACTIVE");
  }
  display.display();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  setenv("TZ", "ICT-7", 1);
  tzset();

  setupWatchdog();
  loadLocationConfig();

  Serial.println("\n--- STARTING NODE B (MESH REPEATER NODE) ---");

  pinMode(BAT_ADC_PIN, INPUT);
  initOLED();
  delay(300);

  WiFi.mode(WIFI_OFF); // ปิด WiFi บน Node B เพื่อประหยัดพลังงานเสาทวนสัญญาณ

  SPI.begin(SCK, MISO, MOSI, SS);
  LoRa.setPins(SS, RST, DIO0);
  while (!LoRa.begin(BAND)) {
    Serial.println("LoRa Init Failed! Retrying...");
    delay(1000);
    yield();
  }

  configureLoRaRadio();
}

void loop() {
  esp_task_wdt_reset();

  if (millis() - lastOledSwitch > 20000) {
    lastOledSwitch = millis();
    oledPage = (oledPage + 1) % 2;
  }
  
  if (millis() - lastOledRender > 1000) {
    lastOledRender = millis();
    updateOLED();
  }

  // ส่ง LoRa ผ่าน Queue
  String packetToSend = "";
  {
    std::lock_guard<std::mutex> lock(txMutex);
    if (!txQueue.empty()) {
      packetToSend = txQueue.front();
      txQueue.pop();
    }
  }
  if (packetToSend.length() > 0) {
    sendLoRaRaw(packetToSend);
    delay(40);
  }

  // Heartbeat Node B
  if (millis() - lastHeartbeat > heartbeatInterval) {
    lastHeartbeat = millis();
    heartbeatInterval = 40000 + random(0, 5000);
    hbCounterB++;

    float vB = getAccurateBatteryVoltage();
    int pctB = calculateBatteryPercentage(vB);
    uint32_t freeHeapB = ESP.getFreeHeap() / 1024;
    uint32_t uptimeB = millis() / 1000;
    unsigned long currentEpoch = (unsigned long)time(NULL);

    String hbPacket = "HBB" + String(hbCounterB) + "|3|B|ALL|HB|Repeater|" + currentLocationB + "|" + String(pctB) + "|" + String(vB, 2) + "|" + String(freeHeapB) + "|" + String(uptimeB) + "|" + String(relayedPacketsCount) + "|" + String(currentEpoch);
    {
      std::lock_guard<std::mutex> lock(txMutex);
      txQueue.push(hbPacket);
    }
  }

  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String rawIncoming = "";
    while (LoRa.available()) rawIncoming += (char)LoRa.read();
    rawIncoming.trim();
    lastRssi = LoRa.packetRssi();
    lastSnr = LoRa.packetSnr();

    int pLast = rawIncoming.lastIndexOf('|');
    if (pLast == -1) return;
    String payload = rawIncoming.substring(0, pLast);
    uint16_t rxCrc = (uint16_t)strtol(rawIncoming.substring(pLast + 1).c_str(), NULL, 16);
    if (calculateCRC16(payload) != rxCrc) return;

    std::vector<String> tokens = splitString(payload, '|');
    if (tokens.size() < 5) return;

    String msgId      = tokens[0];
    int ttl           = tokens[1].toInt();
    String senderNode = tokens[2];
    String targetNode = tokens[3];
    String msgType    = tokens[4];

    if (isDuplicate(msgId)) return; // ป้องกันทวนแพ็กเกจซ้ำ

    // ซิงค์เวลาลงชิป RTC ถ้ามี Epoch ส่งมากับ Heartbeat
    if (msgType == "HB" && tokens.size() >= 13) {
      syncTimeFromEpoch(tokens[12].toInt());
    }

    // 1. [LOCAL PROCESSING] ถ้าส่งถึง Node B โดยตรง
    if (targetNode == MY_NODE_ID || targetNode == "ALL") {
      if (msgType == "CMD" && tokens.size() >= 6) {
        if (tokens[5] == "REBOOT") performReboot();
      }
    }

    // 2. [FULL MESH RELAY LOGIC] ทวนสัญญาณส่งต่อทันทีสำหรับทุกข้อความที่เป้าหมายไม่ใช่ B และ TTL > 1
    if (targetNode != MY_NODE_ID && ttl > 1) {
      ttl--;
      tokens[1] = String(ttl);
      String forwardedPayload = "";
      for (size_t i = 0; i < tokens.size(); i++) {
        forwardedPayload += tokens[i] + (i < tokens.size() - 1 ? "|" : "");
      }
      relayedPacketsCount++;
      delay(random(100, 300)); // สุ่มหน่วงเวลาป้องกันคลื่นชนกัน
      std::lock_guard<std::mutex> lock(txMutex);
      txQueue.push(forwardedPayload);
    }
  }
}
