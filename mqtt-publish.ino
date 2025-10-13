#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>
#include <time.h>   // NTP

// ====== 사용자 설정 ======
const char* ssid        = "yooni";
const char* password    = "00000000";
const char* mqtt_server = "damoa.io";
const int   mqtt_port   = 1883;   

// 본인 학번/이름
#define MY_ID   "2391017"
#define MY_NAME "이윤지"

// DHT 센서 핀/종류
#define DHTPIN   4
#define DHTTYPE  DHT11

// MQTT 토픽
const char* PUB_TOPIC        = "ewha/2391017";             
const char* CHECKIN_TOPIC    = "ewha/checkin";             
const char* STATUS_TOPIC     = "ewha/status/2391017";     

// 발행 간격(ms)
const unsigned long PUB_INTERVAL_MS = 3000;

// NTP (KST, UTC+9, DST 없음)
const long GMT_OFFSET_SEC = 9 * 3600;
const int  DST_OFFSET_SEC = 0;
const char* NTP_SERVER    = "pool.ntp.org";

// ====== 전역 객체 ======
WiFiClient espClient;
PubSubClient client(espClient);
DHT_Unified dht(DHTPIN, DHTTYPE);

// ====== 유틸 ======
static bool timeSynced = false;

time_t nowEpoch() {
  time_t now = time(nullptr);
  if (now > 100000) { // NTP 동기화 성공 판단
    timeSynced = true;
    return now;
  }
  // 아직 동기화 안 됐으면 millis() 기반 임시 타임스탬프
  return (time_t)(millis() / 1000);
}

void connectWiFi() {
  Serial.printf("[WiFi] Connecting to %s", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (millis() - start > 20000) { // 20초 타임아웃
      Serial.println("\n[WiFi] Retry...");
      WiFi.disconnect(true);
      delay(500);
      WiFi.begin(ssid, password);
      start = millis();
    }
  }
  Serial.printf("\n[WiFi] Connected. IP=%s RSSI=%ddBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());

  // NTP 시작(최초 1회)
  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
}

void connectMQTT() {
  // LWT 설정: offline 메시지를 retained로 남김
  while (!client.connected()) {
    String cid = String("esp32-") + String((uint32_t)ESP.getEfuseMac(), HEX);
    Serial.printf("[MQTT] Connecting as %s ...\n", cid.c_str());

    // LWT payload
    const char* willTopic   = STATUS_TOPIC;
    const char* willMessage = "offline";
    bool willRetain = true;
    int  willQos    = 0; 

    if (client.connect(
          cid.c_str(),
          /*username*/ nullptr, /*password*/ nullptr,
          willTopic, willQos, willRetain, willMessage, /*cleanSession*/ true
        )) {
      Serial.println("[MQTT] Connected.");
      // 상태 online (retained)
      client.publish(STATUS_TOPIC, "online", true);
      // 접속 체크인
      String hello = String(MY_ID) + " " + String(MY_NAME) + " (ESP32 connected)";
      client.publish(CHECKIN_TOPIC, hello.c_str(), false);
    } else {
      Serial.printf("[MQTT] Failed, rc=%d. Retry in 1.5s\n", client.state());
      delay(1500);
    }
  }
}

void safeReadDHT(float &t, float &h) {
  sensors_event_t event;
  t = NAN; h = NAN;

  dht.temperature().getEvent(&event);
  if (!isnan(event.temperature)) t = event.temperature;

  dht.humidity().getEvent(&event);
  if (!isnan(event.relative_humidity)) h = event.relative_humidity;
}

void publishJSON() {
  if (!client.connected()) return;

  float t, h;
  safeReadDHT(t, h);
  if (isnan(t) || isnan(h)) {
    Serial.println("[DHT] Read failed. Skip publish.");
    return;
  }

  unsigned long ts = (unsigned long)nowEpoch();

  // JSON 예: {"id":"2391017","name":"이윤지","t":24.3,"h":57.1,"ts":1690000000,"synced":true}
  char payload[220];
  snprintf(payload, sizeof(payload),
           "{\"id\":\"%s\",\"name\":\"%s\",\"t\":%.1f,\"h\":%.1f,\"ts\":%lu,\"synced\":%s}",
           MY_ID, MY_NAME, t, h, ts, timeSynced ? "true" : "false");

  // 최신값을 새로 열었을 때 바로 보여주고 싶으면 retained=true
  bool ok = client.publish(PUB_TOPIC, payload, true /*retained*/);
  Serial.printf("[PUB] topic=%s payload=%s (%s)\n",
                PUB_TOPIC, payload, ok ? "OK" : "FAIL");
}

// ====== 아두이노 표준 콜백 ======
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32 DHT11 → MQTT(JSON) 3s Publisher (Improved) ===");

  dht.begin();

  // MQTT 커넥션 튜닝
  client.setServer(mqtt_server, mqtt_port);
  client.setKeepAlive(30);          
  client.setSocketTimeout(5);       
  client.setBufferSize(512);      

  connectWiFi();
  connectMQTT();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!client.connected())          connectMQTT();
  client.loop();

  static unsigned long lastPub = 0;
  unsigned long now = millis();
  if (now - lastPub >= PUB_INTERVAL_MS) {
    lastPub = now;
    publishJSON();
  }
}
