#include <Arduino.h>         // ไลบรารีพื้นฐาน Arduino ESP32[cite: 79]
#include <SPI.h>             // ไลบรารีบัส SPI สำหรับ LoRa[cite: 79]
#include <LoRa.h>            // ไลบรารีควบคุมชิป LoRa[cite: 79]
#include <Wire.h>            // ไลบรารี I2C สำหรับ OLED[cite: 79]
#include <Adafruit_GFX.h>    // ไลบรารีกราฟิก[cite: 79]
#include <Adafruit_SSD1306.h>// ไลบรารีจอ OLED[cite: 79]
#include <Preferences.h>     // ไลบรารีอ่าน/เขียน Flash Memory[cite: 79]
#include <time.h>            // ไลบรารีจัดการเวลา[cite: 79]
#include <sys/time.h>        // ไลบรารีซิงค์ RTC ระบบ[cite: 79]
#include <vector>            // โครงสร้างข้อมูล Vector[cite: 79]
#include <esp_task_wdt.h>    // ระบบป้องกันบอร์ดค้าง Watchdog[cite: 79]

#define WDT_TIMEOUT 8        // ตั้งเวลา Watchdog 8 วินาที[cite: 79]

// ขาเชื่อมต่อฮาร์ดแวร์ SPI LoRa[cite: 79]
#define SCK 5
#define MISO 19
#define MOSI 27
#define SS 18
#define RST 14
#define DIO0 26
#define BAND 923E6           // ความถี่ 923 MHz[cite: 79]
#define BAT_ADC_PIN 35       // ขาอ่านแบตเตอรี่[cite: 79]

// ขาเชื่อมต่อ OLED[cite: 79]
#define OLED_SDA 21
#define OLED_SCL 22
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1); // ประกาศจอ OLED[cite: 79]
Preferences preferences;     // วัตถุ Flash Memory[cite: 79]

const char* DEFAULT_LOCATION_B = "7.0050,100.4700"; // พิกัดละติจูด,ลองจิจูด จุดติดตั้ง Node B[cite: 79]
String currentLocationB;     // ตัวแปรเก็บพิกัด[cite: 79]

int hbCounterB = 0;          // นับจำนวนแพ็กเกจ Heartbeat[cite: 79]
int relayedCount = 0;        // ตัวนับจำนวนแพ็กเกจที่ได้รับการทวนสัญญาณส่งต่อสำเร็จ[cite: 79]
int lastRssi = 0;            // ค่า RSSI สัญญาณล่าสุด[cite: 79]
float lastSnr = 0.0;         // ค่า SNR สัญญาณล่าสุด[cite: 79]
unsigned long lastHeartbeat = 0; // เวลา Heartbeat ล่าสุด[cite: 79]
unsigned long heartbeatInterval = 40000; // ระยะเวลา Heartbeat Node B (~40 วินาที)[cite: 79]
bool oledReady = false;      // สถานะ OLED[cite: 79]

int oledPage = 0;            // หน้า OLED[cite: 80]
unsigned long lastOledSwitch = 0; // เวลาสลับหน้า OLED[cite: 80]
unsigned long lastOledRender = 0; // เวลาวาดหน้า OLED[cite: 80]

std::vector<String> seenMsgIDs; // รายการ ID แพ็กเกจที่ทวนสัญญาณไปแล้ว ป้องกันการทวนวนลูป (Infinite Loop)[cite: 80]
bool isDuplicate(String msgId) {
  for (const auto& id : seenMsgIDs) {
    if (id == msgId) return true; // เคยทวนแล้ว ข้ามแพ็กเกจนี้[cite: 80]
  }
  if (seenMsgIDs.size() >= 60) seenMsgIDs.erase(seenMsgIDs.begin()); // ลบข้อมูลเก่าเมื่อเกิน 60 รายการ[cite: 80]
  seenMsgIDs.push_back(msgId); // บันทึก ID ใหม่[cite: 80]
  return false;
}

// ฟังก์ชันตัดแบ่งสตริง[cite: 80]
std::vector<String> splitString(const String &str, char delim) {
  std::vector<String> tokens; int start = 0; int end = str.indexOf(delim);
  while (end != -1) {
    tokens.push_back(str.substring(start, end)); start = end + 1; end = str.indexOf(delim, start); //[cite: 80]
  }
  tokens.push_back(str.substring(start)); return tokens; //[cite: 80]
}

// คำนวณค่าตรวจสอบ CRC16[cite: 80]
uint16_t calculateCRC16(const String &str) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < str.length(); i++) {
    crc ^= (uint8_t)str[i];
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001; else crc >>= 1; //[cite: 80]
    }
  }
  return crc;
}

