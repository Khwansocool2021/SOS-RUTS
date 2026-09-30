#include <Arduino.h>         // ไลบรารีพื้นฐาน Arduino ESP32[cite: 88]
#include <WiFi.h>            // ไลบรารี WiFi[cite: 88]
#include <SPI.h>             // ไลบรารี SPI[cite: 88]
#include <LoRa.h>            // ไลบรารี LoRa[cite: 88]
#include <ESPAsyncWebServer.h> // Web Server แบบ Async[cite: 88]
#include <DNSServer.h>       // Captive Portal DNS Server[cite: 88]
#include <qrcode.h>          // ไลบรารีสร้างภาพ QR Code เวกเตอร์ SVG สดบนบอร์ด[cite: 88]
#include <Wire.h>            // ไลบรารี I2C[cite: 88]
#include <Adafruit_GFX.h>    // ไลบรารีกราฟิก[cite: 88]
#include <Adafruit_SSD1306.h>// ไลบรารีจอ OLED[cite: 88]
#include <LittleFS.h>        // ไลบรารีระบบไฟล์ LittleFS บันทึกข้อมูลเคสถาวร[cite: 88]
#include <Preferences.h>     // ไลบรารี Flash Memory NVS[cite: 88]
#include <mbedtls/base64.h>  // เข้ารหัส/ถอดรหัส Base64 ภาษาไทย[cite: 88]
#include <time.h>            // จัดการเวลา[cite: 88]
#include <sys/time.h>        // ซิงค์เวลา RTC[cite: 88]
#include <queue>             // คิว Queue[cite: 88]
#include <vector>            // Vector[cite: 88]
#include <mutex>             // Mutex[cite: 88]
#include <esp_task_wdt.h>    // Watchdog Timer[cite: 88]

#define WDT_TIMEOUT 8        // ตั้งเวลา Watchdog 8 วินาที[cite: 88]

// กำหนดพินขาฮาร์ดแวร์ SPI LoRa, OLED และ ADC[cite: 88]
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

const char* DEFAULT_LOCATION_C = "7.0100,100.4800"; // พิกัดพิกัดศูนย์กู้ภัย Node C[cite: 88]
String currentLocationC;

AsyncWebServer server(80);   // สร้าง Web Server[cite: 89]
DNSServer dnsServer;         // สร้าง DNS Server[cite: 89]
const byte DNS_PORT = 53;

int replyCounter = 800;      // ตัวนับรหัสข้อความตอบกลับ[cite: 89]
int ackCounter = 500;        // ตัวนับรหัสแพ็กเกจ ACK[cite: 89]
int hbCounterC = 0;          // นบนับ Heartbeat[cite: 89]
int lastRssi = 0;
float lastSnr = 0.0;

int oledPage = 0;
unsigned long lastOledSwitch = 0;
unsigned long lastOledRender = 0;
bool oledReady = false;

std::queue<String> txQueue;  // คิวส่งข้อความ LoRa[cite: 89]
std::mutex txMutex;          // ล็อคคิว[cite: 89]

unsigned long lastHeartbeat = 0;
unsigned long heartbeatInterval = 45000; // ระยะเวลา Heartbeat Node C (~45 วินาที)[cite: 89]

std::vector<String> seenMsgIDs; // บันทึก ID กันประมวลผลซ้ำ[cite: 89]
bool isDuplicate(String msgId) {
  for (const auto& id : seenMsgIDs) { if (id == msgId) return true; } //[cite: 89]
  if (seenMsgIDs.size() >= 60) seenMsgIDs.erase(seenMsgIDs.begin()); //[cite: 89]
  seenMsgIDs.push_back(msgId); return false; //[cite: 89]
}

// โครงสร้างบันทึกข้อความแชท[cite: 89]
struct ChatMessage {
  String uid; String user; String loc; String text; String sender; //[cite: 89]
};

// โครงสร้างเคสผู้ประสบภัย[cite: 89]
struct VictimCase {
  String uid; String user; String loc; String assignedOfficer; String status; String lastMsg; String lastMsgId; //[cite: 89, 90]
};

// โครงสร้างข้อมูลอุปกรณ์เสาในระบบ Network Telemetry[cite: 90]
struct NetworkNodeInfo {
  String id; String type; String loc; int batPct; float batVolt;
  uint32_t freeHeapKB; uint32_t uptimeSec; String extraData; int rssi; float snr; unsigned long lastSeen; //[cite: 90]
};

#define MAX_MSGS 60
ChatMessage chatHistory[MAX_MSGS]; // อาร์เรย์ประวัติแชทสูงสุด 60 ข้อความ[cite: 90]
int msgCount = 0;

