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

// ==========================================
// 2. KONFIGURASI WIFI
// ==========================================

// Untuk simulasi Wokwi
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

// Untuk ESP32 fisik, ganti dengan WiFi milikmu.

// ==========================================
// 3. KONFIGURASI MQTT
// ==========================================

const char* MQTT_BROKER = "broker.emqx.io";
const int MQTT_PORT = 1883;

const char* MQTT_TOPIC = "wokwi-smartgarden/data";

const char* MQTT_CLIENT_ID = "ArHydroSense-ESP32";

// ==========================================
// 4. INISIALISASI KOMPONEN
// ==========================================

DHT dht(DHT_PIN, DHT_TYPE);

WiFiClient espClient;
PubSubClient mqtt(espClient);

// Batas kelembapan tanah
const int BATAS_KELEMBAPAN = 40;

// Interval pembacaan sensor
unsigned long previousMillis = 0;
const unsigned long interval = 3000;

// ==========================================
// 5. KONEKSI WIFI
// ==========================================

void setupWiFi() {

    Serial.println();
    Serial.print("Menghubungkan ke WiFi");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    while (WiFi.status() != WL_CONNECTED) {

        delay(500);
        Serial.print(".");

    }

    Serial.println();
    Serial.println("WiFi berhasil terhubung!");

    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

}

// ==========================================
// 6. KONEKSI MQTT
// ==========================================

void reconnectMQTT() {

    while (!mqtt.connected()) {

        Serial.print("Menghubungkan ke MQTT...");

        if (mqtt.connect(MQTT_CLIENT_ID)) {

            Serial.println("Terhubung!");

        } else {

            Serial.print("Gagal, kode: ");
            Serial.println(mqtt.state());

            Serial.println("Mencoba kembali dalam 3 detik...");

            delay(3000);

        }

    }

}

// ==========================================
// 7. KONTROL LED DAN RELAY
// ==========================================

void kontrolPenyiraman(int kelembapanTanah) {

    if (kelembapanTanah < BATAS_KELEMBAPAN) {

        // Tanah kering
        digitalWrite(RELAY_PIN, HIGH);

        digitalWrite(LED_MERAH, LOW);
        digitalWrite(LED_HIJAU, HIGH);

        Serial.println("Kondisi Tanah : KERING");
        Serial.println("Status Pompa  : NYALA");

    } else {

        // Tanah lembap
        digitalWrite(RELAY_PIN, LOW);

        digitalWrite(LED_MERAH, HIGH);
        digitalWrite(LED_HIJAU, LOW);

        Serial.println("Kondisi Tanah : LEMBAP");
        Serial.println("Status Pompa  : MATI");

    }

}

// ==========================================
// 8. SETUP
// ==========================================

void setup() {

    Serial.begin(115200);

    // Inisialisasi sensor
    dht.begin();

    // Inisialisasi ADC
    analogReadResolution(12);

    // Inisialisasi relay dan LED
    pinMode(RELAY_PIN, OUTPUT);

    pinMode(LED_MERAH, OUTPUT);
    pinMode(LED_HIJAU, OUTPUT);

    // Kondisi awal
    digitalWrite(RELAY_PIN, LOW);

    digitalWrite(LED_MERAH, LOW);
    digitalWrite(LED_HIJAU, LOW);

    Serial.println();
    Serial.println("================================");
    Serial.println("       ARHYDROSENSE ESP32       ");
    Serial.println("   SISTEM PENYIRAMAN OTOMATIS   ");
    Serial.println("================================");

    // Koneksi WiFi
    setupWiFi();

    // Konfigurasi MQTT
    mqtt.setServer(MQTT_BROKER, MQTT_PORT);

    // Koneksi MQTT
    reconnectMQTT();

}

// ==========================================
// 9. LOOP UTAMA
// ==========================================

void loop() {

    // Periksa koneksi WiFi
    if (WiFi.status() != WL_CONNECTED) {

        setupWiFi();

    }

    // Periksa koneksi MQTT
    if (!mqtt.connected()) {

        reconnectMQTT();

    }

    mqtt.loop();

    unsigned long currentMillis = millis();

    // Pembacaan sensor setiap 3 detik
    if (currentMillis - previousMillis >= interval) {

        previousMillis = currentMillis;

        Serial.println();
        Serial.println("========== DATA SENSOR ==========");

        // ----------------------------------
        // Membaca DHT22
        // ----------------------------------

        float suhu = dht.readTemperature();
        float kelembapanUdara = dht.readHumidity();

        if (isnan(suhu) || isnan(kelembapanUdara)) {

            Serial.println("Gagal membaca sensor DHT22!");
            return;

        }

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
        // Menampilkan Data Sensor
        // ----------------------------------

        Serial.print("Suhu Udara       : ");
        Serial.print(suhu);
        Serial.println(" C");

        Serial.print("Kelembapan Udara : ");
        Serial.print(kelembapanUdara);
        Serial.println(" %");

        Serial.print("Kelembapan Tanah : ");
        Serial.print(kelembapanTanah);
        Serial.println(" %");

        // ----------------------------------
        // Kontrol Penyiraman Otomatis
        // ----------------------------------

        kontrolPenyiraman(kelembapanTanah);

        // ----------------------------------
        // Membuat Payload JSON
        // ----------------------------------

        String payload = "{";

        payload += "\"temp\":" + String(suhu, 1) + ",";
        payload += "\"humidity\":" + String(kelembapanUdara, 1) + ",";
        payload += "\"soil\":" + String(kelembapanTanah) + ",";
        payload += "\"pump\":\"";

        payload += (kelembapanTanah < BATAS_KELEMBAPAN)
                   ? "NYALA"
                   : "MATI";

        payload += "\"}";

        // ----------------------------------
        // Mengirim Data ke MQTT
        // ----------------------------------

        Serial.println("--------------------------------");

        Serial.println("Mengirim data ke MQTT...");

        if (mqtt.publish(MQTT_TOPIC, payload.c_str())) {

            Serial.println("Data berhasil dikirim!");

            Serial.print("Payload: ");
            Serial.println(payload);

        } else {

            Serial.println("Gagal mengirim data MQTT!");

        }

        Serial.println("================================");

    }

}
