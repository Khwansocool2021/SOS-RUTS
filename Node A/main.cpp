#include <Arduino.h>         // ไลบรารีพื้นฐานของ Arduino สำหรับ ESP32[cite: 57]
#include <WiFi.h>            // ไลบรารีจัดการระบบ WiFi (Access Point)[cite: 57]
#include <SPI.h>             // ไลบรารีสื่อสารบัส SPI สำหรับโมดูล LoRa[cite: 57]
#include <LoRa.h>            // ไลบรารีควบคุมชิปวิทยุ LoRa (SX1276/SX1278)[cite: 57]
#include <ESPAsyncWebServer.h> // ไลบรารีสร้าง Web Server แบบ Async ไม่บล็อกการทำงาน[cite: 57]
#include <DNSServer.h>       // ไลบรารีทำ Captive Portal DNS Server (ดักจับหน้าเว็บ)[cite: 57]
#include <Wire.h>            // ไลบรารีสื่อสารบัส I2C สำหรับหน้าจอ OLED[cite: 57]
#include <Adafruit_GFX.h>    // ไลบรารีกราฟิกพื้นฐาน Adafruit[cite: 57]
#include <Adafruit_SSD1306.h>// ไลบรารีควบคุมจอ OLED SSD1306[cite: 57]
#include <Preferences.h>     // ไลบรารีบันทึกข้อมูลลง Flash Memory (NVS)[cite: 57]
#include <mbedtls/base64.h>  // ไลบรารีเข้ารหัส/ถอดรหัส Base64 ภาษาไทย[cite: 57]
#include <time.h>            // ไลบรารีจัดการเวลามาตรฐาน C[cite: 57]
#include <sys/time.h>        // ไลบรารีตั้งค่าเวลา RTC ของระบบระบบ ESP32[cite: 57]
#include <queue>             // ไลบรารีโครงสร้างข้อมูล คิว (Queue)[cite: 57]
#include <vector>            // ไลบรารีโครงสร้างข้อมูล เว็กเตอร์ (Vector)[cite: 57]
#include <mutex>             // ไลบรารีป้องกัน Race Condition บน Dual Core[cite: 57]
#include <esp_task_wdt.h>    // ไลบรารีระบบรีเซ็ตบอร์ดอัตโนมัติเมื่อค้าง (Watchdog)[cite: 57]

#define WDT_TIMEOUT 8        // ตั้งเวลา Watchdog ไว้ที่ 8 วินาที[cite: 57]

// กำหนดพินขาเชื่อมต่อฮาร์ดแวร์ SPI ของ LoRa[cite: 57]
#define SCK 5                // ขา SPI Clock[cite: 57]
#define MISO 19              // ขา SPI MISO[cite: 57]
#define MOSI 27              // ขา SPI MOSI[cite: 57]
#define SS 18                // ขา SPI Chip Select[cite: 57]
#define RST 14               // ขา LoRa Reset[cite: 57]
#define DIO0 26              // ขา LoRa DIO0 Interrupt[cite: 57]
#define BAND 923E6           // ความถี่ LoRa ประเทศไทย (923 MHz)[cite: 57]
#define BAT_ADC_PIN 35       // ขา Analog อ่านแรงดันแบตเตอรี่[cite: 57]

// กำหนดพินขาเชื่อมต่อ OLED I2C[cite: 57]
#define OLED_SDA 21          // ขา I2C Data[cite: 57]
#define OLED_SCL 22          // ขา I2C Clock[cite: 57]
#define SCREEN_WIDTH 128     // ความกว้างหน้าจอ OLED (พิกเซล)[cite: 57]
#define SCREEN_HEIGHT 64     // ความสูงหน้าจอ OLED (พิกเซล)[cite: 57]

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1); // ประกาศวัตถุควบคุมหน้าจอ OLED[cite: 57]
Preferences preferences;     // ประกาศวัตถุสำหรับอ่าน/เขียน Flash Memory[cite: 57]

const char* DEFAULT_LOCATION_A = "7.0081,100.4742"; // พิกัดละติจูด,ลองจิจูด เริ่มต้นของ Node A[cite: 57]
String currentLocationA;     // ตัวแปรเก็บพิกัดปัจจุบัน[cite: 57]

AsyncWebServer server(80);   // สร้าง Async Web Server ที่พอร์ต 80[cite: 57]
DNSServer dnsServer;         // สร้าง DNS Server สำหรับ Captive Portal[cite: 57]
const byte DNS_PORT = 53;    // พอร์ตของ DNS Server[cite: 58]

int msgCounter = 0;          // นับจำนวนข้อความที่ส่งออกจาก Node A[cite: 58]
int hbCounterA = 0;          // นับจำนวนแพ็กเกจ Heartbeat[cite: 58]
unsigned long lastHeartbeatTime = 0; // เวลาที่ส่ง Heartbeat ล่าสุด[cite: 58]
unsigned long heartbeatInterval = 35000; // ระยะเวลาส่ง Heartbeat (~35 วินาที)[cite: 58]

int oledPage = 0;            // หน้าปัจจุบันของ OLED (0=Dash, 1=Network)[cite: 58]
unsigned long lastOledSwitch = 0; // เวลาสลับหน้า OLED ล่าสุด[cite: 58]
unsigned long lastOledRender = 0; // เวลาวาดหน้า OLED ล่าสุด[cite: 58]
bool oledReady = false;      // สถานะความพร้อมของจอ OLED[cite: 58]

std::queue<String> txQueue;  // คิวเก็บแพ็กเกจ LoRa รอการส่ง[cite: 58]
std::mutex txMutex;          // ตัวล็อคการเข้าถึงคิว txQueue กันแย่งดึงข้อมูล[cite: 58]

