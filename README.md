# Smart Pet Feeder - Sistem Pemberi Pakan Otomatis

Sistem pemberi pakan otomatis berbasis **ESP32** yang memantau level stok pakan dengan sensor ultrasonik, membuka katup pakan dengan servo secara terjadwal maupun manual, dan dapat dipantau/dikendalikan dari **dashboard web** secara real-time melalui protokol **MQTT**.

Repo ini berisi dua bagian utama:

| Bagian | File | Deskripsi |
|---|---|---|
| Firmware | `auto_feeder.ino` | Kode yang berjalan di ESP32 (Wemos D1 R32) |
| Dashboard | `index.html` | Panel kendali berbasis web (MQTT over WebSocket) |

---

## 1. Manajemen Dashboard

Dashboard adalah file HTML tunggal (tanpa build tool, tanpa server backend) yang terhubung **langsung dari browser** ke broker MQTT (`shiftr.io`) lewat WebSocket, sehingga bisa dibuka dari laptop maupun HP tanpa instalasi apa pun.

Dashboard dibagi menjadi beberapa panel:

- **Pengaturan Koneksi** : host broker, username, password, topik status/perintah. Nilai bawaan sudah disamakan dengan kredensial di firmware.
- **Level Wadah Pakan** : visualisasi silo (SVG) yang mengisi sesuai `stok_persen`, plus jarak mentah dari sensor ultrasonik (`jarak_cm`).
- **Indikator Panel (Cermin LED Fisik)** : meniru status LED merah/oranye/hijau/buzzer fisik di alat berdasarkan data yang diterima, **bukan** mengendalikan LED itu sendiri (LED fisik dikendalikan penuh oleh firmware).
- **Kendali Manual** : tombol "Beri Pakan Sekarang" yang mempublish perintah `feed` ke topik perintah, lengkap dengan progress bar durasi buka katup.
- **Jadwal Pakan Otomatis & Waktu Alat** : menampilkan jam alat (hasil sinkronisasi NTP) dan jadwal makan yang **benar-benar tersimpan di flash memory (NVS) ESP32**, serta form untuk mengirim jadwal baru ke alat.
- **Log MQTT** : mirip Serial Monitor, menampilkan setiap payload mentah yang lewat di topik status/perintah untuk keperluan debugging.

---

## 2. RTOS (FreeRTOS Multitasking)

ESP32 menjalankan **FreeRTOS**, sehingga `loop()` bawaan Arduino sengaja dikosongkan (`vTaskDelete(NULL)`) dan digantikan oleh 4 task independen yang berjalan paralel di 2 inti (dual-core) ESP32:

| Task | Core | Prioritas | Fungsi |
|---|---|---|---|
| `TaskSensorLED` | 1 | 1 | Baca sensor ultrasonik tiap 2 detik, hitung `stok_persen`, kendalikan LED + buzzer |
| `TaskServoButton` | 1 | 2 | Polling 2 tombol fisik untuk memicu pemberian pakan manual |
| `TaskMQTT` | 0 | 1 | Menjaga koneksi broker, publish telemetri tiap 3 detik, proses perintah masuk |
| `TaskSchedule` | 0 | 2 | Cek jam NTP tiap 15 detik, cocokkan dengan jadwal tersimpan di NVS |

Beberapa poin penting terkait RTOS di kode ini:

- **`xTaskCreatePinnedToCore()`** dipakai agar task sensor/servo (yang butuh respons cepat & real-time) dipisah dari task jaringan (MQTT) yang bisa saja blocking saat reconnect, ini mencegah pembacaan sensor "macet" saat WiFi/MQTT bermasalah.
- **`vTaskDelay(pdMS_TO_TICKS(ms))`** dipakai alih-alih `delay()` biasa di dalam task, karena `vTaskDelay` melepas CPU ke task lain selama menunggu, sedangkan `delay()` akan memblokir seluruh core.
- **Prioritas berbeda** (servo/jadwal = 2, sensor/mqtt = 1) memastikan aksi memberi pakan tidak "kalah rebutan" jadwal CPU saat MQTT sedang sibuk publish data.
- Semua task menerima pointer `(void*)&systemData` yang sama, sehingga mereka membaca/menulis **struct data yang sama secara bersamaan** (lihat bagian Pointer di bawah).

---

## 3. MQTT

Komunikasi antara ESP32 dan dashboard sepenuhnya lewat broker **MQTT** publik `shiftr.io`, dengan dua topik:

| Topik | Arah | Isi |
|---|---|---|
| `/feeder/status` | ESP32 → Dashboard | Telemetri berkala + konfirmasi jadwal |
| `/feeder/command` | Dashboard → ESP32 | Perintah beri pakan / ubah jadwal |

Detail implementasi:

