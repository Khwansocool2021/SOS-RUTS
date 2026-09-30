#include <Arduino.h>            // ไลบรารีพื้นฐานของ Arduino สำหรับ ESP32
#include <WiFi.h>               // ไลบรารีจัดการระบบ WiFi Access Point
#include <SPI.h>                // ไลบรารีสื่อสารบัส SPI สำหรับโมดูล LoRa
#include <LoRa.h>               // ไลบรารีควบคุมชิปวิทยุ LoRa (SX1276/SX1278)
#include <ESPAsyncWebServer.h>    // ไลบรารี Async Web Server ไม่บล็อกการทำงานหลัก
#include <DNSServer.h>          // ไลบรารี Captive Portal DNS Server
#include <qrcode.h>             // ไลบรารีสร้างภาพ QR Code เวกเตอร์ SVG สดบนบอร์ด
#include <Wire.h>               // ไลบรารีสื่อสารบัส I2C สำหรับหน้าจอ OLED
#include <Adafruit_GFX.h>       // ไลบรารีกราฟิกพื้นฐาน Adafruit
#include <Adafruit_SSD1306.h>   // ไลบรารีควบคุมจอ OLED SSD1306
#include <LittleFS.h>           // ไลบรารีระบบไฟล์ LittleFS บันทึกข้อมูลเคสถาวรลง Flash Memory
#include <Preferences.h>        // ไลบรารีบันทึกค่าคอนฟิก NVS
#include <mbedtls/base64.h>     // ไลบรารีเข้ารหัส/ถอดรหัส Base64 ภาษาไทย
#include <time.h>               // ไลบรารีจัดการเวลา C Standard
#include <sys/time.h>           // ไลบรารีตั้งค่าเวลา RTC ภายในชิป ESP32
#include <queue>                // ไลบรารีคิว (Queue)
#include <vector>               // ไลบรารีเว็กเตอร์ (Vector)
#include <mutex>                // ไลบรารีป้องกันการแย่งใช้ข้อมูลระหว่าง Dual Core (Mutex)
#include <esp_task_wdt.h>       // ไลบรารีระบบรีเซ็ตบอร์ดอัตโนมัติเมื่อค้าง (Watchdog Timer)

#define WDT_TIMEOUT 8           // ตั้งเวลา Watchdog ไว้ที่ 8 วินาที

// กำหนดพินขาเชื่อมต่อฮาร์ดแวร์ SPI ของ LoRa
#define SCK 5                   // ขา SPI Clock
#define MISO 19                 // ขา SPI MISO
#define MOSI 27                 // ขา SPI MOSI
#define SS 18                   // ขา SPI Chip Select
#define RST 14                  // ขา LoRa Reset
#define DIO0 26                 // ขา LoRa DIO0 Interrupt
#define BAND 923E6              // ความถี่วิทยุ LoRa (923 MHz)
#define BAT_ADC_PIN 35          // ขา Analog อ่านแรงดันแบตเตอรี่

// กำหนดพินขาเชื่อมต่อ OLED I2C
#define OLED_SDA 21             // ขา I2C Data
#define OLED_SCL 22             // ขา I2C Clock
#define SCREEN_WIDTH 128        // ความกว้างจอ OLED (พิกเซล)
#define SCREEN_HEIGHT 64        // ความสูงจอ OLED (พิกเซล)

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1); // วัตถุควบคุม OLED
Preferences preferences;        // วัตถุ Flash Memory NVS

const char* DEFAULT_LOCATION_C = "7.0100,100.4800"; // พิกัดละติจูด,ลองจิจูด เริ่มต้นของศูนย์กู้ภัย Node C
String currentLocationC;        // ตัวแปรเก็บพิกัดปัจจุบัน

AsyncWebServer server(80);      // Async Web Server พอร์ต 80
DNSServer dnsServer;            // DNS Server สำหรับ Captive Portal
const byte DNS_PORT = 53;       // พอร์ต DNS

int replyCounter = 800;         // ตัวนับรหัสข้อความตอบกลับ (Reply ID)
int ackCounter = 500;           // ตัวนับรหัสแพ็กเกจยืนยัน (ACK ID)
int hbCounterC = 0;             // ตัวนับแพ็กเกจ Heartbeat
int lastRssi = 0;               // ค่าความแรงสัญญาณ RSSI ล่าสุด
float lastSnr = 0.0;            // ค่าคุณภาพสัญญาณ SNR ล่าสุด

int oledPage = 0;               // หน้าจอ OLED ปัจจุบัน (0=Dash, 1=Control AP)
unsigned long lastOledSwitch = 0; // เวลาสลับหน้า OLED ล่าสุด
unsigned long lastOledRender = 0; // เวลาวาดหน้า OLED ล่าสุด
bool oledReady = false;         // ความพร้อมจอ OLED

std::queue<String> txQueue;     // คิวเก็บแพ็กเกจ LoRa รอส่งออก
std::mutex txMutex;             // ตัวล็อคป้องกัน Race Condition ของ txQueue

unsigned long lastHeartbeat = 0; // เวลาส่ง Heartbeat ล่าสุด
unsigned long heartbeatInterval = 45000; // ระยะเวลาส่ง Heartbeat (~45 วินาที)

std::vector<String> seenMsgIDs; // บันทึก ID แพ็กเกจที่ประมวลผลแล้ว กันซ้ำ
bool isDuplicate(String msgId) {
  for (const auto& id : seenMsgIDs) {
    if (id == msgId) return true; // ถ้าเคยพบ ถือว่าซ้ำ
  }
  if (seenMsgIDs.size() >= 60) seenMsgIDs.erase(seenMsgIDs.begin()); // ลบข้อมูลเก่าสุด
  seenMsgIDs.push_back(msgId);
  return false;
}

// โครงสร้างประวัติข้อความแชท
struct ChatMessage {
  String uid;                   // UID ผู้ประสบภัย
  String user;                  // ชื่อผู้ประสบภัย
  String loc;                   // พิกัดสถานที่
  String text;                  // เนื้อหาข้อความ
  String sender;                // ผู้ส่ง (victim / officer)
};

// โครงสร้างเคสผู้ประสบภัย
struct VictimCase {
  String uid;                   // UID
  String user;                  // ชื่อ
  String loc;                   // พิกัด
  String assignedOfficer;       // เจ้าหน้าที่ผู้รับผิดชอบเคส
  String status;                // สถานะ (pending / claimed / resolved)
  String lastMsg;               // ข้อความล่าสุด
  String lastMsgId;             // ID ข้อความล่าสุด
};

// โครงสร้างสถานะเสาในเครือข่าย (Network Telemetry)
struct NetworkNodeInfo {
  String id;                    // ID เสา (A, B, C)
  String type;                  // ประเภทเสา
  String loc;                   // พิกัด
  int batPct;                   // % แบตเตอรี่
  float batVolt;                // แรงดันแบตเตอรี่ (V)
  uint32_t freeHeapKB;          // RAM เหลือ (KB)
  uint32_t uptimeSec;           // เวลาทำงานต่อเนื่อง (วินาที)
  String extraData;             // ข้อมูลเพิ่มเติม
  int rssi;                     // RSSI
  float snr;                    // SNR
  unsigned long lastSeen;       // เวลาที่เจอครั้งล่าสุด (ms)
};

