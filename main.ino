#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ElegantOTA.h>

// --- Настройки сети и хоста ---
const char* ssid = "ssid нашей точки доступа";
const char* password = "пароль нашей точки доступа";
const char* hostname = "heat_pump";
const char* register_url = "http://192.168.1.100/?register_new_device";

WebServer server(80);

// --- Настройка HardwareSerial (UART1) ---
HardwareSerial acSerial(1);
#define AC_RX_PIN 4
#define AC_TX_PIN 5

// --- Переменные состояния ---
bool ac_power = false;
uint8_t ac_mode = 0x06;   // Mode 0x06 (Cool), 0x04 (Heat)
uint8_t ac_temp = 24;     // 24°C по умолчанию
uint8_t ac_fan = 0x01;    // Low (0x01)
uint8_t ac_swing = 0x10;  // Off (0x10)

// Буферы для логирования UART
char last_tx_hex[80] = "NONE";
char last_rx_hex[80] = "NONE";

uint8_t rx_buffer[64];
size_t rx_index = 0;

// Вспомогательная функция перевода HEX-символа в число
uint8_t hexCharToByte(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

// --- Отправка готового массива байт в UART ---
void sendRawBytes(uint8_t* frame, size_t len) {
  char *ptr = last_tx_hex;
  for (size_t i = 0; i < len; i++) {
    ptr += sprintf(ptr, "%02X ", frame[i]);
  }

  acSerial.write(frame, len);
  acSerial.flush();

  Serial.print("TX Raw: ");
  Serial.println(last_tx_hex);
}

// --- Чтение ответов и статусов от платы кондиционера ---
void readACResponse() {
  while (acSerial.available()) {
    uint8_t b = acSerial.read();

    // Ищем стартовый байт кадра (0xAA или 0xBB)
    if (rx_index == 0 && b != 0xAA && b != 0xBB) {
      continue;
    }

    rx_buffer[rx_index++] = b;

    // Стандартный кадр — 22 байта
    if (rx_index >= 22) {
      char *ptr = last_rx_hex;
      for (size_t i = 0; i < 22; i++) {
        ptr += sprintf(ptr, "%02X ", rx_buffer[i]);
      }

      Serial.print("RX (From AC/Remote): ");
      Serial.println(last_rx_hex);

      rx_index = 0; // Сброс буфера для следующего пакета
    }
  }
}

// --- Формирование и отправка точной команды управления ---
void sendACCommand() {
  uint8_t frame[22];
  memset(frame, 0, sizeof(frame));

  // Ограничиваем температуру диапазоном 16..30°C
  uint8_t target_temp = ac_temp;
  if (target_temp < 16) target_temp = 16;
  if (target_temp > 30) target_temp = 30;

  frame[0]  = 0xAA;
  frame[1]  = 0x14;
  frame[2]  = 0x02;                             // Cmd 0x02 (Set)
  frame[3]  = ac_power ? 0x01 : 0x00;           // Power (0x01 = ON, 0x00 = OFF)
  frame[4]  = 0x00;
  frame[5]  = ac_mode;                          // Mode (0x06 Cool, 0x04 Heat)
  frame[6]  = 0x18;                             // Статический байт
  frame[7]  = ac_fan;                           // Fan speed
  frame[8]  = ac_swing;                         // Swing
  frame[9]  = 0x00;
  frame[10] = target_temp;                      // Уставка температуры (16 = 0x10, 22 = 0x16, 24 = 0x18)
  frame[11] = 0x17;                             // Маркер пакета управления
  
  for (int i = 12; i < 21; i++) {
    frame[i] = 0x00;
  }

  // Расчет CRC (Sum % 256)
  uint16_t sum = 0;
  for (int i = 0; i < 21; i++) {
    sum += frame[i];
  }
  frame[21] = (uint8_t)(sum % 256);

  sendRawBytes(frame, 22);
}

// --- POST-регистрация устройства ---
void registerDevice() {
  HTTPClient http;
  http.begin(register_url);
  http.addHeader("Content-Type", "application/json");

  const char* payload = "{\"device\":\"AC\",\"name\":\"Heat Pump\",\"features\":\"power;mode;temperature;fan_speed\",\"values\":\"on,off;cool,heat,int(16-32);high,low,auto\"}";

  Serial.print("Sending device registration POST to ");
  Serial.println(register_url);

  int httpCode = http.POST(payload);
  if (httpCode > 0) {
    Serial.printf("Registration code: %d\n", httpCode);
  } else {
    Serial.printf("Registration error: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}

const char* getFanString(uint8_t fan) {
  switch (fan) {
    case 0x00: return "auto";
    case 0x01: return "low";
    case 0x02: return "medium";
    case 0x03: return "high";
    default:   return "unknown";
  }
}

const char* getModeString(uint8_t mode) {
  switch (mode) {
    case 0x06: return "cool";
    case 0x04: return "heat";
    case 0x02: return "dry";
    case 0x03: return "fan_only";
    case 0x05: return "auto";
    default:   return "custom";
  }
}

const char* getSwingString(uint8_t swing) {
  return (swing == 0x01) ? "on" : "off";
}

void handleRoot() {
  bool state_changed = false;
  bool is_raw = false;

  // 0. ОБРАБОТКА RAW HEX КАДРА (?raw=AA1402...)
  if (server.hasArg("raw")) {
    String rawStr = server.arg("raw");
    rawStr.trim();

    size_t hexLen = rawStr.length();
    size_t byteCount = hexLen / 2;

    if (byteCount > 0 && byteCount <= 22) {
      uint8_t rawFrame[22];
      memset(rawFrame, 0, sizeof(rawFrame));

      for (size_t i = 0; i < byteCount; i++) {
        rawFrame[i] = (hexCharToByte(rawStr[i * 2]) << 4) | hexCharToByte(rawStr[i * 2 + 1]);
      }

      if (byteCount == 21) {
        uint16_t sum = 0;
        for (int i = 0; i < 21; i++) sum += rawFrame[i];
        rawFrame[21] = (uint8_t)(sum % 256);
        byteCount = 22;
      }

      sendRawBytes(rawFrame, byteCount);
      is_raw = true;
    }
  }

  if (!is_raw) {
    // 1. Управление питанием (?power=on|off)
    if (server.hasArg("power")) {
      String p = server.arg("power");
      p.toLowerCase();
      if (p == "on" || p == "1" || p == "true") {
        ac_power = true;
        state_changed = true;
      } else if (p == "off" || p == "0" || p == "false") {
        ac_power = false;
        state_changed = true;
      }
    }

    // 2. Температура (?temp=22)
    if (server.hasArg("temp")) {
      int t = server.arg("temp").toInt();
      if (t >= 16 && t <= 30) {
        ac_temp = (uint8_t)t;
        state_changed = true;
      }
    }

    // 3. Вентилятор (?fan=auto|low|mid|high)
    if (server.hasArg("fan")) {
      String f = server.arg("fan");
      f.toLowerCase();
      if (f == "auto" || f == "0") ac_fan = 0x00;
      else if (f == "low" || f == "1") ac_fan = 0x01;
      else if (f == "mid" || f == "medium" || f == "2") ac_fan = 0x02;
      else if (f == "high" || f == "3") ac_fan = 0x03;
      state_changed = true;
    }

    // 4. Режим (?mode=cool|heat|dry|fan|auto)
    if (server.hasArg("mode")) {
      String m = server.arg("mode");
      m.toLowerCase();
      if (m == "cool") ac_mode = 0x06;
      else if (m == "heat") ac_mode = 0x04;
      else if (m == "dry") ac_mode = 0x02;
      else if (m == "fan" || m == "fan_only") ac_mode = 0x03;
      else if (m == "auto") ac_mode = 0x05;
      state_changed = true;
    }

    // 5. Жалюзи (?swing=on|off)
    if (server.hasArg("swing")) {
      String s = server.arg("swing");
      s.toLowerCase();
      if (s == "on" || s == "1" || s == "true") ac_swing = 0x01;
      else if (s == "off" || s == "0" || s == "false") ac_swing = 0x10;
      state_changed = true;
    }

    if (server.hasArg("send")) state_changed = true;

    if (state_changed) {
      sendACCommand();
    }
  }

  // Ответ JSON
  char jsonBuffer[512];
  snprintf(jsonBuffer, sizeof(jsonBuffer),
    "{"
      "\"power\":%s,"
      "\"temp\":%u,"
      "\"mode\":\"%s\","
      "\"fan\":\"%s\","
      "\"swing\":\"%s\","
      "\"raw_mode\":%s,"
      "\"last_tx_hex\":\"%s\","
      "\"last_rx_hex\":\"%s\""
    "}",
    ac_power ? "true" : "false",
    ac_temp,
    getModeString(ac_mode),
    getFanString(ac_fan),
    getSwingString(ac_swing),
    is_raw ? "true" : "false",
    last_tx_hex,
    last_rx_hex
  );

  server.send(200, "application/json", jsonBuffer);
}

void setup() {
  Serial.begin(115200);
  acSerial.begin(9600, SERIAL_8N1, AC_RX_PIN, AC_TX_PIN);

  WiFi.setHostname(hostname);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected.");
  registerDevice();

  ElegantOTA.begin(&server);
  server.on("/", handleRoot);
  server.begin();
}

void loop() {
  server.handleClient();
  ElegantOTA.loop();

  readACResponse();
}