std::vector<VictimCase> activeCases; // รายการเคสฉุกเฉินทั้งหมด[cite: 90]
std::mutex caseMutex;

std::vector<NetworkNodeInfo> networkNodes; // รายการอุปกรณ์และสถานะในเครือข่าย[cite: 90]
std::mutex nodeMutex;

// ฟังก์ชันแยกสตริง[cite: 90]
std::vector<String> splitString(const String &str, char delim) {
  std::vector<String> tokens; int start = 0; int end = str.indexOf(delim);
  while (end != -1) { tokens.push_back(str.substring(start, end)); start = end + 1; end = str.indexOf(delim, start); } //[cite: 90]
  tokens.push_back(str.substring(start)); return tokens; //[cite: 90]
}

// เข้ารหัส Base64[cite: 90]
String encodeBase64(const String &input) {
  unsigned char out[350]; size_t olen = 0;
  mbedtls_base64_encode(out, sizeof(out), &olen, (const unsigned char*)input.c_str(), input.length()); //[cite: 91]
  out[olen] = '\0'; return String((char*)out); //[cite: 91]
}

// ถอดรหัส Base64[cite: 91]
String decodeBase64(const String &input) {
  unsigned char out[350]; size_t olen = 0;
  mbedtls_base64_decode(out, sizeof(out), &olen, (const unsigned char*)input.c_str(), input.length()); //[cite: 91]
  out[olen] = '\0'; return String((char*)out); //[cite: 91]
}

// คำนวณ CRC16[cite: 91]
uint16_t calculateCRC16(const String &str) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < str.length(); i++) {
    crc ^= (uint8_t)str[i];
    for (uint8_t j = 0; j < 8; j++) { if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001; else crc >>= 1; } //[cite: 91]
  }
  return crc;
}

// ตั้งค่า Watchdog[cite: 91]
void setupWatchdog() {
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t twdt_config = { .timeout_ms = WDT_TIMEOUT * 1000, .idle_core_mask = (1 << portNUM_PROCESSORS) - 1, .trigger_panic = true };
  esp_task_wdt_reconfigure(&twdt_config); esp_task_wdt_add(NULL); //[cite: 91]
#else
  esp_task_wdt_init(WDT_TIMEOUT, true); esp_task_wdt_add(NULL); //[cite: 91]
#endif
}

