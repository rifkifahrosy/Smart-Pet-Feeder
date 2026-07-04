#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <Preferences.h> // Tambahan: Untuk menyimpan jadwal di NVS Flash Memory
#include "time.h" 

// --- Konfigurasi WiFi ---
const char* ssid = "";
const char* password = "";

// --- Konfigurasi shiftr.io (MQTT) ---
const char* mqtt_server = "";
const int mqtt_port = 1883;
const char* mqtt_user = ""; 
const char* mqtt_pass = "";   
const char* client_id = "esp32_feeder_rki";

const char* topic_publish = "/feeder/status";
const char* topic_subscribe = "/feeder/command";

// --- Konfigurasi NTP Server Lokal ---
const char* ntp_server1 = "id.pool.ntp.org";   
const char* ntp_server2 = "time.google.com";   
const long  gmt_offset_sec = 25200;            // GMT+7 (WIB)
const int   daylight_offset_sec = 0;

// --- Definisi PIN (Wemos D1 R32) ---
#define PIN_TRIG 12
#define PIN_ECHO 13
#define PIN_SERVO 18
#define PIN_BUTTON_1 27    
#define PIN_BUTTON_2 5     
#define PIN_BUZZER 19      
#define PIN_LED_RED 25
#define PIN_LED_GREEN 26
#define PIN_LED_ORANGE 33

// --- DEKLARASI VARIABEL GLOBAL ---
Servo feederServo;
WiFiClient espClient;
PubSubClient client(espClient);
Preferences preferences; // Objek NVS

struct FeederData {
  int distance;
  int stockPercentage;
  bool isFeeding;
  String lastTriggerSource; 
  String currentTimeStr; 
};
FeederData systemData = {0, 0, false, "None", "00:00:00"};

// --- REVISI: MANAJEMEN JADWAL DINAMIS (Maksimal 5 Jadwal) ---
struct FeedingSchedule {
  int hour;
  int minute;
  bool active;
};
FeedingSchedule schedules[5]; 
int lastFedMinute = -1; 
bool needToPublishSchedules = false; // Flag untuk publish jadwal setelah berhasil connect MQTT

// Kalibrasi Jarak Sensor
const int DISTANCE_FULL = 5;      
const int DISTANCE_CRITICAL = 12; 
const int DISTANCE_EMPTY = 17;    

// --- Prototype Fungsi ---
void setupWiFi();
void callback(char* topic, byte* payload, unsigned int length);
void triggerFeeding(FeederData* data, int durationMs, String source);
void updateLocalTime(FeederData* data);
void loadSchedulesFromNVS();
void saveSchedulesToNVS();
void publishCurrentSchedules();

// --- FreeRTOS Task Handlers ---
void TaskSensorLED(void *pvParameters);
void TaskServoButton(void *pvParameters);
void TaskMQTT(void *pvParameters);
void TaskSchedule(void *pvParameters); 

void setup() {
  Serial.begin(115200);
  
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_BUTTON_1, INPUT_PULLUP);
  pinMode(PIN_BUTTON_2, INPUT_PULLUP); 
  pinMode(PIN_BUZZER, OUTPUT);         
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_ORANGE, OUTPUT);
  
  digitalWrite(PIN_BUZZER, LOW); 
  feederServo.attach(PIN_SERVO);
  feederServo.write(0); 

  // Load jadwal pakan yang tersimpan di memori flash (NVS)
  loadSchedulesFromNVS();

  setupWiFi();
  configTime(gmt_offset_sec, daylight_offset_sec, ntp_server1, ntp_server2);

  // Validasi Sinkronisasi NTP (Blocking setup maksimal 10 detik)
  Serial.print("[NTP] Menyingkronkan waktu internal");
  struct tm timeinfo;
  int retryCount = 0;
  while (!getLocalTime(&timeinfo) && retryCount < 20) {
    delay(500);
    Serial.print(".");
    retryCount++;
  }
  if (retryCount >= 20) {
    Serial.println("\n[ERROR] NTP gagal respon. Pastikan Port UDP 123 Terbuka!");
  } else {
    Serial.println("\n[SUCCESS] Waktu Sinkron!");
  }

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  // Jalankan Multi-Tasking FreeRTOS
  xTaskCreatePinnedToCore(TaskSensorLED, "SensorTask", 4096, (void*)&systemData, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskServoButton, "ServoTask", 4096, (void*)&systemData, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskMQTT, "MQTTTask", 8192, (void*)&systemData, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskSchedule, "ScheduleTask", 4096, (void*)&systemData, 2, NULL, 0); 
}

void loop() {
  vTaskDelete(NULL);
}

// ==================== IMPLEMENTASI FREE RTOS TASKS ====================

