/**
 * @file    beacon.ino
 * @author  Maciej Sikorski
 * @version 3.6
 * @date    03.04.2026
 *
 * @brief   Emulator beacona BTHome zgodny z Shelly BLU Button1
 * @brief   BTHome beacon emulator compatible with Shelly BLU Button1
 *
 * PL:
 * Szkic emuluje ramkę BLE nadawaną przez Shelly BLU Button1.
 * Pakiet advertising oraz scan response są identyczne bajt po bajcie
 * z oryginałem urządzenia Shelly, co pozwala na integrację z systemami
 * obsługującymi protokół BTHome (np. Home Assistant, Shelly app).
 * Stan baterii odczytywany jest z wbudowanego dzielnika napięcia (SAADC)
 * i wysyłany w każdym pakiecie jako wartość procentowa (0–100%).
 * 
 * EN:
 * This sketch emulates the BLE advertising frame broadcast by the Shelly BLU Button1.
 * Both the advertising packet and scan response are byte-for-byte identical to the
 * original Shelly device, enabling integration with BTHome-compatible systems
 * (e.g. Home Assistant, Shelly app).
 * Battery level is read from the built-in voltage divider (SAADC) and transmitted
 * in every packet as a percentage value (0–100%).
 *
 * Hardware:  Seeed XIAO nRF52840
 * Library:   bluefruit.h (Seeed/Adafruit nRF52 BSP – wbudowana / bundled)
 * License:   Apache License 2.0
 * Copyright: (c) 2026 Maciej Sikorski (S.M. DIY Home)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * ── Architektura oszczędzania energii / Power saving architecture ─────────────
 *
 *  ┌───────────────────────────────────────────────────────────────┐
 *  │  setup()                                                      │
 *  │    • DCDC ON, NFC OFF, UART OFF (produkcja), pull-upy OFF     │
 *  │    • Wyłącz wszystkie diody LED (brak wymuszonego migania)    │
 *  │    • BLE advertising start (ciągłe, interwał 100 ms)          │
 *  │    • Inicjalizacja układu Watchdog (limit 8 sekund)           │
 *  └────────────────────┬──────────────────────────────────────────┘
 *                       │
 *  ┌───────────────────────────────────────────────────────────────┐
 *  │  loop()                                                       │
 *  │    • delay() → FreeRTOS usypia rdzeń (System ON sleep, WFE)   │
 *  │    • reset układu Watchdog (ochrona przed zawieszeniem RTOS)  │
 *  │    • packetId++                                               │
 *  │    • co BAT_MEASURE_EVERY cykli: odczyt SAADC → batteryLevel  │
 *  │    • przebuduj i nadaj ramkę ADV                              │
 *  └───────────────────────────────────────────────────────────────┘
 *
 *  Szacowane zużycie (3.7V LiPo):
 *    BLE advertising 100ms interwał:  ~90 µA średnio
 *    CPU WFE podczas delay():          ~3 µA
 *    Razem ≈ 93 µA → przy 200 mAh ≈ 90 dni
 */

// =============================================================================
//  Konfiguracja logowania / Logging configuration
//
//  DEBUG_ENABLED 1 → wszystkie logi (development)
//  DEBUG_ENABLED 0 → tylko WARN i ERROR (production, zero overhead)
// =============================================================================
#define DEBUG_ENABLED 1

#if DEBUG_ENABLED
  #define LOG_DEBUG(fmt, ...)  Serial.printf("[DEBUG] " fmt "\r\n", ##__VA_ARGS__)
  #define LOG_INFO(fmt, ...)   Serial.printf("[INFO]  " fmt "\r\n", ##__VA_ARGS__)
  #define LOG_PRINTLN(msg)     Serial.println(msg)
  #define LOG_PRINTF(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
#else
  #define LOG_DEBUG(fmt, ...)  ((void)0)
  #define LOG_INFO(fmt, ...)   ((void)0)
  #define LOG_PRINTLN(msg)     ((void)0)
  #define LOG_PRINTF(fmt, ...) ((void)0)