// โครงสร้างข้อมูลสำหรับข้อความที่รอการส่งซ้ำ (Auto-Retry)[cite: 58]
struct RetryMessage {
  String msgId;              // รหัสข้อความ[cite: 58]
  String fullPacket;         // เนื้อหาแพ็กเกจเต็ม[cite: 58]
  int retriesLeft;           // จำนวนครั้งการส่งซ้ำที่เหลือ[cite: 58]
  unsigned long lastSentTime;// เวลาที่ส่งออกไปครั้งล่าสุด[cite: 58]
};
std::vector<RetryMessage> pendingRetryMsgs; // รายการข้อความรอส่งซ้ำ[cite: 58]
std::mutex retryMutex;       // ตัวล็อคการเข้าถึงรายการส่งซ้ำ[cite: 58]

// โครงสร้างข้อมูลสำหรับข้อความตอบกลับจากศูนย์กู้ภัย[cite: 58]
struct PendingReply {
  String targetUid;          // UID ผู้ประสบภัยปลายทาง[cite: 58]
  String officer;            // ชื่อเจ้าหน้าที่[cite: 58]
  String text;               // ข้อความตอบกลับ[cite: 58]
};
std::vector<PendingReply> pendingReplies; // รายการข้อความตอบกลับรอส่งให้เบราว์เซอร์[cite: 58]

// โครงสร้างข้อมูลอัปเดตสถานะการส่งข้อความ[cite: 58]
struct MsgStatusUpdate {
  String msgId;              // รหัสข้อความ[cite: 58]
  String status;             // สถานะ (DELIVERED/CLAIMED/RESOLVED)[cite: 58]
  String officer;            // ชื่อเจ้าหน้าที่[cite: 58]
};
std::vector<MsgStatusUpdate> pendingStatusUpdates; // รายการสถานะรอแจ้งเบราว์เซอร์[cite: 58]
std::mutex dataMutex;        // ตัวล็อคการจัดการข้อมูลข้อความ[cite: 58]

std::vector<String> seenMsgIDs; // บันทึก ID แพ็กเกจที่เคยได้รับ ป้องกันการประมวลผลซ้ำ[cite: 58]
bool isDuplicate(String msgId) {
  for (const auto& id : seenMsgIDs) {
    if (id == msgId) return true; // ถ้าเคยพบแล้ว ถือว่าเป็นแพ็กเกจซ้ำ[cite: 58]
  }
  if (seenMsgIDs.size() >= 50) seenMsgIDs.erase(seenMsgIDs.begin()); // ลบข้อมูลเก่าถ้าเกิน 50 รายการ[cite: 58]
  seenMsgIDs.push_back(msgId); // เพิ่ม ID เข้าสู่รายการ[cite: 59]
  return false; // ไม่ซ้ำ[cite: 59]
}

// โครงสร้างบันทึกสถานะเสาในเครือข่าย[cite: 59]
struct NodeStatus {
  int rssi = 0;              // ความแรงสัญญาณ[cite: 59]
  float snr = 0.0;           // อัตราส่วนสัญญาณต่อสัญญาณรบกวน[cite: 59]
  unsigned long lastSeen = 0;// เวลาที่พบครั้งล่าสุด[cite: 59]
  bool active = false;       // สถานะออนไลน์[cite: 59]
};
NodeStatus nodeB, nodeC;     // บันทึกสถานะ Node B และ Node C[cite: 59]
int lastRssi = 0;            // ค่า RSSI ล่าสุดที่รับได้[cite: 59]
float lastSnr = 0.0;         // ค่า SNR ล่าสุดที่รับได้[cite: 59]

// ฟังก์ชันตัดแบ่งข้อความตามตัวคั่น (Delimiter)[cite: 59]
std::vector<String> splitString(const String &str, char delim) {
  std::vector<String> tokens; // ตัวแปรเก็บผลลัพธ์การตัด[cite: 59]
  int start = 0;              // จุดเริ่มต้นตัด[cite: 59]
  int end = str.indexOf(delim); // ค้นหาตำแหน่งตัวคั่น[cite: 59]
  while (end != -1) {
    tokens.push_back(str.substring(start, end)); // ดึงข้อความช่วงดังกล่าว[cite: 59]
    start = end + 1;                             // ขยับจุดเริ่มต้นไปหลังตัวคั่น[cite: 59]
    end = str.indexOf(delim, start);             // หาจุดคั่นถัดไป[cite: 59]
  }
  tokens.push_back(str.substring(start));        // ดึงชิ้นส่วนสุดท้าย[cite: 59]
  return tokens;                                 // คืนค่าอาร์เรย์ข้อความ[cite: 59]
}

// ฟังก์ชันเข้ารหัสข้อความภาษาไทยเป็น Base64[cite: 59]
String encodeBase64(const String &input) {
  unsigned char out[350];     // บัฟเฟอร์เก็บผลลัพธ์[cite: 59]
  size_t olen = 0;            // ขนาดผลลัพธ์[cite: 59]
  mbedtls_base64_encode(out, sizeof(out), &olen, (const unsigned char*)input.c_str(), input.length()); // เข้ารหัส[cite: 59]
  out[olen] = '\0';           // เติม Null terminator[cite: 59]
  return String((char*)out);  // คืนค่าสตริง Base64[cite: 59]
}

// ฟังก์ชันถอดรหัส Base64 กลับเป็นข้อความ[cite: 59]
String decodeBase64(const String &input) {
  unsigned char out[350];     // บัฟเฟอร์เก็บผลลัพธ์[cite: 59]
  size_t olen = 0;            // ขนาดผลลัพธ์[cite: 59]
  mbedtls_base64_decode(out, sizeof(out), &olen, (const unsigned char*)input.c_str(), input.length()); // ถอดรหัส[cite: 59]
  out[olen] = '\0';           // เติม Null terminator[cite: 59]
  return String((char*)out);  // คืนค่าสตริงที่ถอดรหัสแล้ว[cite: 59]
}