// ตั้งค่าระบบ Watchdog Timer[cite: 80]
void setupWatchdog() {
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = WDT_TIMEOUT * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_reconfigure(&twdt_config); esp_task_wdt_add(NULL); //[cite: 81]
#else
  esp_task_wdt_init(WDT_TIMEOUT, true); esp_task_wdt_add(NULL); //[cite: 81]
#endif
}

// สั่งรีสตาร์ทบอร์ด[cite: 81]
void performReboot() {
  if (oledReady) {
    display.clearDisplay(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
    display.setCursor(15, 25); display.println("REBOOTING NODE B..."); display.display(); //[cite: 81]
  }
  LoRa.sleep(); delay(200); esp_task_wdt_delete(NULL); esp_restart(); while (true) { yield(); } //[cite: 81]
}

// ฟังก์ชันซิงค์เวลาจากค่า Epoch Timestamp ที่ได้รับผ่านคลื่น LoRa ไร้สาย[cite: 81]
void syncTimeFromEpoch(unsigned long epoch) {
  if (epoch > 1700000000) {      // ตรวจสอบความถูกต้อง timestamp[cite: 81]
    struct timeval tv = { (time_t)epoch, 0 };
    settimeofday(&tv, NULL);     // ปรับแต่งตั้งค่าเวลาภายในชิป ESP32 ทันที[cite: 81]
    setenv("TZ", "ICT-7", 1);     // กำหนดเวลาไทย UTC+7[cite: 81]
    tzset();                     // บันทึกการตั้งค่า[cite: 81]
  }
}

// โหลดพิกัดเสา[cite: 81]
void loadLocationConfig() {
  preferences.begin("node_cfg", true);
  currentLocationB = preferences.getString("gps_loc", DEFAULT_LOCATION_B); //[cite: 81]
  preferences.end();
}

// บันทึกพิกัดเสา[cite: 81]
void saveLocationConfig(String newLoc) {
  newLoc.trim();
  if (newLoc.length() > 0) {
    currentLocationB = newLoc;
    preferences.begin("node_cfg", false); preferences.putString("gps_loc", newLoc); preferences.end(); //[cite: 82]
  }
}

// ปรับค่าพารามิเตอร์ส่งระยะไกล LoRa[cite: 82]
void configureLoRaRadio() {
  LoRa.setTxPower(20);            // 20 dBm กำลังส่งสูงสุด[cite: 82]
  LoRa.setSpreadingFactor(9);     // SF9[cite: 82]
  LoRa.setSignalBandwidth(125E3); // 125 kHz[cite: 82]
  LoRa.setCodingRate4(5);         // CR 4/5[cite: 82]
  LoRa.setPreambleLength(12);     // Preamble 12[cite: 82]
  LoRa.setGain(0);                // Auto Gain[cite: 82]
}

// ส่งข้อมูล LoRa พร้อม CRC16[cite: 82]
void sendLoRaRaw(const String &packet) {
  uint16_t crc = calculateCRC16(packet);
  String finalPacket = packet + "|" + String(crc, HEX);
  LoRa.beginPacket(); LoRa.print(finalPacket); LoRa.endPacket(); //[cite: 82]
}

// อ่านแรงดันแบตเตอรี่[cite: 82]
float getAccurateBatteryVoltage() {
  uint32_t rawSum = 0;
  for (int i = 0; i < 16; i++) { rawSum += analogRead(BAT_ADC_PIN); delayMicroseconds(100); } //[cite: 82]
  float rawAvg = rawSum / 16.0;
  float volts = (rawAvg / 4095.0) * 2.0 * 3.3 * 1.08;
  if (volts > 4.25) volts = 4.25; if (volts < 3.00) volts = 3.00;
  return volts; //[cite: 82]
}

// คำนวณ % แบตเตอรี่[cite: 82]
int calculateBatteryPercentage(float volts) {
  if (volts >= 4.15) return 100; if (volts >= 4.00) return 90;
  if (volts >= 3.85) return 75;  if (volts >= 3.75) return 50;
  if (volts >= 3.65) return 30;  if (volts >= 3.50) return 15;
  if (volts >= 3.30) return 5;   return 0; //[cite: 82, 83]
}

// ดึงเวลาปัจจุบัน[cite: 83]
String getFormattedTime() {
  time_t now; time(&now); struct tm timeinfo; localtime_r(&now, &timeinfo);
  if (timeinfo.tm_year < 120) return "00:00:00"; // คืนค่าเริ่มต้นถ้ายังไม่ได้รับการซิงค์เวลาผ่าน LoRa[cite: 83]
  char buf[20]; strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo); return String(buf); //[cite: 83]
}