#endif
// WARN i ERROR logują zawsze — zdarzenia krytyczne produkcyjne
// WARN and ERROR always log — production-critical events
#define LOG_WARN(fmt, ...)   Serial.printf("[WARN]  " fmt "\r\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)  Serial.printf("[ERROR] " fmt "\r\n", ##__VA_ARGS__)

// =============================================================================

#include <bluefruit.h>
#include <nrf_soc.h>
#include <nrf_error.h>

// ─────────────────────────────────────────────────────────────────────────────
// Stałe konfiguracyjne / Configuration constants
// ─────────────────────────────────────────────────────────────────────────────

// Interwał aktualizacji ramki BLE / BLE frame update interval (ms)
const uint32_t UPDATE_INTERVAL_MS = 3000;

// Co ile cykli mierzyć baterię (20 cykli × 3s = co 60s)
// How many cycles between battery reads (20 × 3s = every 60s)
const uint8_t BAT_MEASURE_EVERY = 20;

// Timeout busy-wait SAADC w iteracjach pętli
// SAADC busy-wait timeout in loop iterations
const uint32_t SAADC_TIMEOUT = 50000UL;

// Interwał nadawania BLE: 160 × 0.625 ms = 100 ms
// BLE advertising interval: 160 × 0.625 ms = 100 ms
const uint16_t ADV_INTERVAL = 160;

// Pin włączający dzielnik napięcia baterii (LOW = pomiar aktywny)
// Battery voltage divider enable pin (LOW = measurement active)
#define PIN_VBAT_ENABLE 14

// Maksymalna moc nadajnika / Maximum TX power
// nRF52840: -40, -20, -16, -12, -8, -4, 0, +4, +8 dBm
#define TX_POWER_DBM 8

// Zakres napięcia LiPo / LiPo voltage range
#define LIPO_VMAX  4.20f
#define LIPO_VMIN  3.00f

// Dzielnik napięcia: R1=1MΩ (góra) + R2=510kΩ (dół)
// Voltage divider: R1=1MΩ (high) + R2=510kΩ (low)
#define VDIV_RATIO  ((1000.0f + 510.0f) / 510.0f)

// Pierwsze i ostatnie packetId zgodne z Shelly BLU Button1
#define PACKET_ID_MIN  152
#define PACKET_ID_MAX  250

// ─────────────────────────────────────────────────────────────────────────────
// Retained RAM — przeżywa soft reset, kasowany tylko przy power-on
// Retained RAM — survives soft reset, cleared only on power-on
//
// Sekcja .noinit nie jest zerowana przez startup, więc wartości z poprzedniego
// uruchomienia są dostępne natychmiast po resecie — bez czekania na pomiar.
// The .noinit section is not zeroed by startup, so values from the previous
// run are immediately available after reset — no wait for first measurement.
// ─────────────────────────────────────────────────────────────────────────────
static uint8_t  s_retainedBattery  __attribute__((section(".noinit")));
static uint8_t  s_retainedPacketId __attribute__((section(".noinit")));
static uint32_t s_retainedMagic    __attribute__((section(".noinit")));

// Wartość magiczna potwierdzająca że retained RAM jest zainicjowany
// Magic value confirming retained RAM has been initialized
#define RETAINED_MAGIC  0xBEAC0133UL

static bool retainedValid() {
  return s_retainedMagic == RETAINED_MAGIC;
}

static void retainedSave(uint8_t battery, uint8_t packetId) {
  s_retainedBattery  = battery;
  s_retainedPacketId = packetId;
  s_retainedMagic    = RETAINED_MAGIC;
}