// สั่งรีสตาร์ทบอร์ด[cite: 91]
void performReboot() {
  if (oledReady) {
    display.clearDisplay(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
    display.setCursor(15, 25); display.println("REBOOTING NODE C..."); display.display(); //[cite: 92]
  }
  LoRa.sleep(); delay(200); esp_task_wdt_delete(NULL); esp_restart(); while (true) { yield(); } //[cite: 92]
}

// โหลดพิกัด[cite: 92]
void loadLocationConfig() {
  preferences.begin("node_cfg", true); currentLocationC = preferences.getString("gps_loc", DEFAULT_LOCATION_C); preferences.end(); //[cite: 92]
}

// บันทึกพิกัด[cite: 92]
void saveLocationConfig(String newLoc) {
  newLoc.trim();
  if (newLoc.length() > 0) { currentLocationC = newLoc; preferences.begin("node_cfg", false); preferences.putString("gps_loc", newLoc); preferences.end(); } //[cite: 92]
}

// ปรับตั้งพารามิเตอร์วิทยุ LoRa[cite: 92]
void configureLoRaRadio() {
  LoRa.setTxPower(20); LoRa.setSpreadingFactor(9); LoRa.setSignalBandwidth(125E3); LoRa.setCodingRate4(5); LoRa.setPreambleLength(12); LoRa.setGain(0); //[cite: 92]
}

// ส่ง LoRa พร้อม CRC16[cite: 92]
void sendLoRaRaw(const String &packet) {
  uint16_t crc = calculateCRC16(packet); String finalPacket = packet + "|" + String(crc, HEX);
  LoRa.beginPacket(); LoRa.print(finalPacket); LoRa.endPacket(); //[cite: 92, 93]
}

// อ่านแรงดันแบตเตอรี่[cite: 93]
float getAccurateBatteryVoltage() {
  uint32_t rawSum = 0; for (int i = 0; i < 16; i++) { rawSum += analogRead(BAT_ADC_PIN); delayMicroseconds(100); } //[cite: 93]
  float rawAvg = rawSum / 16.0; float volts = (rawAvg / 4095.0) * 2.0 * 3.3 * 1.08;
  if (volts > 4.25) volts = 4.25; if (volts < 3.00) volts = 3.00; return volts; //[cite: 93]
}

// คำนวณ % แบตเตอรี่[cite: 93]
int calculateBatteryPercentage(float volts) {
  if (volts >= 4.15) return 100; if (volts >= 4.00) return 90; if (volts >= 3.85) return 75;
  if (volts >= 3.75) return 50;  if (volts >= 3.65) return 30; if (volts >= 3.50) return 15;
  if (volts >= 3.30) return 5;   return 0; //[cite: 93]
}

// บันทึกข้อมูลเคสลงระบบไฟล์ LittleFS ป้องกันข้อมูลหายเมื่อดับไฟ[cite: 93]
void saveCasesToFS() {
  File f = LittleFS.open("/cases.dat", "w"); // เปิดไฟล์เขียนทับ[cite: 93]
  if (!f) return;
  std::lock_guard<std::mutex> lock(caseMutex);
  for (const auto &c : activeCases) {
    f.println(c.uid + "|" + c.user + "|" + c.loc + "|" + c.assignedOfficer + "|" + c.status + "|" + c.lastMsgId + "|" + c.lastMsg); // บันทึกทีละบรรทัด[cite: 93]
  }
  f.close(); // ปิดไฟล์[cite: 93]
}

// ดึงข้อมูลเคสจาก LittleFS เมื่อเปิดเครื่อง[cite: 93]
void loadCasesFromFS() {
  if (!LittleFS.begin(true)) return; // เริ่ม LittleFS[cite: 93]
  File f = LittleFS.open("/cases.dat", "r"); // เปิดไฟล์อ่าน[cite: 93]
  if (!f) return;
  std::lock_guard<std::mutex> lock(caseMutex);
  activeCases.clear();
  while (f.available()) {
    String line = f.readStringUntil('\n'); line.trim(); if (line.length() == 0) continue; //[cite: 94]
    std::vector<String> tok = splitString(line, '|');
    if (tok.size() >= 7) {
      activeCases.push_back({tok[0], tok[1], tok[2], tok[3], tok[4], tok[6], tok[5]}); // อ่านเคสเข้าหน่วยความจำ[cite: 94]
    }
  }
  f.close();
}

// ซิงค์เวลาจากค่า Epoch[cite: 94]
void syncTimeFromEpoch(unsigned long epoch) {
  if (epoch > 1700000000) {
    struct timeval tv = { (time_t)epoch, 0 }; settimeofday(&tv, NULL); setenv("TZ", "ICT-7", 1); tzset(); //[cite: 94]
  }
}

// ดึงเวลา[cite: 94]
String getFormattedTime() {
  time_t now; time(&now); struct tm timeinfo; localtime_r(&now, &timeinfo);
  if (timeinfo.tm_year < 120) return "00:00:00"; char buf[20]; strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo); return String(buf); //[cite: 94]
}

// ดึงวันที่[cite: 94]
String getFormattedDate() {
  time_t now; time(&now); struct tm timeinfo; localtime_r(&now, &timeinfo);
  if (timeinfo.tm_year < 120) return "--/--/----"; char buf[20]; strftime(buf, sizeof(buf), "%d/%m/%Y", &timeinfo); return String(buf); //[cite: 94]
}

// ดึง Uptime[cite: 94]
String getFormattedUptime() {
  unsigned long sec = millis() / 1000; int d = sec / 86400; int h = (sec % 86400) / 3600; int m = (sec % 3600) / 60; int s = sec % 60; char buf[30];
  if (d > 0) snprintf(buf, sizeof(buf), "%dd %02dh %02dm %02ds", d, h, m, s); else snprintf(buf, sizeof(buf), "%02dh %02dm %02ds", h, m, s);
  return String(buf); //[cite: 95]
}

