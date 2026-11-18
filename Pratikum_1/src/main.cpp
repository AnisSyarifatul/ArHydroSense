#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>

// ==========================================
// 1. KONFIGURASI PIN ESP32
// ==========================================

#define DHT_PIN 15
#define DHT_TYPE DHT22

#define SOIL_PIN 34
#define RELAY_PIN 25

#define LED_MERAH 27
#define LED_HIJAU 26

// Pushbutton: satu kaki ke GPIO 18, kaki lainnya ke GND (memakai INPUT_PULLUP)
#define TOMBOL_PIN 18

// ==========================================
// 2. MODE DEMO: BLOCKING vs NON-BLOCKING
// ==========================================
// 1 = BLOCKING     -> program "tidur" dengan delay(3000). Selama tidur,
//                     tombol TIDAK dibaca (respons lambat / tidak terbaca).
// 0 = NON-BLOCKING -> pewaktu millis(). Tombol dibaca ribuan kali per detik
//                     (respons langsung).
#define MODE_BLOCKING 0

// ==========================================
// 3. KONFIGURASI WIFI
// ==========================================

// Untuk simulasi Wokwi
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

// Untuk ESP32 fisik, ganti dengan WiFi milikmu.

// ==========================================
// 4. KONFIGURASI MQTT
// ==========================================

const char* MQTT_BROKER = "broker.emqx.io";
const int MQTT_PORT = 1883;

const char* MQTT_TOPIC = "wokwi-smartgarden/data";

const char* MQTT_CLIENT_ID = "ArHydroSense-ESP32";

// ==========================================
// 5. INISIALISASI KOMPONEN DAN STATUS
// ==========================================

DHT dht(DHT_PIN, DHT_TYPE);

WiFiClient espClient;
PubSubClient mqtt(espClient);

// Batas kelembapan tanah
const int BATAS_KELEMBAPAN = 40;

// Interval pembacaan sensor
unsigned long previousMillis = 0;
const unsigned long interval = 3000;

// Status kontrol pompa
int kelembapanTanahTerakhir = 100;  // anggap lembap sampai ada pembacaan pertama
bool modeManual = false;            // true = pompa dipaksa nyala oleh tombol
bool pompaNyala = false;

// Status tombol
bool tombolSebelumnya = HIGH;
unsigned long waktuDebounce = 0;
const unsigned long DEBOUNCE_MS = 50;

// Pengukuran respons loop
unsigned long terakhirPolling = 0;
unsigned long jedaPollingMaks = 0;
unsigned long jumlahLoop = 0;

// Status koneksi (tanpa loop yang memblokir)
unsigned long terakhirCobaKoneksi = 0;
const unsigned long JEDA_COBA_KONEKSI = 3000;
bool wifiTerhubungSebelumnya = false;

// ==========================================
// 6. KONEKSI WIFI & MQTT (NON-BLOCKING)
// ==========================================

void jagaKoneksi() {

    bool wifiOk = (WiFi.status() == WL_CONNECTED);

    if (wifiOk && !wifiTerhubungSebelumnya) {

        Serial.print("[WiFi] Terhubung! IP: ");
        Serial.println(WiFi.localIP());

    }

    wifiTerhubungSebelumnya = wifiOk;

    unsigned long sekarang = millis();

    if (sekarang - terakhirCobaKoneksi < JEDA_COBA_KONEKSI) {
        return;
    }

    if (!wifiOk) {

        terakhirCobaKoneksi = sekarang;
        Serial.println("[WiFi] Menunggu koneksi...");
        return;

    }

    if (!mqtt.connected()) {

        terakhirCobaKoneksi = sekarang;

        Serial.print("[MQTT] Menghubungkan...");

        if (mqtt.connect(MQTT_CLIENT_ID)) {

            Serial.println(" terhubung!");

        } else {

            Serial.print(" gagal, kode: ");
            Serial.println(mqtt.state());

        }

    }

}