// ดึงวันที่ปัจจุบัน[cite: 83]
String getFormattedDate() {
  time_t now; time(&now); struct tm timeinfo; localtime_r(&now, &timeinfo);
  if (timeinfo.tm_year < 120) return "--/--/----"; // คืนค่าเริ่มต้นถ้ายังไม่ได้รับการซิงค์เวลาผ่าน LoRa[cite: 83]
  char buf[20]; strftime(buf, sizeof(buf), "%d/%m/%Y", &timeinfo); return String(buf); //[cite: 83]
}

// ดึงเวลาเปิดบอร์ด[cite: 83]
String getFormattedUptime() {
  unsigned long sec = millis() / 1000; int d = sec / 86400; int h = (sec % 86400) / 3600;
  int m = (sec % 3600) / 60; int s = sec % 60; char buf[30];
  if (d > 0) snprintf(buf, sizeof(buf), "%dd %02dh %02dm %02ds", d, h, m, s);
  else snprintf(buf, sizeof(buf), "%02dh %02dm %02ds", h, m, s);
  return String(buf); //[cite: 83]
}

// วาดผลบนหน้าจอ OLED Node B[cite: 83]
void updateOLED() {
  if (!oledReady) return;
  display.clearDisplay(); display.fillRect(0, 0, 128, 14, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK); display.setTextSize(1); display.setCursor(4, 3);
  display.print(oledPage == 0 ? "NODE B: SYSTEM DASH" : "NODE B: MESH REPEAT"); //[cite: 84]
  display.setTextColor(SSD1306_WHITE); display.drawFastHLine(0, 14, 128, SSD1306_WHITE);

  float v = getAccurateBatteryVoltage();
  int pct = calculateBatteryPercentage(v);

  if (oledPage == 0) { // หน้าแสดงเวลาและซิงค์วันที่สดจาก LoRa[cite: 84]
    display.setCursor(0, 17); display.print("DATE: "); display.print(getFormattedDate()); //[cite: 84]
    display.setTextSize(2); display.setCursor(16, 27); display.print(getFormattedTime()); //[cite: 84]
    display.drawFastHLine(0, 44, 128, SSD1306_WHITE);
    display.setTextSize(1); display.setCursor(0, 47); display.print("UP  : "); display.print(getFormattedUptime()); //[cite: 84]
    display.setCursor(0, 56); display.printf("BAT : %3d%% (%.2fV)", pct, v); //[cite: 84]
  } else {             // หน้าแสดงสถานะการทวนสัญญาณ[cite: 84]
    display.setCursor(0, 18); display.printf("Relayed: %d pkts", relayedCount); // จำนวนแพ็กเกจที่ทวนไป[cite: 84]
    display.setCursor(0, 28); display.printf("RSSI   : %d dBm", lastRssi);        // ค่า RSSI[cite: 84]
    display.setCursor(0, 38); display.printf("Battery: %d%% (%.2fV)", pct, v);   // แบตเตอรี่[cite: 84]
    display.drawFastHLine(0, 48, 128, SSD1306_WHITE);
    display.setCursor(0, 52); display.print("Status : ACTIVE REPEAT"); // สถานะการทำงาน[cite: 84]
  }
  display.display();
}

// ส่วนเริ่มต้นระบบ Setup[cite: 84]
void setup() {
  Serial.begin(115200); delay(1000);
  setenv("TZ", "ICT-7", 1); tzset();

  setupWatchdog();      // เปิดใช้งาน WDT[cite: 84]
  loadLocationConfig(); // โหลดพิกัด[cite: 84]

  Serial.println("\n--- STARTING NODE B (REPEATER) ---");

  pinMode(BAT_ADC_PIN, INPUT);
  Wire.begin(OLED_SDA, OLED_SCL);
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    oledReady = true; updateOLED(); //[cite: 85]
  }

  SPI.begin(SCK, MISO, MOSI, SS);
  LoRa.setPins(SS, RST, DIO0);
  while (!LoRa.begin(BAND)) { // เปิดชิป LoRa 923MHz[cite: 85]
    Serial.println("LoRa Init Failed! Retrying..."); delay(1000); yield();
  }

  configureLoRaRadio(); // ตั้งค่ากำลังส่ง[cite: 85]
}