- Firmware pakai library **`PubSubClient`** dengan koneksi **TCP mentah di port 1883** (`client.connect(client_id, mqtt_user, mqtt_pass)`), dijalankan di `TaskMQTT` dengan auto-reconnect loop.
- Dashboard pakai **MQTT.js** dengan koneksi **WebSocket aman (`wss://`) di port 443**, karena browser tidak bisa membuka koneksi TCP mentah. Keduanya tetap bisa saling bertukar pesan karena terhubung ke broker `shiftr.io` yang sama.
- ESP32 **subscribe** ke `/feeder/command` dan **publish** ke `/feeder/status`; dashboard melakukan kebalikannya, pola **pub/sub dua arah** yang umum di sistem IoT.
- Setiap kali ESP32 berhasil (re)connect ke broker, ia langsung mempublish ulang jadwal yang tersimpan (`needToPublishSchedules`), sehingga dashboard yang baru dibuka otomatis mendapat data terbaru tanpa perlu diminta.

---

## 4. JSON

Semua payload MQTT diformat sebagai **JSON**, di firmware menggunakan library **ArduinoJson** (`StaticJsonDocument`), dan di dashboard menggunakan `JSON.parse` / `JSON.stringify` bawaan JavaScript.

**Telemetri (ESP32 → Dashboard, tiap 3 detik):**
```json
{
  "jarak_cm": 9,
  "stok_persen": 68,
  "sedang_pakan": false,
  "pemicu_terakhir": "Automatic Schedule (07:00)",
  "waktu_alat": "07:00:03"
}
```

**Konfirmasi jadwal (ESP32 → Dashboard):**
```json
{
  "type": "active_schedules",
  "schedules": ["07:00", "12:00", "19:00"]
}
```

**Perintah beri pakan (Dashboard → ESP32):**
```json
{ "action": "feed", "duration": 3000 }
```

**Perintah ubah jadwal (Dashboard → ESP32):**
```json
{ "action": "set_schedule", "schedules": ["06:30", "13:00", "18:30"] }
```

Karena kedua arah komunikasi memakai struktur JSON yang konsisten, dashboard cukup mengecek keberadaan field tertentu (`data.type`, `doc["action"]`) untuk menentukan jenis pesan dan memprosesnya sesuai kebutuhan (telemetri biasa vs konfirmasi jadwal, feed vs set_schedule).

---

## 5. `millis()`, Pointer, dan Konektivitas Internet

**`millis()` timing non-blocking**
Alih-alih `delay()` yang memblokir seluruh program, firmware memakai pola *"catat waktu terakhir, bandingkan dengan sekarang"*:
```cpp
if (millis() - previousMillis >= 2000) {
  previousMillis = millis();
  // baca sensor ultrasonik
}
```
Pola ini dipakai untuk jeda baca sensor (2 detik), publish telemetri MQTT (3 detik), dan progress bar durasi pakan di dashboard (`Date.now()` versi JavaScript-nya). Semua bisa berjalan "bersamaan" tanpa saling memblokir.

**Pointer, berbagi satu sumber data ke banyak task**
Firmware punya satu struct global `FeederData systemData` yang menyimpan seluruh kondisi alat (jarak, stok, status pakan, dsb). Alih-alih menyalin data ini ke tiap task, firmware mengoper **pointer** ke struct yang sama:
```cpp
FeederData* data = (FeederData*) pvParameters;
data->distance = duration * 0.034 / 2;
```
Dengan begini, saat `TaskSensorLED` mengubah `data->distance`, task lain (`TaskMQTT`) yang membaca pointer yang sama langsung melihat nilai terbaru tanpa perlu mekanisme sinkronisasi tambahan, cara efisien untuk berbagi state antar-task di RTOS.

**Internet (WiFi + NTP)**
- Koneksi internet dimulai lewat `setupWiFi()` yang menyambungkan ESP32 ke access point (`WiFi.begin`), diikuti sinkronisasi waktu lewat **NTP** (`configTime` + `getLocalTime`) ke `id.pool.ntp.org` / `time.google.com` dengan offset GMT+7 (WIB).
- Waktu hasil NTP inilah yang dipakai `TaskSchedule` untuk mencocokkan jam saat ini dengan jadwal pakan tersimpan, dan yang dikirim ke dashboard lewat field `waktu_alat`.
- Bila NTP gagal sinkron (jaringan lambat/port UDP 123 tertutup), firmware tidak berhenti total — ia tetap jalan tapi mengirim string `"NTP Error"`, yang oleh dashboard ditampilkan sebagai peringatan visual (jam berwarna merah).

---

## 6. PWM (Pulse Width Modulation)
 
