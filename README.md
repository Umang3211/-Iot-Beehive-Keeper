# IoT Beehive Keeper — v3.1

A solar-powered ESP32 monitoring system for beehives. Logs temperature, humidity, sound spectrum, and battery health to an SD card and serves a real-time dashboard with research-grade derived metrics over WiFi. Supports wireless firmware updates (OTA).

## What it does

The device sits on a hive, reads its sensors every 15 seconds, logs a row to the SD card every 15 minutes, records a 3-second WAV audio clip every hour (plus extra clips on acoustic events), and serves a live web dashboard at `http://192.168.4.1` (AP mode) or its DHCP-assigned IP (STA mode).

### Raw sensors

- Brood temperature (DS18B20 waterproof probe inside the hive)
- Humidity (DHT22, replacing the older DHT11 for accuracy and readings above 90% RH)
- Sound (SPH0645 I2S microphone, 16 kHz mono)
- Battery voltage (via voltage divider on GPIO 35, optional)

### Derived scientific metrics

- Dewpoint, Magnus formula
- Vapor Pressure Deficit (VPD), Tetens equation
- FFT band-power in four bands: low (0–100 Hz), worker (100–300 Hz), queen (300–600 Hz), agitation (600–2k Hz)
- Queen Index, ratio of queen-band to worker-band power
- Spectral Centroid, "center of mass" of the sound spectrum
- Wingbeat Peak Frequency, dominant frequency in 100–500 Hz
- Temperature Stability Index, rolling 60-min standard deviation of brood temp (Stabentheiner thermoregulation metric)
- Swarm Warning, temperature derivative >0.1°C/min sustained above 35.5°C
- Fanning Detector, cross-sensor trigger when temp, sound, and wingbeat all align
- Acoustic Event Detector, RMS spike >2σ above 10-min baseline fires a tagged WAV recording
- Hive Health Score, composite 0–100 weighted by research priorities
- Battery State of Charge, Li-ion voltage curve, with charging direction inferred from voltage trend

### Hive State Machine

Outputs one categorical status: HEALTHY / WARM / COOL / HUMID / DRY / OVERHEATING / FREEZING / CONDENSATION_RISK / SWARM_WARNING / SILENT_HIVE / NO_DATA.

### Wireless firmware updates (OTA)

Once an OTA-capable firmware version has been loaded over USB at least once, all future firmware updates can be pushed wirelessly through the dashboard's `/update` endpoint. No need to open a sealed enclosure to update the device.

## Hardware

| Component | Purpose |
|---|---|
| ESP32 WROOM-32 dev board | Microcontroller, WiFi |
| DS18B20 waterproof probe | Hive temperature |
| DHT22 | Humidity (more accurate and higher RH range than DHT11) |
| SPH0645 | I2S microphone |
| DS3231 | Real-time clock with coin-cell battery (CR2032) |
| Adafruit BQ25185 | USB/DC/Solar Li-ion charger with 5V boost |
| 18650 cell (3000 mAh+) | Primary power, name-brand (Samsung 30Q, LG MJ1, etc.) |
| Premade solar panel with USB-C output | Charging, plugs directly into BQ25185's USB-C input |
| Catalex-style microSD module + 32 GB or larger FAT32 card | CSV log + WAV audio storage |
| Two 100 kΩ resistors | Battery voltage divider (optional but recommended) |
| One 10 µF ceramic capacitor | Optional: EN-GND for reliable auto-reset over USB |
| One 10 kΩ resistor | DHT22 pullup, if using the bare 4-pin sensor |

## Wiring

### DS18B20 (temperature)
```
Red    → 3.3V
Black  → GND
Yellow → GPIO 4
+ 4.7 kΩ pullup between Yellow and 3.3V (built into most breakouts)
```

### DHT22 (humidity)
```
VCC   → 3.3V
Data  → GPIO 14
GND   → GND
+ 10 kΩ pullup between Data and 3.3V (built into 3-pin modules; required if using a bare 4-pin sensor)
```
The DHT22 is a drop-in replacement for the DHT11 (same wires, same pin). Change `#define DHT_TYPE` to `DHT22` in the firmware. Reads above 90% RH, which the DHT11 cannot.

