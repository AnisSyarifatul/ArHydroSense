#include "wokwi-api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#define BMP280_ADDRESS 0x76

#define REG_ID       0xD0
#define REG_RESET    0xE0
#define REG_STATUS   0xF3
#define REG_CTRL     0xF4
#define REG_CONFIG   0xF5

// Koefisien kalibrasi (sesuai register 0x88..0x9F di bawah)
#define DIG_T1 27504
#define DIG_T2 26435
#define DIG_T3 (-1000)
#define DIG_P1 36477
#define DIG_P2 (-10685)
#define DIG_P3 3024
#define DIG_P4 2855
#define DIG_P5 140
#define DIG_P6 (-7)
#define DIG_P7 15500
#define DIG_P8 (-14600)
#define DIG_P9 6000

#define TEKANAN_LAUT_PA 101325.0f

typedef struct {
  pin_t sda;
  pin_t scl;

  uint32_t attr_temp;
  uint32_t attr_pres;
  uint32_t attr_alt;
  float last_pres;
  float last_alt;
  bool pakai_altitude;   // true jika slider Altitude yang terakhir digeser

  uint8_t registers[256];
  uint8_t register_pointer;
  bool first_write;
} bmp280_state_t;

// ==========================================
// RUMUS KOMPENSASI BOSCH (sama dengan library Adafruit)
// ==========================================
static int64_t hitung_t_fine(int32_t adc_T) {
  int64_t var1 = ((((int64_t)(adc_T >> 3) - ((int64_t)DIG_T1 << 1))) * DIG_T2) >> 11;
  int64_t d = (int64_t)(adc_T >> 4) - DIG_T1;
  int64_t var2 = (((d * d) >> 12) * DIG_T3) >> 14;
  return var1 + var2;
}

// Hasil dalam 0,01 derajat Celsius
static int32_t suhu_centi(int32_t adc_T) {
  return (int32_t)((hitung_t_fine(adc_T) * 5 + 128) >> 8);
}

// Hasil dalam Q24.8 (bagi 256 untuk Pa)
static int64_t tekanan_q8(int32_t adc_P, int64_t t_fine) {
  int64_t var1 = t_fine - 128000;
  int64_t var2 = var1 * var1 * (int64_t)DIG_P6;
  var2 = var2 + ((var1 * (int64_t)DIG_P5) << 17);
  var2 = var2 + (((int64_t)DIG_P4) << 35);
  var1 = ((var1 * var1 * (int64_t)DIG_P3) >> 8) + ((var1 * (int64_t)DIG_P2) << 12);
  var1 = ((((int64_t)1 << 47) + var1) * (int64_t)DIG_P1) >> 33;
  if (var1 == 0) return 0;
  int64_t p = 1048576 - adc_P;
  p = (((p << 31) - var2) * 3125) / var1;
  var1 = (((int64_t)DIG_P9) * (p >> 13) * (p >> 13)) >> 25;
  var2 = (((int64_t)DIG_P8) * p) >> 19;
  p = ((p + var1 + var2) >> 8) + (((int64_t)DIG_P7) << 4);
  return p;
}

