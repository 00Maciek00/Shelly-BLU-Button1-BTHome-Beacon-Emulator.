/**
 * @file    beacon.ino
 * @author  Maciej Sikorski
 * @version 1.0
 * @date    01.04.2026
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
 * UWAGA: Biblioteka ArduinoBLE wymusza krótkie mignięcie niebieskiej diody
 * LED podczas każdego wysłania pakietu BLE. Jest to zachowanie wewnętrzne
 * biblioteki i nie można go wyłączyć bez modyfikacji jej kodu źródłowego.
 * Próba sprzętowego odłączenia pinów przez killAllLEDs() jest nieskuteczna,
 * ponieważ biblioteka reaktywuje PWM przy każdym wywołaniu advertise().
 *
 * EN:
 * This sketch emulates the BLE advertising frame broadcast by the Shelly BLU Button1.
 * Both the advertising packet and scan response are byte-for-byte identical to the
 * original Shelly device, enabling integration with BTHome-compatible systems
 * (e.g. Home Assistant, Shelly app).
 * Battery level is read from the built-in voltage divider (SAADC) and transmitted
 * in every packet as a percentage value (0–100%).
 *
 * NOTE: The ArduinoBLE library forces a brief blue LED blink on every BLE packet
 * transmission. This is internal library behaviour and cannot be disabled without
 * modifying its source code. Attempts to hardware-disconnect the pins via
 * killAllLEDs() are ineffective because the library reactivates PWM on each
 * advertise() call.
 *
 * Hardware / Sprzęt:
 *   Seeed XIAO nRF52840
 *
 * Dependencies / Zależności:
 *   ArduinoBLE (https://github.com/arduino-libraries/ArduinoBLE)
 *
 * License / Licencja:
 *   Apache License 2.0
 *   https://www.apache.org/licenses/LICENSE-2.0
 *
 * Copyright (c) 2026 Maciej Sikorski (S.M. DIY Home)
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
 */

#include <ArduinoBLE.h>

// PL: Piny RGB LED dla Seeed XIAO nRF52840 (diody aktywne stanem LOW)
// EN: RGB LED pins for Seeed XIAO nRF52840 (active LOW)
#ifndef LEDR
  #define LEDR  11   // P0.26
  #define LEDG  13   // P0.30
  #define LEDB  12   // P0.06
#endif

// PL: Interwał między aktualizacjami pakietu (ms)
// EN: Interval between packet updates (ms)
const unsigned long UPDATE_INTERVAL_MS = 3000;

// PL: Licznik pakietów (152–250), inkrementowany co cykl
// EN: Packet counter (152–250), incremented each cycle
uint8_t packetId = 152;

// PL: Stan baterii w procentach, aktualizowany co cykl
// EN: Battery level in percent, updated each cycle
uint8_t batteryLevel = 80;

// PL: Adres MAC płytki (6 bajtów, MSB-first)
// EN: Board MAC address (6 bytes, MSB-first)
uint8_t myMac[6];

// PL: Pin włączający dzielnik napięcia baterii (stan LOW = pomiar aktywny)
// EN: Pin enabling battery voltage divider (LOW = measurement active)
#define PIN_VBAT_ENABLE 14


// ─────────────────────────────────────────────────────────────────────────────
// PL: Sprzętowe odłączenie pinu GPIO – wejście bez bufora, bez pull-up/down
// EN: Hardware GPIO pin disconnect – input with buffer disabled, no pull
// ─────────────────────────────────────────────────────────────────────────────
static inline void disconnectPin(NRF_GPIO_Type* port, uint8_t bit) {
  port->PIN_CNF[bit] =
      (GPIO_PIN_CNF_DIR_Input        << GPIO_PIN_CNF_DIR_Pos)   |
      (GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos) |
      (GPIO_PIN_CNF_PULL_Disabled    << GPIO_PIN_CNF_PULL_Pos)  |
      (GPIO_PIN_CNF_SENSE_Disabled   << GPIO_PIN_CNF_SENSE_Pos);
}

// ─────────────────────────────────────────────────────────────────────────────
// PL: Wyłącza wszystkie moduły PWM i sprzętowo odłącza piny LED
//     UWAGA: ArduinoBLE reaktywuje PWM przy każdym advertise() –
//     niebieska dioda będzie nadal migać mimo wywołania tej funkcji
// EN: Disables all PWM modules and hardware-disconnects LED pins
//     NOTE: ArduinoBLE reactivates PWM on each advertise() call –
//     the blue LED will still blink despite calling this function
// ─────────────────────────────────────────────────────────────────────────────
void killAllLEDs() {
  NRF_PWM0->ENABLE = 0;
  NRF_PWM1->ENABLE = 0;
  NRF_PWM2->ENABLE = 0;
  disconnectPin(NRF_P0, 26);   // LED_RED
  disconnectPin(NRF_P0, 30);   // LED_GREEN
  disconnectPin(NRF_P0,  6);   // LED_BLUE
}