#define MAX_MSGS 60
ChatMessage chatHistory[MAX_MSGS]; // อาร์เรย์เก็บประวัติข้อความแชท 60 ข้อความล่าสุด
int msgCount = 0;

std::vector<VictimCase> activeCases; // รายการเคสทั้งหมด
std::mutex caseMutex;                // ตัวล็อคป้องกันเคสชนกัน

std::vector<NetworkNodeInfo> networkNodes; // รายการเสาในเครือข่าย Telemetry
std::mutex nodeMutex;                      // ตัวล็อคเสาในเครือข่าย

// ฟังก์ชันตัดแบ่งสตริงด้วยตัวคั่น
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

// เข้ารหัสข้อความภาษาไทยเป็น Base64
String encodeBase64(const String &input) {
  unsigned char out[350];
  size_t olen = 0;
  mbedtls_base64_encode(out, sizeof(out), &olen, (const unsigned char*)input.c_str(), input.length());
  out[olen] = '\0';
  return String((char*)out);
}

// ถอดรหัส Base64 เป็นข้อความภาษาไทย
String decodeBase64(const String &input) {
  unsigned char out[350];
  size_t olen = 0;
  mbedtls_base64_decode(out, sizeof(out), &olen, (const unsigned char*)input.c_str(), input.length());
  out[olen] = '\0';
  return String((char*)out);
}

// คำนวณรหัสตรวจสอบความถูกต้องข้อมูล CRC16
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

// ตั้งค่า Watchdog Timer ป้องกันระบบค้าง
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

// สั่งรีสตาร์ทบอร์ด Node C
void performReboot() {
  if (oledReady) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(15, 25);
    display.println("REBOOTING NODE C...");
    display.display();
  }
  LoRa.sleep();
  delay(200);
  esp_task_wdt_delete(NULL);
  esp_restart();
  while (true) { yield(); }
}

// โหลดพิกัดเสาจาก Flash Memory
void loadLocationConfig() {
  preferences.begin("node_cfg", true);
  currentLocationC = preferences.getString("gps_loc", DEFAULT_LOCATION_C);
  preferences.end();
}

// บันทึกพิกัดเสาลง Flash Memory
void saveLocationConfig(String newLoc) {
  newLoc.trim();
  if (newLoc.length() > 0) {
    currentLocationC = newLoc;
    preferences.begin("node_cfg", false);
    preferences.putString("gps_loc", newLoc);
    preferences.end();
  }
}

// ตั้งค่าปรับแต่งวิทยุ LoRa ระยะไกล
void configureLoRaRadio() {
  LoRa.setTxPower(20);            // กำลังส่งสูงสุด 20 dBm (100mW)
  LoRa.setSpreadingFactor(9);     // SF9 เพิ่ม Receiver Sensitivity ถึง -130 dBm
  LoRa.setSignalBandwidth(125E3); // 125 kHz
  LoRa.setCodingRate4(5);         // CR 4/5
  LoRa.setPreambleLength(12);     // Preamble 12 ไบต์
  LoRa.setGain(0);                // Auto LNA Gain
}

// ส่งข้อความ LoRa พร้อมแนบค่า CRC16
void sendLoRaRaw(const String &packet) {
  uint16_t crc = calculateCRC16(packet);
  String finalPacket = packet + "|" + String(crc, HEX);
  
  LoRa.beginPacket();
  LoRa.print(finalPacket);
  LoRa.endPacket();
}

// อ่านแรงดันแบตเตอรี่แบบเฉลี่ยแม่นยำ
float getAccurateBatteryVoltage() {
  uint32_t rawSum = 0;
  for (int i = 0; i < 16; i++) {
    rawSum += analogRead(BAT_ADC_PIN);
    delayMicroseconds(100);
  }
  float rawAvg = rawSum / 16.0;
  float volts = (rawAvg / 4095.0) * 2.0 * 3.3 * 1.08;
  if (volts > 4.25) volts = 4.25;
  if (volts < 3.00) volts = 3.00;
  return volts;
}

// คำนวณเปอร์เซ็นต์แบตเตอรี่
int calculateBatteryPercentage(float volts) {
  if (volts >= 4.15) return 100;
  if (volts >= 4.00) return 90;
  if (volts >= 3.85) return 75;
  if (volts >= 3.75) return 50;
  if (volts >= 3.65) return 30;
  if (volts >= 3.50) return 15;
  if (volts >= 3.30) return 5;
  return 0;
}

// บันทึกรายการเคสฉุกเฉินลงไฟล์ ถาวรใน LittleFS
void saveCasesToFS() {
  File f = LittleFS.open("/cases.dat", "w");
  if (!f) return;
  std::lock_guard<std::mutex> lock(caseMutex);
  for (const auto &c : activeCases) {
    f.println(c.uid + "|" + c.user + "|" + c.loc + "|" + c.assignedOfficer + "|" + c.status + "|" + c.lastMsgId + "|" + c.lastMsg);
  }
  f.close();
}

// โหลดรายการเคสถาวรจากระบบไฟล์ LittleFS
void loadCasesFromFS() {
  if (!LittleFS.begin(true)) return;
  File f = LittleFS.open("/cases.dat", "r");
  if (!f) return;
  std::lock_guard<std::mutex> lock(caseMutex);
  activeCases.clear();
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    std::vector<String> tok = splitString(line, '|');
    if (tok.size() >= 7) {
      activeCases.push_back({tok[0], tok[1], tok[2], tok[3], tok[4], tok[6], tok[5]});
    }
  }
  f.close();
}

// ซิงค์เวลาลงชิป RTC ภายใน ESP32 จาก Epoch
void syncTimeFromEpoch(unsigned long epoch) {
  if (epoch > 1700000000) {
    struct timeval tv = { (time_t)epoch, 0 };
    settimeofday(&tv, NULL);
    setenv("TZ", "ICT-7", 1);
    tzset();
  }
}

// ดึงเวลาปัจจุบัน HH:MM:SS
String getFormattedTime() {
  time_t now; time(&now);
  struct tm timeinfo; localtime_r(&now, &timeinfo);
  if (timeinfo.tm_year < 120) return "00:00:00";
  char buf[20]; strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
  return String(buf);
}

// ดึงวันที่ปัจจุบัน DD/MM/YYYY
String getFormattedDate() {
  time_t now; time(&now);
  struct tm timeinfo; localtime_r(&now, &timeinfo);
  if (timeinfo.tm_year < 120) return "--/--/----";
  char buf[20]; strftime(buf, sizeof(buf), "%d/%m/%Y", &timeinfo);
  return String(buf);
}