// ลูปการทำงานหลัก Loop[cite: 85]
void loop() {
  esp_task_wdt_reset(); // รีเซ็ต WDT[cite: 85]

  if (millis() - lastOledSwitch > 15000) { lastOledSwitch = millis(); oledPage = (oledPage + 1) % 2; } // สลับหน้า OLED[cite: 85]
  if (millis() - lastOledRender > 1000) { lastOledRender = millis(); updateOLED(); }                   // วาด OLED[cite: 85]

  // ส่ง Heartbeat Node B[cite: 85]
  if (millis() - lastHeartbeat > heartbeatInterval) {
    lastHeartbeat = millis(); heartbeatInterval = 40000 + random(0, 5000); hbCounterB++; //[cite: 85]

    float v = getAccurateBatteryVoltage(); int pct = calculateBatteryPercentage(v);
    uint32_t freeHeap = ESP.getFreeHeap(); uint32_t uptimeSec = millis() / 1000;
    unsigned long currentEpoch = (unsigned long)time(NULL); // เวลา Epoch ปัจจุบัน[cite: 85]

    // แนบค่าเวลา Epoch ออกไปพร้อมกับ Heartbeat Node B[cite: 85]
    String hbPacket = "HBB" + String(hbCounterB) + "|3|B|ALL|HB|Repeater|" + currentLocationB + "|" + String(pct) + "|" + String(v, 2) + "|" + String(freeHeap) + "|" + String(uptimeSec) + "|" + String(relayedCount) + "|" + String(currentEpoch); //[cite: 85]
    sendLoRaRaw(hbPacket); // ส่งทันที[cite: 86]
  }

  // ระบบดักรับข้อมูลจาก LoRa และทวนสัญญาณ (Mesh Relay)[cite: 86]
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String rawIncoming = "";
    while (LoRa.available()) rawIncoming += (char)LoRa.read();
    rawIncoming.trim();
    lastRssi = LoRa.packetRssi(); lastSnr = LoRa.packetSnr(); //[cite: 86]

    int pLast = rawIncoming.lastIndexOf('|');
    if (pLast == -1) return;
    String payload = rawIncoming.substring(0, pLast);
    uint16_t rxCrc = (uint16_t)strtol(rawIncoming.substring(pLast + 1).c_str(), NULL, 16);
    if (calculateCRC16(payload) != rxCrc) return; // ทิ้งแพ็กเกจถ้า CRC ผิดพลาด[cite: 86]

    std::vector<String> tokens = splitString(payload, '|');
    if (tokens.size() < 5) return;

    String msgId = tokens[0];
    int ttl = tokens[1].toInt();  // อ่านค่า Time-To-Live (จำนวนกระโดดที่เหลือ)[cite: 86]
    String targetNode = tokens[3];
    String msgType = tokens[4];

    // *** พระเอกหลักในการแก้ปัญหาเวลาไม่ตรง ***[cite: 86]
    // เมื่อพบแพ็กเกจประเภท Heartbeat ("HB") จาก Node A หรือ C ที่แนบ Timestamp มา[cite: 86]
    if (msgType == "HB" && tokens.size() >= 13) {
      unsigned long rxEpoch = strtoul(tokens[12].c_str(), NULL, 10); // ดึงค่า Unix Epoch[cite: 86]
      syncTimeFromEpoch(rxEpoch); // ซิงค์เวลาลงชิป Node B ทันที[cite: 86]
    }

    if (isDuplicate(msgId)) return; // ข้ามถ้าเคยทวนแพ็กเกจนี้ไปแล้ว ป้องกันลูป[cite: 86]

    // คำสั่งรีสตาร์ทบอร์ดระยะไกล[cite: 86]
    if (msgType == "CMD" && (targetNode == "B" || targetNode == "ALL")) {
      if (tokens.size() >= 6 && tokens[5] == "REBOOT") performReboot(); //[cite: 86]
    }

    // *** กลไก Mesh Repeater ทวนสัญญาณต่อเมื่อค่า TTL > 1 ***[cite: 86]
    if (ttl > 1) {
      delay(random(30, 70)); // หน่วงเวลาสุ่มสั้นๆ ป้องกันคลื่นชนกับเสาต้นทาง[cite: 86]
      
      String relayedPayload = tokens[0] + "|" + String(ttl - 1); // ลดค่า TTL ลง 1 เช่น จาก 3 เหลือ 2[cite: 86, 87]
      for (size_t i = 2; i < tokens.size(); i++) {
        relayedPayload += "|" + tokens[i];                   // ประกอบแพ็กเกจเดิม[cite: 87]
      }
      
      sendLoRaRaw(relayedPayload); // ทวนสัญญาณกระจายคลื่นออกไปทันที[cite: 87]
      relayedCount++;              // เพิ่มจำนวนแพ็กเกจที่ทวนสำเร็จ[cite: 87]
    }
  }
}