// ─────────────────────────────────────────────────────────────────────────────
// PL: Włącza wbudowaną antenę (tylko dla wariantów z przełącznikiem RF)
// EN: Enables onboard antenna (only for variants with RF switch)
// ─────────────────────────────────────────────────────────────────────────────
void enableOnboardAntenna() {
#if defined(PIN_RF_SWITCH_PWR) && defined(PIN_RF_SWITCH)
  pinMode(PIN_RF_SWITCH_PWR, OUTPUT);
  digitalWrite(PIN_RF_SWITCH_PWR, HIGH);
  pinMode(PIN_RF_SWITCH, OUTPUT);
  digitalWrite(PIN_RF_SWITCH, HIGH);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// PL: Ustawia maksymalną moc nadajnika (+8 dBm)
//     Bezpośredni zapis do rejestru NRF_RADIO->TXPOWER po advertise().
//     SoftDevice może nadpisać tę wartość – skuteczność zależy od wersji BSP.
// EN: Sets transmitter to maximum power (+8 dBm)
//     Direct write to NRF_RADIO->TXPOWER register after advertise().
//     SoftDevice may override this value – effectiveness depends on BSP version.
// ─────────────────────────────────────────────────────────────────────────────
void setMaxTxPower() {
  NRF_RADIO->TXPOWER = (uint32_t)RADIO_TXPOWER_TXPOWER_Pos8dBm;
}

// ─────────────────────────────────────────────────────────────────────────────
// PL: Odczyt napięcia baterii przez SAADC (polling, bez przerwań)
//     Wzmocnienie 1/6, Vref=0.6V, rozdzielczość 10-bit, dzielnik napięcia x2
//     Zakres: 3.2V (0%) – 4.18V (100%)
// EN: Battery voltage readout via SAADC (polling, no interrupts)
//     Gain 1/6, Vref=0.6V, 10-bit resolution, voltage divider x2
//     Range: 3.2V (0%) – 4.18V (100%)
// ─────────────────────────────────────────────────────────────────────────────
uint8_t readBatteryPercent() {
  pinMode(PIN_VBAT_ENABLE, OUTPUT);
  digitalWrite(PIN_VBAT_ENABLE, LOW);   // PL: włącz dzielnik / EN: enable divider
  delay(5);

  NRF_SAADC->CH[0].CONFIG =
      (SAADC_CH_CONFIG_GAIN_Gain1_6    << SAADC_CH_CONFIG_GAIN_Pos)   |
      (SAADC_CH_CONFIG_MODE_SE         << SAADC_CH_CONFIG_MODE_Pos)   |
      (SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) |
      (SAADC_CH_CONFIG_TACQ_10us       << SAADC_CH_CONFIG_TACQ_Pos);
  NRF_SAADC->CH[0].PSELP  = SAADC_CH_PSELP_PSELP_AnalogInput7;
  NRF_SAADC->RESOLUTION   = SAADC_RESOLUTION_VAL_10bit;
  NRF_SAADC->ENABLE       = SAADC_ENABLE_ENABLE_Enabled;

  static int16_t result;
  NRF_SAADC->RESULT.PTR    = (uint32_t)&result;
  NRF_SAADC->RESULT.MAXCNT = 1;

  NRF_SAADC->TASKS_START = 1;
  while (!NRF_SAADC->EVENTS_STARTED);   NRF_SAADC->EVENTS_STARTED = 0;
  NRF_SAADC->TASKS_SAMPLE = 1;
  while (!NRF_SAADC->EVENTS_END);       NRF_SAADC->EVENTS_END     = 0;
  NRF_SAADC->TASKS_STOP = 1;
  while (!NRF_SAADC->EVENTS_STOPPED);   NRF_SAADC->EVENTS_STOPPED = 0;
  NRF_SAADC->ENABLE = 0;

  digitalWrite(PIN_VBAT_ENABLE, HIGH);  // PL: wyłącz dzielnik / EN: disable divider

  float vbat    = (result * 3.6f / 1024.0f) * 2.0f;
  float percent = (vbat - 3.2f) / (4.18f - 3.2f) * 100.0f;
  if (percent < 0.0f)   percent = 0.0f;
  if (percent > 100.0f) percent = 100.0f;
  return (uint8_t)percent;
}

// ─────────────────────────────────────────────────────────────────────────────
// PL: Odczytuje adres MAC z BLE i zapisuje w myMac[] w kolejności MSB-first
// EN: Reads MAC address from BLE and stores in myMac[] in MSB-first order
// ─────────────────────────────────────────────────────────────────────────────
void parseMac() {
  String addr = BLE.address();
  for (int i = 0; i < 6; i++) {
    myMac[i] = strtoul(addr.substring(i * 3, i * 3 + 2).c_str(), nullptr, 16);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// PL: Buduje i wysyła ramkę BLE identyczną z Shelly BLU Button1
//
//     Struktura pakietu advertising (advRaw, 31 bajtów):
//       [0..2]   Flags: LE General Discoverable, BR/EDR Not Supported
//       [3..13]  Service Data UUID 0xFCD2 (BTHome): packetId, batteryLevel
//       [14..30] Manufacturer Specific Data: dane urządzenia + MAC (LSB-first)
//
//     Scan Response (scanRaw, 11 bajtów):
//       Skrócona nazwa urządzenia: "SBBT-002C"
//
// EN: Builds and sends BLE frame identical to Shelly BLU Button1
//
//     Advertising packet structure (advRaw, 31 bytes):
//       [0..2]   Flags: LE General Discoverable, BR/EDR Not Supported
//       [3..13]  Service Data UUID 0xFCD2 (BTHome): packetId, batteryLevel
//       [14..30] Manufacturer Specific Data: device data + MAC (LSB-first)
//
//     Scan Response (scanRaw, 11 bytes):
//       Shortened local name: "SBBT-002C"
// ─────────────────────────────────────────────────────────────────────────────
void buildAndSendFrames() {

  // !! nie zmieniać kolejności ani wartości stałych bajtów !!
  // !! do not change the order or values of constant bytes !!
  uint8_t advRaw[31] = {
    0x02, 0x01, 0x06,
    0x0A, 0x16, 0xD2, 0xFC, 0x44, 0x00, packetId, 0x01, batteryLevel, 0x3A, 0x00,
    0x10, 0xFF,
    0xA9, 0x0B, 0x01, 0x09, 0x00, 0x0B, 0x01, 0x00, 0x0A,
    myMac[5], myMac[4], myMac[3], myMac[2], myMac[1], myMac[0]
  };
  uint8_t scanRaw[11] = { 0x0A, 0x08, 'S', 'B', 'B', 'T', '-', '0', '0', '2', 'C' };

  BLEAdvertisingData advData;
  advData.setRawData(advRaw, sizeof(advRaw));
  BLEAdvertisingData scanData;
  scanData.setRawData(scanRaw, sizeof(scanRaw));

  BLE.stopAdvertise();
  BLE.setAdvertisingData(advData);
  BLE.setScanResponseData(scanData);
  BLE.setAdvertisingInterval(640);   // PL: 640 × 0.625 ms = 400 ms / EN: same
  BLE.advertise();

  // PL: Próba wyłączenia PWM/LED po advertise() – częściowo skuteczna
  // EN: Attempt to disable PWM/LED after advertise() – partially effective
  NRF_PWM0->ENABLE = 0;
  NRF_PWM1->ENABLE = 0;
  NRF_PWM2->ENABLE = 0;

  setMaxTxPower();
  killAllLEDs();
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  // PL: Włącz przetwornicę DCDC – lepsza sprawność energetyczna
  // EN: Enable DCDC converter – better power efficiency
  NRF_POWER->DCDCEN = 1;

  // PL: Sprzętowo wyłącz LED przed inicjalizacją BLE
  // EN: Hardware-disable LEDs before BLE initialization
  killAllLEDs();

  enableOnboardAntenna();

  if (!BLE.begin()) {
    // PL: Błąd inicjalizacji – sygnalizacja przez czerwoną diodę
    // EN: Initialization error – signal via red LED
    pinMode(LEDR, OUTPUT);
    while (1) {
      digitalWrite(LEDR, LOW);  delay(100);
      digitalWrite(LEDR, HIGH); delay(100);
    }
  }

  // PL: Wyłącz UART – oszczędność energii
  // EN: Disable UART – power saving
  NRF_UART0->ENABLE = 0;

  parseMac();
  batteryLevel = readBatteryPercent();
  buildAndSendFrames();
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  static unsigned long lastUpdate = 0;

  // PL: Ponowna próba wyłączenia LED w każdej iteracji pętli
  // EN: Retry LED disable on each loop iteration
  killAllLEDs();

  if (millis() - lastUpdate >= UPDATE_INTERVAL_MS) {
    lastUpdate = millis();

    // PL: Inkrementacja licznika pakietów z zawinięciem
    // EN: Packet counter increment with wraparound
    packetId++;
    if (packetId > 250) packetId = 152;

    batteryLevel = readBatteryPercent();
    buildAndSendFrames();
  }
}