// ฟังก์ชันคำนวณค่า CRC16 ป้องกันข้อความผิดพลาด/ขาดหาย[cite: 60]
uint16_t calculateCRC16(const String &str) {
  uint16_t crc = 0xFFFF;      // ค่า CRC เริ่มต้น[cite: 60]
  for (size_t i = 0; i < str.length(); i++) {
    crc ^= (uint8_t)str[i];   // คำนวณ XOR ไบต์[cite: 60]
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001; // เลื่อนบิตและ XOR Polynomial[cite: 60]
      else crc >>= 1;
    }
  }
  return crc;                 // คืนค่า CRC16[cite: 60]
}

// ตั้งค่าเฝ้าระวังระบบค้าง (Watchdog Timer)[cite: 60]
void setupWatchdog() {
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = WDT_TIMEOUT * 1000,           // ตั้งเวลาเป็นมิลลิวินาที[cite: 60]
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1, // เฝ้าระวังทุก Core[cite: 60]
    .trigger_panic = true                       // รีบูตบอร์ดทันทีเมื่อ WDT ทำงาน[cite: 60]
  };
  esp_task_wdt_reconfigure(&twdt_config);
  esp_task_wdt_add(NULL);                      // เพิ่ม Task ปัจจุบันเข้า WDT[cite: 60]
#else
  esp_task_wdt_init(WDT_TIMEOUT, true);         // โหมดดั้งเดิม[cite: 60]
  esp_task_wdt_add(NULL);                       // เพิ่ม Task ปัจจุบันเข้า WDT[cite: 60]
#endif
}

// ฟังก์ชันสั่งรีสตาร์ทบอร์ดอย่างปลอดภัย[cite: 60]
void performReboot() {
  if (oledReady) {            // แสดงข้อความบนจอ OLED[cite: 60]
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(15, 25);
    display.println("REBOOTING NODE A..."); // แสดงข้อความกำลังรีสตาร์ท[cite: 60]
    display.display();
  }
  LoRa.sleep();               // ปิดชิป LoRa ชั่วคราว[cite: 60]
  delay(200);                 // หน่วงเวลา[cite: 60]
  esp_task_wdt_delete(NULL);  // ยกเลิก WDT[cite: 60]
  esp_restart();              // สั่งรีเซ็ตบอร์ด ESP32[cite: 60]
  while (true) { yield(); }   // วนลูปคอยการรีบูต[cite: 60]
}

// ฟังก์ชันโหลดค่าคอนฟิกจาก Flash Memory[cite: 61]
void loadNodeConfig() {
  preferences.begin("node_cfg", true); // เปิดอ่านข้อมูลโหมด Read-Only[cite: 61]
  currentLocationA = preferences.getString("gps_loc", DEFAULT_LOCATION_A); // ดึงพิกัด[cite: 61]
  msgCounter = preferences.getInt("msg_cnt", 0);                           // ดึงตัวนับข้อความ[cite: 61]
  preferences.end();                   // ปิดการใช้งาน Flash[cite: 61]
}

// ฟังก์ชันบันทึกพิกัดใหม่ลง Flash Memory[cite: 61]
void saveLocationConfig(String newLoc) {
  newLoc.trim();                      // ตัดช่องว่าง[cite: 61]
  if (newLoc.length() > 0) {
    currentLocationA = newLoc;        // อัปเดตตัวแปร[cite: 61]
    preferences.begin("node_cfg", false); // เปิดเขียนข้อมูล[cite: 61]
    preferences.putString("gps_loc", newLoc); // บันทึกพิกัด[cite: 61]
    preferences.end();               // ปิดการใช้งาน Flash[cite: 61]
  }
}

// ฟังก์ชันบันทึกจำนวนนับข้อความลง Flash Memory[cite: 61]
void saveMsgCounterConfig() {
  preferences.begin("node_cfg", false);   // เปิดเขียนข้อมูล[cite: 61]
  preferences.putInt("msg_cnt", msgCounter); // บันทึกค่าmsgCounter[cite: 61]
  preferences.end();                     // ปิดการใช้งาน Flash[cite: 61]
}

// ฟังก์ชันปรับแต่งพารามิเตอร์วิทยุ LoRa สำหรับการส่งระยะไกล[cite: 61]
void configureLoRaRadio() {
  LoRa.setTxPower(20);            // ปรับกำลังส่งสูงสุด 20 dBm (100mW)[cite: 61]
  LoRa.setSpreadingFactor(9);     // ค่า SF9 เพื่อความสมดุลระหว่างระยะทางและความเร็ว[cite: 61]
  LoRa.setSignalBandwidth(125E3); // แบนด์วิดท์ 125 kHz[cite: 61]
  LoRa.setCodingRate4(5);         // Coding Rate 4/5 ช่วยซ่อมแซมข้อมูลถูกรบกวน[cite: 61]
  LoRa.setPreambleLength(12);     // ความยาว Preamble 12 ไบต์ ช่วยในการซิงค์สัญญาณ[cite: 61]
  LoRa.setGain(0);                // ปรับ LNA Gain อัตโนมัติ[cite: 61]
}

// ฟังก์ชันส่งแพ็กเกจ LoRa พร้อมแนบ CRC16[cite: 61]
void sendLoRaRaw(const String &packet) {
  uint16_t crc = calculateCRC16(packet);        // คำนวณค่า CRC16[cite: 61]
  String finalPacket = packet + "|" + String(crc, HEX); // แนบค่า CRC16 ท้ายแพ็กเกจ[cite: 61]
  
  LoRa.beginPacket();             // เริ่มสร้างแพ็กเกจ[cite: 61]
  LoRa.print(finalPacket);        // บรรจุข้อมูล[cite: 61]
  LoRa.endPacket();               // สั่งส่งคลื่นวิทยุออกไป[cite: 61]
}