### SPH0645 (microphone)
```
3V    → 3.3V
GND   → GND
BCLK  → GPIO 26
DOUT  → GPIO 32
LRCL  → GPIO 25
SEL   → GND (or unconnected)
```

### microSD Card module (Catalex-style)
```
5V    → ESP32 5V/VIN     (the module's onboard regulator needs 5V input, not 3.3V)
3V    → unconnected
GND   → GND
CLK   → GPIO 18
DI    → GPIO 23           (data into the module, equivalent to MOSI)
DO    → GPIO 19           (data out of the module, equivalent to MISO)
CS    → GPIO 5
CD    → unconnected
```
Common gotcha: if VCC is wired to 3.3V instead of 5V, the module's internal regulator outputs only about 2.8V and the card fails to initialize (`cardType=NONE`).

### DS3231 RTC (I2C)
```
VCC  → 3.3V
GND  → GND
SDA  → GPIO 21
SCL  → GPIO 22
32K, SQW → unconnected
```
Install a CR2032 coin cell on the back of the module before deployment, otherwise time resets on every power loss.

### BQ25185 charger
```
Solar panel USB-C output → BQ25185 USB-C input port
Battery (18650)          → BQ25185 battery JST connector
BQ25185 5V output        → ESP32 VIN
BQ25185 GND              → ESP32 GND
```

Using a premade solar panel with a USB-C output simplifies the power side. The panel plugs directly into the charger's USB-C input, with no separate solar terminal wiring required.

### Battery voltage monitoring (optional, recommended)
```
BAT pin (BQ25185) ──[100 kΩ]──┬──[100 kΩ]── GND
                              │
                              └── GPIO 35 (ESP32, input-only ADC1)
```
The `BATT_CAL` constant in firmware compensates for ADC input loading. After installing, multimeter the BAT pin and adjust `BATT_CAL` to match if needed.

### Auto-reset capacitor (optional, for clean USB uploads)
```
ESP32 EN ──[10 µF ceramic cap, "106"]── GND
```
Without this, the DTR/RTS pulse during upload may not reliably trigger the bootloader, requiring you to hold the BOOT button during every flash. Less important once OTA is enabled, since you'll rarely use USB.

## Setup