// ==========================================
// 7. KONTROL POMPA, RELAY, DAN LED
// ==========================================

void terapkanPompa() {

    bool tanahKering = (kelembapanTanahTerakhir < BATAS_KELEMBAPAN);

    // Pompa nyala jika mode manual aktif ATAU tanah kering
    pompaNyala = modeManual || tanahKering;

    digitalWrite(RELAY_PIN, pompaNyala ? HIGH : LOW);

    // LED hijau = pompa nyala, LED merah = pompa mati
    digitalWrite(LED_HIJAU, pompaNyala ? HIGH : LOW);
    digitalWrite(LED_MERAH, pompaNyala ? LOW : HIGH);

}

// ==========================================
// 8. PEMBACAAN TOMBOL
// ==========================================

void periksaTombol() {

    unsigned long sekarang = millis();

    // Jeda sejak tombol terakhir dibaca = batas keterlambatan respons
    unsigned long jeda = sekarang - terakhirPolling;

    if (jeda > jedaPollingMaks) {
        jedaPollingMaks = jeda;
    }

    terakhirPolling = sekarang;

    bool bacaTombol = digitalRead(TOMBOL_PIN);

    // Terdeteksi saat tombol baru ditekan (HIGH -> LOW), dengan debounce
    if (bacaTombol == LOW && tombolSebelumnya == HIGH
        && (sekarang - waktuDebounce > DEBOUNCE_MS)) {

        waktuDebounce = sekarang;

        modeManual = !modeManual;

        // Respons langsung: relay dan LED diperbarui saat itu juga
        terapkanPompa();

        Serial.println();

        if (modeManual) {
            Serial.println("[TOMBOL] Mode MANUAL AKTIF: pompa dipaksa NYALA.");
        } else {
            Serial.println("[TOMBOL] Mode MANUAL MATI: kembali OTOMATIS (sensor).");
        }

        Serial.print("[TOMBOL] Jeda sejak tombol terakhir dibaca: ");
        Serial.print(jeda);
        Serial.println(" ms");

    }

    tombolSebelumnya = bacaTombol;

}

// ==========================================
// 9. SIKLUS PEMBACAAN SENSOR (tiap 3 detik)
// ==========================================