// ฟังก์ชันอ่านแรงดันแบตเตอรี่แบบเฉลี่ยแม่นยำ[cite: 61]
float getAccurateBatteryVoltage() {
  uint32_t rawSum = 0;            // ผลรวมค่า Analog[cite: 61]
  for (int i = 0; i < 16; i++) {
    rawSum += analogRead(BAT_ADC_PIN); // อ่านค่า ADC 16 ครั้ง[cite: 62]
    delayMicroseconds(100);       // หน่วงเวลาสั้นๆ[cite: 62]
  }
  float rawAvg = rawSum / 16.0;   // ค่าเฉลี่ย[cite: 62]
  float volts = (rawAvg / 4095.0) * 2.0 * 3.3 * 1.08; // คำนวณผ่าน Voltage Divider และเทียบ Vref[cite: 62]
  if (volts > 4.25) volts = 4.25; // จำกัดค่าสูงสุดลิเธียมไอออน[cite: 62]
  if (volts < 3.00) volts = 3.00; // จำกัดค่าต่ำสุด[cite: 62]
  return volts;                   // คืนค่าแรงดัน (โวลต์)[cite: 62]
}

// ฟังก์ชันคำนวณเปอร์เซ็นต์แบตเตอรี่[cite: 62]
int calculateBatteryPercentage(float volts) {
  if (volts >= 4.15) return 100;  // 100%[cite: 62]
  if (volts >= 4.00) return 90;   // 90%[cite: 62]
  if (volts >= 3.85) return 75;   // 75%[cite: 62]
  if (volts >= 3.75) return 50;   // 50%[cite: 62]
  if (volts >= 3.65) return 30;   // 30%[cite: 62]
  if (volts >= 3.50) return 15;   // 15%[cite: 62]
  if (volts >= 3.30) return 5;    // 5%[cite: 62]
  return 0;                       // แบตเตอรี่หมด[cite: 62]
}

// ฟังก์ชันซิงค์เวลาจากค่า Unix Epoch Timestamp[cite: 62]
void syncTimeFromEpoch(unsigned long epoch) {
  if (epoch > 1700000000) {      // ตรวจสอบความถูกต้องของเวลา[cite: 62]
    struct timeval tv = { (time_t)epoch, 0 }; // โครงสร้างเวลา[cite: 62]
    settimeofday(&tv, NULL);     // ตั้งค่าเวลา RTC ใน ESP32[cite: 62]
    setenv("TZ", "ICT-7", 1);     // ตั้งค่า Timezone ประเทศไทย (UTC+7)[cite: 62]
    tzset();                     // บันทึก Timezone[cite: 62]
  }
}

// ฟังก์ชันจัดรูปแบบเวลา HH:MM:SS[cite: 62]
String getFormattedTime() {
  time_t now; time(&now);         // ดึงเวลาปัจจุบัน[cite: 62]
  struct tm timeinfo; localtime_r(&now, &timeinfo); // แปลงโครงสร้างเวลา local[cite: 62]
  if (timeinfo.tm_year < 120) return "00:00:00";    // คืนค่าเริ่มต้นถ้ายังไม่ซิงค์เวลา[cite: 62]
  char buf[20]; strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo); // จัดรูปแบบสตริง[cite: 62]
  return String(buf);
}

// ฟังก์ชันจัดรูปแบบวันที่ DD/MM/YYYY[cite: 62]
String getFormattedDate() {
  time_t now; time(&now);         // ดึงเวลาปัจจุบัน[cite: 62]
  struct tm timeinfo; localtime_r(&now, &timeinfo); // แปลงโครงสร้างเวลา local[cite: 63]
  if (timeinfo.tm_year < 120) return "--/--/----";    // คืนค่าเริ่มต้นถ้ายังไม่ซิงค์เวลา[cite: 63]
  char buf[20]; strftime(buf, sizeof(buf), "%d/%m/%Y", &timeinfo); // จัดรูปแบบสตริง[cite: 63]
  return String(buf);
}

// ฟังก์ชันจัดรูปแบบเวลาเปิดบอร์ด (Uptime)[cite: 63]
String getFormattedUptime() {
  unsigned long sec = millis() / 1000; // แปลงมิลลิวินาทีเป็นวินาที[cite: 63]
  int d = sec / 86400;                 // จำนวนวัน[cite: 63]
  int h = (sec % 86400) / 3600;        // จำนวนชั่วโมง[cite: 63]
  int m = (sec % 3600) / 60;           // จำนวนนาที[cite: 63]
  int s = sec % 60;                    // จำนวนวินาที[cite: 63]
  char buf[30];
  if (d > 0) snprintf(buf, sizeof(buf), "%dd %02dh %02dm %02ds", d, h, m, s); // รูปแบบมีวัน[cite: 63]
  else snprintf(buf, sizeof(buf), "%02dh %02dm %02ds", h, m, s);              // รูปแบบไม่มีวัน[cite: 63]
  return String(buf);
}

// ฟังก์ชันเริ่มต้นหน้าจอ OLED[cite: 63]
void initOLED() {
  Wire.begin(OLED_SDA, OLED_SCL);     // เริ่มต้นบัส I2C[cite: 63]
  Wire.setTimeOut(1000);              // กำหนด Timeout I2C[cite: 63]
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { // เชื่อมต่อ OLED Address 0x3C[cite: 63]
    oledReady = false;                // หากล้มเหลว[cite: 63]
  } else {
    oledReady = true;                 // จอพร้อมใช้งาน[cite: 63]
    display.clearDisplay();           // ล้างหน้าจอ[cite: 63]
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 20);
    display.println("BOOTING NODE A..."); // แสดงสถานะกำลังเปิดเครื่อง[cite: 63]
    display.display();
  }
}