Aktuator servo pada katup pakan dikendalikan dengan **PWM**, bukan sekadar `HIGH`/`LOW` digital biasa. PWM dipakai karena servo membutuhkan sinyal berulang dengan **lebar pulsa (duty cycle) tertentu** untuk menentukan sudut putarnya, tidak hanya ON/OFF saja.
 
```cpp
#include <ESP32Servo.h>
Servo feederServo;
 
feederServo.attach(PIN_SERVO);   // pasang servo ke pin PWM-capable (GPIO18)
feederServo.write(0);            // sudut 0° → katup tertutup
feederServo.write(180);          // sudut 180° → katup terbuka
```
 
Yang terjadi di balik `feederServo.write()`:
 
- Library **`ESP32Servo`** membangkitkan sinyal PWM lewat modul **LEDC** (LED Control peripheral) bawaan ESP32, dengan periode ±20 ms (frekuensi ±50 Hz) — standar untuk servo hobi.
- Sudut servo ditentukan dari **lebar pulsa HIGH** dalam periode tersebut: sekitar 1 ms untuk 0°, sekitar 2 ms untuk 180°. Semakin lebar pulsa HIGH, semakin besar sudut putar motor.
- Fungsi `write(angle)` menerjemahkan sudut (0–180°) menjadi nilai duty cycle PWM yang sesuai secara otomatis, sehingga di kode aplikasi (`triggerFeeding()`) kita cukup memanggil `feederServo.write(180)` / `feederServo.write(0)` tanpa perlu menghitung duty cycle manual.
Alur penggunaannya di `triggerFeeding()`:
```cpp
feederServo.write(180);                     // PWM: buka katup
vTaskDelay(pdMS_TO_TICKS(durationMs));      // katup tetap terbuka selama durasi tertentu
feederServo.write(0);                       // PWM: tutup katup kembali
```
 
Durasi katup terbuka (`durationMs`) inilah yang bisa diatur dari dashboard (default 3000 ms), sehingga secara tidak langsung dashboard juga ikut mengatur berapa lama sinyal PWM "posisi terbuka" dipertahankan sebelum servo dikembalikan ke posisi tertutup.

## 7. Serial Monitor (Input/Output)

Serial Monitor (`Serial.begin(115200)`) dipakai murni untuk **debugging lewat USB**, bukan untuk kendali normal sistem (kendali normal lewat MQTT/dashboard). Contoh output yang bisa dilihat saat alat menyala:

```
[NTP] Menyingkronkan waktu internal.....
[SUCCESS] Waktu Sinkron!
Mencoba konek shiftr.io...TERHUBUNG!
[NVS] Memuat data jadwal dari flash memory:
 -> Jadwal 1 aktif pada jam 07:00
 -> Jadwal 2 aktif pada jam 12:00
[ALARM] Berhasil memicu jadwal pakan dinamis ke-1
[MQTT COMMAND] Request penggantian jadwal baru diterima.
[NVS] Jadwal baru berhasil disimpan permanen.
[MQTT PUBLISH] Mengirim data konfigurasi jadwal terpasang ke dashboard: {...}
```

Ringkasan titik-titik log penting di firmware:

| Tahap | Output Serial |
|---|---|
| Boot | Progres sinkronisasi NTP (retry hingga 20x, timeout ±10 detik) |
| WiFi | `"WiFi Connected."` setelah `WiFi.begin()` berhasil |
| MQTT | Status percobaan koneksi ke broker (berhasil/gagal, auto-retry tiap 5 detik) |
| NVS | Daftar jadwal yang berhasil dimuat saat boot, dan konfirmasi saat jadwal baru disimpan |
| Jadwal | Notifikasi tiap kali sebuah jadwal terpicu otomatis |
| Perintah MQTT | Notifikasi saat menerima perintah `set_schedule` dari dashboard |

Serial Monitor tidak menerima input balik dari user (bersifat *output-only* di firmware ini), semua kendali interaktif (beri pakan, ubah jadwal) masuk lewat MQTT, bukan lewat input serial manual.

---

## Ringkasan Arsitektur

```
[Sensor Ultrasonik] → TaskSensorLED ─┐
[Tombol Manual]     → TaskServoButton┤
                                     ├─► systemData (struct, diakses via pointer)
[Jadwal NVS + NTP]  → TaskSchedule  ─┤
                                     └─► TaskMQTT ──(JSON via MQTT)──► shiftr.io ──(WSS)──► Dashboard Web
```

Empat task FreeRTOS bekerja paralel, saling berbagi satu struct data lewat pointer, dan hanya `TaskMQTT` yang "berbicara" ke dunia luar, untuk menjaga sensor & servo tetap responsif meski jaringan sedang tidak stabil.

## Demo Singkat
[![Alternatif Teks](https://img.youtube.com/vi/u8sXHdluWr4/0.jpg)](https://www.youtube.com/watch?v=u8sXHdluWr4)