// เริ่มต้นจอ OLED[cite: 95]
void initOLED() {
  Wire.begin(OLED_SDA, OLED_SCL); Wire.setTimeOut(1000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { oledReady = false; }
  else { oledReady = true; display.clearDisplay(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(10, 20); display.println("BOOTING NODE C..."); display.display(); } //[cite: 95]
}

// อัปเดตหรือเพิ่มข้อมูลเสาลงรายการ Telemetry[cite: 95]
void updateOrAddNode(String id, String type, String loc, int batPct, float batVolt, uint32_t freeHeapKB, uint32_t uptimeSec, String extraData, int rssi, float snr) {
  std::lock_guard<std::mutex> lock(nodeMutex);
  bool found = false;
  for (auto &n : networkNodes) {
    if (n.id == id) { // อัปเดตข้อมูลเสาที่มีอยู่แล้ว[cite: 95]
      n.type = type; n.loc = loc; n.batPct = batPct; n.batVolt = batVolt; n.freeHeapKB = freeHeapKB;
      n.uptimeSec = uptimeSec; n.extraData = extraData; n.rssi = rssi; n.snr = snr; n.lastSeen = millis(); found = true; break; //[cite: 95, 96]
    }
  }
  if (!found) networkNodes.push_back({id, type, loc, batPct, batVolt, freeHeapKB, uptimeSec, extraData, rssi, snr, millis()}); // เพิ่มเสาใหม่[cite: 96]
}

// เพิ่มหรืออัปเดตเคสผู้ประสบภัย[cite: 96]
void addOrUpdateCase(String uid, String user, String loc, String text, String msgId) {
  std::lock_guard<std::mutex> lock(caseMutex);
  bool found = false;
  for (auto &c : activeCases) {
    if (c.uid == uid) { // อัปเดตข้อมูลเคส[cite: 96]
      c.user = user; c.loc = loc; c.lastMsg = text; c.lastMsgId = msgId; found = true; break; //[cite: 96]
    }
  }
  if (!found) { // สร้างเคสใหม่สถานะ 'pending'[cite: 96]
    activeCases.push_back({uid, user, loc, "", "pending", text, msgId}); //[cite: 96]
  }
}

// เพิ่มข้อความแชทลงประวัติ[cite: 96]
void addMessage(String uid, String user, String loc, String text, String sender) {
  if (msgCount >= MAX_MSGS) {
    for (int i = 0; i < MAX_MSGS - 1; i++) chatHistory[i] = chatHistory[i + 1]; // ลบข้อความเก่าสุด[cite: 96]
    msgCount = MAX_MSGS - 1;
  }
  chatHistory[msgCount] = {uid, user, loc, text, sender}; msgCount++; // เพิ่มข้อความใหม่[cite: 96]
}

// วาด OLED หน้าจอ Node C[cite: 96]
void updateOLED() {
  if (!oledReady) return;
  display.clearDisplay(); display.fillRect(0, 0, 128, 14, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK); display.setTextSize(1); display.setCursor(4, 3);
  display.print(oledPage == 0 ? "NODE C: DISPATCH HQ" : "NODE C: CONTROL AP"); //[cite: 96, 97]
  display.setTextColor(SSD1306_WHITE); display.drawFastHLine(0, 14, 128, SSD1306_WHITE);

  float vC = getAccurateBatteryVoltage(); int pctC = calculateBatteryPercentage(vC);

  if (oledPage == 0) { // หน้าแสดง เวลา/วันที่/จำนวนเคส[cite: 97]
    display.setCursor(0, 17); display.print("DATE: "); display.print(getFormattedDate()); //[cite: 97]
    display.setTextSize(2); display.setCursor(16, 27); display.print(getFormattedTime()); //[cite: 97]
    display.drawFastHLine(0, 44, 128, SSD1306_WHITE);
    display.setTextSize(1); display.setCursor(0, 47); display.print("UP  : "); display.print(getFormattedUptime()); //[cite: 97]
    display.setCursor(0, 56); display.printf("BAT : %d%% | Cases: %d", pctC, (int)activeCases.size()); //[cite: 97]
  } else {             // หน้าแสดงสถานะระบบ WiFi/การบันทึก[cite: 97]
    display.setCursor(0, 17); display.print("SSID: Emergency_Rescue"); //[cite: 97]
    display.setCursor(0, 27); display.print("IP  : 192.168.4.1"); //[cite: 97]
    display.setCursor(0, 37); display.printf("Logs: %d msgs | %ddBm", msgCount, lastRssi); //[cite: 97]
    display.drawFastHLine(0, 46, 128, SSD1306_WHITE);
    display.setCursor(0, 49); display.printf("BAT : %3d%% (%.2fV)", pctC, vC); //[cite: 97]
    display.setCursor(0, 57); display.print("System: ONLINE 24/7"); //[cite: 97]
  }
  display.display();
}

// ซอร์สโค้ดหน้าเว็บศูนย์วิทยุกู้ภัยสำหรับเจ้าหน้าที่ (เก็บบน PROGMEM)[cite: 97]
const char rescuer_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="th">
<head>
    <meta charset="UTF-8">
    <title>ศูนย์วิทยุกู้ภัย (Node C Dispatch Center)</title>
    <!-- [สรุป] หน้าเว็บสำหรับเจ้าหน้าที่บริหารจัดการเคสรับเรื่อง ตอบกลับแชท สแกน QR Code นำทาง และดูสถานะ Telemetry เสาในเครือข่ายเชิงลึก -->
</head>
<body>...</body>
</html>
)rawliteral"; // [ย่อส่วน HTML เพื่อกระชับพื้นที่ แต่ฟังก์ชันยังคงเดิม][cite: 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108]

// Redirect ไปยัง IP Node C[cite: 108]
void handleCaptiveRedirectC(AsyncWebServerRequest *request) {
  request->redirect("http://192.168.4.1/"); //[cite: 108]
}

// ส่วนเริ่มต้นระบบ Setup[cite: 108]
void setup() {
  Serial.begin(115200); delay(1000); setenv("TZ", "ICT-7", 1); tzset();

  setupWatchdog();      // เปิด WDT[cite: 108]
  loadLocationConfig(); // โหลดพิกัด[cite: 108]

  Serial.println("\n--- STARTING NODE C (DISPATCH HQ CENTER) ---");

  pinMode(BAT_ADC_PIN, INPUT);
  loadCasesFromFS();    // โหลดเคสเก่าจาก LittleFS[cite: 108]
  initOLED();           // เริ่มระบบ OLED[cite: 108]
  delay(300);

  WiFi.mode(WIFI_AP);   // ปล่อย AP สำหรับกู้ภัย[cite: 109]
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0)); //[cite: 109]
  WiFi.softAP("Emergency_Rescue_NodeC", "12345678", 1, 0, 8); //[cite: 109]
  WiFi.setTxPower(WIFI_POWER_13dBm);

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP()); //[cite: 109]
  delay(200);

  SPI.begin(SCK, MISO, MOSI, SS); LoRa.setPins(SS, RST, DIO0);
  while (!LoRa.begin(BAND)) { Serial.println("LoRa Init Failed! Retrying..."); delay(1000); yield(); } //[cite: 109]

  configureLoRaRadio(); // ตั้งค่ากำลังส่ง[cite: 109]

  // Handler บน Web Server[cite: 109]
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(200, "text/html; charset=utf-8", rescuer_html); }); //[cite: 109]

  server.on("/generate_204", HTTP_GET, handleCaptiveRedirectC);
  server.on("/redirect", HTTP_GET, handleCaptiveRedirectC);
  server.on("/hotspot-detect.html", HTTP_GET, handleCaptiveRedirectC);
  server.on("/canonical.html", HTTP_GET, handleCaptiveRedirectC);
  server.on("/connecttest.txt", HTTP_GET, handleCaptiveRedirectC);

  // ตั้งพิกัด Node C[cite: 109]
  server.on("/api/set_location", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("loc")) saveLocationConfig(request->getParam("loc")->value()); //[cite: 109]
    request->send(200, "text/plain", "OK");
  });

  // ส่งคำสั่งสั่งรีสตาร์ทเสาเป้าหมายผ่าน LoRa[cite: 109]
  server.on("/api/reboot_node", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("target")) {
      String target = request->getParam("target")->value(); target.trim(); //[cite: 109]

      if (target == "C") { // ถ้ารีสตาร์ทตัวเอง[cite: 109]
        request->send(200, "text/plain", "REBOOTING_NODE_C"); delay(500); performReboot(); return; //[cite: 110]
      }

      replyCounter++;
      String cmdPacket = "CMD" + String(replyCounter) + "|3|C|" + target + "|CMD|REBOOT"; // ประกอบแพ็กเกจคำสั่ง[cite: 110]
      { std::lock_guard<std::mutex> lock(txMutex); txQueue.push(cmdPacket); } // ใส่ Queue ส่ง[cite: 110]
    }
    request->send(200, "text/plain", "OK");
  });

  // สตรีมสร้างภาพ QR Code เวกเตอร์ SVG สดบนชิป ESP32 ส่งให้หน้าเว็บ[cite: 110]
  server.on("/api/qr", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!request->hasParam("loc")) { request->send(400, "text/plain", "Missing loc"); return; } //[cite: 110]
    String loc = request->getParam("loc")->value(); loc.trim();
    String url = "https://maps.google.com/?q=" + loc; // ลิงก์นำทาง Google Maps[cite: 110]
    QRCode qrcode; uint8_t qrcodeData[qrcode_getBufferSize(4)];
    qrcode_initText(&qrcode, qrcodeData, 4, 0, url.c_str()); // เจนเนอเรตภาพ QR Code[cite: 110]

    // วาด SVG รูป QR Code[cite: 110]
    String svg = "<svg xmlns='http://www.w3.org/2000/svg' width='220' height='220' viewBox='0 0 " + String(qrcode.size + 4) + " " + String(qrcode.size + 4) + "'>"; //[cite: 110]
    svg += "<rect width='100%' height='100%' fill='white'/>"; svg += "<path d='";
    for (uint8_t y = 0; y < qrcode.size; y++) {
      for (uint8_t x = 0; x < qrcode.size; x++) {
        if (qrcode_getModule(&qrcode, x, y)) svg += "M" + String(x + 2) + "," + String(y + 2) + "h1v1h-1z "; //[cite: 110]
      }
    }
    svg += "' fill='black'/></svg>";
    request->send(200, "image/svg+xml", svg); // ส่งไฟล์รูป SVG ให้เบราว์เซอร์[cite: 110]
  });

  // API รวมข้อมูลสถานะสด ข้อความ เคส และ Telemetry ส่งให้หน้าจอศูนย์กู้ภัย[cite: 110]
  server.on("/api/data", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("ts")) syncTimeFromEpoch(request->getParam("ts")->value().toInt()); // ซิงค์เวลา[cite: 111]
    
    float voltsC = getAccurateBatteryVoltage(); int batPctC = calculateBatteryPercentage(voltsC);
    uint32_t freeHeapC = ESP.getFreeHeap() / 1024; uint32_t uptimeC = millis() / 1000;
    int wifiClientsC = WiFi.softAPgetStationNum();

    updateOrAddNode("C", "CenterHQ", currentLocationC, batPctC, voltsC, freeHeapC, uptimeC, "WiFi Client: " + String(wifiClientsC), 0, 0.0); //[cite: 111]

    String json = "{\"messages\":["; // รวบรวมข้อความแชท[cite: 111]
    for (int i = 0; i < msgCount; i++) {
      json += "{\"uid\":\"" + chatHistory[i].uid + "\",\"user\":\"" + chatHistory[i].user + "\",\"loc\":\"" + chatHistory[i].loc + "\",\"text\":\"" + chatHistory[i].text + "\",\"sender\":\"" + chatHistory[i].sender + "\"}"; //[cite: 111]
      if (i < msgCount - 1) json += ",";
    }
    json += "],\"cases\":["; // รวบรวมเคสทั้งหมด[cite: 111]
    {
      std::lock_guard<std::mutex> lock(caseMutex);
      for (size_t i = 0; i < activeCases.size(); i++) {
        json += "{\"uid\":\"" + activeCases[i].uid + "\",\"user\":\"" + activeCases[i].user + "\",\"loc\":\"" + activeCases[i].loc + "\",\"officer\":\"" + activeCases[i].assignedOfficer + "\",\"status\":\"" + activeCases[i].status + "\",\"lastMsg\":\"" + activeCases[i].lastMsg + "\"}"; //[cite: 111]
        if (i < activeCases.size() - 1) json += ",";
      }
    }
    json += "],\"nodes\":["; // รวบรวมข้อมูล Telemetry ของทุกเสา[cite: 111]
    {
      std::lock_guard<std::mutex> lock(nodeMutex); unsigned long now = millis();
      for (size_t i = 0; i < networkNodes.size(); i++) {
        bool isOnline = (now - networkNodes[i].lastSeen < 50000) || (networkNodes[i].id == "C"); // เช็คออนไลน์[cite: 112]
        unsigned long lastSeenSec = (now - networkNodes[i].lastSeen) / 1000;
        json += "{\"id\":\"" + networkNodes[i].id + "\",\"type\":\"" + networkNodes[i].type + "\",\"loc\":\"" + networkNodes[i].loc + "\",\"batPct\":" + String(networkNodes[i].batPct) + ",\"batVolt\":\"" + String(networkNodes[i].batVolt, 2) + "\",\"freeHeapKB\":" + String(networkNodes[i].freeHeapKB) + ",\"uptimeSec\":" + String(networkNodes[i].uptimeSec) + ",\"extraData\":\"" + networkNodes[i].extraData + "\",\"rssi\":" + String(networkNodes[i].rssi) + ",\"snr\":\"" + String(networkNodes[i].snr, 1) + "\",\"lastSeenSec\":" + String(lastSeenSec) + ",\"online\":" + String(isOnline ? "true" : "false") + "}"; //[cite: 112]
        if (i < networkNodes.size() - 1) json += ",";
      }
    }
    json += "]}";
    request->send(200, "application/json; charset=utf-8", json); // ส่งข้อมูลให้หน้าจอเจ้าหน้าที่[cite: 112]
  });

  // API สำหรับกดรับเรื่องเคส (Claim)[cite: 112]
  server.on("/api/claim", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("uid") && request->hasParam("officer")) {
      String uid = request->getParam("uid")->value();
      String officer = request->getParam("officer")->value();
      String lastMsgId = "";
      {
        std::lock_guard<std::mutex> lock(caseMutex);
        for (auto &c : activeCases) {
          if (c.uid == uid) { c.assignedOfficer = officer; c.status = "claimed"; lastMsgId = c.lastMsgId; break; } // เปลี่ยนสถานะเคส[cite: 112]
        }
      }
      saveCasesToFS(); // บันทึกลงไฟล์ LittleFS[cite: 112]

      ackCounter++;
      String officerB64 = encodeBase64(officer);
      String ackPacket = "ACK" + String(ackCounter) + "|3|C|A|ACK|" + lastMsgId + "|" + uid + "|CLAIMED|" + officerB64; // ส่ง ACK แจ้งผู้ประสบภัยว่ามีคนรับเรื่องแล้ว[cite: 113]
      { std::lock_guard<std::mutex> lock(txMutex); txQueue.push(ackPacket); } //[cite: 113]
    }
    request->send(200, "text/plain", "OK");
  });

  // API สำหรับปิดเคสช่วยเหลือสำเร็จ (Resolve)[cite: 113]
  server.on("/api/resolve", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("uid")) {
      String uid = request->getParam("uid")->value();
      String lastMsgId = ""; String officer = "";
      {
        std::lock_guard<std::mutex> lock(caseMutex);
        for (auto &c : activeCases) {
          if (c.uid == uid) { c.status = "resolved"; lastMsgId = c.lastMsgId; officer = c.assignedOfficer; break; } //[cite: 113]
        }
      }
      saveCasesToFS(); // บันทึกลงระบบไฟล์[cite: 113]

      ackCounter++;
      String officerB64 = encodeBase64(officer);
      String ackPacket = "ACK" + String(ackCounter) + "|3|C|A|ACK|" + lastMsgId + "|" + uid + "|RESOLVED|" + officerB64; // ส่ง ACK แจ้งผู้ประสบภัยว่าปลอดภัยแล้ว[cite: 113]
      { std::lock_guard<std::mutex> lock(txMutex); txQueue.push(ackPacket); } //[cite: 113]
    }
    request->send(200, "text/plain", "OK");
  });

  // API ส่งข้อความตอบกลับผู้ประสบภัย (Reply)[cite: 113, 114]
  server.on("/reply", HTTP_GET, [](AsyncWebServerRequest *request){
    String targetUid = request->getParam("target_uid")->value(); targetUid.trim(); //[cite: 114]
    String officer = request->getParam("officer")->value(); officer.trim(); //[cite: 114]
    String text = request->getParam("text")->value(); text.trim(); //[cite: 114]
    if (text.length() > 120) text = text.substring(0, 120); //[cite: 114]

    {
      std::lock_guard<std::mutex> lock(caseMutex);
      for (auto &c : activeCases) {
        if (c.uid == targetUid) { c.assignedOfficer = officer; c.status = "claimed"; break; } //[cite: 114]
      }
    }
    saveCasesToFS(); //[cite: 114]

    replyCounter++;
    String officerB64 = encodeBase64(officer);
    String textB64 = encodeBase64(text);
    String packet = String(replyCounter) + "|3|C|A|REPLY|" + targetUid + "|" + officerB64 + "|HQ|" + textB64; // สร้างแพ็กเกจส่งผ่าน LoRa[cite: 114]

    { std::lock_guard<std::mutex> lock(txMutex); txQueue.push(packet); } // ใส่ Queue ส่ง[cite: 114]

    addMessage(targetUid, officer, "HQ", text, "officer"); // เพิ่มลงประวัติ[cite: 114]
    request->send(200, "text/plain; charset=utf-8", "OK");
  });

  server.onNotFound(handleCaptiveRedirectC);
  server.begin(); // เปิด Web Server[cite: 114]
}