// ฟังก์ชันอัปเดตข้อมูลบนหน้าจอ OLED[cite: 63]
void updateOLED() {
  if (!oledReady) return;             // ถ้าจอไม่พร้อม ให้ข้ามไป[cite: 63]
  display.clearDisplay();             // ล้างหน้าจอ[cite: 63]
  display.fillRect(0, 0, 128, 14, SSD1306_WHITE); // วาดแถบสีขาวด้านบน[cite: 63]
  display.setTextColor(SSD1306_BLACK); // ตัวอักษรสีดำ[cite: 63]
  display.setTextSize(1);
  display.setCursor(4, 3);
  display.print(oledPage == 0 ? "NODE A: SYSTEM DASH" : "NODE A: NETWORK AP"); // ชื่อหน้าปัจจุบัน[cite: 63, 64]
  display.setTextColor(SSD1306_WHITE); // เปลี่ยนตัวอักษรเป็นสีขาว[cite: 64]
  display.drawFastHLine(0, 14, 128, SSD1306_WHITE); // วาดเส้นแบ่ง[cite: 64]

  float v = getAccurateBatteryVoltage();        // อ่านแรงดันแบตเตอรี่[cite: 64]
  int pct = calculateBatteryPercentage(v);       // คำนวณ % แบตเตอรี่[cite: 64]
  unsigned long now = millis();
  bool activeB = (now - nodeB.lastSeen < 60000) && nodeB.active; // เช็ค Node B ออนไลน์[cite: 64]
  bool activeC = (now - nodeC.lastSeen < 60000) && nodeC.active; // เช็ค Node C ออนไลน์[cite: 64]

  if (oledPage == 0) { // หน้าแสดง วันที่/เวลา/สถานะแบตเตอรี่[cite: 64]
    display.setCursor(0, 17); display.print("DATE: "); display.print(getFormattedDate()); // แสดงวันที่[cite: 64]
    display.setTextSize(2); display.setCursor(16, 27); display.print(getFormattedTime());  // แสดงเวลา[cite: 64]
    display.drawFastHLine(0, 44, 128, SSD1306_WHITE);
    display.setTextSize(1); display.setCursor(0, 47); display.print("UP  : "); display.print(getFormattedUptime()); // แสดง Uptime[cite: 64]
    display.setCursor(0, 56); display.printf("BAT : %3d%% (%.2fV)", pct, v);             // แสดง % แบตเตอรี่[cite: 64]
  } else {             // หน้าแสดงสถานะเครือข่าย WiFi/LoRa[cite: 64]
    display.setCursor(0, 17); display.print("SSID: Emergency_Help");                       // ชื่อ AP[cite: 64]
    display.setCursor(0, 27); display.print("IP  : 192.168.4.1");                           // IP Address[cite: 64]
    display.setCursor(0, 37); display.printf("Sent: %d | RSSI:%ddBm", msgCounter, lastRssi);// ข้อความที่ส่งและ RSSI[cite: 64]
    display.drawFastHLine(0, 46, 128, SSD1306_WHITE);
    display.setCursor(0, 49); display.print("Node B: "); display.print(activeB ? "ONLINE" : "OFFLINE"); // สถานะ Node B[cite: 64]
    display.setCursor(0, 57); display.print("Node C: "); display.print(activeC ? "ONLINE" : "OFFLINE"); // สถานะ Node C[cite: 64]
  }
  display.display(); // แสดงผลภาพขึ้น OLED[cite: 64]
}

// โค้ดซอร์ส HTML/CSS/JS หน้าเว็บสำหรับผู้ประสบภัย (เก็บบน Flash Memory PROGMEM)[cite: 64]
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="th">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <title>SOS Emergency Chat</title>
    <!-- [สรุป] หน้าเว็บแสดงผลส่วนแชทฉุกเฉิน, สถานะเสา Node B/C, สถานะการส่งข้อความสด และตั้งค่าพิกัดเสา -->
</head>
<body>...</body>
</html>
)rawliteral"; // [ย่อส่วน HTML เพื่อกระชับพื้นที่ แต่ฟังก์ชันยังคงเดิม][cite: 64, 65, 66, 67, 68, 69, 70, 71]

// ฟังก์ชันดักจับเบราว์เซอร์แล้วเปลี่ยนทิศทางมายังหน้าเว็บหลัก (Captive Portal)[cite: 71]
void handleCaptiveRedirect(AsyncWebServerRequest *request) {
  request->redirect("http://192.168.4.1/"); // ส่งเบราว์เซอร์ไปที่ IP ของ Node A[cite: 71]
}