// ─────────────────────────────────────────────────────────────────────────────
// Stan urządzenia / Device state
// ─────────────────────────────────────────────────────────────────────────────
struct BeaconState {
  uint8_t  packetId;       // licznik ramek BLE / BLE frame counter
  uint8_t  batteryLevel;   // poziom baterii [0–100 %] / battery level [0–100 %]
  uint8_t  mac[6];         // adres MAC (MSB-first) / MAC address (MSB-first)
  bool     advRunning;     // czy advertising jest aktywny / advertising active flag
  uint8_t  batCycleCount;  // licznik cykli do pomiaru baterii / battery cycle counter
};

// =============================================================================
// Prototypy funkcji (rozwiązanie błędu preprocesora Arduino IDE)
// Function prototypes (workaround for Arduino IDE preprocessor bug)
// =============================================================================
static bool buildAndSendFrames(const BeaconState &s);

static BeaconState g_beacon = {
  .packetId      = PACKET_ID_MIN,
  .batteryLevel  = 0,
  .mac           = { 0 },
  .advRunning    = false,
  .batCycleCount = 0,
};

// ─────────────────────────────────────────────────────────────────────────────
// Watchdog (ochrona przed zawieszeniem RTOS)
// Watchdog (protection against RTOS lockup)
// ─────────────────────────────────────────────────────────────────────────────
static void initWatchdog() {
  NRF_WDT->CONFIG = (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos) |   // zatrzymaj w debug / pause in debug
                    (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos);    // działaj w uśpieniu / run in sleep
  NRF_WDT->CRV = 32768 * 8; // limit 8 sekund / 8 seconds timeout
  NRF_WDT->RREN |= WDT_RREN_RR0_Msk; // włącz rejestr reload 0 / enable reload register 0
  NRF_WDT->TASKS_START = 1;
  LOG_DEBUG("Watchdog started (8s timeout)");
}

static void reloadWatchdog() {
  NRF_WDT->RR[0] = WDT_RR_RR_Reload;
}

// ─────────────────────────────────────────────────────────────────────────────
// Zarządzanie zegarem HFCLK (wymagane dla stabilnego ADC we FreeRTOS)
// HFCLK clock management (required for stable ADC in FreeRTOS)
// ─────────────────────────────────────────────────────────────────────────────
static bool hfclk_request() {
  uint32_t err = sd_clock_hfclk_request();
  if (err != NRF_SUCCESS) {
    LOG_ERROR("hfclk request failed: 0x%08X", err);
    return false;
  }
  uint32_t is_running = 0;
  while (1) {
    err = sd_clock_hfclk_is_running(&is_running);
    if (err != NRF_SUCCESS) {
      LOG_ERROR("hfclk is_running failed: 0x%08X", err);
      return false;
    }
    if (is_running) break;
    __WFE(); // CPU śpi czekając na start zegara / CPU sleeps waiting for clock
  }
  return true;
}

static void hfclk_release() {
  sd_clock_hfclk_release();
}

// ─────────────────────────────────────────────────────────────────────────────
// Wyłącz nieużywane peryferia sprzętowe
// Disable unused hardware peripherals
// ─────────────────────────────────────────────────────────────────────────────
static void disableUnusedPeripherals() {
#if !DEBUG_ENABLED
  NRF_UART0->ENABLE = 0;
#endif

  // NFC — domyślnie skonfigurowany jako NFC; wyłącz, odblokuj piny P0.09/P0.10 jako GPIO
  // NFC — configured as NFC by default; disable to free P0.09/P0.10 as GPIO
  NRF_NFCT->TASKS_DISABLE = 1;

  // I2C, SPI — nieużywane / unused
  NRF_TWIM0->ENABLE = 0;
  NRF_TWIM1->ENABLE = 0;
  NRF_SPIM0->ENABLE = 0;
  NRF_SPIM1->ENABLE = 0;
  NRF_SPIM2->ENABLE = 0;

  // SAADC globalnie wyłączony; włączany tylko na czas pomiaru baterii
  // SAADC globally disabled; enabled only during battery measurement
  NRF_SAADC->ENABLE = 0;

  // Wyłącz pull-upy na nieużywanych pinach GPIO — każdy pull to ~10 µA
  // Disable pulls on unused GPIO pins — each pull wastes ~10 µA
  // Piny aktywne: P0.06 (LED_BLUE), P0.14 (VBAT_ENABLE), P0.31 (PIN_VBAT)
  const uint8_t unusedPins[] = {
    2, 3, 4, 5,       // D0–D5 (user analog/I2C pins)
    11, 12, 13, 15,   // P1.11–P1.15 (user UART/SPI pins)
    26, 30,           // LED_RED (P0.26), LED_GREEN (P0.30)
  };
  for (uint8_t pin : unusedPins) {
    pinMode(pin, INPUT);  // INPUT = no pull in nRF52 BSP
  }

  LOG_DEBUG("Unused peripherals disabled");
}