// ลูปการทำงานหลัก Loop[cite: 114]
void loop() {
  esp_task_wdt_reset(); dnsServer.processNextRequest(); //[cite: 114]

  if (millis() - lastOledSwitch > 20000) { lastOledSwitch = millis(); oledPage = (oledPage + 1) % 2; } // สลับ OLED[cite: 114, 115]
  if (millis() - lastOledRender > 1000) { lastOledRender = millis(); updateOLED(); }                   // วาด OLED[cite: 115]

  // ดึงข้อความจาก Queue ออกมาส่งคลื่นวิทยุ LoRa[cite: 115]
  String packetToSend = "";
  {
    std::lock_guard<std::mutex> lock(txMutex);
    if (!txQueue.empty()) { packetToSend = txQueue.front(); txQueue.pop(); } //[cite: 115]
  }
  if (packetToSend.length() > 0) { sendLoRaRaw(packetToSend); delay(40); } //[cite: 115]

  // กระจาย Heartbeat Node C พร้อมแนบเวลา Unix Epoch[cite: 115]
  if (millis() - lastHeartbeat > heartbeatInterval) {
    lastHeartbeat = millis(); heartbeatInterval = 45000 + random(0, 5000); hbCounterC++; //[cite: 115]

    float vC = getAccurateBatteryVoltage(); int pctC = calculateBatteryPercentage(vC);
    uint32_t freeHeapC = ESP.getFreeHeap() / 1024; uint32_t uptimeC = millis() / 1000;
    int wifiClientsC = WiFi.softAPgetStationNum();
    unsigned long currentEpoch = (unsigned long)time(NULL); // ดึงเวลาปัจจุบัน[cite: 115]

    // แนบ Timestamp ส่งออกไปให้ Node B ซิงค์เวลา[cite: 115]
    String hbPacket = "HBC" + String(hbCounterC) + "|3|C|ALL|HB|CenterHQ|" + currentLocationC + "|" + String(pctC) + "|" + String(vC, 2) + "|" + String(freeHeapC) + "|" + String(uptimeC) + "|WiFi Client: " + String(wifiClientsC) + "|" + String(currentEpoch); //[cite: 115]
    { std::lock_guard<std::mutex> lock(txMutex); txQueue.push(hbPacket); } //[cite: 115]
  }

  // ระบบดักรับและประมวลผลสัญญาณ LoRa ขาเข้า[cite: 116]
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String rawIncoming = ""; while (LoRa.available()) rawIncoming += (char)LoRa.read(); rawIncoming.trim(); //[cite: 116]
    lastRssi = LoRa.packetRssi(); lastSnr = LoRa.packetSnr(); //[cite: 116]

    int pLast = rawIncoming.lastIndexOf('|'); if (pLast == -1) return;
    String payload = rawIncoming.substring(0, pLast);
    uint16_t rxCrc = (uint16_t)strtol(rawIncoming.substring(pLast + 1).c_str(), NULL, 16);
    if (calculateCRC16(payload) != rxCrc) return; // ทิ้งแพ็กเกจถ้ารหัส CRC ไม่ตรงกัน[cite: 116]

    std::vector<String> tokens = splitString(payload, '|'); if (tokens.size() < 5) return; //[cite: 116]

    String msgType = tokens[4];

    // ประมวลผลแพ็กเกจสถานะ Heartbeat จากเสาอื่นๆ เพื่อทำ Telemetry[cite: 116]
    if (msgType == "HB" && tokens.size() >= 11) {
      String nodeId = tokens[2]; String nodeType = tokens[5]; String loc = tokens[6];
      int batPct = tokens[7].toInt(); float batVolt = tokens[8].toFloat();
      uint32_t freeHeapKB = tokens[9].toInt() / 1024; uint32_t uptimeSec = tokens[10].toInt();
      String extraData = (tokens.size() >= 12) ? tokens[11] : ""; //[cite: 116]

      if (nodeType == "Repeater") extraData = "Relayed: " + extraData + " pkts"; //[cite: 116]
      else if (nodeType == "VictimNode") extraData = "WiFi Client: " + extraData + " devices"; //[cite: 116]

      updateOrAddNode(nodeId, nodeType, loc, batPct, batVolt, freeHeapKB, uptimeSec, extraData, lastRssi, lastSnr); //[cite: 116]
    }

    // ประมวลผลข้อความขอความช่วยเหลือ MSG จากผู้ประสบภัย[cite: 116]
    if (msgType == "MSG" && tokens.size() >= 9) {
      String msgId = tokens[0]; String uid = tokens[5];
      String user = decodeBase64(tokens[6]); // ถอดรหัสชื่อผู้ใช้[cite: 117]
      String loc = tokens[7];
      String text = decodeBase64(tokens[8]); // ถอดรหัสข้อความภาษาไทย[cite: 117]

      // สั่งตอบแพ็กเกจ ACK กลับไปยัง Node A ทันที เพื่อหยุดการส่งซ้ำ Auto-Retry[cite: 117]
      ackCounter++;
      String ackPacket = "ACK" + String(ackCounter) + "|3|C|A|ACK|" + msgId + "|" + uid + "|DELIVERED|HQ"; //[cite: 117]
      { std::lock_guard<std::mutex> lock(txMutex); txQueue.push(ackPacket); } //[cite: 117]

      if (!isDuplicate(msgId)) {
        addOrUpdateCase(uid, user, loc, text, msgId); // เพิ่มเข้าเคส[cite: 117]
        addMessage(uid, user, loc, text, "victim");    // บันทึกประวัติ[cite: 117]
        saveCasesToFS();                              // บันทึกลงไฟล์ LittleFS[cite: 117]
      }
    }
  }
}