// ฟังก์ชันเริ่มต้นทำงานหลัก (Setup)[cite: 71]
void setup() {
  Serial.begin(115200);        // เปิด Serial Monitor ความเร็ว 115200 bps[cite: 71]
  delay(1000);

  setenv("TZ", "ICT-7", 1);     // กำหนด Timezone ไทย[cite: 71]
  tzset();

  setupWatchdog();             // เริ่มเปิดการทำงาน Watchdog Timer[cite: 71]
  loadNodeConfig();            // โหลดคอนฟิกจาก Flash Memory[cite: 71]

  Serial.println("\n--- STARTING NODE A (VICTIM AP NODE) ---");

  pinMode(BAT_ADC_PIN, INPUT); // กำหนดขาอ่านแบตเตอรี่เป็น Input[cite: 71]
  initOLED();                  // เริ่มระบบ OLED[cite: 71]
  delay(300);

  WiFi.mode(WIFI_AP);          // ตั้งค่าเป็นโหมด WiFi Access Point[cite: 72]
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0)); // กำหนด IP[cite: 72]
  WiFi.softAP("Emergency_Help_NodeA", "12345678", 1, 0, 8); // ปล่อย SSID และ รหัสผ่าน[cite: 72]
  WiFi.setTxPower(WIFI_POWER_13dBm); // กำหนดกำลังส่ง WiFi[cite: 72]

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP()); // เปิดใช้งาน DNS Server ให้ทุก URL วิ่งมาที่บอร์ด[cite: 72]
  delay(200);

  SPI.begin(SCK, MISO, MOSI, SS); // เริ่มต้นการเชื่อมต่อ SPI กับ LoRa[cite: 72]
  LoRa.setPins(SS, RST, DIO0);    // กำหนดขาควบคุม LoRa[cite: 72]
  while (!LoRa.begin(BAND)) {     // เริ่มต้นเปิดระบบ LoRa ความถี่ 923MHz[cite: 72]
    Serial.println("LoRa Init Failed! Retrying...");
    delay(1000); yield();
  }

  configureLoRaRadio();        // ตั้งค่าปรับแต่งพารามิเตอร์ส่ง LoRa ระยะไกล[cite: 72]

  // กำหนด Handler เส้นทางต่างๆ บน Async Web Server[cite: 72]
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html; charset=utf-8", index_html); // ส่งหน้าเว็บหลัก[cite: 72]
  });

  // ดักจับการเช็คอินเทอร์เน็ตของมือถือระบบต่างๆ (Captive Portal)[cite: 72]
  server.on("/generate_204", HTTP_GET, handleCaptiveRedirect);
  server.on("/redirect", HTTP_GET, handleCaptiveRedirect);
  server.on("/hotspot-detect.html", HTTP_GET, handleCaptiveRedirect);
  server.on("/canonical.html", HTTP_GET, handleCaptiveRedirect);
  server.on("/connecttest.txt", HTTP_GET, handleCaptiveRedirect);

  // API คำสั่งรีสตาร์ทบอร์ด Node A[cite: 72]
  server.on("/api/reboot", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", "REBOOTING"); // ตอบกลับเบราว์เซอร์[cite: 72]
    delay(500); performReboot();                  // ทำการรีสตาร์ท[cite: 72]
  });

  // API สำหรับอัปเดตพิกัดตำแหน่งของ Node A[cite: 72]
  server.on("/api/set_location", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("loc")) {
      saveLocationConfig(request->getParam("loc")->value()); // บันทึกพิกัดใหม่[cite: 72]
    }
    request->send(200, "text/plain", "OK");
  });

  // API สำหรับรับข้อความฉุกเฉินจากหน้าเว็บเบราว์เซอร์ของผู้ใช้[cite: 73]
  server.on("/send", HTTP_GET, [](AsyncWebServerRequest *request){
    String uid = request->getParam("uid")->value();     // อ่าน UID ผู้ใช้[cite: 73]
    String user = request->getParam("user")->value();   // อ่านชื่อผู้ใช้[cite: 73]
    String text = request->getParam("text")->value();   // อ่านข้อความ[cite: 73]
    if (text.length() > 120) text = text.substring(0, 120); // ตัดข้อความไม่ให้เกิน 120 ตัวอักษร[cite: 73]
    if (user.length() > 40) user = user.substring(0, 40);

    msgCounter++; saveMsgCounterConfig(); // เพิ่มและบันทึกตัวนับข้อความ[cite: 73]

    String msgId = "A" + String(msgCounter); // สร้าง ID ข้อความ เช่น A1, A2[cite: 73]
    
    String userB64 = encodeBase64(user);   // เข้ารหัสชื่อเป็น Base64[cite: 73]
    String textB64 = encodeBase64(text);   // เข้ารหัสข้อความภาษาไทยเป็น Base64[cite: 73]

    // สร้างโครงสร้างแพ็กเกจ LoRa[cite: 73]
    String packetPayload = msgId + "|3|A|C|MSG|" + uid + "|" + userB64 + "|" + currentLocationA + "|" + textB64; //[cite: 73]
    
    // ยัดเข้าตารางรอส่งซ้ำ (Auto-Retry) ส่งสูงสุด 4 ครั้ง ทุก 3 วินาที[cite: 73]
    {
      std::lock_guard<std::mutex> lock(retryMutex);
      pendingRetryMsgs.push_back({msgId, packetPayload, 4, millis()}); //[cite: 73]
    }

    // ยัดเข้า Queue รอส่งออกคลื่นวิทยุ LoRa[cite: 73]
    {
      std::lock_guard<std::mutex> lock(txMutex);
      txQueue.push(packetPayload); //[cite: 73]
    }

    String resp = "{\"status\":\"OK\",\"msgId\":\"" + msgId + "\"}"; // สร้าง JSON ตอบกลับมือถือ[cite: 73]
    request->send(200, "application/json; charset=utf-8", resp); //[cite: 73]
  });

  // API ดึงข้อความตอบกลับและอัปเดตสถานะจากศูนย์กู้ภัย[cite: 73]
  server.on("/get", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("ts")) syncTimeFromEpoch(request->getParam("ts")->value().toInt()); // ซิงค์เวลา[cite: 73]
    if (!request->hasParam("uid")) {
      request->send(200, "application/json; charset=utf-8", "{\"replies\":[],\"statuses\":[]}");
      return;
    }
    String reqUid = request->getParam("uid")->value(); reqUid.trim(); //[cite: 73]

    String json = "{\"replies\":["; // เริ่มสร้างสตริง JSON[cite: 73]
    {
      std::lock_guard<std::mutex> lock(dataMutex);
      bool first = true;
      auto it = pendingReplies.begin();
      while (it != pendingReplies.end()) {
        if (it->targetUid == reqUid) {
          if (!first) json += ",";
          json += "{\"officer\":\"" + it->officer + "\",\"text\":\"" + it->text + "\"}"; // ข้อความตอบกลับ[cite: 74]
          first = false;
          it = pendingReplies.erase(it); // ลบออกจากรายการเมื่อส่งให้มือถือแล้ว[cite: 74]
        } else ++it;
      }
      json += "],\"statuses\":[";
      first = true;
      auto itS = pendingStatusUpdates.begin();
      while (itS != pendingStatusUpdates.end()) {
        if (!first) json += ",";
        json += "{\"msgId\":\"" + itS->msgId + "\",\"status\":\"" + itS->status + "\",\"officer\":\"" + itS->officer + "\"}"; // อัปเดตสถานะ[cite: 74]
        first = false;
        itS = pendingStatusUpdates.erase(itS); // ลบออกจากรายการ[cite: 74]
      }
    }
    json += "]}";
    request->send(200, "application/json; charset=utf-8", json); // ส่ง JSON ให้เบราว์เซอร์[cite: 74]
  });

  // API ดึงสถานะระบบไปแสดงบนหน้ามือถือ[cite: 74]
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("ts")) syncTimeFromEpoch(request->getParam("ts")->value().toInt()); // ซิงค์เวลา[cite: 74]
    float volts = getAccurateBatteryVoltage();        // อ่านแบตเตอรี่[cite: 74]
    int batPct = calculateBatteryPercentage(volts);     // คำนวณ %[cite: 74]
    unsigned long now = millis();
    bool activeB = (now - nodeB.lastSeen < 60000) && nodeB.active; // เช็ค Node B[cite: 74]
    bool activeC = (now - nodeC.lastSeen < 60000) && nodeC.active; // เช็ค Node C[cite: 74]

    String json = "{"; // สร้าง JSON สถานะ[cite: 74]
    json += "\"bat_pct\":" + String(batPct) + ",";
    json += "\"bat_v\":\"" + String(volts, 2) + "\",";
    json += "\"rssi\":" + String(lastRssi) + ",";
    json += "\"loc\":\"" + currentLocationA + "\",";
    json += "\"node_b\":" + String(activeB ? "true" : "false") + ",";
    json += "\"rssi_b\":" + String(nodeB.rssi) + ",";
    json += "\"node_c\":" + String(activeC ? "true" : "false") + ",";
    json += "\"rssi_c\":" + String(nodeC.rssi);
    json += "}";
    request->send(200, "application/json", json); // ส่งสถานะกลับไป[cite: 75]
  });

  server.onNotFound(handleCaptiveRedirect); // ปลายทางอื่นให้ Redirect มาหน้าหลัก[cite: 75]
  server.begin(); // เปิดเริ่มทำงาน Web Server[cite: 75]
}

