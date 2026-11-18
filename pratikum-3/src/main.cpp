#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_system.h>

// 0 = pakai chip BMP280 (custom Wokwi), 1 = data simulasi (jalur cadangan)
#define SENSOR_SIMULASI 0

#define LEBAR_LAYAR 128
#define TINGGI_LAYAR 64
#define ALAMAT_OLED 0x3C
#define PIN_LED_STATUS 2
#define PIN_WAKEUP_TOMBOL GPIO_NUM_15
#define DURASI_TIDUR_DETIK 10
#define UKURAN_STACK_TASK 8192   // naik dari 4096: printf float + JSON + String butuh banyak stack

Adafruit_SSD1306 display(LEBAR_LAYAR, TINGGI_LAYAR, &Wire, -1);
Adafruit_BMP280 bmp;

// Variabel persisten di RTC memory (hilang jika board reboot penuh)
RTC_DATA_ATTR int nomorSiklusBoot = 0;
RTC_DATA_ATTR float riwayatSuhuTerakhir = 0.0f;
RTC_DATA_ATTR float riwayatTekananTerakhir = 0.0f;
// Rekor ekstrem suhu (soal C6). Nilai awal = penanda "belum ada data"
#define SUHU_MAKS_AWAL -1000.0f
#define SUHU_MIN_AWAL   1000.0f
RTC_DATA_ATTR float suhuMaks = SUHU_MAKS_AWAL;
RTC_DATA_ATTR float suhuMin  = SUHU_MIN_AWAL;

struct DataSensor {
  float suhu;
  float tekanan;
  float elevasi;
  bool valid;
} dataCuaca = {0, 0, 0, false};

SemaphoreHandle_t mutexData;
volatile bool taskSelesai = false;
const char *labelPemicu = "BOOT";

const char *namaResetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC (crash)";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP (normal)";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    default:                return "LAINNYA";
  }
}

void cetakDiagnostikWakeup() {
  esp_sleep_wakeup_cause_t alasan = esp_sleep_get_wakeup_cause();
  Serial.printf("\n========================================\n");
  Serial.printf("[RESET] Reset reason: %s\n", namaResetReason());
  Serial.printf("[RTC] Siklus Bangun Ke-%d\n", nomorSiklusBoot);
  switch (alasan) {
    case ESP_SLEEP_WAKEUP_TIMER:
      labelPemicu = "TIMER";
      Serial.println("[RTC WAKEUP] Pemicu: Pewaktu Internal (RTC Timer)");
      break;
    case ESP_SLEEP_WAKEUP_EXT0:
      labelPemicu = "EXT0 BTN";
      Serial.println("[RTC WAKEUP] Pemicu: Tombol Darurat Luar (EXT0 pada GPIO 15)");
      break;
    default:
      labelPemicu = "BOOT";
      Serial.printf("[RTC WAKEUP] Pemicu: bukan dari deep sleep (kode %d)\n", (int)alasan);
      break;
  }
  Serial.printf("[RTC MEM] Riwayat Siklus Lalu -> Suhu: %.2f C | Tekanan: %.2f hPa\n",
                riwayatSuhuTerakhir, riwayatTekananTerakhir);
  Serial.printf("[RTC MEM] Rekor Suhu -> Max: %.2f C | Min: %.2f C\n",
                suhuMaks > SUHU_MAKS_AWAL ? suhuMaks : 0.0f,
                suhuMin < SUHU_MIN_AWAL ? suhuMin : 0.0f);
}

// Diagnostik I2C Scanner (Eksperimen 1 / Tabel 3.1)
void pindaiI2C() {
  Serial.println("[I2C] Memindai bus I2C (SDA=21, SCL=22)...");
  int jumlah = 0;
  for (uint8_t alamat = 1; alamat < 127; alamat++) {
    Wire.beginTransmission(alamat);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[I2C] Perangkat ditemukan pada alamat 0x%02X", alamat);
      if (alamat == 0x76 || alamat == 0x77) Serial.print("  -> Sensor BMP/BME280");
      if (alamat == 0x3C || alamat == 0x3D) Serial.print("  -> OLED SSD1306");
      Serial.println();
      jumlah++;
    }
  }
  Serial.printf("[I2C] Total perangkat terdeteksi: %d\n", jumlah);
}

void perbaruiTampilanOled(float t, float p, float alt, int siklus, const char *pemicu) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("UNIB IoT - STASIUN CUACA");
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

  display.setCursor(0, 12);
  display.printf("Suhu    : %.1f C", t);
  display.setCursor(0, 22);
  display.printf("Tekanan : %.1f hPa", p);
  display.setCursor(0, 32);
  display.printf("Elevasi : %.1f m", alt);

  // Baris rekor ekstrem suhu (RTC memory) - soal C6
  display.drawLine(0, 42, 127, 42, SSD1306_WHITE);
  display.setCursor(0, 45);
  if (suhuMaks > SUHU_MAKS_AWAL && suhuMin < SUHU_MIN_AWAL) {
    display.printf("Max:%.1f Min:%.1f C", suhuMaks, suhuMin);
  } else {
    display.print("Max:-- Min:-- C");
  }

  // Baris status siklus & pemicu bangun
  display.setCursor(0, 55);
  display.printf("#%d | %s", siklus, pemicu);
  display.display();
}