void TaskSensorLED(void *pvParameters) {
  FeederData* data = (FeederData*) pvParameters;
  unsigned long previousMillis = 0;
  for(;;) {
    if (millis() - previousMillis >= 2000) {
      previousMillis = millis();
      digitalWrite(PIN_TRIG, LOW); delayMicroseconds(2);
      digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
      digitalWrite(PIN_TRIG, LOW);
      
      long duration = pulseIn(PIN_ECHO, HIGH);
      data->distance = duration * 0.034 / 2;
      data->stockPercentage = constrain(map(data->distance, DISTANCE_EMPTY, DISTANCE_FULL, 0, 100), 0, 100);

      if (!data->isFeeding) {
        if (data->distance >= DISTANCE_EMPTY) { 
          digitalWrite(PIN_LED_RED, HIGH); digitalWrite(PIN_LED_ORANGE, LOW); digitalWrite(PIN_LED_GREEN, LOW);
          digitalWrite(PIN_BUZZER, HIGH); vTaskDelay(pdMS_TO_TICKS(100)); digitalWrite(PIN_BUZZER, LOW);
        } else if (data->distance >= DISTANCE_CRITICAL && data->distance < DISTANCE_EMPTY) { 
          digitalWrite(PIN_LED_RED, LOW); digitalWrite(PIN_LED_ORANGE, HIGH); digitalWrite(PIN_LED_GREEN, LOW);
          digitalWrite(PIN_BUZZER, LOW);
        } else if (data->distance <= DISTANCE_FULL) { 
          digitalWrite(PIN_LED_RED, LOW); digitalWrite(PIN_LED_ORANGE, LOW); digitalWrite(PIN_LED_GREEN, HIGH);
          digitalWrite(PIN_BUZZER, LOW);
        } else { 
          digitalWrite(PIN_LED_RED, LOW); digitalWrite(PIN_LED_ORANGE, LOW); digitalWrite(PIN_LED_GREEN, LOW);
          digitalWrite(PIN_BUZZER, LOW);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50)); 
  }
}

void TaskServoButton(void *pvParameters) {
  FeederData* data = (FeederData*) pvParameters;
  bool lastB1 = HIGH, lastB2 = HIGH;
  for(;;) {
    bool b1 = digitalRead(PIN_BUTTON_1);
    if (b1 == LOW && lastB1 == HIGH) triggerFeeding(data, 3000, "Button Manual 1");
    lastB1 = b1;

    bool b2 = digitalRead(PIN_BUTTON_2);
    if (b2 == LOW && lastB2 == HIGH) triggerFeeding(data, 3000, "Button Manual 2");
    lastB2 = b2;

    vTaskDelay(pdMS_TO_TICKS(50)); 
  }
}

void TaskMQTT(void *pvParameters) {
  FeederData* data = (FeederData*) pvParameters;
  unsigned long lastPublish = 0;
  for(;;) {
    if (!client.connected()) {
      while (!client.connected()) {
        Serial.print("Mencoba konek shiftr.io...");
        if (client.connect(client_id, mqtt_user, mqtt_pass)) {
          Serial.println("TERHUBUNG!");
          client.subscribe(topic_subscribe);
          needToPublishSchedules = true; // Set flag untuk kirim info jadwal pasca boot
        } else {
          vTaskDelay(pdMS_TO_TICKS(5000));
        }
      }
    }
    client.loop();

    // Mengirim telemetri berkala (Setiap 3 detik)
    if (millis() - lastPublish > 3000) {
      lastPublish = millis();
      updateLocalTime(data); 

      StaticJsonDocument<300> doc;
      doc["jarak_cm"] = data->distance;
      doc["stok_persen"] = data->stockPercentage;
      doc["sedang_pakan"] = data->isFeeding;
      doc["pemicu_terakhir"] = data->lastTriggerSource;
      doc["waktu_alat"] = data->currentTimeStr; 

      char buffer[300];
      serializeJson(doc, buffer);
      client.publish(topic_publish, buffer);
    }

    // Eksekusi kirim balik jadwal tersimpan ke dashboard secara asinkron
    if (needToPublishSchedules) {
      publishCurrentSchedules();
      needToPublishSchedules = false;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void TaskSchedule(void *pvParameters) {
  FeederData* data = (FeederData*) pvParameters;
  struct tm timeinfo;
  for(;;) {
    if (getLocalTime(&timeinfo)) {
      int currentHour = timeinfo.tm_hour;
      int currentMinute = timeinfo.tm_min;

      for (int i = 0; i < 5; i++) {
        if (schedules[i].active && currentHour == schedules[i].hour && currentMinute == schedules[i].minute) {
          if (lastFedMinute != currentMinute) {
            lastFedMinute = currentMinute;
            
            String logSource = String("Automatic Schedule (") + 
                               (currentHour < 10 ? "0" : "") + String(currentHour) + ":" + 
                               (currentMinute < 10 ? "0" : "") + String(currentMinute) + ")";
                               
            Serial.printf("[ALARM] Berhasil memicu jadwal pakan dinamis ke-%d\n", i+1);
            triggerFeeding(data, 3000, logSource);
          }
        }
      }
      if (currentMinute != lastFedMinute && lastFedMinute != -1) lastFedMinute = -1;
    }
    vTaskDelay(pdMS_TO_TICKS(15000)); 
  }
}

// ==================== LOKAL STORAGE (NVS) & MQTT HANDLER ====================

// Mengambil jadwal dari memori internal ESP32 saat pertama kali dinyalakan
void loadSchedulesFromNVS() {
  preferences.begin("feeder_cfg", true); // Membuka namespace "feeder_cfg" mode Read-Only
  
  Serial.println("[NVS] Memuat data jadwal dari flash memory:");
  for (int i = 0; i < 5; i++) {
    String keyH = "h" + String(i);
    String keyM = "m" + String(i);
    String keyA = "a" + String(i);
    
    // Ambil data, jika belum pernah diset (bawaan pabrik), pasang nilai default -1
    schedules[i].hour = preferences.getInt(keyH.c_str(), -1);
    schedules[i].minute = preferences.getInt(keyM.c_str(), -1);
    schedules[i].active = preferences.getBool(keyA.c_str(), false);
    
    if (schedules[i].active) {
      Serial.printf(" -> Jadwal %d aktif pada jam %02d:%02d\n", i+1, schedules[i].hour, schedules[i].minute);
    }
  }
  preferences.end();
}

// Menyimpan jadwal baru dari MQTT ke memori internal
void saveSchedulesToNVS() {
  preferences.begin("feeder_cfg", false); // Membuka mode Read-Write
  for (int i = 0; i < 5; i++) {
    String keyH = "h" + String(i);
    String keyM = "m" + String(i);
    String keyA = "a" + String(i);
    
    preferences.putInt(keyH.c_str(), schedules[i].hour);
    preferences.putInt(keyM.c_str(), schedules[i].minute);
    preferences.putBool(keyA.c_str(), schedules[i].active);
  }
  preferences.end();
  Serial.println("[NVS] Jadwal baru berhasil disimpan permanen.");
}

// Fungsi opsional untuk me-publish balik jadwal aktif ke dashboard
void publishCurrentSchedules() {
  StaticJsonDocument<300> doc;
  doc["type"] = "active_schedules";
  JsonArray arrayJadwal = doc.createNestedArray("schedules");
  
  for (int i = 0; i < 5; i++) {
    if (schedules[i].active) {
      char timeBuff[6];
      snprintf(timeBuff, sizeof(timeBuff), "%02d:%02d", schedules[i].hour, schedules[i].minute);
      arrayJadwal.add(timeBuff);
    }
  }
  char buffer[300];
  serializeJson(doc, buffer);
  client.publish(topic_publish, buffer);
  Serial.print("[MQTT PUBLISH] Mengirim data konfigurasi jadwal terpasang ke dashboard: ");
  Serial.println(buffer);
}

void callback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<400> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) return;

  if (doc.containsKey("action")) {
    const char* actionCmd = doc["action"];
    
    // Handler 1: Pemberian pakan instan manual dari dashboard
    if (strcmp(actionCmd, "feed") == 0) {
      int duration = doc.containsKey("duration") ? doc["duration"] : 3000;
      triggerFeeding(&systemData, duration, "Dashboard MQTT");
    }
    
    // Handler 2: Penggantian Jadwal Dinamis dari Dashboard
    else if (strcmp(actionCmd, "set_schedule") == 0) {
      Serial.println("[MQTT COMMAND] Request penggantian jadwal baru diterima.");
      JsonArray incomingSchedules = doc["schedules"];
      
      // Reset semua jadwal lama terlebih dahulu
      for (int i = 0; i < 5; i++) { schedules[i].active = false; }
      
      int i = 0;
      for (JsonVariant v : incomingSchedules) {
        if (i >= 5) break; // Batasi maksimal 5 jadwal demi menghemat memori
        const char* timeStr = v.as<const char*>(); // Format data "HH:MM"
        
        int hh, mm;
        if (sscanf(timeStr, "%d:%d", &hh, &mm) == 2) {
          schedules[i].hour = hh;
          schedules[i].minute = mm;
          schedules[i].active = true;
          i++;
        }
      }
      
      // Simpan perubahan ke dalam storage permanen NVS
      saveSchedulesToNVS();
      // Publish balik ke dashboard untuk konfirmasi visual
      publishCurrentSchedules();
    }
  }
}

// ==================== FUNGSI AKSI PENDUKUNG ====================

void triggerFeeding(FeederData* data, int durationMs, String source) {
  if (data->isFeeding) return; 
  data->isFeeding = true;
  data->lastTriggerSource = source;
  
  digitalWrite(PIN_LED_RED, LOW); digitalWrite(PIN_LED_ORANGE, LOW);
  digitalWrite(PIN_LED_GREEN, HIGH); digitalWrite(PIN_BUZZER, LOW);

  feederServo.write(180); 
  vTaskDelay(pdMS_TO_TICKS(durationMs)); 
  feederServo.write(0);  
  
  digitalWrite(PIN_LED_GREEN, LOW); 
  data->isFeeding = false;
}

void updateLocalTime(FeederData* data) {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    data->currentTimeStr = "NTP Error";
    return;
  }
  char timeStringBuff[50];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M:%S", &timeinfo);
  data->currentTimeStr = String(timeStringBuff);
}

void setupWiFi() {
  delay(10);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  Serial.println("\nWiFi Connected.");
}