// Cari nilai mentah suhu untuk target (0,01 C). Suhu naik seiring adc_T.
static int32_t cari_adc_suhu(int32_t target_centi) {
  int32_t lo = 0, hi = 1048575;
  while (lo < hi) {
    int32_t mid = (lo + hi) / 2;
    if (suhu_centi(mid) < target_centi) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

// Cari nilai mentah tekanan untuk target (Pa). Tekanan turun seiring adc_P.
static int32_t cari_adc_tekanan(float target_pa, int64_t t_fine) {
  int64_t target_q8 = (int64_t)(target_pa * 256.0f);
  int32_t lo = 0, hi = 1048575;
  while (lo < hi) {
    int32_t mid = (lo + hi) / 2;
    if (tekanan_q8(mid, t_fine) > target_q8) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

// ==========================================
// PERBARUI REGISTER DATA DARI SLIDER
// ==========================================
static float clampf(float v, float min_v, float max_v) {
  if (v < min_v) return min_v;
  if (v > max_v) return max_v;
  return v;
}

static void perbarui_pengukuran(bmp280_state_t *chip) {
  float temp = attr_read_float(chip->attr_temp);   // 0,01 C
  float pres = attr_read_float(chip->attr_pres);   // Pa
  float alt  = attr_read_float(chip->attr_alt);    // m

  // Slider yang terakhir digeser menentukan sumber tekanan.
  // Gunakan toleransi kecil agar perubahan floating-point dari slider tidak
  // menimbulkan flapping yang berulang saat nilai sama secara visual.
  if (fabsf(alt - chip->last_alt) > 0.001f) {
    chip->pakai_altitude = true;
  } else if (fabsf(pres - chip->last_pres) > 0.001f) {
    chip->pakai_altitude = false;
  }

  chip->last_alt = alt;
  chip->last_pres = pres;

  temp = clampf(temp, -4000.0f, 8500.0f);
  alt = clampf(alt, -500.0f, 9000.0f);
  pres = clampf(pres, 30000.0f, 110000.0f);

  float tekanan_pa;
  if (chip->pakai_altitude) {
    // Kebalikan dari readAltitude(1013.25) pada library Adafruit
    tekanan_pa = TEKANAN_LAUT_PA * powf(1.0f - alt / 44330.0f, 1.0f / 0.1903f);
  } else {
    tekanan_pa = pres;
  }

  tekanan_pa = clampf(tekanan_pa, 30000.0f, 110000.0f);

  int32_t adc_T = cari_adc_suhu((int32_t)(temp < 0 ? temp - 0.5f : temp + 0.5f));
  int32_t adc_P = cari_adc_tekanan(tekanan_pa, hitung_t_fine(adc_T));

  chip->registers[0xF7] = (adc_P >> 12) & 0xFF;
  chip->registers[0xF8] = (adc_P >> 4) & 0xFF;
  chip->registers[0xF9] = (adc_P & 0x0F) << 4;

  chip->registers[0xFA] = (adc_T >> 12) & 0xFF;
  chip->registers[0xFB] = (adc_T >> 4) & 0xFF;
  chip->registers[0xFC] = (adc_T & 0x0F) << 4;
}

// ==========================================
// CALLBACK I2C
// ==========================================
static bool on_i2c_connect(void *user_data, uint32_t address, bool read) {
  bmp280_state_t *chip = user_data;
  if (!read) {
    chip->first_write = true;
  }
  return address == BMP280_ADDRESS;
}

static uint8_t on_i2c_read(void *user_data) {
  bmp280_state_t *chip = user_data;
  uint8_t data = chip->registers[chip->register_pointer];
  chip->register_pointer++;
  return data;
}

static bool on_i2c_write(void *user_data, uint8_t data) {
  bmp280_state_t *chip = user_data;

  if (chip->first_write) {          // byte pertama = alamat register
    chip->register_pointer = data;
    chip->first_write = false;
    return true;
  }

  uint8_t reg = chip->register_pointer;

  if (reg == REG_RESET) {           // soft reset
    if (data == 0xB6) {
      chip->registers[REG_CTRL] = 0x00;
      chip->registers[REG_CONFIG] = 0x00;
      chip->register_pointer = 0;
    }
    chip->register_pointer++;
    return true;
  }

  // Register hanya-baca tidak boleh ditimpa
  if (reg == REG_ID || reg == REG_STATUS || reg >= 0xF7) {
    chip->register_pointer++;
    return true;
  }

  chip->registers[reg] = data;

  // Forced/normal mode = pengukuran baru, ambil nilai slider terbaru
  if (reg == REG_CTRL) {
    chip->registers[REG_CTRL] = data & 0x3F;
    if ((data & 0x03) != 0) {
      perbarui_pengukuran(chip);
    }
  }

  chip->register_pointer++;
  return true;
}

static void on_i2c_disconnect(void *user_data) {
  bmp280_state_t *chip = user_data;
  chip->register_pointer = 0;
  chip->first_write = true;
}

// ==========================================
// INISIALISASI
// ==========================================
static void initialize_bmp280(bmp280_state_t *chip) {
  memset(chip->registers, 0, sizeof(chip->registers));

  chip->registers[0xD0] = 0x58;  // Chip ID BMP280

  // Kalibrasi suhu T1..T3
  chip->registers[0x88] = 0x70; chip->registers[0x89] = 0x6B;
  chip->registers[0x8A] = 0x43; chip->registers[0x8B] = 0x67;
  chip->registers[0x8C] = 0x18; chip->registers[0x8D] = 0xFC;

  // Kalibrasi tekanan P1..P9
  chip->registers[0x8E] = 0x7D; chip->registers[0x8F] = 0x8E;
  chip->registers[0x90] = 0x43; chip->registers[0x91] = 0xD6;
  chip->registers[0x92] = 0xD0; chip->registers[0x93] = 0x0B;
  chip->registers[0x94] = 0x27; chip->registers[0x95] = 0x0B;
  chip->registers[0x96] = 0x8C; chip->registers[0x97] = 0x00;
  chip->registers[0x98] = 0xF9; chip->registers[0x99] = 0xFF;
  chip->registers[0x9A] = 0x8C; chip->registers[0x9B] = 0x3C;
  chip->registers[0x9C] = 0xF8; chip->registers[0x9D] = 0xC6;
  chip->registers[0x9E] = 0x70; chip->registers[0x9F] = 0x17;

  chip->registers[REG_STATUS] = 0x00;
  chip->registers[REG_CTRL] = 0x00;
  chip->registers[REG_CONFIG] = 0x00;

  chip->register_pointer = 0;
  chip->first_write = true;
}

void chip_init() {
  bmp280_state_t *chip = malloc(sizeof(bmp280_state_t));
  if (chip == NULL) {
    printf("BMP280: Gagal mengalokasikan memori!\n");
    return;
  }

  chip->sda = pin_init("SDA", INPUT_PULLUP);
  chip->scl = pin_init("SCL", INPUT_PULLUP);

  // Nilai awal slider (id harus sama dengan di chip.json)
  chip->attr_temp = attr_init_float("TEMP", 2500.0f);
  chip->attr_pres = attr_init_float("PRES", 101325.0f);
  chip->attr_alt  = attr_init_float("ALTITUDE", 0.0f);
  chip->last_pres = 101325.0f;
  chip->last_alt = 0.0f;
  chip->pakai_altitude = false;

  initialize_bmp280(chip);
  perbarui_pengukuran(chip);   // isi register data dengan nilai awal

  const i2c_config_t i2c_config = {
    .address = BMP280_ADDRESS,
    .sda = chip->sda,
    .scl = chip->scl,
    .connect = on_i2c_connect,
    .read = on_i2c_read,
    .write = on_i2c_write,
    .disconnect = on_i2c_disconnect,
    .user_data = chip
  };

  i2c_init(&i2c_config);

  printf("BMP280 custom chip aktif (alamat 0x76, ID 0x58)\n");
}