// Task di Core 0: UI + JSON
void TaskTampilanDanSerial(void *pvParameters) {
  xSemaphoreTake(mutexData, portMAX_DELAY);
  DataSensor lokal = dataCuaca;
  xSemaphoreGive(mutexData);

  if (lokal.valid) {
    // Perbarui rekor suhu tertinggi/terendah (disimpan di RTC memory)
    if (lokal.suhu > suhuMaks) suhuMaks = lokal.suhu;
    if (lokal.suhu < suhuMin)  suhuMin  = lokal.suhu;

    perbaruiTampilanOled(lokal.suhu, lokal.tekanan, lokal.elevasi, nomorSiklusBoot, labelPemicu);

    JsonDocument doc;
    doc["device_id"] = "esp32-weather-st01";
    doc["boot_count"] = nomorSiklusBoot;
    doc["wakeup_cause"] = labelPemicu;
    doc["temp_c"] = serialized(String(lokal.suhu, 2));
    doc["pressure_hpa"] = serialized(String(lokal.tekanan, 2));
    doc["altitude_m"] = serialized(String(lokal.elevasi, 1));
    doc["prev_temp_c"] = serialized(String(riwayatSuhuTerakhir, 2));
    doc["temp_max_c"] = serialized(String(suhuMaks, 2));
    doc["temp_min_c"] = serialized(String(suhuMin, 2));
    serializeJson(doc, Serial);
    Serial.println();

    riwayatSuhuTerakhir = lokal.suhu;
    riwayatTekananTerakhir = lokal.tekanan;
  }

  taskSelesai = true;   // beri tahu setup() bahwa task sudah selesai
  vTaskDelete(NULL);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_LED_STATUS, OUTPUT);
  digitalWrite(PIN_LED_STATUS, HIGH);  // LED nyala = aktif
  // CATATAN: pull-up tombol diatur lewat rtc_gpio di bawah, bukan pinMode()

  nomorSiklusBoot++;
  cetakDiagnostikWakeup();

  mutexData = xSemaphoreCreateMutex();
  Wire.begin(21, 22);
  pindaiI2C();

  if (!display.begin(SSD1306_SWITCHCAPVCC, ALAMAT_OLED)) {
    Serial.println("Gagal menemukan layar OLED SSD1306!");
  }

#if SENSOR_SIMULASI
  xSemaphoreTake(mutexData, portMAX_DELAY);
  dataCuaca.suhu = 27.0f + (nomorSiklusBoot % 5) * 0.3f + random(-5, 6) / 10.0f;
  dataCuaca.tekanan = 1008.0f + random(-20, 21) / 10.0f;
  dataCuaca.elevasi = 44330.0f * (1.0f - pow(dataCuaca.tekanan / 1013.25f, 0.1903f));
  dataCuaca.valid = true;
  xSemaphoreGive(mutexData);
#else
  if (!bmp.begin(0x76)) {
    Serial.println("Gagal mendeteksi sensor BMP/BME280 pada alamat 0x76!");
    dataCuaca.valid = false;
  } else {
    bmp.setSampling(Adafruit_BMP280::MODE_FORCED,
                    Adafruit_BMP280::SAMPLING_X2,
                    Adafruit_BMP280::SAMPLING_X16,
                    Adafruit_BMP280::FILTER_X16,
                    Adafruit_BMP280::STANDBY_MS_500);
    bmp.takeForcedMeasurement();

    xSemaphoreTake(mutexData, portMAX_DELAY);
    dataCuaca.suhu = bmp.readTemperature();
    dataCuaca.tekanan = bmp.readPressure() / 100.0F;
    dataCuaca.elevasi = bmp.readAltitude(1013.25);
    dataCuaca.valid = true;
    xSemaphoreGive(mutexData);
  }
#endif

  // Jalankan task UI, lalu tunggu benar-benar selesai (maks 3 detik)
  taskSelesai = false;
  xTaskCreatePinnedToCore(TaskTampilanDanSerial, "TaskUI", UKURAN_STACK_TASK, NULL, 1, NULL, 0);
  unsigned long mulai = millis();
  while (!taskSelesai && millis() - mulai < 3000) {
    delay(10);
  }
  if (!taskSelesai) {
    Serial.println("[PERINGATAN] Task UI belum selesai setelah 3 detik!");
  }
  delay(200);

  Serial.printf("[POWER] Menyiapkan Deep Sleep selama %d detik...\n", DURASI_TIDUR_DETIK);
  esp_sleep_enable_timer_wakeup((uint64_t)DURASI_TIDUR_DETIK * 1000000ULL);
  esp_sleep_enable_ext0_wakeup(PIN_WAKEUP_TOMBOL, 0);   // bangun saat pin LOW

  // Pull-up lewat domain RTC agar tetap aktif saat deep sleep
  rtc_gpio_pulldown_dis(PIN_WAKEUP_TOMBOL);
  rtc_gpio_pullup_en(PIN_WAKEUP_TOMBOL);

  digitalWrite(PIN_LED_STATUS, LOW);  // LED mati = tidur
  Serial.println("[POWER] Zzz... Memasuki Deep Sleep.");
  Serial.flush();
  esp_deep_sleep_start();
}

void loop() {}