1. Install [Arduino IDE](https://www.arduino.cc/en/software) or [arduino-cli](https://arduino.github.io/arduino-cli/).
2. Add ESP32 board manager URL: `https://espressif.github.io/arduino-esp32/package_esp32_index.json` and install the `esp32:esp32` core (3.x).
3. Install libraries: OneWire, DallasTemperature, DHT sensor library, RTClib, arduinoFFT.
4. Open `beehive_v3_1.ino`, edit these lines near the top to set the **default** WiFi credentials baked into the firmware:
   ```cpp
   const char* STA_SSID = "YOUR_WIFI_SSID";
   const char* STA_PASS = "YOUR_WIFI_PASSWORD";  // "" for open networks
   ```
   These defaults are only used on a fresh device that has no saved credentials yet. After the first flash, you can change networks at any time through the dashboard's "Setup WiFi" form — the new credentials are saved to non-volatile storage and override the compiled-in defaults. They survive reboots and re-flashes.
5. If your WiFi network blocks device-to-device traffic (most "guest" networks do by default), set `FORCE_AP_MODE 1` and connect to the device's own `Beehive-AP` (password `beehive123`) at `http://192.168.4.1`. Alternatively, ask your network administrator to disable client isolation for the device. Note: even in STA-first mode, if the device boots out of WiFi range and falls back to AP, it automatically retries STA every 10 minutes, so it will rejoin the configured network when it comes back into range without needing a power cycle.
6. Format your SD card as FAT32 (not exFAT).
7. Flash to ESP32 Dev Module. After this first USB flash, all future firmware updates can be done wirelessly via the `/update` page.

## Dashboard

Access by visiting either:
- `http://<DHCP-IP>` (printed in serial monitor as `STA connected. IP: x.x.x.x`)
- `http://192.168.4.1` if device fell back to AP mode

### Layout

- Hero panel: Hive state badge, plain-language summary, key numbers (health, temp, humidity, sound, battery, timestamp)
- 3 primary metric cards: Temperature, Humidity, Activity, each with a status badge and a per-metric "Learn more" expand panel
- 2-week sparkline charts from SD log
- Advanced Indicators: Health Score, Temp Stability, Fanning detector, Acoustic Event counter
- Climate Details: VPD, Dewpoint Margin, Dewpoint
- Acoustic Details: Wingbeat Peak, Queen Index, Spectral Centroid, RMS, FFT band powers
- Power: Battery V, %, charging status
- Audio Clips: HTML5 audio players for every WAV on SD, with download buttons, plus a "Record 3 sec now" button to trigger an immediate clip
- Device Log: rolling display of the last 30 device events (boot, WiFi state, NTP sync, audio saves, OTA, errors). Acts as a remote serial monitor over WiFi.
- System and Controls: refresh button (forces immediate live read), full CSV download
- Maintenance: "Wipe SD data" button to delete the CSV log and all audio files for a fresh start, and a "Setup WiFi" form to save a new SSID/password to non-volatile memory and reboot. Password is optional (leave blank for open networks).

### Endpoints

| Path | Method | Returns / does |
|---|---|---|
| `/` | GET | dashboard HTML |
| `/data` | GET | current sensor values as JSON |
| `/readnow` | GET | forces a fresh sensor read, then returns JSON |
| `/recent` | GET | last 2 weeks of CSV log, downsampled to ~168 points |
| `/audiolist` | GET | list of recent WAV clips on SD |
| `/audio?f=NAME.wav` | GET | stream a specific WAV file |
| `/record` | GET | trigger a 3-second WAV recording on demand |
| `/log` | GET | last 30 device events as JSON (boot, WiFi, audio, errors) |
| `/download` | GET | full `/hive_log.csv` |
| `/id` | GET | device hostname, MAC addresses, SD/RTC availability |
| `/update` | GET/POST | OTA firmware upload (GET serves the page, POST receives the .bin) |
| `/wipe` | POST | delete `hive_log.csv` and all `audio_*.wav` from the SD card |
| `/wifi` | POST | save new WiFi credentials (form fields `ssid`, `pass`) to NVS and reboot |

## Data on the SD card

- `/hive_log.csv`, one row every 15 minutes with timestamp + all metrics + state
- `/audio_YYYYMMDD_HHMMSS.wav`, 3-second 16 kHz mono WAV files (about 96 KB each)
- Auto-prune keeps the newest 72 audio clips (about 3 days). CSV grows indefinitely (about 7 MB/year).
- Files survive reboots, power cycles, and re-flashing.

## Network considerations

The ESP32 uses 2.4 GHz WiFi only (no 5 GHz). For deployment on a school or institutional guest network:

- Provide the device's STA MAC address to the network administrator so it can be registered.
- Most guest networks enable client isolation by default, which prevents devices on the same SSID from reaching each other. You'll need an exemption for the device's MAC, otherwise the dashboard is unreachable even when both your phone and the device are on the same network.
- A DHCP reservation (static IP) is recommended so the dashboard URL doesn't change.
- The device hostname `Beehive-ESP32` may also be useful in admin tooling.
- NTP outbound (UDP port 123) is needed for accurate timekeeping. If blocked, the device falls back to its battery-backed RTC.

## Safe hive conditions

| Metric | Optimal | Decent | Bad |
|---|---|---|---|
| Brood temp | 33–36°C | 30–37°C | <30 or >37°C |
| Humidity | 50–65% | 40–75% | >85% (condensation) or <40% (dehydration) |
| VPD | 0.4–0.8 kPa | 0.8–1.2 kPa | >2.0 kPa (drying stress) |
| Sound | −30 to −15 dBFS | — | <−50 (silent) |
| Wingbeat peak | 200–250 Hz worker, 300–450 Hz queen | — | <150 Hz no bees |
| Queen index | 0.15–0.50 | — | <0.10 (queenless?) |
| Temp stability (σ) | <0.5°C | <1.0°C | >1.0°C (failing) |
| Battery | 3.7–4.2 V | 3.4–3.7 V | <3.4 V |

## Troubleshooting

- Temp reads -127°C: 4.7 kΩ pullup missing or DS18B20 wire broken.
- Humidity reads `nan` or `0.0`: on a bare 4-pin DHT22, add a 10 kΩ pullup between Data and 3.3V; verify VCC and GND aren't swapped; if a 3-pin module, reseat the wires. The DHT11/DHT22 are also fragile, and a sensor that fails after repeated handling may simply be dead and need replacement.
- SD card not detected (`cardType=NONE`): VCC must be on 5V/VIN, not 3.3V; card must be FAT32, not exFAT; verify DI and DO aren't swapped; card must be firmly seated. If a card works in a computer but not in the device, its SPI mode interface may be damaged (brownout corruption) even though SD-native mode still works. Replace the card.
- No serial output: wrong baud (115200), or hold BOOT button during connect if no auto-reset cap installed.
- STA mode fails repeatedly: WiFi out of range, hidden SSID, 5 GHz-only network (ESP32 is 2.4 GHz only), or DHCP issue.
- Dashboard unreachable on guest WiFi: client isolation enabled. Either set `FORCE_AP_MODE 1` and connect to `Beehive-AP` directly, or ask your network admin to disable client isolation for the device's MAC.
- Sensors keep dropping out (humidity to `nan`, mic to -120 dBFS, etc.): jumper-wire connections are the cause. Secure every wire with hot glue or transfer the build to a soldered protoboard before deployment.
- Chip stops accepting USB uploads: brownout damage from a deeply discharged battery can corrupt the bootloader. Try `esptool --before usb-reset` or a manual BOOT-button sequence. If still unresponsive, the board may be permanently bricked. Keep a spare ESP32 in the BOM.

## Lessons from development

- Battery health matters. A deeply discharged 18650 caused repeated brownouts during firmware writes, which permanently corrupted one ESP32 board and one SD card. Keep the battery above 3.4 V at all times during bench work, and never let it sit below 3.0 V.
- Jumper wires are not deployment-grade. Every sensor dropout during this project came from a loose breadboard jumper. Before sealing the enclosure, every connection must be hot-glued or soldered to a protoboard.
- Flash OTA before sealing. The device's `/update` endpoint allows wireless firmware updates, but only if an OTA-capable firmware was flashed over USB first. Always do this before the enclosure is permanently closed.
- Catalex SD modules need 5 V on their "5V" pin, not 3.3 V. The onboard regulator expects 5 V input. Wiring it to 3.3 V results in about 2.8 V at the card and silent initialization failure.
- SD cards can fail in SPI mode while still working in computers (which use SD-native mode). After repeated brownout events, replace the SD card if it persistently fails in the ESP32 but reads fine in your computer.
- Guest WiFi networks usually have client isolation. Connecting the device to the WiFi isn't enough. Your phone won't be able to reach the dashboard unless a network admin whitelists the device, or you fall back to AP mode for on-site access.

## Project status

Built as a redesign of v3 with major additions: real-time clock, SD logging, FFT acoustic analysis, scientific indices (VPD, dewpoint, queen index, wingbeat peak, temp stability, hive health score), audio recording with playback, battery monitoring, OTA wireless updates, redesigned dashboard, DHT22 humidity upgrade.

## Possible future upgrades

- Cloud sync (Google Sheets webhook or Firebase) for remote viewing outside the local network
- Deep sleep between sensor cycles to extend battery runtime to weeks
- MFCC audio feature extraction (saved alongside WAV) as input for an ML classifier
- Larger solar panel (5 W or higher) for reliable year-round operation through winter
- Hall-effect or reed-switch entrance counter for pollination activity proxy
- HX711 with load cell for hive weight (requires mechanical mount)
- SHT35 or BME280 for laboratory-grade humidity readings
- Second DS18B20 to measure the brood-entrance temperature gradient