// ดึงเวลาเปิดบอร์ด (Uptime)
String getFormattedUptime() {
  unsigned long sec = millis() / 1000;
  int d = sec / 86400;
  int h = (sec % 86400) / 3600;
  int m = (sec % 3600) / 60;
  int s = sec % 60;
  char buf[30];
  if (d > 0) snprintf(buf, sizeof(buf), "%dd %02dh %02dm %02ds", d, h, m, s);
  else snprintf(buf, sizeof(buf), "%02dh %02dm %02ds", h, m, s);
  return String(buf);
}

// เริ่มระบบหน้าจอ OLED
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
    display.println("BOOTING NODE C...");
    display.display();
  }
}

// อัปเดตหรือเพิ่มข้อมูลเสาในรายการ Telemetry Network
void updateOrAddNode(String id, String type, String loc, int batPct, float batVolt, uint32_t freeHeapKB, uint32_t uptimeSec, String extraData, int rssi, float snr) {
  std::lock_guard<std::mutex> lock(nodeMutex);
  bool found = false;
  for (auto &n : networkNodes) {
    if (n.id == id) {
      n.type = type;
      n.loc = loc;
      n.batPct = batPct;
      n.batVolt = batVolt;
      n.freeHeapKB = freeHeapKB;
      n.uptimeSec = uptimeSec;
      n.extraData = extraData;
      n.rssi = rssi;
      n.snr = snr;
      n.lastSeen = millis();
      found = true;
      break;
    }
  }
  if (!found) networkNodes.push_back({id, type, loc, batPct, batVolt, freeHeapKB, uptimeSec, extraData, rssi, snr, millis()});
}

// เพิ่มหรืออัปเดตเคสขอความช่วยเหลือ
void addOrUpdateCase(String uid, String user, String loc, String text, String msgId) {
  std::lock_guard<std::mutex> lock(caseMutex);
  bool found = false;
  for (auto &c : activeCases) {
    if (c.uid == uid) {
      c.user = user;
      c.loc = loc;
      c.lastMsg = text;
      c.lastMsgId = msgId;
      found = true;
      break;
    }
  }
  if (!found) {
    activeCases.push_back({uid, user, loc, "", "pending", text, msgId});
  }
}

// บันทึกข้อความแชท
void addMessage(String uid, String user, String loc, String text, String sender) {
  if (msgCount >= MAX_MSGS) {
    for (int i = 0; i < MAX_MSGS - 1; i++) chatHistory[i] = chatHistory[i + 1];
    msgCount = MAX_MSGS - 1;
  }
  chatHistory[msgCount] = {uid, user, loc, text, sender};
  msgCount++;
}