// ─────────────────────────────────────────────────────────────────────────────
// Przełącznik anteny PCB / PCB antenna switch
// ─────────────────────────────────────────────────────────────────────────────
static void enableOnboardAntenna() {
#if defined(PIN_RF_SWITCH_PWR) && defined(PIN_RF_SWITCH)
  pinMode(PIN_RF_SWITCH_PWR, OUTPUT);
  digitalWrite(PIN_RF_SWITCH_PWR, HIGH);
  pinMode(PIN_RF_SWITCH, OUTPUT);
  digitalWrite(PIN_RF_SWITCH, HIGH);
  LOG_DEBUG("RF switch: PCB antenna selected");
#else
  LOG_DEBUG("RF switch: not defined (integrated antenna assumed)");
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Resetowanie rejestrów SAADC / SAADC registers reset
// ─────────────────────────────────────────────────────────────────────────────
static void resetSAADC() {
  NRF_SAADC->ENABLE = 0;
  NRF_SAADC->TASKS_STOP = 1;
  volatile int wait = 100;
  while (wait--);
  NRF_SAADC->EVENTS_STOPPED = 0;
  NRF_SAADC->EVENTS_END = 0;
  NRF_SAADC->EVENTS_STARTED = 0;
  NRF_SAADC->CH[0].CONFIG = 0;
  NRF_SAADC->CH[0].PSELP = 0;
  NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_10bit;
}

// ─────────────────────────────────────────────────────────────────────────────
// Odczyt napięcia baterii przez SAADC
// Battery voltage read via SAADC
//
// Zwraca false jeśli SAADC nie odpowiedział w czasie SAADC_TIMEOUT — wtedy
// caller może użyć poprzedniej wartości zamiast nadpisywać zerem.
// Returns false if SAADC did not respond within SAADC_TIMEOUT — caller can
// then keep the previous value instead of overwriting with zero.
//
// VBAT_ENABLE (P0.14) LOW  = dzielnik aktywny / divider active
// PIN_VBAT    (P0.31) AIN7 = wejście ADC / ADC input
// Gain 1/6, Vref=0.6 V, 10-bit → Vpin = raw × 3.6/1024, Vbat = Vpin × VDIV_RATIO
// ─────────────────────────────────────────────────────────────────────────────
static bool readBatteryPercent(uint8_t &outPercent) {
  reloadWatchdog(); // Reset przed dłuższą operacją / Reload before long operation

  if (!hfclk_request()) {
    return false;
  }

  // Włącz dzielnik / Enable voltage divider
  pinMode(PIN_VBAT_ENABLE, OUTPUT);
  digitalWrite(PIN_VBAT_ENABLE, LOW);
  delay(5);  // ustabilizuj napięcie / let voltage settle

  // Upewnij się że P0.31 jest w trybie analogowym / Ensure P0.31 is in analog mode
  pinMode(PIN_VBAT, INPUT);

  resetSAADC();

  NRF_SAADC->CH[0].CONFIG =
      (SAADC_CH_CONFIG_GAIN_Gain1_6    << SAADC_CH_CONFIG_GAIN_Pos)   |
      (SAADC_CH_CONFIG_MODE_SE         << SAADC_CH_CONFIG_MODE_Pos)   |
      (SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) |
      (SAADC_CH_CONFIG_TACQ_10us       << SAADC_CH_CONFIG_TACQ_Pos);
  NRF_SAADC->CH[0].PSELP  = SAADC_CH_PSELP_PSELP_AnalogInput7;  // P0.31 = AIN7 = PIN_VBAT
  NRF_SAADC->RESOLUTION   = SAADC_RESOLUTION_VAL_10bit;
  NRF_SAADC->ENABLE       = SAADC_ENABLE_ENABLE_Enabled;

  static int16_t result;
  result = 0;
  NRF_SAADC->RESULT.PTR    = (uint32_t)&result;
  NRF_SAADC->RESULT.MAXCNT = 1;

  // Busy-wait z timeoutem — zabezpieczenie przed nieskończoną pętlą
  // Busy-wait with timeout — guard against infinite loop
  uint32_t t;
  bool success = true;

  NRF_SAADC->TASKS_START = 1;
  for (t = 0; t < SAADC_TIMEOUT && !NRF_SAADC->EVENTS_STARTED; t++);
  NRF_SAADC->EVENTS_STARTED = 0;
  if (t >= SAADC_TIMEOUT) success = false;

  if (success) {
    NRF_SAADC->TASKS_SAMPLE = 1;
    for (t = 0; t < SAADC_TIMEOUT && !NRF_SAADC->EVENTS_END; t++);
    NRF_SAADC->EVENTS_END = 0;
    if (t >= SAADC_TIMEOUT) success = false;
  }

  NRF_SAADC->TASKS_STOP = 1;
  for (t = 0; t < SAADC_TIMEOUT && !NRF_SAADC->EVENTS_STOPPED; t++);
  NRF_SAADC->EVENTS_STOPPED = 0;

  // Wyłącz SAADC i dzielnik / Disable SAADC and divider
  NRF_SAADC->ENABLE = 0;
  digitalWrite(PIN_VBAT_ENABLE, HIGH);
  
  hfclk_release();

  if (!success) {
    LOG_ERROR("SAADC timeout!");
    return false;
  }

  float vbat = (result * 3.6f / 1024.0f) * VDIV_RATIO;

  // Krzywa rozładowania LiPo – segmentowa aproksymacja
  // LiPo discharge curve – piecewise linear approximation
  struct { float vHigh; float vLow; uint8_t pHigh; uint8_t pLow; } segs[] = {
    { 4.20f, 4.10f, 100,  95 },
    { 4.10f, 3.95f,  95,  80 },
    { 3.95f, 3.80f,  80,  65 },
    { 3.80f, 3.70f,  65,  50 },
    { 3.70f, 3.60f,  50,  35 },
    { 3.60f, 3.50f,  35,  20 },
    { 3.50f, 3.30f,  20,   8 },
    { 3.30f, 3.00f,   8,   0 },
  };

  uint8_t percent = 0;
  if (vbat >= LIPO_VMAX) {
    percent = 100;
  } else if (vbat > LIPO_VMIN) {
    for (auto &s : segs) {
      if (vbat <= s.vHigh && vbat > s.vLow) {
        float ratio = (vbat - s.vLow) / (s.vHigh - s.vLow);
        percent = (uint8_t)(s.pLow + ratio * (s.pHigh - s.pLow) + 0.5f);
        break;
      }
    }
  }

  LOG_DEBUG("BAT raw=%d  vbat=%.3fV  pct=%d%%", result, vbat, percent);
  outPercent = percent;
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Odczyt MAC z SoftDevice (LSB-first → MSB-first dla ramki Shelly)
// Read MAC from SoftDevice (LSB-first → MSB-first for Shelly frame)
//
// Zwraca false przy błędzie — setup() traktuje to jako błąd krytyczny.
// Returns false on error — setup() treats this as a fatal error.
// ─────────────────────────────────────────────────────────────────────────────
static bool readMac(uint8_t out[6]) {
  ble_gap_addr_t addr;
  uint32_t err = sd_ble_gap_addr_get(&addr);
  if (err != NRF_SUCCESS) {
    LOG_ERROR("sd_ble_gap_addr_get failed: 0x%08X", err);
    return false;
  }
  for (int i = 0; i < 6; i++) {
    out[i] = addr.addr[5 - i];
  }
  LOG_DEBUG("MAC: %02X:%02X:%02X:%02X:%02X:%02X",
            out[0], out[1], out[2], out[3], out[4], out[5]);
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Buduj i nadawaj ramkę BLE / Build and transmit BLE frame
//
// Zwraca false jeśli Advertising.start() nie powiódł się.
// Returns false if Advertising.start() failed.
// ─────────────────────────────────────────────────────────────────────────────
static bool buildAndSendFrames(const BeaconState &s) {

  // !! Nie zmieniać kolejności ani wartości stałych bajtów !!
  // !! Do not change the order or values of constant bytes !!
  uint8_t advRaw[31] = {
    0x02, 0x01, 0x06,
    0x0A, 0x16, 0xD2, 0xFC, 0x44, 0x00, s.packetId, 0x01, s.batteryLevel, 0x3A, 0x00,
    0x10, 0xFF,
    0xA9, 0x0B, 0x01, 0x09, 0x00, 0x0B, 0x01, 0x00, 0x0A,
    s.mac[5], s.mac[4], s.mac[3], s.mac[2], s.mac[1], s.mac[0]
  };
  uint8_t scanRaw[11] = { 0x0A, 0x08, 'S', 'B', 'B', 'T', '-', '0', '0', '2', 'C' };

  LOG_DEBUG("ADV frame: packetId=%d  batt=%d%%", s.packetId, s.batteryLevel);

  if (s.advRunning) {
    Bluefruit.Advertising.stop();
  }

  Bluefruit.Advertising.clearData();
  if (!Bluefruit.Advertising.setData(advRaw, sizeof(advRaw))) {
    LOG_ERROR("Advertising.setData() failed");
    return false;
  }

  Bluefruit.ScanResponse.clearData();
  if (!Bluefruit.ScanResponse.setData(scanRaw, sizeof(scanRaw))) {
    LOG_ERROR("ScanResponse.setData() failed");
    return false;
  }

  Bluefruit.Advertising.setInterval(ADV_INTERVAL, ADV_INTERVAL);

  if (!Bluefruit.Advertising.start(0)) {
    LOG_ERROR("Advertising.start() failed");
    return false;
  }

  LOG_INFO("Advertising: packetId=%d  batt=%d%%  interval=%d×0.625ms",
           s.packetId, s.batteryLevel, ADV_INTERVAL);
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Funkcja startowa / Initial setup
// ─────────────────────────────────────────────────────────────────────────────
void setup() {

#if DEBUG_ENABLED
  Serial.begin(115200);
  for (int i = 0; i < 30 && !Serial; i++) delay(100);
  delay(300);
  LOG_PRINTLN("=== BEACON v3.6 ===");
#endif

  // DCDC converter — lepsza sprawność vs LDO (~20% oszczędności prądu)
  // DCDC converter — better efficiency vs LDO (~20% current saving)
  NRF_POWER->DCDCEN = 1;

  disableUnusedPeripherals();
  enableOnboardAntenna();

  if (!Bluefruit.begin(1, 0)) {
    LOG_ERROR("Bluefruit.begin() failed — halting");
    while (true) delay(1000);
  }

  // Wyłącz miganie diody / Disable LED blinking
  Bluefruit.autoConnLed(false);
  pinMode(LED_BLUE, OUTPUT);
  digitalWrite(LED_BLUE, HIGH);   // active-LOW: HIGH = zgaszona / off

  // Ustaw moc TX / Set TX power
  Bluefruit.setTxPower(TX_POWER_DBM);
  LOG_INFO("TX power: +%d dBm (confirmed: %d dBm)", TX_POWER_DBM, Bluefruit.getTxPower());

  // Odczyt MAC — błąd krytyczny: bez MAC beacon jest bezużyteczny
  // Read MAC — fatal error: without MAC the beacon is useless
  if (!readMac(g_beacon.mac)) {
    LOG_ERROR("MAC read failed — halting");
    while (true) delay(1000);
  }

  // Uruchom Watchdog po włączeniu BLE
  // Start Watchdog after BLE is up
  initWatchdog();

  // Przywróć stan z retained RAM jeśli dostępny (po soft reset)
  // Restore state from retained RAM if available (after soft reset)
  if (retainedValid()) {
    g_beacon.batteryLevel = s_retainedBattery;
    g_beacon.packetId     = s_retainedPacketId;
    LOG_INFO("Retained: battery=%d%%  packetId=%d", g_beacon.batteryLevel, g_beacon.packetId);
  } else {
    // Pierwsze uruchomienie (power-on) — zmierz baterię natychmiast
    // First boot (power-on) — measure battery immediately
    uint8_t initLevel;
    if (readBatteryPercent(initLevel)) {
      g_beacon.batteryLevel = initLevel;
    }
    retainedSave(g_beacon.batteryLevel, g_beacon.packetId);
    LOG_INFO("First boot: battery=%d%%", g_beacon.batteryLevel);
  }

  if (!buildAndSendFrames(g_beacon)) {
    LOG_ERROR("Initial advertising failed — halting");
    while (true) delay(1000);
  }
  g_beacon.advRunning = true;

  LOG_PRINTLN("=== SETUP DONE ===");
}

// ─────────────────────────────────────────────────────────────────────────────
// Główna pętla programu / Main program loop
//
// Zastępuje onTimer(). Użycie delay() we FreeRTOS oddaje czas procesora
// i usypia układ (WFE), zachowując identyczny, niski pobór prądu.
// Replaces onTimer(). Using delay() in FreeRTOS yields CPU time
// and puts the chip to sleep (WFE), maintaining identical low power consumption.
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  
  // Czekaj zadany czas, usypiając CPU (oszczędzanie baterii)
  // Wait specified time, putting CPU to sleep (saving battery)
  delay(UPDATE_INTERVAL_MS);

  // Zabezpieczenie Watchdoga / Watchdog safeguard
  reloadWatchdog();

  g_beacon.packetId++;
  if (g_beacon.packetId > PACKET_ID_MAX) g_beacon.packetId = PACKET_ID_MIN;

  // Pomiar baterii tylko co BAT_MEASURE_EVERY cykli
  // Measure battery only every BAT_MEASURE_EVERY cycles
  g_beacon.batCycleCount++;
  if (g_beacon.batCycleCount >= BAT_MEASURE_EVERY) {
    g_beacon.batCycleCount = 0;
    uint8_t newLevel;
    
    // Bezpieczne wywołanie — nie blokuje zadań RTOS / Safe call — does not block RTOS tasks
    if (readBatteryPercent(newLevel)) {
      g_beacon.batteryLevel = newLevel;
      // Zapisz do retained RAM — dostępne natychmiast po ewentualnym resecie
      // Save to retained RAM — immediately available after a potential reset
      retainedSave(g_beacon.batteryLevel, g_beacon.packetId);
      LOG_INFO("Battery: %d%%", g_beacon.batteryLevel);
    } else {
      // Błąd SAADC — zachowaj poprzednią wartość, zaloguj ostrzeżenie
      // SAADC error — keep previous value, log warning
      LOG_WARN("Battery read failed — keeping previous value: %d%%", g_beacon.batteryLevel);
    }
  }

  // Nadaj ramkę; przy błędzie advRunning = false → następny cykl spróbuje ponownie
  // Transmit frame; on error advRunning = false → next cycle will retry
  if (buildAndSendFrames(g_beacon)) {
    g_beacon.advRunning = true;
  } else {
    g_beacon.advRunning = false;
    LOG_WARN("Advertising failed — will retry in %dms", UPDATE_INTERVAL_MS);
  }
}