// ฟังก์ชันทำงานวนลูปต่อเนื่อง (Main Loop)[cite: 75]
void loop() {
  esp_task_wdt_reset();        // รีเซ็ตตัวนับ Watchdog ป้องกันบอร์ดรีบูต[cite: 75]
  dnsServer.processNextRequest(); // ประมวลผลคำร้องขอ Captive Portal DNS[cite: 75]

  // สลับหน้า OLED ทุกๆ 20 วินาที[cite: 75]
  if (millis() - lastOledSwitch > 20000) {
    lastOledSwitch = millis();
    oledPage = (oledPage + 1) % 2; //[cite: 75]
  }
  
  // อัปเดตหน้าจอ OLED ทุกๆ 1 วินาที[cite: 75]
  if (millis() - lastOledRender > 1000) {
    lastOledRender = millis();
    updateOLED(); //[cite: 75]
  }

  // ดึงแพ็กเกจจาก Queue ออกมาส่งทางวิทยุ LoRa[cite: 75]
  String packetToSend = "";
  {
    std::lock_guard<std::mutex> lock(txMutex);
    if (!txQueue.empty()) {
      packetToSend = txQueue.front(); // อ่านแพ็กเกจหัวคิว[cite: 75]
      txQueue.pop();                 // ดึงออกจากคิว[cite: 75]
    }
  }
  if (packetToSend.length() > 0) {
    sendLoRaRaw(packetToSend);      // สั่งส่งออกคลื่นวิทยุ[cite: 75]
    delay(40);                      // หน่วงเวลาสั้นๆ ป้องกันคลื่นชนกัน[cite: 75]
  }

  // ระบบ Auto-Retry (ARQ) ตรวจสอบข้อความที่ยังไม่ได้รับตอบรับ ACK[cite: 75]
  {
    std::lock_guard<std::mutex> lock(retryMutex);
    for (auto it = pendingRetryMsgs.begin(); it != pendingRetryMsgs.end(); ) {
      if (millis() - it->lastSentTime > 3000) { // ถ้าผ่านไปเกิน 3 วินาทีแล้วยังไม่ได้รับ ACK[cite: 75]
        if (it->retriesLeft > 0) {             // ถ้ายังเหลือโควตาการส่งซ้ำ[cite: 75]
          {
            std::lock_guard<std::mutex> qLock(txMutex);
            txQueue.push(it->fullPacket);      // ใส่แพ็กเกจลง Queue เพื่อส่งอีกครั้ง[cite: 76]
          }
          it->retriesLeft--;                  // ลดจำนวนครั้งส่งซ้ำลง 1[cite: 76]
          it->lastSentTime = millis();         // รีเซ็ตเวลาส่ง[cite: 76]
          ++it;
        } else {
          it = pendingRetryMsgs.erase(it);    // ลบออกจากรายการเมื่อลองส่งครบ 4 รอบแล้ว[cite: 76]
        }
      } else {
        ++it;
      }
    }
  }

  // ระบบกระจายสัญญาณ Heartbeat Node A (พร้อมแนบเวลา Unix Epoch Timestamp)[cite: 76]
  if (millis() - lastHeartbeatTime > heartbeatInterval) {
    lastHeartbeatTime = millis();
    heartbeatInterval = 35000 + random(0, 5000); // หน่วงเวลาแบบสุ่มป้องกันส่งพร้อมเสาอื่น[cite: 76]
    float v = getAccurateBatteryVoltage();        // อ่านแบตเตอรี่[cite: 76]
    int pct = calculateBatteryPercentage(v);       // คำนวณ % แบตเตอรี่[cite: 76]
    hbCounterA++;                                 // นับจำนวนแพ็กเกจ Heartbeat[cite: 76]
    
    uint32_t freeHeap = ESP.getFreeHeap();        // อ่าน RAM เหลือ[cite: 76]
    uint32_t uptimeSec = millis() / 1000;          // อ่านเวลา Uptime[cite: 76]
    int wifiClients = WiFi.softAPgetStationNum();  // อ่านจำนวนคนต่อ WiFi[cite: 76]
    unsigned long currentEpoch = (unsigned long)time(NULL); // ดึงเวลา Epoch ปัจจุบัน[cite: 76]

    // ประกอบแพ็กเกจ Heartbeat[cite: 76]
    String hbPacket = "HBA" + String(hbCounterA) + "|3|A|ALL|HB|VictimNode|" + currentLocationA + "|" + String(pct) + "|" + String(v, 2) + "|" + String(freeHeap) + "|" + String(uptimeSec) + "|" + String(wifiClients) + "|" + String(currentEpoch); //[cite: 76]
    {
      std::lock_guard<std::mutex> lock(txMutex);
      txQueue.push(hbPacket);                      // ใส่ลง Queue เพื่อส่งกระจายคลื่น[cite: 76]
    }
  }

  // ระบบรับคลื่นวิทยุ LoRa เข้ามาประมวลผล[cite: 76]
  int packetSize = LoRa.parsePacket(); // ตรวจจับขนาดแพ็กเกจรับเข้า[cite: 76]
  if (packetSize) {
    String rawIncoming = "";
    while (LoRa.available()) rawIncoming += (char)LoRa.read(); // อ่านข้อมูลจนครบ[cite: 76]
    rawIncoming.trim();
    lastRssi = LoRa.packetRssi(); // ดึงค่าความแรงสัญญาณ RSSI[cite: 76, 77]
    lastSnr = LoRa.packetSnr();   // ดึงค่าคุณภาพสัญญาณ SNR[cite: 77]

    int pLast = rawIncoming.lastIndexOf('|'); // ค้นหาจุดแยกค่า CRC[cite: 77]
    if (pLast == -1) return;
    String payload = rawIncoming.substring(0, pLast); // ดึงเนื้อหาแพ็กเกจ[cite: 77]
    uint16_t rxCrc = (uint16_t)strtol(rawIncoming.substring(pLast + 1).c_str(), NULL, 16); // อ่านค่า CRC ที่แนบมา[cite: 77]
    if (calculateCRC16(payload) != rxCrc) return;     // เช็ค CRC หากไม่ตรงกันให้ทิ้งแพ็กเกจทันที[cite: 77]

    std::vector<String> tokens = splitString(payload, '|'); // แยกชิ้นส่วนแพ็กเกจด้วยตัวคั่น `|`[cite: 77]
    if (tokens.size() < 5) return;

    String senderNode = tokens[2]; // ผู้ส่ง[cite: 77]
    String targetNode = tokens[3]; // ผู้รับปลายทาง[cite: 77]
    String msgType = tokens[4];    // ประเภทข้อความ[cite: 77]

    // บันทึกสถานะการออนไลน์ของ Node B หรือ Node C[cite: 77]
    if (senderNode == "B") {
      nodeB.rssi = lastRssi; nodeB.snr = lastSnr; nodeB.lastSeen = millis(); nodeB.active = true; //[cite: 77]
    } else if (senderNode == "C") {
      nodeC.rssi = lastRssi; nodeC.snr = lastSnr; nodeC.lastSeen = millis(); nodeC.active = true; //[cite: 77]
    }

    if (targetNode != "A" && targetNode != "ALL") return; // ข้ามแพ็กเกจที่ไม่ใช่ของ Node A[cite: 77]

    // แพ็กเกจยืนยันการรับข้อความ ACK จากศูนย์กู้ภัย[cite: 77]
    if (msgType == "ACK" && tokens.size() >= 8) {
      String targetMsgId = tokens[5]; // รหัสข้อความที่ได้รับการยอมรับ[cite: 77]
      String uid = tokens[6];         // UID ผู้รับ[cite: 77]
      String status = tokens[7];      // สถานะ (DELIVERED/CLAIMED/RESOLVED)[cite: 77]
      String officer = (tokens.size() >= 9) ? decodeBase64(tokens[8]) : "HQ"; // ชื่อเจ้าหน้าที่[cite: 77]

      // ลบข้อความออกจากรายการรอส่งซ้ำ (Auto-Retry) ทันที[cite: 77]
      {
        std::lock_guard<std::mutex> lock(retryMutex);
        for (auto it = pendingRetryMsgs.begin(); it != pendingRetryMsgs.end(); ) {
          if (it->msgId == targetMsgId) it = pendingRetryMsgs.erase(it); //[cite: 77, 78]
          else ++it;
        }
      }

      // ยัดข้อมูลอัปเดตสถานะให้มือถือดึงไปแสดงผล[cite: 78]
      std::lock_guard<std::mutex> lock(dataMutex);
      pendingStatusUpdates.push_back({targetMsgId, status, officer}); //[cite: 78]
    }
    
    // แพ็กเกจข้อความตอบกลับ REPLY จากเจ้าหน้าที่[cite: 78]
    if (msgType == "REPLY" && tokens.size() >= 9) {
      String replyMsgId = tokens[0]; // ID ข้อความตอบกลับ[cite: 78]
      if (isDuplicate(replyMsgId)) return; // ข้ามถ้าประมวลผลไปแล้ว[cite: 78]

      String targetUid = tokens[5];           // UID เป้าหมาย[cite: 78]
      String officer = decodeBase64(tokens[6]); // ชื่อเจ้าหน้าที่ถอดรหัสแล้ว[cite: 78]
      String text = decodeBase64(tokens[8]);    // ข้อความตอบกลับถอดรหัสแล้ว[cite: 78]

      std::lock_guard<std::mutex> lock(dataMutex);
      pendingReplies.push_back({targetUid, officer, text}); // ยัดเข้าคิวให้มือถือดึง[cite: 78]
    }

    // คำสั่งรีสตาร์ทบอร์ดระยะไกล CMD[cite: 78]
    if (msgType == "CMD" && tokens.size() >= 6) {
      if (tokens[5] == "REBOOT") performReboot(); // รีเซ็ตบอร์ดทันที[cite: 78]
    }
  }
}