// อัปเดตหน้าจอ OLED
void updateOLED() {
  if (!oledReady) return;
  display.clearDisplay();
  display.fillRect(0, 0, 128, 14, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(4, 3);
  display.print(oledPage == 0 ? "NODE C: DISPATCH HQ" : "NODE C: CONTROL AP");
  display.setTextColor(SSD1306_WHITE);
  display.drawFastHLine(0, 14, 128, SSD1306_WHITE);

  float vC = getAccurateBatteryVoltage();
  int pctC = calculateBatteryPercentage(vC);

  if (oledPage == 0) {
    display.setCursor(0, 17); display.print("DATE: "); display.print(getFormattedDate());
    display.setTextSize(2); display.setCursor(16, 27); display.print(getFormattedTime());
    display.drawFastHLine(0, 44, 128, SSD1306_WHITE);
    display.setTextSize(1); display.setCursor(0, 47); display.print("UP  : "); display.print(getFormattedUptime());
    display.setCursor(0, 56); display.printf("BAT : %d%% | Cases: %d", pctC, (int)activeCases.size());
  } else {
    display.setTextSize(1);
    display.setCursor(0, 17); display.print("SSID: Emergency_Rescue");
    display.setCursor(0, 27); display.print("IP  : 192.168.4.1");
    display.setCursor(0, 37); display.printf("Logs: %d msgs | %ddBm", msgCount, lastRssi);
    display.drawFastHLine(0, 46, 128, SSD1306_WHITE);
    display.setCursor(0, 49); display.printf("BAT : %3d%% (%.2fV)", pctC, vC);
    display.setCursor(0, 57); display.print("System: ONLINE 24/7");
  }
  display.display();
}

// ซอร์สโค้ดหน้าเว็บครบถ้วนของศูนย์กู้ภัย (HTML/CSS/JS)
const char rescuer_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="th">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ศูนย์วิทยุกู้ภัย (Node C Dispatch Center)</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
        body { background: #0f172a; color: #f8fafc; height: 100vh; display: flex; flex-direction: column; }
        .header { background: #1e293b; padding: 10px 16px; border-bottom: 1px solid #334155; display: flex; justify-content: space-between; align-items: center; }
        .header h1 { font-size: 1.1rem; color: #ef4444; display: flex; align-items: center; gap: 8px; }
        .header-actions { display: flex; align-items: center; gap: 10px; }
        .officer-box { display: flex; align-items: center; gap: 8px; font-size: 0.82rem; }
        .officer-box input { padding: 6px 10px; border-radius: 6px; border: 1px solid #334155; background: #0f172a; color: #38bdf8; font-weight: bold; outline: none; }
        .btn-set-loc { background: #0284c7; color: white; border: none; padding: 6px 10px; border-radius: 6px; font-size: 0.75rem; font-weight: bold; cursor: pointer; }
        .telemetry-bar { background: #0b1120; padding: 8px 16px; border-bottom: 1px solid #334155; display: flex; gap: 10px; overflow-x: auto; align-items: center; }
        .node-card { background: #182234; border: 1px solid #243352; padding: 8px 12px; border-radius: 8px; font-size: 0.75rem; display: flex; align-items: center; gap: 12px; white-space: nowrap; flex-shrink: 0; cursor: pointer; transition: 0.2s; }
        .node-card:hover { background: #22314d; border-color: #38bdf8; }
        .node-card.online { border-color: #059669; }
        .node-card.offline { border-color: #475569; opacity: 0.6; }
        .node-title { font-weight: bold; color: #f8fafc; font-size: 0.82rem; }
        .node-stat { color: #38bdf8; font-weight: 600; }
        .main-container { flex: 1; display: flex; overflow: hidden; }
        .sidebar { width: 320px; background: #182234; border-right: 1px solid #334155; display: flex; flex-direction: column; }
        .sidebar-title { padding: 10px 14px; background: #0f172a; font-size: 0.78rem; font-weight: 700; color: #94a3b8; border-bottom: 1px solid #334155; display: flex; justify-content: space-between; align-items: center; }
        .user-list { flex: 1; overflow-y: auto; }
        .user-item { padding: 12px 14px; border-bottom: 1px solid #243352; cursor: pointer; transition: 0.2s; }
        .user-item:hover { background: #1e293b; }
        .user-item.active { background: #1e293b; border-left: 4px solid #38bdf8; }
        .user-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 4px; }
        .user-name { font-weight: 700; font-size: 0.88rem; }
        .user-sub { font-size: 0.72rem; color: #94a3b8; word-break: break-all; }
        .badge { font-size: 0.65rem; font-weight: bold; padding: 2px 6px; border-radius: 4px; display: inline-block; }
        .badge-pending { background: #7f1d1d; color: #fca5a5; border: 1px solid #ef4444; }
        .badge-claimed { background: #854d0e; color: #fef08a; border: 1px solid #eab308; }
        .badge-resolved { background: #065f46; color: #6ee7b7; border: 1px solid #10b981; }
        .chat-area { flex: 1; display: flex; flex-direction: column; background: #0b0f19; }
        .chat-header { padding: 12px 20px; background: #1e293b; border-bottom: 1px solid #334155; display: flex; justify-content: space-between; align-items: center; }
        .chat-logs { flex: 1; padding: 20px; overflow-y: auto; display: flex; flex-direction: column; gap: 12px; }
        .msg-bubble { max-width: 75%; padding: 10px 14px; border-radius: 12px; font-size: 0.9rem; line-height: 1.4; }
        .msg-victim { background: #1e293b; color: white; align-self: flex-start; border: 1px solid #334155; }
        .msg-officer { background: #2563eb; color: white; align-self: flex-end; }
        .claim-banner { padding: 10px 16px; background: #182234; border-bottom: 1px solid #334155; display: flex; justify-content: space-between; align-items: center; font-size: 0.82rem; }
        .btn-claim { background: #eab308; color: #000; border: none; padding: 6px 14px; border-radius: 6px; font-weight: bold; cursor: pointer; }
        .btn-resolve { background: #10b981; color: #fff; border: none; padding: 6px 14px; border-radius: 6px; font-weight: bold; cursor: pointer; }
        .btn-qr { background: #0284c7; color: #fff; border: none; padding: 6px 12px; border-radius: 6px; font-weight: bold; cursor: pointer; margin-left: 8px; }
        .reply-bar { padding: 14px; background: #1e293b; border-top: 1px solid #334155; display: flex; gap: 10px; }
        .reply-bar input { flex: 1; padding: 10px 14px; border-radius: 8px; border: 1px solid #334155; background: #0f172a; color: white; outline: none; }
        .reply-bar button { padding: 10px 20px; background: #ef4444; color: white; border: none; border-radius: 8px; font-weight: bold; cursor: pointer; }
        .modal { display: none; position: fixed; z-index: 100; left: 0; top: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.85); justify-content: center; align-items: center; }
        .modal-content { background: #1e293b; padding: 24px; border-radius: 16px; border: 1px solid #38bdf8; text-align: center; max-width: 440px; width: 92%; }
        .modal-content h3 { color: #38bdf8; margin-bottom: 8px; }
        .modal-content p { font-size: 0.82rem; color: #94a3b8; margin-bottom: 16px; }
        .qr-wrapper { background: white; padding: 12px; border-radius: 12px; display: inline-block; margin: 0 auto 16px auto; }
        .qr-wrapper img { display: block; width: 200px; height: 200px; }
        .node-detail-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; text-align: left; font-size: 0.8rem; margin: 14px 0; background: #0f172a; padding: 14px; border-radius: 10px; border: 1px solid #334155; }
        .node-detail-item { font-size: 0.78rem; color: #cbd5e1; }
        .node-detail-item span { color: #38bdf8; font-weight: bold; }
        .btn-close { background: #ef4444; color: white; border: none; padding: 8px 20px; border-radius: 8px; font-weight: bold; cursor: pointer; }
        .btn-maps { background: #059669; color: white; text-decoration: none; padding: 4px 8px; border-radius: 4px; font-size: 0.7rem; font-weight: bold; display: inline-block; margin-left: 6px; }
    </style>
</head>
<body>
<div class="header">
    <h1>🚨 ศูนย์รับแจ้งเหตุฉุกเฉิน (Node C Dispatch Center)</h1>
    <div class="header-actions">
        <button class="btn-set-loc" onclick="openNodeCLocModal()">📍 ตั้งค่าพิกัด Node C</button>
        <div class="officer-box">
            <span>👤 ประจำเครื่อง:</span>
            <input type="text" id="officerName" placeholder="ระบุชื่อของคุณ" onchange="saveOfficerName(this.value)">
        </div>
    </div>
</div>
<div class="telemetry-bar" id="telemetryBar">
    <div style="font-size:0.75rem; color:#94a3b8; font-weight:bold;">📡 สถานะเสาในเครือข่าย:</div>
    <div style="font-size:0.72rem; color:#64748b;">กำลังรับข้อมูล Telemetry สด...</div>
</div>
<div class="main-container">
    <div class="sidebar">
        <div class="sidebar-title">
            <span>รายการเคสทั้งหมด</span>
            <span id="caseCount" style="color:#38bdf8;">0 เคส</span>
        </div>
        <div class="user-list" id="userList"><div style="padding:16px; text-align:center; color:#64748b; font-size:0.8rem;">ยังไม่มีเคสขอความช่วยเหลือเข้ามา</div></div>
    </div>
    <div class="chat-area">
        <div class="chat-header" id="chatHeader">
            <div>
                <span style="font-weight:bold; color:#ef4444; font-size:1rem;" id="currentUserName">กรุณาเลือกเคสฝั่งซ้าย</span>
                <span id="currentLoc" style="font-size:0.78rem; margin-left:10px; color:#94a3b8;"></span>
            </div>
            <button id="btnShowQrHeader" class="btn-qr" style="display:none;" onclick="openQrModal()">📱 สแกนพิกัดลงมือถือ</button>
        </div>
        <div class="claim-banner" id="claimBanner" style="display:none;">
            <span id="claimStatusText">สถานะ: รอรับเรื่อง</span>
            <div id="claimActionBtns"></div>
        </div>
        <div class="chat-logs" id="chatLogs"><div style="text-align:center; color:#475569; margin-top:40px;">เลือกรายชื่อฝั่งซ้ายเพื่อดูประวัติและรับเรื่องดูแลเคส</div></div>
        <div class="reply-bar">
            <input type="text" id="replyInput" placeholder="พิมพ์ข้อความตอบกลับผู้ประสบภัย..." oninput="checkByteLimit(this)">
            <button onclick="sendReply()">ส่งข้อความ</button>
        </div>
    </div>
</div>

<div class="modal" id="nodeDetailModal">
    <div class="modal-content">
        <h3 id="popNodeTitle">📡 รายละเอียดเชิงลึกอุปกรณ์</h3>
        <div class="node-detail-grid" id="popNodeDetails"></div>
        <div style="display:flex; justify-content:space-between; gap:8px;">
            <button class="btn-close" style="background:#dc2626; flex:1;" id="popBtnReboot" onclick="">🔄 รีสตาร์ทเสานี้</button>
            <button class="btn-close" style="flex:1;" onclick="closeNodeDetailModal()">ปิดหน้าต่าง</button>
        </div>
    </div>
</div>

<div class="modal" id="qrModal">
    <div class="modal-content">
        <h3>📱 สแกนนำทางด้วยมือถือ</h3>
        <p>ใช้มือถือที่มีเน็ตสแกน QR Code นี้เพื่อเปิด Google Maps นำทางได้ทันที</p>
        <div class="qr-wrapper"><img id="qrImg" src="" alt="QR Code"></div>
        <div style="font-weight:bold; color:#38bdf8; margin-bottom:12px; font-size:0.9rem;" id="modalLocText">พิกัด: --</div>
        <button class="btn-close" onclick="closeQrModal()">ปิดหน้าต่าง</button>
    </div>
</div>

<div class="modal" id="nodeCLocModal">
    <div class="modal-content">
        <h3>📍 แก้ไขพิกัดศูนย์วิทยุ Node C</h3>
        <input type="text" id="nodeCLocInput" style="width:100%; padding:10px; margin:14px 0; border-radius:8px; background:#0f172a; border:1px solid #334155; color:white;" placeholder="เช่น 7.0100,100.4800">
        <div style="display:flex; justify-content:space-between; gap:8px;">
            <button class="btn-close" style="background:#475569; flex:1;" onclick="closeNodeCLocModal()">ยกเลิก</button>
            <button class="btn-close" style="background:#0284c7; flex:1;" onclick="saveNodeCLoc()">บันทึกพิกัด</button>
        </div>
    </div>
</div>

<script>
let activeUID = "", currentCaseLoc = "", allMsgs = [], allCases = [], allNodes = [];
let savedOfficer = localStorage.getItem('officer_name') || ("เจ้าหน้าที่ " + Math.floor(1 + Math.random() * 99));
localStorage.setItem('officer_name', savedOfficer);
document.getElementById('officerName').value = savedOfficer;

function saveOfficerName(val) { if(val.trim()) localStorage.setItem('officer_name', val.trim()); }
function getUtf8Bytes(str) { return new Blob([str]).size; }
function checkByteLimit(el) {
    let maxBytes = 120, str = el.value;
    while (getUtf8Bytes(str) > maxBytes) str = str.slice(0, -1);
    el.value = str;
}

function refreshData() {
    let ts = Math.floor(Date.now() / 1000);
    fetch('/api/data?ts=' + ts).then(r => r.json()).then(data => {
        allMsgs = data.messages; allCases = data.cases; allNodes = data.nodes;
        renderNodes(); renderUsers();
        if(activeUID) { renderChat(activeUID); updateBanner(activeUID); }
    }).catch(e => console.log('Syncing...'));
}

function formatUptime(sec) {
    let h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60;
    return `${h} ชม. ${m} นาที ${s} วินาที`;
}

function renderNodes() {
    let bar = document.getElementById('telemetryBar');
    let html = '<div style="font-size:0.75rem; color:#94a3b8; font-weight:bold;">📡 สถานะเสาในเครือข่าย (คลิกดูรายละเอียดเชิงลึก):</div>';
    if(!allNodes || allNodes.length === 0) {
        html += '<div style="font-size:0.72rem; color:#64748b;">กำลังรับข้อมูล Telemetry สด...</div>';
    } else {
        allNodes.forEach(n => {
            let statusClass = n.online ? 'online' : 'offline';
            let dot = n.online ? '🟢' : '🔴';
            html += `<div class="node-card ${statusClass}" onclick="openNodeDetailModal('${n.id}')">
                <div>
                    <div class="node-title">${dot} เสา ${n.id} (${n.type})</div>
                    <div style="color:#94a3b8; font-size:0.68rem; margin-top:2px;">📍 ${n.loc}</div>
                </div>
                <div>🔋 <span class="node-stat">${n.batPct}%</span> (${n.batVolt}V)</div>
                <div>📶 <span class="node-stat">${n.rssi} dBm</span></div>
                <div style="color:#64748b; font-size:0.68rem;">⏱️ ${n.lastSeenSec}s ที่แล้ว</div>
            </div>`;
        });
    }
    bar.innerHTML = html;
}

function openNodeDetailModal(nodeId) {
    let n = allNodes.find(item => item.id === nodeId);
    if(!n) return;
    document.getElementById('popNodeTitle').innerText = '📡 Telemetry เชิงลึก: เสา Node ' + n.id;
    let mapsUrl = "https://maps.google.com/?q=" + encodeURIComponent(n.loc.replace(/\s+/g, ''));
    let detailsHtml = `
        <div class="node-detail-item">สถานะการเชื่อมต่อ: <span>${n.online ? '🟢 ออนไลน์ปกติ' : '🔴 ขาดการติดต่อ'}</span></div>
        <div class="node-detail-item">ประเภทเสา: <span>${n.type}</span></div>
        <div class="node-detail-item">แบตเตอรี่: <span>${n.batPct}% (${n.batVolt} V)</span></div>
        <div class="node-detail-item">ความแรงสัญญาณ (RSSI): <span>${n.rssi} dBm</span></div>
        <div class="node-detail-item">คุณภาพสัญญาณ (SNR): <span>${n.snr} dB</span></div>
        <div class="node-detail-item">หน่วยความจำ RAM เหลือ: <span>${n.freeHeapKB} KB</span></div>
        <div class="node-detail-item">เวลาทำงานต่อเนื่อง: <span>${formatUptime(n.uptimeSec)}</span></div>
        <div class="node-detail-item">ข้อมูลเพิ่มเติม: <span>${n.extraData}</span></div>
        <div class="node-detail-item" style="grid-column:span 2;">พิกัดตั้งเสา: <span>${n.loc}</span> <a href="${mapsUrl}" target="_blank" class="btn-maps">🗺️ แผนที่</a></div>
    `;
    document.getElementById('popNodeDetails').innerHTML = detailsHtml;
    document.getElementById('popBtnReboot').setAttribute('onclick', `requestRebootNode('${n.id}')`);
    document.getElementById('nodeDetailModal').style.display = 'flex';
}

function closeNodeDetailModal() { document.getElementById('nodeDetailModal').style.display = 'none'; }

function openNodeCLocModal() { document.getElementById('nodeCLocModal').style.display = 'flex'; }
function closeNodeCLocModal() { document.getElementById('nodeCLocModal').style.display = 'none'; }

function saveNodeCLoc() {
    let newLoc = document.getElementById('nodeCLocInput').value.trim();
    if(!newLoc) return;
    fetch('/api/set_location?loc=' + encodeURIComponent(newLoc)).then(() => {
        closeNodeCLocModal();
        alert('อัปเดตพิกัด Node C เรียบร้อยแล้ว');
    });
}

function requestRebootNode(nodeId) {
    if(confirm('ยืนยันส่งคำสั่งรีสตาร์ทไปยัง "เสา ' + nodeId + '" หรือไม่?')) {
        fetch('/api/reboot_node?target=' + nodeId).then(() => {
            alert('ส่งคำสั่งรีสตาร์ทไปยัง เสา ' + nodeId + ' แล้ว');
            closeNodeDetailModal();
        });
    }
}

function renderUsers() {
    document.getElementById('caseCount').innerText = allCases.length + " เคส";
    let listHtml = "";
    allCases.forEach(c => {
        let activeClass = (c.uid === activeUID) ? 'active' : '', badgeHtml = "";
        if(c.status === 'pending') badgeHtml = '<span class="badge badge-pending">🔴 รอรับเรื่อง</span>';
        else if(c.status === 'claimed') badgeHtml = `<span class="badge badge-claimed">🟡 ${escapeHtml(c.officer)}</span>`;
        else if(c.status === 'resolved') badgeHtml = '<span class="badge badge-resolved">🟢 ปลอดภัยแล้ว</span>';

        listHtml += `<div class="user-item ${activeClass}" onclick="selectUser('${c.uid}', '${escapeHtml(c.user)}', '${c.loc}')">
            <div class="user-header"><span class="user-name">${escapeHtml(c.user)} (${c.uid})</span>${badgeHtml}</div>
            <div class="user-sub">📍 ${c.loc} | ${escapeHtml(c.lastMsg)}</div>
        </div>`;
    });
    if(allCases.length > 0) document.getElementById('userList').innerHTML = listHtml;
}

function selectUser(uid, name, loc) {
    activeUID = uid; currentCaseLoc = loc;
    document.getElementById('currentUserName').innerText = name + " (" + uid + ")";
    document.getElementById('currentLoc').innerHTML = `📍 พิกัด: ${loc}`;
    document.getElementById('btnShowQrHeader').style.display = 'inline-block';
    document.getElementById('claimBanner').style.display = 'flex';
    renderUsers(); renderChat(uid); updateBanner(uid);
}

function openQrModal() {
    if(!currentCaseLoc) return;
    let cleanLoc = currentCaseLoc.replace(/\s+/g, '');
    document.getElementById('modalLocText').innerText = "พิกัด: " + cleanLoc;
    document.getElementById('qrImg').src = "/api/qr?loc=" + encodeURIComponent(cleanLoc) + "&t=" + Date.now();
    document.getElementById('qrModal').style.display = 'flex';
}

function closeQrModal() { document.getElementById('qrModal').style.display = 'none'; }

function updateBanner(uid) {
    let c = allCases.find(item => item.uid === uid);
    if(!c) return;
    let bannerText = document.getElementById('claimStatusText'), actionBtns = document.getElementById('claimActionBtns');
    let myName = document.getElementById('officerName').value.trim();

    if(c.status === 'pending') {
        bannerText.innerHTML = '<b style="color:#ef4444;">🔴 สถานะ: ยังไม่มีผู้รับเรื่อง</b>';
        actionBtns.innerHTML = `<button class="btn-claim" onclick="claimCase('${uid}')">✋ กดรับเรื่องเคสนี้</button>`;
    } else if(c.status === 'claimed') {
        if(c.officer === myName) {
            bannerText.innerHTML = `<b style="color:#eab308;">🟡 คุณ (${escapeHtml(c.officer)}) กำลังดูแลเคสนี้อยู่</b>`;
            actionBtns.innerHTML = `<button class="btn-resolve" onclick="resolveCase('${uid}')">🟢 ปิดเคส (ช่วยเหลือสำเร็จ)</button>`;
        } else {
            bannerText.innerHTML = `<b style="color:#eab308;">🟡 เจ้าหน้าที่ [${escapeHtml(c.officer)}] กำลังดูแลเคสนี้อยู่</b>`;
            actionBtns.innerHTML = `<button class="btn-claim" style="background:#334155; color:white;" onclick="claimCase('${uid}')">🔄 เปลี่ยนมาให้ฉันดูแลแทน</button>`;
        }
    } else if(c.status === 'resolved') {
        bannerText.innerHTML = '<b style="color:#10b981;">🟢 สถานะ: ช่วยเหลือเสร็จสิ้นแล้ว</b>';
        actionBtns.innerHTML = `<button class="btn-claim" style="background:#334155; color:white;" onclick="claimCase('${uid}')">🔓 เปิดเคสใหม่</button>`;
    }
}

function claimCase(uid) {
    let officer = document.getElementById('officerName').value.trim();
    if(!officer) { alert('กรุณาระบุชื่อเจ้าหน้าที่ประจำเครื่องก่อนครับ'); return; }
    fetch(`/api/claim?uid=${uid}&officer=${encodeURIComponent(officer)}`).then(() => refreshData());
}

function resolveCase(uid) {
    let officer = document.getElementById('officerName').value.trim();
    fetch(`/api/resolve?uid=${uid}&officer=${encodeURIComponent(officer)}`).then(() => refreshData());
}

function renderChat(uid) {
    let logsHtml = "";
    allMsgs.filter(m => m.uid === uid).forEach(m => {
        let bubbleClass = (m.sender === 'victim') ? 'msg-victim' : 'msg-officer';
        let senderName = (m.sender === 'victim') ? m.user : (m.user + ' (เจ้าหน้าที่)');
        logsHtml += `<div class="msg-bubble ${bubbleClass}"><div style="font-size:0.7rem; font-weight:bold; margin-bottom:3px; opacity:0.8;">${escapeHtml(senderName)}</div>${escapeHtml(m.text)}</div>`;
    });
    let logsDiv = document.getElementById('chatLogs');
    logsDiv.innerHTML = logsHtml;
    logsDiv.scrollTop = logsDiv.scrollHeight;
}

function sendReply() {
    if(!activeUID) { alert('กรุณาเลือกเคสที่ต้องการตอบกลับก่อนครับ'); return; }
    let txt = document.getElementById('replyInput').value.trim();
    let officer = document.getElementById('officerName').value.trim();
    if(!txt) return;
    if(!officer) { alert('กรุณาระบุชื่อเจ้าหน้าที่ประจำเครื่อง'); return; }

    fetch(`/reply?target_uid=${activeUID}&officer=${encodeURIComponent(officer)}&text=${encodeURIComponent(txt)}`);
    document.getElementById('replyInput').value = '';
    refreshData();
}

function escapeHtml(text) { return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;"); }
setInterval(refreshData, 1500);
</script>
</body>
</html>
)rawliteral";

// ดักจับ URL อื่นให้ Redirect กลับมาหน้าหลัก
void handleCaptiveRedirectC(AsyncWebServerRequest *request) {
  request->redirect("http://192.168.4.1/");
}

// ฟังก์ชันเริ่มต้นการทำงานหลัก (Setup)
void setup() {
  Serial.begin(115200);
  delay(1000);

  setenv("TZ", "ICT-7", 1);
  tzset();

  setupWatchdog();             // เริ่มเปิด Watchdog Timer
  loadLocationConfig();        // โหลดพิกัดเสา C

  Serial.println("\n--- STARTING NODE C (DISPATCH HQ CENTER) ---");

  pinMode(BAT_ADC_PIN, INPUT);
  loadCasesFromFS();           // โหลดรายการเคสถาวรจาก LittleFS
  initOLED();                  // เริ่มระบบ OLED
  delay(300);

  WiFi.mode(WIFI_AP);          // เปิด Access Point
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP("Emergency_Rescue_NodeC", "12345678", 1, 0, 8); // ชื่อ WiFi ศูนย์กู้ภัย
  WiFi.setTxPower(WIFI_POWER_13dBm);

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP()); // เปิด Captive Portal DNS
  delay(200);

  SPI.begin(SCK, MISO, MOSI, SS);
  LoRa.setPins(SS, RST, DIO0);
  while (!LoRa.begin(BAND)) {  // เปิดระบบ LoRa 923MHz
    Serial.println("LoRa Init Failed! Retrying...");
    delay(1000);
    yield();
  }

  configureLoRaRadio();        // ตั้งค่าส่งระยะไกล

  // เส้นทาง Web Server Endpoint
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html; charset=utf-8", rescuer_html);
  });

  server.on("/generate_204", HTTP_GET, handleCaptiveRedirectC);
  server.on("/redirect", HTTP_GET, handleCaptiveRedirectC);
  server.on("/hotspot-detect.html", HTTP_GET, handleCaptiveRedirectC);
  server.on("/canonical.html", HTTP_GET, handleCaptiveRedirectC);
  server.on("/connecttest.txt", HTTP_GET, handleCaptiveRedirectC);

  // API ตั้งค่าพิกัดเสา Node C
  server.on("/api/set_location", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("loc")) {
      saveLocationConfig(request->getParam("loc")->value());
    }
    request->send(200, "text/plain", "OK");
  });

  // API สั่งรีสตาร์ทบอร์ดเป้าหมายระยะไกล
  server.on("/api/reboot_node", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("target")) {
      String target = request->getParam("target")->value();
      target.trim();

      if (target == "C") {
        request->send(200, "text/plain", "REBOOTING_NODE_C");
        delay(500);
        performReboot();
        return;
      }

      replyCounter++;
      String cmdPacket = "CMD" + String(replyCounter) + "|3|C|" + target + "|CMD|REBOOT";
      {
        std::lock_guard<std::mutex> lock(txMutex);
        txQueue.push(cmdPacket);
      }
    }
    request->send(200, "text/plain", "OK");
  });

  // API สตรีมส่งไฟล์ QR Code รูปแบบ SVG ให้เบราว์เซอร์
  server.on("/api/qr", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!request->hasParam("loc")) {
      request->send(400, "text/plain", "Missing loc");
      return;
    }
    String loc = request->getParam("loc")->value();
    loc.trim();
    String url = "https://maps.google.com/?q=" + loc;
    QRCode qrcode;
    uint8_t qrcodeData[qrcode_getBufferSize(4)];
    qrcode_initText(&qrcode, qrcodeData, 4, 0, url.c_str());

    String svg = "<svg xmlns='http://www.w3.org/2000/svg' width='220' height='220' viewBox='0 0 " + String(qrcode.size + 4) + " " + String(qrcode.size + 4) + "'>";
    svg += "<rect width='100%' height='100%' fill='white'/>";
    svg += "<path d='";
    for (uint8_t y = 0; y < qrcode.size; y++) {
      for (uint8_t x = 0; x < qrcode.size; x++) {
        if (qrcode_getModule(&qrcode, x, y)) svg += "M" + String(x + 2) + "," + String(y + 2) + "h1v1h-1z ";
      }
    }
    svg += "' fill='black'/></svg>";
    request->send(200, "image/svg+xml", svg);
  });

  // API รวมข้อมูลสดทั้งหมดส่งให้สคริปต์หน้าเว็บศูนย์กู้ภัย
  server.on("/api/data", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("ts")) syncTimeFromEpoch(request->getParam("ts")->value().toInt()); // ซิงค์เวลาสด
    
    float voltsC = getAccurateBatteryVoltage();
    int batPctC = calculateBatteryPercentage(voltsC);
    uint32_t freeHeapC = ESP.getFreeHeap() / 1024;
    uint32_t uptimeC = millis() / 1000;
    int wifiClientsC = WiFi.softAPgetStationNum();

    updateOrAddNode("C", "CenterHQ", currentLocationC, batPctC, voltsC, freeHeapC, uptimeC, "WiFi Client: " + String(wifiClientsC), 0, 0.0);

    String json = "{\"messages\":[";
    for (int i = 0; i < msgCount; i++) {
      json += "{";
      json += "\"uid\":\"" + chatHistory[i].uid + "\",";
      json += "\"user\":\"" + chatHistory[i].user + "\",";
      json += "\"loc\":\"" + chatHistory[i].loc + "\",";
      json += "\"text\":\"" + chatHistory[i].text + "\",";
      json += "\"sender\":\"" + chatHistory[i].sender + "\"";
      json += "}";
      if (i < msgCount - 1) json += ",";
    }
    json += "],\"cases\":[";

    {
      std::lock_guard<std::mutex> lock(caseMutex);
      for (size_t i = 0; i < activeCases.size(); i++) {
        json += "{";
        json += "\"uid\":\"" + activeCases[i].uid + "\",";
        json += "\"user\":\"" + activeCases[i].user + "\",";
        json += "\"loc\":\"" + activeCases[i].loc + "\",";
        json += "\"officer\":\"" + activeCases[i].assignedOfficer + "\",";
        json += "\"status\":\"" + activeCases[i].status + "\",";
        json += "\"lastMsg\":\"" + activeCases[i].lastMsg + "\"";
        json += "}";
        if (i < activeCases.size() - 1) json += ",";
      }
    }

    json += "],\"nodes\":[";
    {
      std::lock_guard<std::mutex> lock(nodeMutex);
      unsigned long now = millis();
      for (size_t i = 0; i < networkNodes.size(); i++) {
        bool isOnline = (now - networkNodes[i].lastSeen < 50000) || (networkNodes[i].id == "C");
        unsigned long lastSeenSec = (now - networkNodes[i].lastSeen) / 1000;
        json += "{";
        json += "\"id\":\"" + networkNodes[i].id + "\",";
        json += "\"type\":\"" + networkNodes[i].type + "\",";
        json += "\"loc\":\"" + networkNodes[i].loc + "\",";
        json += "\"batPct\":" + String(networkNodes[i].batPct) + ",";
        json += "\"batVolt\":\"" + String(networkNodes[i].batVolt, 2) + "\",";
        json += "\"freeHeapKB\":" + String(networkNodes[i].freeHeapKB) + ",";
        json += "\"uptimeSec\":" + String(networkNodes[i].uptimeSec) + ",";
        json += "\"extraData\":\"" + networkNodes[i].extraData + "\",";
        json += "\"rssi\":" + String(networkNodes[i].rssi) + ",";
        json += "\"snr\":\"" + String(networkNodes[i].snr, 1) + "\",";
        json += "\"lastSeenSec\":" + String(lastSeenSec) + ",";
        json += "\"online\":" + String(isOnline ? "true" : "false");
        json += "}";
        if (i < networkNodes.size() - 1) json += ",";
      }
    }

    json += "]}";
    request->send(200, "application/json; charset=utf-8", json);
  });

  // API เจ้าหน้าที่กดรับเรื่องดูแลเคส
  server.on("/api/claim", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("uid") && request->hasParam("officer")) {
      String uid = request->getParam("uid")->value();
      String officer = request->getParam("officer")->value();
      String lastMsgId = "";
      {
        std::lock_guard<std::mutex> lock(caseMutex);
        for (auto &c : activeCases) {
          if (c.uid == uid) {
            c.assignedOfficer = officer;
            c.status = "claimed";
            lastMsgId = c.lastMsgId;
            break;
          }
        }
      }
      saveCasesToFS(); // บันทึกลงระบบไฟล์

      ackCounter++;
      String officerB64 = encodeBase64(officer);
      String ackPacket = "ACK" + String(ackCounter) + "|3|C|A|ACK|" + lastMsgId + "|" + uid + "|CLAIMED|" + officerB64;
      {
        std::lock_guard<std::mutex> lock(txMutex);
        txQueue.push(ackPacket);
      }
    }
    request->send(200, "text/plain", "OK");
  });

  // API เจ้าหน้าที่กดปิดเคสช่วยเหลือสำเร็จ
  server.on("/api/resolve", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("uid")) {
      String uid = request->getParam("uid")->value();
      String lastMsgId = "";
      String officer = "";
      {
        std::lock_guard<std::mutex> lock(caseMutex);
        for (auto &c : activeCases) {
          if (c.uid == uid) {
            c.status = "resolved";
            lastMsgId = c.lastMsgId;
            officer = c.assignedOfficer;
            break;
          }
        }
      }
      saveCasesToFS(); // บันทึกลงระบบไฟล์

      ackCounter++;
      String officerB64 = encodeBase64(officer);
      String ackPacket = "ACK" + String(ackCounter) + "|3|C|A|ACK|" + lastMsgId + "|" + uid + "|RESOLVED|" + officerB64;
      {
        std::lock_guard<std::mutex> lock(txMutex);
        txQueue.push(ackPacket);
      }
    }
    request->send(200, "text/plain", "OK");
  });

  // API ตอบกลับข้อความแชทไปยังผู้ประสบภัย
  server.on("/reply", HTTP_GET, [](AsyncWebServerRequest *request){
    String targetUid = request->getParam("target_uid")->value();
    targetUid.trim();
    String officer = request->getParam("officer")->value();
    officer.trim();
    String text = request->getParam("text")->value();
    text.trim();

    if (text.length() > 120) text = text.substring(0, 120);

    {
      std::lock_guard<std::mutex> lock(caseMutex);
      for (auto &c : activeCases) {
        if (c.uid == targetUid) {
          c.assignedOfficer = officer;
          c.status = "claimed";
          break;
        }
      }
    }
    saveCasesToFS();

    replyCounter++;
    String officerB64 = encodeBase64(officer);
    String textB64 = encodeBase64(text);
    String packet = String(replyCounter) + "|3|C|A|REPLY|" + targetUid + "|" + officerB64 + "|HQ|" + textB64;

    {
      std::lock_guard<std::mutex> lock(txMutex);
      txQueue.push(packet);
    }

    addMessage(targetUid, officer, "HQ", text, "officer");
    request->send(200, "text/plain; charset=utf-8", "OK");
  });

  server.onNotFound(handleCaptiveRedirectC);
  server.begin(); // เปิด Web Server
}

// ลูปการทำงานหลัก (Loop)
void loop() {
  esp_task_wdt_reset();        // รีเซ็ต Watchdog Timer
  dnsServer.processNextRequest();

  if (millis() - lastOledSwitch > 20000) {
    lastOledSwitch = millis();
    oledPage = (oledPage + 1) % 2;
  }
  
  if (millis() - lastOledRender > 1000) {
    lastOledRender = millis();
    updateOLED();
  }

  // ส่งแพ็กเกจ LoRa ออกจาก Queue
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

  // ส่ง Heartbeat จาก Node C (พร้อมแนบเวลา Unix Epoch Timestamp สดๆ ไปให้ Node B ซิงค์เวลา)
  if (millis() - lastHeartbeat > heartbeatInterval) {
    lastHeartbeat = millis();
    heartbeatInterval = 45000 + random(0, 5000);
    hbCounterC++;

    float vC = getAccurateBatteryVoltage();
    int pctC = calculateBatteryPercentage(vC);
    uint32_t freeHeapC = ESP.getFreeHeap() / 1024;
    uint32_t uptimeC = millis() / 1000;
    int wifiClientsC = WiFi.softAPgetStationNum();
    unsigned long currentEpoch = (unsigned long)time(NULL);

    String hbPacket = "HBC" + String(hbCounterC) + "|3|C|ALL|HB|CenterHQ|" + currentLocationC + "|" + String(pctC) + "|" + String(vC, 2) + "|" + String(freeHeapC) + "|" + String(uptimeC) + "|WiFi Client: " + String(wifiClientsC) + "|" + String(currentEpoch);
    {
      std::lock_guard<std::mutex> lock(txMutex);
      txQueue.push(hbPacket);
    }
  }

  // รับและประมวลผลสัญญาณ LoRa ขาเข้า
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
    if (calculateCRC16(payload) != rxCrc) return; // หาก CRC16 ไม่ตรงกัน ให้ทิ้งแพ็กเกจทันที

    std::vector<String> tokens = splitString(payload, '|');
    if (tokens.size() < 5) return;

    String msgType = tokens[4];

    // ประมวลผลแพ็กเกจ Heartbeat จาก Node อื่นเพื่อทำระบบ Telemetry
    if (msgType == "HB" && tokens.size() >= 11) {
      String nodeId = tokens[2];
      String nodeType = tokens[5];
      String loc = tokens[6];
      int batPct = tokens[7].toInt();
      float batVolt = tokens[8].toFloat();
      uint32_t freeHeapKB = tokens[9].toInt() / 1024;
      uint32_t uptimeSec = tokens[10].toInt();
      String extraData = (tokens.size() >= 12) ? tokens[11] : "";

      if (nodeType == "Repeater") extraData = "Relayed: " + extraData + " pkts";
      else if (nodeType == "VictimNode") extraData = "WiFi Client: " + extraData + " devices";

      updateOrAddNode(nodeId, nodeType, loc, batPct, batVolt, freeHeapKB, uptimeSec, extraData, lastRssi, lastSnr);
    }

    // ประมวลผลข้อความขอความช่วยเหลือฉุกเฉิน (MSG) จาก Node A
    if (msgType == "MSG" && tokens.size() >= 9) {
      String msgId = tokens[0];
      String uid = tokens[5];
      String user = decodeBase64(tokens[6]);
      String loc = tokens[7];
      String text = decodeBase64(tokens[8]);

      // ส่งแพ็กเกจตอบรับ ACK กลับทันทีเพื่อหยุดการ Auto-Retry ของ Node A
      ackCounter++;
      String ackPacket = "ACK" + String(ackCounter) + "|3|C|A|ACK|" + msgId + "|" + uid + "|DELIVERED|HQ";
      {
        std::lock_guard<std::mutex> lock(txMutex);
        txQueue.push(ackPacket);
      }

      if (!isDuplicate(msgId)) {
        addOrUpdateCase(uid, user, loc, text, msgId);
        addMessage(uid, user, loc, text, "victim");
        saveCasesToFS(); // บันทึกเคสถาวร
      }
    }
  }
}