void siklusSensor() {

    Serial.println();
    Serial.println("========== DATA SENSOR ==========");

    // ----------------------------------
    // Membaca Soil Moisture
    // ----------------------------------

    int nilaiMentah = analogRead(SOIL_PIN);

    int kelembapanTanah = map(
        nilaiMentah,
        0,
        4095,
        0,
        100
    );

    kelembapanTanah = constrain(
        kelembapanTanah,
        0,
        100
    );

    // ----------------------------------
    // Kontrol Penyiraman (tidak bergantung pada DHT22)
    // ----------------------------------

    kelembapanTanahTerakhir = kelembapanTanah;
    terapkanPompa();

    Serial.print("Kelembapan Tanah : ");
    Serial.print(kelembapanTanah);
    Serial.println(" %");

    Serial.print("Kondisi Tanah    : ");
    Serial.println(kelembapanTanah < BATAS_KELEMBAPAN ? "KERING" : "LEMBAP");

    Serial.print("Mode Pompa       : ");
    Serial.println(modeManual ? "MANUAL (tombol)" : "OTOMATIS (sensor)");

    Serial.print("Status Pompa     : ");
    Serial.println(pompaNyala ? "NYALA" : "MATI");

    // ----------------------------------
    // Pengukuran Respons Loop
    // ----------------------------------

    Serial.println("--------------------------------");

    Serial.print("Mode Loop        : ");
#if MODE_BLOCKING
    Serial.println("BLOCKING (delay)");
#else
    Serial.println("NON-BLOCKING (millis)");
#endif

    Serial.print("Putaran loop     : ");
    Serial.print(jumlahLoop);
    Serial.println(" kali");

    Serial.print("Jeda baca tombol : maks ");
    Serial.print(jedaPollingMaks);
    Serial.println(" ms");

    jumlahLoop = 0;
    jedaPollingMaks = 0;

    Serial.println("--------------------------------");

    // ----------------------------------
    // Membaca DHT22
    // ----------------------------------

    float suhu = dht.readTemperature();
    float kelembapanUdara = dht.readHumidity();

    if (isnan(suhu) || isnan(kelembapanUdara)) {

        Serial.println("[GALAT] DHT22 gagal dibaca. Data tidak dikirim (pompa tetap dikontrol).");
        return;

    }

    Serial.print("Suhu Udara       : ");
    Serial.print(suhu);
    Serial.println(" C");

    Serial.print("Kelembapan Udara : ");
    Serial.print(kelembapanUdara);
    Serial.println(" %");

    // ----------------------------------
    // Membuat Payload JSON
    // ----------------------------------

    String payload = "{";

    payload += "\"temp\":" + String(suhu, 1) + ",";
    payload += "\"humidity\":" + String(kelembapanUdara, 1) + ",";
    payload += "\"soil\":" + String(kelembapanTanah) + ",";
    payload += "\"pump\":\"";

    payload += pompaNyala ? "NYALA" : "MATI";

    payload += "\"}";

    Serial.println("Data JSON:");
    Serial.println(payload);

    // ----------------------------------
    // Mengirim Data ke MQTT
    // ----------------------------------

    if (mqtt.connected()) {

        if (mqtt.publish(MQTT_TOPIC, payload.c_str())) {

            Serial.println("[MQTT] Data berhasil dikirim.");

        } else {

            Serial.println("[MQTT] Gagal mengirim data.");

        }

    } else {

        Serial.println("[MQTT] Belum terhubung, data tidak dikirim.");

    }

    Serial.println("================================");

}

// ==========================================
// 10. SETUP
// ==========================================

void setup() {

    Serial.begin(115200);

    // Inisialisasi sensor
    dht.begin();

    // Inisialisasi ADC
    analogReadResolution(12);

    // Inisialisasi relay, LED, dan tombol
    pinMode(RELAY_PIN, OUTPUT);

    pinMode(LED_MERAH, OUTPUT);
    pinMode(LED_HIJAU, OUTPUT);

    pinMode(TOMBOL_PIN, INPUT_PULLUP);

    // Kondisi awal: pompa mati, LED merah menyala
    terapkanPompa();

    Serial.println();
    Serial.println("================================");
    Serial.println("       ARHYDROSENSE ESP32       ");
    Serial.println("   SISTEM PENYIRAMAN OTOMATIS   ");
    Serial.println("================================");

    Serial.print("Mode Loop : ");
#if MODE_BLOCKING
    Serial.println("BLOCKING (delay)");
#else
    Serial.println("NON-BLOCKING (millis)");
#endif

    Serial.println("Tombol (GPIO 18): tekan untuk MANUAL (pompa dipaksa nyala), tekan lagi untuk OTOMATIS.");

    // WiFi dimulai tanpa menunggu; sambungan dijaga di loop()
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    mqtt.setServer(MQTT_BROKER, MQTT_PORT);

    terakhirCobaKoneksi = millis() - JEDA_COBA_KONEKSI;
    terakhirPolling = millis();

}

// ==========================================
// 11. LOOP UTAMA
// ==========================================

void loop() {

    jumlahLoop++;

    jagaKoneksi();

    mqtt.loop();

    periksaTombol();

#if MODE_BLOCKING

    // BLOCKING: selama delay(), program tidak bisa membaca tombol
    siklusSensor();
    delay(interval);

#else

    // NON-BLOCKING: loop terus berputar, sensor dibaca saat waktunya tiba
    unsigned long currentMillis = millis();

    if (currentMillis - previousMillis >= interval) {

        previousMillis = currentMillis;
        siklusSensor();

    }

#endif

}