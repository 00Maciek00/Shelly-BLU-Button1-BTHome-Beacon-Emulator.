# Shelly BLU Button1 BTHome Beacon Emulator

[Polski](#polski) | [English](#english)

---

## Polski

Emulator beacona BTHome kompatybilny z urządzeniem Shelly BLU Button1. Projekt umożliwia emulację ramek BLE (advertising i scan response) w identycznej postaci bajt po bajcie jak oryginalne urządzenie Shelly, co pozwala na bezproblemową integrację z systemami obsługującymi protokół BTHome, takimi jak Home Assistant czy aplikacja Shelly.

![PCB](images/PCB.jpg)

### Funkcjonalności

- **Emulacja 1:1** – ramki BLE są identyczne jak w oryginalnym Shelly BLU Button1.
- **Monitorowanie baterii** – odczyt napięcia baterii przez wbudowany dzielnik napięcia (SAADC) i wysyłanie wartości procentowej (0–100%) w każdym pakiecie.
- **Niska emisja energii** – zastosowanie energooszczędnych ustawień BLE oraz wyłączenie nieużywanych peryferiów (UART, PWM).
- **Maksymalna moc nadajnika** – ręczne ustawienie mocy nadajnika na +8 dBm.
- **Kompatybilność** – działa z Home Assistant (integracja BTHome) oraz aplikacją Shelly.

### Wymagania sprzętowe

- **Płytka**: Seeed XIAO nRF52840 (lub inna kompatybilna z ArduinoBLE i nRF52840).
- **Zasilanie**: bateria litowo-jonowa (Li-Ion) o napięciu nominalnym 3.7 V. Dzielnik napięcia jest kalibrowany dla zakresu 3.2 V (0%) – 4.18 V (100%).
- **Podłączenie**: należy podłączyć dodatni biegun baterii do pinu `VBAT` płytki XIAO (przez dzielnik napięcia wbudowany w płytkę). Dzielnik napięcia jest włączany pinem `PIN_VBAT_ENABLE` (GPIO 14).

### Konfiguracja i uruchomienie

1. Zainstaluj bibliotekę **ArduinoBLE** w Arduino IDE.
2. Skopiuj kod `beacon.ino` do nowego szkicu.
3. Wybierz płytkę **Seeed XIAO nRF52840** i odpowiedni port.
4. Skompiluj i wgraj program na płytkę.

Po uruchomieniu urządzenie zacznie nadawać ramki BLE z interwałem 400 ms. Co 3 sekundy aktualizowany jest licznik pakietów oraz stan baterii.

### Znane ograniczenia

**Migająca niebieska dioda LED**  
Biblioteka ArduinoBLE wymusza krótkie mignięcie niebieskiej diody LED przy każdym wysłaniu pakietu BLE. Jest to wewnętrzne zachowanie biblioteki, które nie może być wyłączone bez modyfikacji jej kodu źródłowego. W kodzie zastosowano próby sprzętowego odłączenia pinów LED i wyłączenia modułów PWM, jednak ArduinoBLE reaktywuje PWM przy każdym wywołaniu `advertise()`. W związku z tym dioda będzie migać w rytm nadawania pakietów (co 400 ms). Nie wpływa to na funkcjonalność emulatora.

### Licencja

Projekt udostępniony na licencji **Apache License 2.0**. Szczegóły w pliku [LICENSE](LICENSE).

### Autor

**Wersja:** 1.0  
**Autor:** Maciej Sikorski  
**Data:** 01.04.2026  
**Licencja:** Apache 2.0

---

## English

BTHome beacon emulator compatible with the Shelly BLU Button1 device. This project emulates BLE advertising and scan response frames byte-for-byte identical to the original Shelly device, enabling seamless integration with BTHome-compatible systems such as Home Assistant or the Shelly app.

![PCB](images/PCB.jpg)

### Features

- **1:1 Emulation** – BLE frames are identical to the original Shelly BLU Button1.
- **Battery Monitoring** – voltage readout via the built-in voltage divider (SAADC) and transmission of percentage value (0–100%) in every packet.
- **Low Power** – power-efficient BLE settings and disabling of unused peripherals (UART, PWM).
- **Maximum Transmitter Power** – manual setting of transmitter power to +8 dBm.
- **Compatibility** – works with Home Assistant (BTHome integration) and the Shelly app.

### Hardware Requirements

- **Board**: Seeed XIAO nRF52840 (or any ArduinoBLE-compatible nRF52840 board).
- **Power**: Li-Ion battery with nominal voltage 3.7 V. The voltage divider is calibrated for the range 3.2 V (0%) – 4.18 V (100%).
- **Connection**: Connect the positive battery terminal to the `VBAT` pin of the XIAO board (through the built-in voltage divider). The voltage divider is enabled via the `PIN_VBAT_ENABLE` pin (GPIO 14).

### Configuration and Setup

1. Install the **ArduinoBLE** library in the Arduino IDE (or PlatformIO).
2. Copy the `beacon.ino` code into a new sketch.
3. Select the **Seeed XIAO nRF52840** board and the appropriate port.
4. Compile and upload the program to the board.

After startup, the device will start broadcasting BLE frames with an interval of 400 ms. Every 3 seconds, the packet counter and battery status are updated.

### Known Limitations

**Blue LED blinking**  
The ArduinoBLE library forces a brief blue LED blink on every BLE packet transmission. This is internal library behaviour and cannot be disabled without modifying its source code. The sketch attempts to hardware-disconnect the LED pins and disable PWM modules, but ArduinoBLE reactivates PWM on each `advertise()` call. Consequently, the LED will blink in sync with packet transmission (every 400 ms). This does not affect the emulator's functionality.

### License

This project is licensed under the **Apache License 2.0**. See the [LICENSE](LICENSE) file for details.

### Author

**Version:** 1.0  
**Author:** Maciej Sikorski  
**Date:** 01.04.2026  
**License:** Apache 2.0
