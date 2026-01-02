Beehive Device Documentation
Project Overview
This project monitors the health of a beehive using an ESP32 microcontroller. The device collects and displays data on temperature, humidity, and hive sound levels. It runs a local web dashboard that can be accessed through a browser and can also store data on an SD card. The goal is to help beekeepers track hive conditions to prevent stress or death caused by humidity, overheating, or inactivity.

Main Functions
Measure and display real-time hive temperature using a DS18B20 sensor.


Measure humidity using a DHT11 sensor.


Monitor hive sound levels using an SPH0645 I2S microphone.


Provide a live web dashboard showing temperature, humidity, and sound data.


Save recorded data to an SD card for long-term tracking.


Optionally record a short sound clip once every hour for manual listening.


Operate on solar power with battery backup.



System Components
ESP32 Development Board (WROOM-32)


DS18B20 Waterproof Temperature Sensor


DHT11 Humidity Sensor


SPH0645 I2S Microphone


Adafruit BQ25185 USB/DC/Solar Charger with 5V Boost


18650 Lithium-Ion Battery (3000 mAh or higher)


6V 1–2W Solar Panel


Micro SD Card Module (SPI interface)


32GB Micro SD Card (Class 10 or better)



Wiring Guide
Temperature Sensor (DS18B20)
Red wire → 3.3V


Black wire → GND


Yellow wire → GPIO 4


Add a 4.7 kΩ resistor between 3.3V and GPIO 4


Humidity Sensor (DHT11)
VCC → 3.3V


Data → GPIO 14


GND → GND


Add a 10 kΩ resistor between Data and 3.3V if using the bare 4-pin DHT11 sensor


Sound Sensor (SPH0645 I2S)
3V → 3.3V


GND → GND


WS → GPIO 25


SCK → GPIO 26


SD → GPIO 32


Micro SD Card Module
VCC → 3.3V


GND → GND


MOSI → GPIO 23


MISO → GPIO 19


SCK → GPIO 18


CS → GPIO 5


Solar Charger and Power
Solar panel positive → SOLAR+ on charger


Solar panel negative → SOLAR–


Battery positive → BAT+


Battery negative → BAT–


5V OUT → ESP32 VIN


GND → ESP32 GND



How the Device Works
When powered on, the ESP32 first attempts to connect to the school or configured WiFi network. If it cannot, it starts its own access point called “Beehive-AP” with password “beehive123.” The dashboard can then be accessed from a browser at “192.168.4.1”.
Once connected, it reads the sensors every 15 minutes and updates the web dashboard. The data can also be saved to the SD card in CSV format. The device records a short 3-second audio clip every hour for optional playback and analysis.
The onboard LED indicates connection status:
Blinking blue: connecting to WiFi


Solid blue: connected to WiFi


Off: access point mode



Web Dashboard Access
When connected to WiFi, the ESP32 prints its IP address in the Arduino Serial Monitor.
 For example:
 STA connected. IP: 10.15.43.217
Access the dashboard by typing that IP into a browser.
 If using the local access point, connect to “Beehive-AP” and go to “192.168.4.1”.

Data and Storage
The ESP32 saves a new line to the SD card every 15 minutes with a timestamp, temperature, humidity, and sound level. Hourly sound clips are saved in a separate folder as WAV files. You can download all data and audio directly through the dashboard or by removing the SD card.

Maintenance and Troubleshooting
If the temperature reads “-127”, check the resistor between data and 3.3V on the DS18B20.


If humidity reads “nan” or “—”, check wiring and ensure the DHT11 has a resistor or module board.


If no sound data appears, verify I2S pin connections and wiring order.


If the device fails to connect to WiFi, verify SSID and password or connect directly to the Beehive-AP network.


To update code, connect via USB and upload from the Arduino IDE.



Possible Future Upgrades
Add a real-time clock (RTC) module to preserve timestamps without internet.


Add an OLED display for on-site readings.


Connect to a cloud database (Firebase or Supabase) for remote monitoring.


Use a DHT22 sensor for more accurate humidity readings.


Implement automatic alerts if humidity or temperature reaches unsafe levels.



Typical Safe Hive Conditions
Temperature: around 34–36°C (93–96°F)


Humidity: ideally between 50–65%
 Values outside this range for long periods can signal hive stress or ventilation problems.



If someone else is continuing this project, they should start by checking the wiring and confirming sensor readings individually before running the full system. After that, they can reupload the combined code and verify that the data appears on the dashboard and SD card.

