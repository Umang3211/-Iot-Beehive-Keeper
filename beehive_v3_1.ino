// =============================================================================
// Beehive Keeper v3.1
// IoT hive monitoring: temperature (brood + entrance), humidity, sound (RMS,
// dBFS, FFT band-power, spectral centroid), with NTP-synced RTC timestamps,
// SD card logging, watchdog, WiFi auto-reconnect, and scientific indices
// (dewpoint, VPD, swarm-warning derivative, hive state machine).
// Hardware: ESP32 WROOM-32, 2x DS18B20 on GPIO 4, DHT22 on GPIO 14,
// SPH0645 I2S mic on 25/26/32, SD card on SPI 23/19/18/5, DS3231 RTC on I2C 21/22.
// =============================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "driver/i2s_std.h"
#include "esp_wifi.h"
#include "esp_task_wdt.h"
#include <Update.h>   // OTA firmware updates over WiFi
#include <Preferences.h>   // persistent WiFi credentials in NVS
#include "DHT.h"
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <RTClib.h>
#include <time.h>
#include <arduinoFFT.h>
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

// ---------- Wi-Fi (STA then AP fallback) ----------
// FORCE_AP_MODE: skip STA entirely, go straight to AP. Set 0 for normal
// STA-first with AP fallback (recommended for deployment).
#define FORCE_AP_MODE 0
// Compiled-in defaults. These are only used if NO credentials have been
// saved to non-volatile storage via the dashboard's "Setup WiFi" form.
// Once you save new creds through the dashboard, those override these.
const char* STA_SSID = "YOUR_WIFI_SSID";
const char* STA_PASS = "YOUR_WIFI_PASSWORD";  // empty string "" for open networks
const char* AP_SSID  = "Beehive-AP";
const char* AP_PASS  = "beehive123";

// Active credentials, loaded from NVS at boot (or fall back to compiled defaults)
String activeSSID;
String activePASS;
Preferences wifiPrefs;

// ---------- LED status ----------
#define LED_PIN 2
#define LED_ACTIVE_HIGH 1

// ---------- DS18B20 (2 sensors on shared OneWire) ----------
#define ONE_WIRE_BUS 4
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);
// Index-based addressing. Run discoverProbes() once and label physically.
#define BROOD_INDEX 0
#define ENTRANCE_INDEX 1

// ---------- DHT (temporarily DHT11 until DHT22 is swapped in) ----------
#define DHT_PIN 14
#define DHT_TYPE DHT22   // upgraded from DHT11 — better accuracy, reads above 90% RH
DHT dht(DHT_PIN, DHT_TYPE);

// ---------- SPH0645 (I2S mic) ----------
#define I2S_WS   25   // LRCL
#define I2S_SCK  26   // BCLK
#define I2S_SD   32   // DOUT
#define I2S_PORT I2S_NUM_0
#define SAMPLE_RATE 16000
#define SAMPLES 1024  // power of 2 for FFT
// New ESP-IDF v5 I2S channel handle (replaces legacy i2s_driver_install)
static i2s_chan_handle_t i2s_rx_handle = NULL;

// ---------- SD card ----------
#define SD_CS 5
bool sdAvailable = false;
const char* LOG_FILE = "/hive_log.csv";

// ---------- DS3231 RTC ----------
RTC_DS3231 rtc;
bool rtcAvailable = false;

// ---------- One-shot SD wipe ----------
// Set to 1 to delete /hive_log.csv and all audio_*.wav at boot, then revert
// to 0 on the next flash so the device doesn't keep wiping every boot.
#define WIPE_OLD_DATA 0

// ---------- Battery monitoring ----------
// Requires a voltage divider: BAT+ -> 100k -> [GPIO 35] -> 100k -> GND
// If not wired, battery readings stay NAN and the dashboard shows "Not wired".
#define BATT_PIN 35
#define BATT_DIVIDER 2.0f    // 100k/100k = divides voltage by 2
// BATT_CAL: empirical multiplier to compensate for ESP32 ADC's input
// impedance (~250kΩ) loading the 100k/100k divider and pulling the
// midpoint slightly low. Measured 3.55V battery → firmware sees 3.02V →
// 3.55/3.02 = 1.18. If your divider has different resistor values, adjust
// this factor by comparing dashboard battery V against a multimeter at BAT.
#define BATT_CAL 1.18f
// Optional: wire BQ25185 CHG status pin (or charging LED cathode) to GPIO 34
// for direct, instantaneous charging detection. The Adafruit BQ25185 breakout
// doesn't expose CHG on any header pin — would require LED cathode SMD tap.
// Defaulting to 0 (voltage-trend inference is good enough).
#define USE_CHRG_PIN 0
#define CHRG_PIN 34

// ---------- Watchdog ----------
#define WDT_TIMEOUT_SEC 30

// ---------- Timing ----------
const unsigned long SENSOR_INTERVAL_MS = 15UL * 1000UL;       // read every 15s
const unsigned long LOG_INTERVAL_MS    = 15UL * 60UL * 1000UL; // log every 15 min
const unsigned long WIFI_CHECK_MS      = 60UL * 1000UL;       // check WiFi every 60s
const unsigned long NTP_RESYNC_MS      = 6UL * 60UL * 60UL * 1000UL; // 6 hours
const unsigned long AUDIO_INTERVAL_MS  = 60UL * 60UL * 1000UL; // 1 hour audio clip
const int           AUDIO_DURATION_SEC = 3;                    // 3-sec WAV clips
const int           AUDIO_KEEP_FILES   = 72;                   // retain last 3 days (72 hours)
unsigned long lastAudioMs      = 0;
unsigned long lastSensorReadMs = 0;
unsigned long lastLogMs        = 0;
unsigned long bootMs           = 0;
bool          firstCycleFired  = false;
const unsigned long FIRST_CYCLE_DELAY_MS = 30UL * 1000UL;  // 30s after boot
unsigned long lastWifiCheckMs  = 0;
unsigned long lastNtpSyncMs    = 0;

// ---------- Globals (cached sensor values) ----------
WebServer server(80);
float currentBroodC      = NAN;
float currentEntranceC   = NAN;
float currentGradientC   = NAN;  // brood - entrance
float currentHum         = NAN;
float currentDbfs        = NAN;
float currentRms         = NAN;
float currentDewpointC   = NAN;
float currentVpdKpa      = NAN;
float currentBandLow     = NAN;  // 0-100 Hz
float currentBandWorker  = NAN;  // 100-300 Hz
float currentBandQueen   = NAN;  // 300-600 Hz
float currentBandAgit    = NAN;  // 600-2000 Hz
float currentQueenIndex  = NAN;
float currentCentroidHz  = NAN;
float currentWingbeatHz  = NAN;
float currentBatteryV    = NAN;
int   currentBatteryPct  = -1;
String currentChargeStatus = "unknown";  // "charging" / "not_charging" / "unknown" / "not_wired"

// ---------- In-memory log ring buffer ----------
// Captures recent device events so the dashboard can display them as a
// remote "serial monitor" view, even when no USB cable is attached.
#define LOG_LINES 30
String logRing[LOG_LINES];
int logRingIdx = 0;
int logRingCount = 0;

void logmsg(const char* fmt, ...) {
  char body[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(body, sizeof(body), fmt, args);
  va_end(args);
  Serial.println(body);
  char prefix[24];
  extern bool rtcAvailable;
  extern RTC_DS3231 rtc;
  if (rtcAvailable) {
    DateTime n = rtc.now();
    snprintf(prefix, sizeof(prefix), "%02d:%02d:%02d ",
             n.hour(), n.minute(), n.second());
  } else {
    snprintf(prefix, sizeof(prefix), "+%lus ", (unsigned long)(millis() / 1000));
  }
  logRing[logRingIdx] = String(prefix) + body;
  logRingIdx = (logRingIdx + 1) % LOG_LINES;
  if (logRingCount < LOG_LINES) logRingCount++;
}

// ---------- Advanced metrics (rolling histories for derived stats) ----------
// Temp stability: 60 min of brood readings at 15s cadence
#define TEMP_STAB_LEN 240
float tempStabHist[TEMP_STAB_LEN];
int   tempStabIdx = 0;
int   tempStabCount = 0;
float currentTempStability = NAN;  // rolling std dev (°C) of brood temp

// Sound event detection: last 40 RMS samples = 10 min at 15s cadence
#define SOUND_HIST_LEN 40
float soundHist[SOUND_HIST_LEN];
int   soundHistIdx = 0;
int   soundHistCount = 0;
unsigned long lastEventMs = 0;
const unsigned long EVENT_COOLDOWN_MS = 5UL * 60UL * 1000UL;  // max 1 event per 5 min
int   eventCount = 0;

// Cross-sensor detectors
bool   currentlyFanning = false;
int    currentHealthScore = -1;

// Battery voltage history for charging inference (last 6 reads at 60s = 6 min)
#define BATT_HIST_LEN 6
float battHist[BATT_HIST_LEN] = {NAN, NAN, NAN, NAN, NAN, NAN};
int   battHistIdx = 0;

// ESP-IDF v5 oneshot ADC handle + calibration scheme. Calibration converts
// the non-linear raw ADC reading to true millivolts using factory data
// burned into the chip's eFuse (or curve-fit fallback if not available).
static adc_oneshot_unit_handle_t adc1_handle = NULL;
static adc_cali_handle_t adc_cali_handle = NULL;
static bool adcReady = false;
static bool caliReady = false;
String currentState      = "INITIALIZING";
String currentAlerts     = "";

// Temperature history for swarm-warning derivative (last 4 readings at 15-min intervals)
float tempHistory[4] = {NAN, NAN, NAN, NAN};
int   tempHistIdx = 0;

// FFT buffers
double vReal[SAMPLES];
double vImag[SAMPLES];
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, SAMPLE_RATE);

inline void setLED(bool on) {
  digitalWrite(LED_PIN, (LED_ACTIVE_HIGH ? (on ? HIGH : LOW) : (on ? LOW : HIGH)));
}

// =============================================================================
// I2S setup
// =============================================================================
void setupI2S() {
  // New ESP-IDF v5 I2S API: allocate channel, configure standard mode, enable.
  // SPH0645 is a 24-bit MSB-aligned PCM mic in I2S Philips format, mono left.
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
  i2s_new_channel(&chan_cfg, NULL, &i2s_rx_handle);

  i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                    I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)I2S_SCK,
      .ws   = (gpio_num_t)I2S_WS,
      .dout = I2S_GPIO_UNUSED,
      .din  = (gpio_num_t)I2S_SD,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  // SPH0645 outputs on the left slot when SEL is tied LOW
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
  i2s_channel_init_std_mode(i2s_rx_handle, &std_cfg);
  i2s_channel_enable(i2s_rx_handle);
}

// =============================================================================
// Scientific calculations
// =============================================================================

// Magnus formula for dewpoint (°C), valid -45 to +60°C
float dewpoint(float t, float rh) {
  if (isnan(t) || isnan(rh) || rh <= 0) return NAN;
  const float a = 17.625;
  const float b = 243.04;
  float gamma = log(rh / 100.0) + (a * t) / (b + t);
  return (b * gamma) / (a - gamma);
}

// Vapor Pressure Deficit (kPa) using Tetens equation
float vpdKpa(float t, float rh) {
  if (isnan(t) || isnan(rh)) return NAN;
  float svp = 0.6108 * exp((17.27 * t) / (t + 237.3));
  float avp = svp * (rh / 100.0);
  return svp - avp;
}

// dBFS from raw RMS, using effective 18-bit range from SPH0645 (24-bit shifted by 14)
float dbfsFromRMS(double rms) {
  const double FS = 131071.0;  // 2^18 - 1, matching the >> 14 shift
  if (rms <= 0.0) return -120.0;
  return 20.0 * log10(rms / FS);
}

// Power in a frequency band (after FFT)
float bandPower(int lowHz, int highHz) {
  int lowBin = (lowHz * SAMPLES) / SAMPLE_RATE;
  int highBin = (highHz * SAMPLES) / SAMPLE_RATE;
  if (lowBin < 1) lowBin = 1;
  if (highBin >= SAMPLES / 2) highBin = SAMPLES / 2 - 1;
  double p = 0;
  for (int i = lowBin; i <= highBin; i++) p += vReal[i] * vReal[i];
  return (float)p;
}

// Spectral centroid (Hz)
float spectralCentroid() {
  double weighted = 0, total = 0;
  for (int i = 1; i < SAMPLES / 2; i++) {
    double freq = (i * (double)SAMPLE_RATE) / SAMPLES;
    weighted += freq * vReal[i];
    total += vReal[i];
  }
  return total > 0 ? (float)(weighted / total) : 0;
}

// Wingbeat peak frequency (Hz): dominant peak within bee-relevant 100-500 Hz band.
// Healthy worker buzz peaks around 200-250 Hz; queen tooting around 300-450 Hz.
// Reference: Eskov & Toboltsev (acoustic bee research), Bencsik et al.
float wingbeatPeakHz() {
  int loBin = (100 * SAMPLES) / SAMPLE_RATE;
  int hiBin = (500 * SAMPLES) / SAMPLE_RATE;
  if (loBin < 1) loBin = 1;
  if (hiBin >= SAMPLES / 2) hiBin = SAMPLES / 2 - 1;
  double peak = 0;
  int peakBin = loBin;
  for (int i = loBin; i <= hiBin; i++) {
    if (vReal[i] > peak) { peak = vReal[i]; peakBin = i; }
  }
  return (float)((peakBin * (double)SAMPLE_RATE) / SAMPLES);
}

// Swarm-warning: returns true if temp rising >0.1°C/min sustained from >35.5°C baseline
bool swarmWarning(float newTemp) {
  tempHistory[tempHistIdx] = newTemp;
  tempHistIdx = (tempHistIdx + 1) % 4;
  // Compare current vs 2 readings ago (30 min back)
  int newestIdx = (tempHistIdx + 3) % 4;
  int oldIdx = (tempHistIdx + 1) % 4;
  float tNow = tempHistory[newestIdx];
  float tOld = tempHistory[oldIdx];
  if (isnan(tNow) || isnan(tOld)) return false;
  float ratePerMin = (tNow - tOld) / 30.0;
  return (ratePerMin > 0.1 && tNow > 35.5);
}

void initBatteryADC() {
  adc_oneshot_unit_init_cfg_t init_cfg = {};
  init_cfg.unit_id  = ADC_UNIT_1;
  init_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;
  if (adc_oneshot_new_unit(&init_cfg, &adc1_handle) != ESP_OK) {
    Serial.println("ADC init FAILED");
    return;
  }
  adc_oneshot_chan_cfg_t chan_cfg = {};
  chan_cfg.atten    = ADC_ATTEN_DB_12;       // ~0-2.5V usable input range
  chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;  // 12-bit
  // GPIO 35 = ADC1 channel 7
  if (adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_7, &chan_cfg) != ESP_OK) {
    Serial.println("ADC channel cfg FAILED");
    return;
  }
  adcReady = true;
  Serial.println("ADC ready (GPIO 35)");

  // Calibration: prefer the line-fitting scheme (uses factory eFuse data on
  // most ESP32 chips). Without it the raw → voltage mapping is non-linear
  // above ~2V and battery readings come out ~half of reality.
  adc_cali_line_fitting_config_t cali_cfg = {};
  cali_cfg.unit_id  = ADC_UNIT_1;
  cali_cfg.atten    = ADC_ATTEN_DB_12;
  cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  if (adc_cali_create_scheme_line_fitting(&cali_cfg, &adc_cali_handle) == ESP_OK) {
    caliReady = true;
    Serial.println("ADC calibration ready (line-fitting)");
  } else {
    Serial.println("ADC calibration FAILED — readings will be approximate");
  }
}

// Take 31 samples spaced 3ms apart and use the median to reject outliers
// caused by the high source impedance of the 100k/100k divider feeding
// the ESP32 ADC. Combined with an exponential moving average across cycles
// for an extra smoothing pass.
static float battEMA = NAN;
float readBatteryVoltage() {
  if (!adcReady) return NAN;
  const int N = 31;
  int s[N];
  int good = 0;
  for (int i = 0; i < N; i++) {
    int raw = 0;
    if (adc_oneshot_read(adc1_handle, ADC_CHANNEL_7, &raw) == ESP_OK) {
      s[good++] = raw;
    }
    delay(3);
  }
  if (good < 5) return NAN;
  // Insertion sort
  for (int i = 1; i < good; i++) {
    int k = s[i], j = i - 1;
    while (j >= 0 && s[j] > k) { s[j+1] = s[j]; j--; }
    s[j+1] = k;
  }
  int raw = s[good / 2];
  if (raw < 100) return NAN;

  // Raw math: ADC_ATTEN_DB_12 has effective full-scale of ~2.5V mapped to
  // 0–4095. BATT_DIVIDER (2.0) recovers source voltage from the midpoint.
  // BATT_CAL compensates for ADC input impedance loading the divider.
  float instantV = (raw / 4095.0f) * 2.5f * BATT_DIVIDER * BATT_CAL;

  // Exponential moving average across read cycles. Battery voltage changes
  // slowly, so heavy smoothing is fine and rejects residual noise.
  if (isnan(battEMA)) battEMA = instantV;
  else battEMA = 0.7f * battEMA + 0.3f * instantV;
  return battEMA;
}

// Approximate Li-ion 18650 state-of-charge from open-circuit voltage.
// Real SoC depends on load, temperature, age — this is a rough indicator only.
int batteryPercent(float v) {
  if (isnan(v)) return -1;
  if (v >= 4.20f) return 100;
  if (v >= 4.10f) return 90;
  if (v >= 4.00f) return 80;
  if (v >= 3.90f) return 65;
  if (v >= 3.80f) return 50;
  if (v >= 3.70f) return 35;
  if (v >= 3.60f) return 20;
  if (v >= 3.50f) return 10;
  if (v >= 3.40f) return 5;
  if (v >= 3.30f) return 1;
  return 0;
}

// Update charging status. With USE_CHRG_PIN=1 reads the BQ25185 status pin
// directly. Otherwise infers from the recent voltage trend.
void updateChargeStatus() {
  if (isnan(currentBatteryV)) { currentChargeStatus = "not_wired"; return; }
#if USE_CHRG_PIN
  pinMode(CHRG_PIN, INPUT_PULLUP);
  currentChargeStatus = (digitalRead(CHRG_PIN) == LOW) ? "charging" : "not_charging";
  return;
#else
  // Push current voltage into ring, then compare oldest vs newest
  battHist[battHistIdx] = currentBatteryV;
  battHistIdx = (battHistIdx + 1) % BATT_HIST_LEN;
  int newest = (battHistIdx + BATT_HIST_LEN - 1) % BATT_HIST_LEN;
  int oldest = battHistIdx; // next slot to overwrite = oldest valid
  float vNew = battHist[newest];
  float vOld = battHist[oldest];
  if (isnan(vOld) || isnan(vNew)) { currentChargeStatus = "unknown"; return; }
  // Need >0.02V rise over 5+ readings (~5+ min) to call it charging
  // (filters out noise; real charging shows a clear upward trend)
  float delta = vNew - vOld;
  if (delta > 0.02f) currentChargeStatus = "charging";
  else if (delta < -0.02f) currentChargeStatus = "discharging";
  else currentChargeStatus = "steady";
#endif
}

// =============================================================================
// Advanced derived metrics (cross-sensor + temporal)
// =============================================================================

// Push the latest brood temperature into the rolling history and compute
// the standard deviation. Std dev of brood temp is the classic indicator
// of thermoregulation health (Stabentheiner et al.). <0.5°C healthy.
void updateTempStability(float t) {
  if (isnan(t)) return;
  tempStabHist[tempStabIdx] = t;
  tempStabIdx = (tempStabIdx + 1) % TEMP_STAB_LEN;
  if (tempStabCount < TEMP_STAB_LEN) tempStabCount++;
  if (tempStabCount < 8) { currentTempStability = NAN; return; }  // need ≥8 samples
  // Welford-like 2-pass for clarity (small N, fine)
  double sum = 0;
  for (int i = 0; i < tempStabCount; i++) sum += tempStabHist[i];
  double mean = sum / tempStabCount;
  double sq = 0;
  for (int i = 0; i < tempStabCount; i++) {
    double d = tempStabHist[i] - mean;
    sq += d * d;
  }
  currentTempStability = (float)sqrt(sq / tempStabCount);
}

// Push latest RMS into rolling history, detect anomalous spikes >2σ above
// recent baseline. Returns true on event trigger (with cooldown).
bool detectAcousticEvent(float rms) {
  if (isnan(rms)) return false;
  // Need enough history to have a meaningful baseline
  bool willTrigger = false;
  if (soundHistCount >= 20) {
    double sum = 0;
    for (int i = 0; i < soundHistCount; i++) sum += soundHist[i];
    double mean = sum / soundHistCount;
    double sq = 0;
    for (int i = 0; i < soundHistCount; i++) {
      double d = soundHist[i] - mean;
      sq += d * d;
    }
    double sd = sqrt(sq / soundHistCount);
    // Trigger when current RMS > mean + 2σ AND >1.5x mean (filter tiny noise)
    if (rms > (mean + 2.0 * sd) && rms > (mean * 1.5)) {
      unsigned long now = millis();
      if (now - lastEventMs >= EVENT_COOLDOWN_MS) {
        lastEventMs = now;
        eventCount++;
        willTrigger = true;
      }
    }
  }
  // Push into ring AFTER decision so the event itself doesn't poison baseline
  soundHist[soundHistIdx] = rms;
  soundHistIdx = (soundHistIdx + 1) % SOUND_HIST_LEN;
  if (soundHistCount < SOUND_HIST_LEN) soundHistCount++;
  return willTrigger;
}

// Fanning detector: bees fanning to cool the hive show a 3-sensor signature
// (rising brood temp + rising sound + wingbeat peak in worker band).
bool detectFanning(float brood, float rms, float wing) {
  if (isnan(brood) || isnan(rms) || isnan(wing)) return false;
  if (brood < 34.5f) return false;  // bees only fan when hive is warm
  if (wing < 200.0f || wing > 350.0f) return false;  // worker fan signature
  if (soundHistCount < 10) return false;
  // RMS noticeably above recent baseline
  double sum = 0;
  for (int i = 0; i < soundHistCount; i++) sum += soundHist[i];
  double mean = sum / soundHistCount;
  return rms > mean * 1.3;
}

// Composite hive health score 0-100. Weighted contributions from each metric.
// Useful for the presentation as a single, plain-language summary.
int computeHealthScore() {
  int score = 0;
  // Temperature in healthy brood range (30 pts)
  if (!isnan(currentBroodC)) {
    if (currentBroodC >= 33.0f && currentBroodC <= 36.0f) score += 30;
    else if (currentBroodC >= 30.0f && currentBroodC <= 37.0f) score += 18;
    else if (currentBroodC >= 25.0f && currentBroodC <= 38.0f) score += 8;
  }
  // Humidity in healthy range (20 pts)
  if (!isnan(currentHum)) {
    if (currentHum >= 50.0f && currentHum <= 65.0f) score += 20;
    else if (currentHum >= 40.0f && currentHum <= 75.0f) score += 12;
    else if (currentHum >= 30.0f && currentHum <= 85.0f) score += 5;
  }
  // Queen index queenright (20 pts)
  if (!isnan(currentQueenIndex)) {
    if (currentQueenIndex >= 0.15f && currentQueenIndex <= 0.50f) score += 20;
    else if (currentQueenIndex >= 0.10f && currentQueenIndex <= 0.60f) score += 10;
  }
  // Temp stability good (15 pts)
  if (!isnan(currentTempStability)) {
    if (currentTempStability < 0.5f) score += 15;
    else if (currentTempStability < 1.0f) score += 8;
    else if (currentTempStability < 2.0f) score += 3;
  }
  // Wingbeat in bee range (10 pts)
  if (!isnan(currentWingbeatHz)) {
    if (currentWingbeatHz >= 200.0f && currentWingbeatHz <= 450.0f) score += 10;
    else if (currentWingbeatHz >= 150.0f && currentWingbeatHz <= 500.0f) score += 5;
  }
  // Sound dBFS in normal buzz range (5 pts)
  if (!isnan(currentDbfs)) {
    if (currentDbfs >= -30.0f && currentDbfs <= -15.0f) score += 5;
    else if (currentDbfs >= -50.0f && currentDbfs <= -10.0f) score += 2;
  }
  return score;
}

// Hive state classifier
String classifyHive(float broodC, float hum, float dew, bool swarm, bool silent) {
  if (isnan(broodC) || isnan(hum)) return "NO_DATA";
  if (swarm) return "SWARM_WARNING";
  if (broodC > 38.0) return "OVERHEATING";
  if (broodC < 6.0) return "FREEZING";
  if (!isnan(dew) && broodC <= dew + 1.0) return "CONDENSATION_RISK";
  if (hum > 70.0) return "HUMID";
  if (hum < 30.0) return "DRY";
  if (silent) return "SILENT_HIVE";
  if (broodC > 35.0) return "WARM";
  if (broodC < 30.0) return "COOL";
  return "HEALTHY";
}

// =============================================================================
// Time / NTP / RTC
// =============================================================================
void syncNTP() {
  if (WiFi.status() != WL_CONNECTED) return;
  configTime(-5 * 3600, 3600, "pool.ntp.org", "time.nist.gov");  // EST/EDT
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5000)) {
    if (rtcAvailable) {
      rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
                          timeinfo.tm_mday, timeinfo.tm_hour,
                          timeinfo.tm_min, timeinfo.tm_sec));
    }
    logmsg("NTP sync OK, RTC updated.");
    lastNtpSyncMs = millis();
  } else {
    logmsg("NTP sync failed.");
  }
}

String isoTimestamp() {
  if (rtcAvailable) {
    DateTime n = rtc.now();
    char buf[25];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
             n.year(), n.month(), n.day(), n.hour(), n.minute(), n.second());
    return String(buf);
  }
  // Fallback: uptime
  unsigned long s = millis() / 1000;
  char buf[32];
  snprintf(buf, sizeof(buf), "UP+%luh%lum%lus", s / 3600, (s / 60) % 60, s % 60);
  return String(buf);
}

// =============================================================================
// Sensor reads
// =============================================================================
void readSensorsOnce() {
  // Temperature: 2 DS18B20 probes
  ds18b20.requestTemperatures();
  float t0 = ds18b20.getTempCByIndex(BROOD_INDEX);
  float t1 = ds18b20.getTempCByIndex(ENTRANCE_INDEX);
  if (t0 > -50 && t0 < 100) currentBroodC = t0;
  if (t1 > -50 && t1 < 100) currentEntranceC = t1;
  if (!isnan(currentBroodC) && !isnan(currentEntranceC)) {
    currentGradientC = currentBroodC - currentEntranceC;
  }

  // Humidity with retry
  for (int i = 0; i < 3; i++) {
    float h = dht.readHumidity();
    if (!isnan(h)) { currentHum = h; break; }
    delay(200);
  }

  // Sound: read I2S, compute RMS + dBFS + FFT
  static int32_t buf[SAMPLES];
  size_t br = 0;
  esp_err_t r = i2s_channel_read(i2s_rx_handle, (void*)buf, sizeof(buf), &br, 100 / portTICK_PERIOD_MS);
  if (r == ESP_OK && br > 0) {
    const size_t n = br / sizeof(int32_t);
    double s2 = 0.0;
    for (size_t i = 0; i < n; i++) {
      int32_t s = buf[i] >> 14;
      s2 += (double)s * (double)s;
      // Fill FFT buffer
      if (i < SAMPLES) {
        vReal[i] = (double)s;
        vImag[i] = 0.0;
      }
    }
    double rms = n ? sqrt(s2 / n) : 0.0;
    currentRms = (float)rms;
    currentDbfs = dbfsFromRMS(rms);

    // FFT
    FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
    FFT.compute(FFTDirection::Forward);
    FFT.complexToMagnitude();
    currentBandLow    = bandPower(0, 100);
    currentBandWorker = bandPower(100, 300);
    currentBandQueen  = bandPower(300, 600);
    currentBandAgit   = bandPower(600, 2000);
    currentQueenIndex = currentBandQueen / (currentBandWorker + 1.0);
    currentCentroidHz = spectralCentroid();
    currentWingbeatHz = wingbeatPeakHz();
  }

  // Battery
  currentBatteryV   = readBatteryVoltage();
  currentBatteryPct = batteryPercent(currentBatteryV);
  updateChargeStatus();

  // Derived metrics
  currentDewpointC = dewpoint(currentBroodC, currentHum);
  currentVpdKpa = vpdKpa(currentBroodC, currentHum);

  // Advanced derived metrics
  updateTempStability(currentBroodC);
  bool eventTriggered = detectAcousticEvent(currentRms);
  currentlyFanning = detectFanning(currentBroodC, currentRms, currentWingbeatHz);
  currentHealthScore = computeHealthScore();

  // If an acoustic event fired, capture a tagged WAV immediately
  if (eventTriggered) {
    Serial.println("[event] acoustic spike detected — recording event clip");
    recordAudioClip();  // saves the next available timestamped WAV
  }

  // State classification
  bool swarmFlag = false;
  bool silentFlag = (currentDbfs < -85.0);  // very quiet baseline
  // Note: swarm-warning derivative runs only on the 15-min log cycle, not every 60s
  currentState = classifyHive(currentBroodC, currentHum, currentDewpointC,
                              swarmFlag, silentFlag);

  // Periodic diagnostic line so we can see exactly what the sensors are
  // reading without needing to open the dashboard.
  Serial.printf("[read] brood=%.2f hum=%.1f dbfs=%.1f wing=%.0f batt=%.2fV pct=%d charge=%s\n",
                currentBroodC, currentHum, currentDbfs, currentWingbeatHz,
                currentBatteryV, currentBatteryPct, currentChargeStatus.c_str());
}

// =============================================================================
// SD logging
// =============================================================================
// Explicit SPI init with our pins + lower 4 MHz clock for reliability on
// long jumper wires. Then probe the card type so we can distinguish
// "no card" vs "card present but bad" failures.
void diagnoseSD(const char* tag) {
  uint8_t ct = SD.cardType();
  const char* name = "?";
  switch (ct) {
    case CARD_NONE:  name = "NONE (no card detected)"; break;
    case CARD_MMC:   name = "MMC"; break;
    case CARD_SD:    name = "SDSC"; break;
    case CARD_SDHC:  name = "SDHC/SDXC"; break;
    case CARD_UNKNOWN: name = "UNKNOWN (wiring or power)"; break;
  }
  uint64_t mb = SD.cardSize() / (1024ULL * 1024ULL);
  Serial.printf("%s cardType=%s size=%lluMB\n", tag, name, mb);
}

void initSD() {
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  SPI.begin(18, 19, 23, SD_CS);  // SCK, MISO, MOSI, CS
  // 1 MHz SPI — slow but tolerant of long jumper wires and marginal contacts.
  if (!SD.begin(SD_CS, SPI, 1000000)) {
    Serial.println("SD init FAILED");
    diagnoseSD("SD diag:");
    sdAvailable = false;
    return;
  }
  sdAvailable = true;
  diagnoseSD("SD init OK,");
  // Write header if file doesn't exist
  if (!SD.exists(LOG_FILE)) {
    File f = SD.open(LOG_FILE, FILE_WRITE);
    if (f) {
      f.println("timestamp,brood_c,entrance_c,gradient_c,humidity_pct,"
                "sound_dbfs,sound_rms,band_low,band_worker,band_queen,band_agit,"
                "queen_index,centroid_hz,dewpoint_c,vpd_kpa,battery_v,battery_pct,charge,state");
      f.close();
    }
  }
}

// Retry SD init periodically if it failed at boot (card inserted later).
unsigned long lastSdRetryMs = 0;
const unsigned long SD_RETRY_MS = 30UL * 1000UL;
void retrySDIfNeeded() {
  if (sdAvailable) return;
  if (millis() - lastSdRetryMs < SD_RETRY_MS) return;
  lastSdRetryMs = millis();
  SD.end();
  if (SD.begin(SD_CS, SPI, 1000000)) {
    sdAvailable = true;
    diagnoseSD("SD retry OK,");
    if (!SD.exists(LOG_FILE)) {
      File f = SD.open(LOG_FILE, FILE_WRITE);
      if (f) {
        f.println("timestamp,brood_c,entrance_c,gradient_c,humidity_pct,"
                  "sound_dbfs,sound_rms,band_low,band_worker,band_queen,band_agit,"
                  "queen_index,centroid_hz,dewpoint_c,vpd_kpa,battery_v,battery_pct,charge,state");
        f.close();
      }
    }
  } else {
    Serial.print("SD retry FAILED. ");
    diagnoseSD("");
  }
}

void logToSD() {
  if (!sdAvailable) return;
  File f = SD.open(LOG_FILE, FILE_APPEND);
  if (!f) {
    Serial.println("SD append FAILED");
    return;
  }
  String row = isoTimestamp() + ",";
  row += (isnan(currentBroodC) ? "" : String(currentBroodC, 2)) + ",";
  row += (isnan(currentEntranceC) ? "" : String(currentEntranceC, 2)) + ",";
  row += (isnan(currentGradientC) ? "" : String(currentGradientC, 2)) + ",";
  row += (isnan(currentHum) ? "" : String(currentHum, 1)) + ",";
  row += (isnan(currentDbfs) ? "" : String(currentDbfs, 1)) + ",";
  row += (isnan(currentRms) ? "" : String(currentRms, 2)) + ",";
  row += (isnan(currentBandLow) ? "" : String(currentBandLow, 0)) + ",";
  row += (isnan(currentBandWorker) ? "" : String(currentBandWorker, 0)) + ",";
  row += (isnan(currentBandQueen) ? "" : String(currentBandQueen, 0)) + ",";
  row += (isnan(currentBandAgit) ? "" : String(currentBandAgit, 0)) + ",";
  row += (isnan(currentQueenIndex) ? "" : String(currentQueenIndex, 3)) + ",";
  row += (isnan(currentCentroidHz) ? "" : String(currentCentroidHz, 1)) + ",";
  row += (isnan(currentDewpointC) ? "" : String(currentDewpointC, 2)) + ",";
  row += (isnan(currentVpdKpa) ? "" : String(currentVpdKpa, 3)) + ",";
  row += (isnan(currentBatteryV) ? "" : String(currentBatteryV, 3)) + ",";
  row += (currentBatteryPct < 0 ? "" : String(currentBatteryPct)) + ",";
  row += currentChargeStatus + ",";
  row += currentState;
  f.println(row);
  f.close();

  // Update swarm-warning history at the 15-min cadence
  if (swarmWarning(currentBroodC)) {
    currentState = "SWARM_WARNING";
  }
}

// =============================================================================
// WAV audio capture (hourly 3-second clips to /audio on SD)
// =============================================================================
static void writeWavHeader(File &f, uint32_t dataBytes, uint32_t sampleRate) {
  uint32_t fileSize    = dataBytes + 36;
  uint16_t channels    = 1;
  uint16_t bitsPer     = 16;
  uint32_t byteRate    = sampleRate * channels * bitsPer / 8;
  uint16_t blockAlign  = channels * bitsPer / 8;
  uint32_t fmtSize     = 16;
  uint16_t fmtPCM      = 1;
  f.write((const uint8_t*)"RIFF", 4);
  f.write((const uint8_t*)&fileSize, 4);
  f.write((const uint8_t*)"WAVE", 4);
  f.write((const uint8_t*)"fmt ", 4);
  f.write((const uint8_t*)&fmtSize, 4);
  f.write((const uint8_t*)&fmtPCM, 2);
  f.write((const uint8_t*)&channels, 2);
  f.write((const uint8_t*)&sampleRate, 4);
  f.write((const uint8_t*)&byteRate, 4);
  f.write((const uint8_t*)&blockAlign, 2);
  f.write((const uint8_t*)&bitsPer, 2);
  f.write((const uint8_t*)"data", 4);
  f.write((const uint8_t*)&dataBytes, 4);
}

// Prune older WAV files in the SD root with prefix "audio_", keeping at most
// AUDIO_KEEP_FILES newest by name (timestamp-prefixed so lexical = chrono).
static void pruneOldAudio() {
  if (!sdAvailable) return;
  File dir = SD.open("/");
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
  const int MAX_FILES = 256;
  String names[MAX_FILES];
  int count = 0;
  File e;
  while ((e = dir.openNextFile())) {
    String n = e.name();
    e.close();
    if (n.startsWith("audio_") && n.endsWith(".wav") && count < MAX_FILES) names[count++] = n;
  }
  dir.close();
  if (count <= AUDIO_KEEP_FILES) return;
  for (int i = 1; i < count; i++) {
    String key = names[i]; int j = i - 1;
    while (j >= 0 && names[j] > key) { names[j+1] = names[j]; j--; }
    names[j+1] = key;
  }
  int toDelete = count - AUDIO_KEEP_FILES;
  for (int i = 0; i < toDelete; i++) {
    String path = String("/") + names[i];
    SD.remove(path);
  }
}

void recordAudioClip() {
  if (!sdAvailable) { Serial.println("audio: no SD"); return; }
  // Write WAVs to the SD root with a recognizable prefix so the dashboard
  // can list them (avoids subdirectory creation which fails on some SD libs).
  char fname[64];
  if (rtcAvailable) {
    DateTime n = rtc.now();
    snprintf(fname, sizeof(fname), "/audio_%04d%02d%02d_%02d%02d%02d.wav",
             n.year(), n.month(), n.day(), n.hour(), n.minute(), n.second());
  } else {
    snprintf(fname, sizeof(fname), "/audio_up_%lu.wav", millis() / 1000UL);
  }
  File f = SD.open(fname, FILE_WRITE);
  if (!f) { Serial.printf("audio open FAILED: %s\n", fname); return; }

  const int totalSamples = AUDIO_DURATION_SEC * SAMPLE_RATE;
  const uint32_t dataBytes = totalSamples * 2;
  writeWavHeader(f, dataBytes, SAMPLE_RATE);

  int32_t buf[256];
  int written = 0;
  unsigned long startMs = millis();
  while (written < totalSamples) {
    size_t br = 0;
    esp_err_t r = i2s_channel_read(i2s_rx_handle, buf, sizeof(buf), &br, 200 / portTICK_PERIOD_MS);
    if (r != ESP_OK || br == 0) break;
    int n = br / sizeof(int32_t);
    for (int i = 0; i < n && written < totalSamples; i++) {
      // SPH0645 is 24-bit MSB-aligned in 32-bit slot; shift to 16-bit signed
      int16_t s = (int16_t)(buf[i] >> 14);
      f.write((const uint8_t*)&s, 2);
      written++;
    }
    esp_task_wdt_reset();
    if (millis() - startMs > (unsigned long)AUDIO_DURATION_SEC * 1000UL + 2000) break;
  }
  f.close();
  Serial.printf("audio saved: %s (%d samples)\n", fname, written);
  pruneOldAudio();
}

// =============================================================================
// Web UI
// =============================================================================
static const char INDEX_HTML[] PROGMEM = R"RAWHTML(
<!doctype html>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Beehive Keeper</title>
<style>
:root{--bg:#0a0f1c;--panel:#101a30;--panel2:#0d1525;--ink:#e6edf3;--ink2:#b6c0d1;--mute:#7e8aa1;--grid:#1a2742;
--ok:#5DCAA5;--watch:#7CC7FF;--warn:#E2A04B;--bad:#E24B4A;
--ok-bg:rgba(93,202,165,.13);--watch-bg:rgba(124,199,255,.13);--warn-bg:rgba(226,160,75,.13);--bad-bg:rgba(226,75,74,.13);--mute-bg:rgba(126,138,161,.13)}
*,*::before,*::after{box-sizing:border-box}
body{margin:0;font-family:-apple-system,BlinkMacSystemFont,'Inter','Segoe UI',Roboto,Arial,sans-serif;background:var(--bg);color:var(--ink);font-feature-settings:'tnum' 1;-webkit-font-smoothing:antialiased}
.wrap{max-width:980px;margin:0 auto;padding:24px 18px 60px}
header{display:flex;align-items:center;justify-content:space-between;margin-bottom:18px}
.brand{font-size:14px;font-weight:600;letter-spacing:.3px;color:var(--ink2)}
.brand .dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--ok);margin-right:8px;vertical-align:middle;box-shadow:0 0 8px var(--ok)}
.foot{font-size:11px;color:var(--mute)}
/* Hero */
.hero{background:linear-gradient(180deg,var(--panel) 0%,var(--panel2) 100%);border:1px solid var(--grid);border-radius:18px;padding:22px 24px;margin-bottom:18px}
.hero-row{display:flex;align-items:center;justify-content:space-between;gap:16px;flex-wrap:wrap}
.hero-title{font-size:13px;text-transform:uppercase;letter-spacing:1.5px;color:var(--ink2);font-weight:600;margin-bottom:4px}
.hero-state{font-size:36px;font-weight:800;letter-spacing:-.5px;line-height:1.05}
.hero-sub{font-size:14px;color:var(--ink2);margin-top:6px;max-width:520px;line-height:1.45}
.hero-pill{padding:8px 14px;border-radius:999px;font-size:12px;font-weight:700;letter-spacing:.5px;text-transform:uppercase}
.hero-meta{display:flex;gap:18px;margin-top:14px;flex-wrap:wrap;font-size:12px;color:var(--mute)}
.hero-meta span b{color:var(--ink2);font-weight:600}
/* Primary metric trio */
.primary{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin-bottom:18px}
@media(max-width:680px){.primary{grid-template-columns:1fr}}
.metric{background:var(--panel);border:1px solid var(--grid);border-radius:14px;padding:16px 18px;position:relative;overflow:hidden}
.metric .lbl{font-size:11px;text-transform:uppercase;letter-spacing:1.2px;color:var(--mute);font-weight:600}
.metric .val{font-size:30px;font-weight:700;margin:6px 0 2px;letter-spacing:-.5px;line-height:1.1}
.metric .unit{font-size:13px;color:var(--ink2);font-weight:500}
.metric .sub{font-size:12px;color:var(--mute);margin-top:4px}
.badge{display:inline-block;padding:3px 9px;border-radius:999px;font-size:10px;font-weight:700;letter-spacing:.4px;margin-top:8px}
.s-ok{background:var(--ok-bg);color:var(--ok)}
.s-watch{background:var(--watch-bg);color:var(--watch)}
.s-warn{background:var(--warn-bg);color:var(--warn)}
.s-bad{background:var(--bad-bg);color:var(--bad)}
.s-mute{background:var(--mute-bg);color:var(--mute)}
/* Trend section */
.section{background:var(--panel);border:1px solid var(--grid);border-radius:14px;padding:18px 20px;margin-bottom:14px}
.section h2{font-size:11px;text-transform:uppercase;letter-spacing:1.5px;color:var(--mute);margin:0 0 12px;font-weight:700}
.spark-row{display:flex;align-items:center;justify-content:space-between;gap:14px;padding:10px 0;border-bottom:1px solid var(--grid)}
.spark-row:last-child{border-bottom:none}
.spark-label{flex:0 0 110px;font-size:13px;color:var(--ink2);font-weight:500}
.spark-svg{flex:1;height:36px}
.spark-num{flex:0 0 90px;text-align:right;font-size:13px;color:var(--ink);font-variant-numeric:tabular-nums}
.spark-num .delta{display:block;font-size:11px;color:var(--mute);font-weight:500}
.empty{font-size:13px;color:var(--mute);padding:14px 0;text-align:center}
/* Collapsible */
details{background:var(--panel);border:1px solid var(--grid);border-radius:14px;margin-bottom:14px}
summary{cursor:pointer;padding:14px 20px;font-size:13px;font-weight:600;color:var(--ink2);text-transform:uppercase;letter-spacing:1.2px;display:flex;justify-content:space-between;align-items:center;list-style:none}
summary::-webkit-details-marker{display:none}
summary::after{content:'＋';color:var(--mute);font-size:18px;font-weight:400}
details[open] summary::after{content:'－'}
.detail-grid{padding:0 20px 18px;display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px}
.detail-card{background:var(--panel2);border:1px solid var(--grid);border-radius:10px;padding:12px 14px}
.detail-card .lbl{font-size:10px;text-transform:uppercase;letter-spacing:1.2px;color:var(--mute);font-weight:600}
.detail-card .val{font-size:20px;font-weight:700;margin:4px 0;letter-spacing:-.3px}
.detail-card .sub{font-size:11px;color:var(--mute)}
.bands{padding:14px 20px 18px;font-size:12px}
.bands .row{display:flex;justify-content:space-between;padding:4px 0;color:var(--ink2)}
.bands .row span:first-child{color:var(--mute)}
.bands .row span:last-child{font-variant-numeric:tabular-nums}
/* Controls */
.controls{display:flex;gap:10px;flex-wrap:wrap;margin-top:8px}
button{background:transparent;color:var(--ink);border:1px solid var(--grid);border-radius:10px;padding:9px 14px;cursor:pointer;font-size:13px;font-weight:500;font-family:inherit;transition:background .15s,border-color .15s}
button:hover{background:var(--panel2);border-color:#2a3a5e}
button.primary{background:var(--watch-bg);border-color:transparent;color:var(--watch)}
button.primary:hover{background:rgba(124,199,255,.22)}
.help-text{font-size:11px;color:var(--mute);margin-top:10px;line-height:1.5}
.logbox{background:#06080f;border:1px solid var(--grid);border-radius:8px;padding:10px 12px;font-family:ui-monospace,SF Mono,Menlo,monospace;font-size:11px;line-height:1.5;color:#9fb1c9;max-height:300px;overflow-y:auto;white-space:pre-wrap;word-break:break-word}
.logbox .row{padding:1px 0}
.audio-item{display:flex;align-items:center;gap:10px;padding:10px 0;border-bottom:1px solid var(--grid);flex-wrap:wrap}
.audio-item:last-child{border-bottom:none}
.audio-name{flex:0 0 160px;font-size:12px;color:var(--ink2);font-variant-numeric:tabular-nums}
.audio-item audio{flex:1;height:34px;min-width:200px;max-width:340px}
.audio-dl{display:inline-flex;align-items:center;gap:5px;padding:6px 12px;background:var(--watch-bg);color:var(--watch);border-radius:8px;font-size:12px;text-decoration:none;font-weight:600}
.audio-dl:hover{background:rgba(124,199,255,.25)}
.audio-size{flex:0 0 55px;text-align:right;font-size:11px;color:var(--mute)}
/* Per-metric explain panel */
.explain{margin-top:10px;font-size:11px}
.explain summary{cursor:pointer;color:var(--mute);user-select:none;list-style:none;font-size:11px;display:inline-flex;align-items:center;gap:4px;padding:3px 8px;border-radius:6px}
.explain summary:hover{background:var(--panel2);color:var(--ink2)}
.explain summary::-webkit-details-marker{display:none}
.explain summary::before{content:'?';width:14px;height:14px;border-radius:50%;background:var(--grid);color:var(--ink2);display:inline-flex;align-items:center;justify-content:center;font-size:10px;font-weight:700}
.explain summary::after{content:'  Learn more'}
.explain[open] summary::after{content:'  Hide'}
.explain-body{margin-top:8px;padding:10px 12px;background:var(--panel2);border-radius:8px;color:var(--ink2);line-height:1.55;font-size:12px}
.explain-body b{color:var(--ink)}
.explain-body .range-line{display:flex;justify-content:space-between;padding:3px 0;font-variant-numeric:tabular-nums}
.explain-body .range-line span:first-child{color:var(--mute)}
</style>
<div class="wrap">
  <header>
    <div class="brand"><span class="dot" id="dot"></span>Beehive Keeper v3.1</div>
    <div class="foot" id="foot">—</div>
  </header>

  <!-- Hero -->
  <div class="hero">
    <div class="hero-row">
      <div>
        <div class="hero-title">Hive Status</div>
        <div class="hero-state" id="state">—</div>
        <div class="hero-sub" id="summary">Loading…</div>
      </div>
      <div id="state_pill" class="hero-pill s-mute">—</div>
    </div>
    <div class="hero-meta">
      <span><b id="m_health">—</b> Health</span>
      <span><b id="m_brood">—</b> Brood Temp</span>
      <span><b id="m_hum">—</b> Humidity</span>
      <span><b id="m_act">—</b> Activity</span>
      <span><b id="m_batt">—</b> Battery</span>
      <span><b id="m_ts">—</b></span>
    </div>
  </div>

  <!-- Primary trio -->
  <div class="primary">
    <div class="metric">
      <div class="lbl">Temperature</div>
      <div class="val"><span id="brood">—</span><span class="unit"> °C</span></div>
      <div class="sub" id="broodF">—</div>
      <span id="brood_badge" class="badge s-mute">—</span>
      <details class="explain"><summary></summary><div class="explain-body">
        <b>What it measures:</b> Internal temperature next to the brood nest (where bees raise their young). Bees actively heat or fan this area to keep it stable.<br><br>
        <div class="range-line"><span>Healthy</span><span>33–36°C</span></div>
        <div class="range-line"><span>Cool / Warm</span><span>30–37°C</span></div>
        <div class="range-line"><span>Cold / Hot stress</span><span>&lt;30 or &gt;37°C</span></div>
        <div class="range-line"><span>Critical (brood death)</span><span>&lt;6°C or &gt;38°C</span></div>
      </div></details>
    </div>
    <div class="metric">
      <div class="lbl">Humidity</div>
      <div class="val"><span id="hum">—</span><span class="unit"> %</span></div>
      <div class="sub" id="dewSub">Dewpoint: —</div>
      <span id="hum_badge" class="badge s-mute">—</span>
      <details class="explain"><summary></summary><div class="explain-body">
        <b>What it measures:</b> Relative humidity inside the hive. Affects whether eggs hatch (need ≥55%), how easily nectar dries to honey, and whether condensation forms in winter.<br><br>
        <div class="range-line"><span>Optimal</span><span>50–65%</span></div>
        <div class="range-line"><span>Decent</span><span>40–75%</span></div>
        <div class="range-line"><span>Dry / Humid</span><span>&lt;40% or &gt;75%</span></div>
        <div class="range-line"><span>Condensation risk</span><span>&gt;85%</span></div>
      </div></details>
    </div>
    <div class="metric">
      <div class="lbl">Activity</div>
      <div class="val"><span id="db">—</span><span class="unit"> dBFS</span></div>
      <div class="sub" id="wingSub">Wingbeat: —</div>
      <span id="db_badge" class="badge s-mute">—</span>
      <details class="explain"><summary></summary><div class="explain-body">
        <b>What it measures:</b> Sound level inside the hive. dBFS = decibels below the mic's clipping point (0 = max, more negative = quieter).<br><br>
        <div class="range-line"><span>Normal buzz</span><span>−30 to −15 dBFS</span></div>
        <div class="range-line"><span>Quiet</span><span>−50 to −30 dBFS</span></div>
        <div class="range-line"><span>Silent (concerning)</span><span>&lt;−50 dBFS</span></div>
        <div class="range-line"><span>Loud / agitated</span><span>&gt;−15 dBFS</span></div>
      </div></details>
    </div>
  </div>

  <!-- Trend / sparklines -->
  <div class="section">
    <h2>Last 2 weeks · downsampled from SD log</h2>
    <div id="sparks">
      <div class="empty" id="empty_msg">Waiting for SD log data… (one row written every 15 min)</div>
    </div>
    <div class="help-text" id="window_info" style="margin-top:8px"></div>
  </div>

  <!-- Details -->
  <details open>
    <summary>Advanced Indicators</summary>
    <div class="detail-grid">
      <div class="detail-card"><div class="lbl">Health Score</div>
        <div class="val"><span id="health">—</span><span class="unit"> / 100</span></div>
        <span id="health_badge" class="badge s-mute">—</span>
        <div class="sub" style="margin-top:6px">Composite of temp, humidity, queen, stability, sound</div>
        <details class="explain"><summary></summary><div class="explain-body">
          <b>What it is:</b> Single 0–100 number combining all metrics with weights based on bee-research priorities. A quick "is this colony OK?" indicator.<br><br>
          <div class="range-line"><span>Healthy</span><span>80–100</span></div>
          <div class="range-line"><span>Watch</span><span>60–79</span></div>
          <div class="range-line"><span>Stressed</span><span>40–59</span></div>
          <div class="range-line"><span>Critical</span><span>&lt;40</span></div>
          <b>Components:</b> Brood temp (30 pts), Humidity (20), Queen index (20), Temp stability (15), Wingbeat (10), Sound (5).
        </div></details></div>
      <div class="detail-card"><div class="lbl">Temp Stability</div>
        <div class="val"><span id="stab">—</span><span class="unit"> °C σ</span></div>
        <span id="stab_badge" class="badge s-mute">—</span>
        <div class="sub" style="margin-top:6px">Rolling 60-min std dev of brood temp</div>
        <details class="explain"><summary></summary><div class="explain-body">
          <b>What it measures:</b> Standard deviation of the brood temperature over the last hour. Healthy colonies thermoregulate within a tight band (±0.5°C). A rising std-dev means the colony is losing its grip — could be sickness, queen loss, or declining population.<br><br>
          <div class="range-line"><span>Healthy thermoregulation</span><span>&lt;0.5°C</span></div>
          <div class="range-line"><span>Watch</span><span>0.5–1.0°C</span></div>
          <div class="range-line"><span>Failing</span><span>&gt;1.0°C</span></div>
          <b>Source:</b> Stabentheiner et al., established metric in bee research.
        </div></details></div>
      <div class="detail-card"><div class="lbl">Fanning</div>
        <div class="val" id="fan" style="font-size:18px">—</div>
        <span id="fan_badge" class="badge s-mute">—</span>
        <div class="sub" style="margin-top:6px">Cross-sensor cooling detection</div>
        <details class="explain"><summary></summary><div class="explain-body">
          <b>What it detects:</b> Bees fanning their wings to cool the hive — a cooperative response when the colony is overheating. Three sensors confirm one behavior:<br><br>
          • Brood temp &gt; 34.5°C (hot enough to need cooling)<br>
          • Sound RMS &gt; 1.3× recent baseline (lots of wings buzzing)<br>
          • Wingbeat peak between 200–350 Hz (worker fanning signature)<br><br>
          When all three align, the dashboard flags "FANNING". A normal hot-day occurrence; persistent fanning that doesn't bring temp down = insufficient ventilation, possible action needed.
        </div></details></div>
      <div class="detail-card"><div class="lbl">Acoustic Events</div>
        <div class="val" id="events">0</div>
        <div class="sub" style="margin-top:6px">Sudden sound spikes since boot</div>
        <details class="explain"><summary></summary><div class="explain-body">
          <b>What it counts:</b> Each time the instantaneous sound RMS jumps &gt;2σ above the rolling 10-minute baseline (and &gt;1.5× the mean to filter trivial noise), the device counts it as an "acoustic event" and <b>immediately records a tagged WAV clip</b>. Catches: predator approach, hive disturbance, swarm events, robber attacks, beekeeper interference.<br><br>
          Cooldown of 5 minutes between events prevents spam. The event WAV files appear in the Audio Clips section like any other recording — just look at the timestamp to correlate.<br><br>
          <b>Demo:</b> clap loudly near the device → an event fires → check Audio Clips for a fresh recording capturing your clap.
        </div></details></div>
    </div>
  </details>

  <details>
    <summary>Climate Details</summary>
    <div class="detail-grid">
      <div class="detail-card"><div class="lbl">VPD</div>
        <div class="val"><span id="vpd">—</span><span class="unit"> kPa</span></div>
        <span id="vpd_badge" class="badge s-mute">—</span>
        <div class="sub" style="margin-top:6px">0.4–0.8 = brood-friendly · &gt;1.5 = drying stress</div>
        <details class="explain"><summary></summary><div class="explain-body">
          <b>What it measures:</b> Vapor Pressure Deficit — how "thirsty" the air is. Unlike raw humidity, this accounts for temperature too. Higher VPD = drier-feeling air → more water evaporates from brood, nectar, and bees themselves. Standard metric in plant science, now used in bee research because it predicts colony water-stress better than humidity alone.<br><br>
          <div class="range-line"><span>Brood-friendly</span><span>0.4–0.8 kPa</span></div>
          <div class="range-line"><span>Balanced</span><span>0.8–1.2 kPa</span></div>
          <div class="range-line"><span>Drying stress</span><span>1.2–2.0 kPa</span></div>
          <div class="range-line"><span>High stress</span><span>&gt;2.0 kPa</span></div>
          <div class="range-line"><span>Saturated (mold risk)</span><span>&lt;0.4 kPa</span></div>
          <b>Formula:</b> Tetens equation. SVP = 0.6108·exp(17.27·T/(T+237.3)); VPD = SVP − (SVP·RH/100).
        </div></details></div>
      <div class="detail-card"><div class="lbl">Dewpoint Margin</div>
        <div class="val"><span id="dewmargin">—</span><span class="unit"> °C</span></div>
        <span id="dew_badge" class="badge s-mute">—</span>
        <div class="sub" style="margin-top:6px">Brood − Dewpoint. ≤1 = condensation risk</div>
        <details class="explain"><summary></summary><div class="explain-body">
          <b>What it measures:</b> How many degrees away the hive temperature is from the point where water droplets start forming on cold surfaces. The <b>#1 cause of winter colony death</b> isn't cold — it's condensation dripping onto clustering bees, soaking them, and they freeze.<br><br>
          <div class="range-line"><span>Safe</span><span>&gt;5°C margin</span></div>
          <div class="range-line"><span>Watch</span><span>1–5°C margin</span></div>
          <div class="range-line"><span>Condensation forming</span><span>≤1°C margin</span></div>
        </div></details></div>
      <div class="detail-card"><div class="lbl">Dewpoint</div>
        <div class="val"><span id="dew">—</span><span class="unit"> °C</span></div>
        <div class="sub" style="margin-top:6px">Magnus formula from temp+RH</div>
        <details class="explain"><summary></summary><div class="explain-body">
          <b>What it measures:</b> The temperature at which the air's current water vapor would saturate (100% RH) and start condensing. Calculated from current temp + humidity. Doesn't change unless humidity changes.<br><br>
          <b>Formula:</b> Magnus equation (valid −45 to +60°C):
          Td = b·γ / (a−γ), where γ = ln(RH/100) + a·T/(b+T), a=17.625, b=243.04.
        </div></details></div>
    </div>
  </details>

  <details>
    <summary>Acoustic Details</summary>
    <div class="detail-grid">
      <div class="detail-card"><div class="lbl">Wingbeat Peak</div>
        <div class="val"><span id="wing">—</span><span class="unit"> Hz</span></div>
        <span id="wing_badge" class="badge s-mute">—</span>
        <div class="sub" style="margin-top:6px">Worker 200–250 · Queen 300–450</div>
        <details class="explain"><summary></summary><div class="explain-body">
          <b>What it measures:</b> The dominant frequency in the 100–500 Hz band — the band where bee wing flapping lives. Found by running an FFT (Fast Fourier Transform) on the mic samples and picking the loudest frequency bin.<br><br>
          <div class="range-line"><span>No bee signal (room noise)</span><span>&lt;150 Hz</span></div>
          <div class="range-line"><span>Healthy worker buzz</span><span>200–250 Hz</span></div>
          <div class="range-line"><span>Queen tooting / piping</span><span>300–450 Hz</span></div>
          <div class="range-line"><span>Elevated / stressed</span><span>&gt;500 Hz</span></div>
          <b>Why it matters:</b> Workers and queens have characteristic wingbeat frequencies. A sudden shift upward signals agitation or pre-swarming.
        </div></details></div>
      <div class="detail-card"><div class="lbl">Queen Index</div>
        <div class="val" id="qi">—</div>
        <span id="qi_badge" class="badge s-mute">—</span>
        <div class="sub" style="margin-top:6px">Queen band / Worker band. 0.15–0.50 = queenright</div>
        <details class="explain"><summary></summary><div class="explain-body">
          <b>What it measures:</b> Ratio of FFT energy in the 300–600 Hz "queen" band to the 100–300 Hz "worker" band. A queenright colony hums with both bands present in balance.<br><br>
          <div class="range-line"><span>Queenright (healthy)</span><span>0.15–0.50</span></div>
          <div class="range-line"><span>Unusual / loud queen</span><span>&gt;0.50</span></div>
          <div class="range-line"><span>Low queen signal — queenless?</span><span>&lt;0.10 sustained</span></div>
          <b>Why it matters:</b> Loss of queen presence shows up as sustained low ratio over hours — a major colony-failure early warning.<br><br>
          <b>Formula:</b> queen_band_power / (worker_band_power + 1).
        </div></details></div>
      <div class="detail-card"><div class="lbl">Spectral Centroid</div>
        <div class="val"><span id="cent">—</span><span class="unit"> Hz</span></div>
        <span id="cent_badge" class="badge s-mute">—</span>
        <div class="sub" style="margin-top:6px">200–400 = healthy · &gt;600 = stressed</div>
        <details class="explain"><summary></summary><div class="explain-body">
          <b>What it measures:</b> The "center of mass" of the sound spectrum — single number summarizing where most of the acoustic energy lives. Computed as the frequency-weighted average of FFT magnitudes.<br><br>
          <div class="range-line"><span>Ambient noise only</span><span>&lt;150 Hz</span></div>
          <div class="range-line"><span>Healthy buzz</span><span>200–400 Hz</span></div>
          <div class="range-line"><span>Elevated</span><span>400–600 Hz</span></div>
          <div class="range-line"><span>Stressed / agitated</span><span>&gt;600 Hz</span></div>
          <b>Why it matters:</b> Stressed colonies shift their sound spectrum upward. Tracks the broad acoustic state in one number.<br><br>
          <b>Formula:</b> Σ(f·X(f)) / Σ(X(f)) — sum of frequency × magnitude divided by total magnitude.
        </div></details></div>
      <div class="detail-card"><div class="lbl">RMS Amplitude</div>
        <div class="val" id="rms">—</div>
        <div class="sub" style="margin-top:6px">Raw mic amplitude (use dBFS instead)</div>
        <details class="explain"><summary></summary><div class="explain-body">
          <b>What it measures:</b> Root-Mean-Square of the raw 16-bit microphone samples. Larger number = louder sound, but the scale isn't intuitive. The dBFS value (in the Activity card on top) is the same info in a more standard format.<br><br>
          <b>Formula:</b> √(mean of (sample²)) over a window of 1024 samples.
        </div></details></div>
    </div>
    <div class="bands">
      <div style="font-size:10px;text-transform:uppercase;letter-spacing:1.2px;color:var(--mute);margin-bottom:6px;font-weight:700">FFT band power</div>
      <div class="row"><span>Low (0–100 Hz)</span><span id="bl">—</span></div>
      <div class="row"><span>Worker (100–300 Hz)</span><span id="bw">—</span></div>
      <div class="row"><span>Queen (300–600 Hz)</span><span id="bq">—</span></div>
      <div class="row"><span>Agitation (600–2k Hz)</span><span id="ba">—</span></div>
    </div>
  </details>

  <details>
    <summary>Power</summary>
    <div class="detail-grid">
      <div class="detail-card"><div class="lbl">Battery</div>
        <div class="val"><span id="batt_pct">—</span><span class="unit"> %</span></div>
        <div class="sub" id="batt_v">— V</div>
        <span id="batt_badge" class="badge s-mute">—</span>
        <div class="sub" style="margin-top:6px">18650 Li-ion. Approximate SoC from voltage curve.</div></div>
      <div class="detail-card"><div class="lbl">Solar / Charging</div>
        <div class="val" id="charge_text" style="font-size:18px">—</div>
        <span id="charge_badge" class="badge s-mute">—</span>
        <div class="sub" style="margin-top:6px">Inferred from battery voltage trend over last few minutes. Rising = charging.</div></div>
    </div>
  </details>

  <details>
    <summary>Audio Clips</summary>
    <div style="padding:14px 20px 0"><button class="primary" id="rec_btn" onclick="recordNow()">Record 3 sec now</button>
      <span id="rec_status" style="font-size:11px;color:var(--mute);margin-left:10px"></span></div>
    <div class="bands" id="audio_list">
      <div class="empty">No audio clips yet. First clip records ~1 hour after boot.</div>
    </div>
  </details>

  <details>
    <summary>Device Log</summary>
    <div class="bands"><div class="logbox" id="logbox">Loading...</div>
      <div class="controls" style="margin-top:8px"><button onclick="logTick()">Refresh log</button></div>
      <div class="help-text">Last 30 events from the device (boot, WiFi state, audio saves, etc.). Auto-refreshes every 30s. Useful for debugging without a USB cable.</div>
    </div>
  </details>

  <details>
    <summary>System &amp; Controls</summary>
    <div class="detail-grid">
      <div class="detail-card"><div class="lbl">Last Reading</div>
        <div class="val" id="ts2" style="font-size:14px">—</div>
        <div class="sub" id="ip" style="margin-top:6px">—</div></div>
      <div class="detail-card"><div class="lbl">SD Log</div>
        <div class="val" id="sd_rows" style="font-size:18px">—</div>
        <div class="sub" style="margin-top:6px">Rows in /hive_log.csv · 15-min cadence</div></div>
      <div class="detail-card" style="grid-column:span 2"><div class="lbl">Actions</div>
        <div class="controls">
          <button class="primary" onclick="tick(true);sparkTick();audioTick()">Refresh all (live read)</button>
          <button onclick="location.href='/download'">Download full CSV</button>
        </div>
        <div class="help-text">Dashboard auto-refreshes every 60s. Sparklines refresh every 5 min from SD.</div></div>
    </div>
  </details>

  <details>
    <summary>Maintenance</summary>
    <div class="detail-grid">
      <div class="detail-card" style="grid-column:span 2"><div class="lbl">Wipe all SD data</div>
        <div class="sub" style="margin:6px 0 10px">Deletes hive_log.csv and every audio_*.wav from the SD card. Useful when starting a fresh deployment.</div>
        <button onclick="wipeData()" style="background:var(--bad-bg);color:var(--bad);border:0">Wipe SD data</button>
        <span id="wipe_status" style="font-size:11px;color:var(--mute);margin-left:10px"></span></div>
      <div class="detail-card" style="grid-column:span 2"><div class="lbl">Setup WiFi</div>
        <div class="sub" style="margin:6px 0 10px">Save new WiFi credentials to the device's non-volatile memory. The device reboots and joins the new network. Leave password blank for open networks (no password needed).</div>
        <form onsubmit="setWifi(event)" style="display:flex;flex-direction:column;gap:8px;max-width:360px">
          <input name="ssid" placeholder="WiFi name (SSID)" required
            style="padding:8px 10px;background:var(--panel2);border:1px solid var(--grid);border-radius:6px;color:var(--ink);font:inherit">
          <input name="pass" placeholder="Password (leave blank for open)" type="password"
            style="padding:8px 10px;background:var(--panel2);border:1px solid var(--grid);border-radius:6px;color:var(--ink);font:inherit">
          <button type="submit" class="primary">Save and reboot</button>
        </form>
        <div id="wifi_status" style="font-size:11px;color:var(--mute);margin-top:8px"></div></div>
    </div>
  </details>
</div>

<script>
function fmt(v,d){return (typeof v==='number'&&!isNaN(v))?v.toFixed(d):'—';}
function sci(v){
  if (typeof v!=='number'||isNaN(v)) return '—';
  if (v===0) return '0';
  var a=Math.abs(v);
  if (a>=1e4||a<1e-2) return v.toExponential(2);
  return v.toFixed(1);
}
function setBadge(id,label,cls){
  var el=document.getElementById(id);
  if (!el) return;
  el.textContent=label;
  el.className='badge '+cls;
}
function broodStatus(t){
  if (t==null||isNaN(t)) return ['NO DATA','s-mute'];
  if (t<6) return ['FREEZING','s-bad'];
  if (t>38) return ['OVERHEATING','s-bad'];
  if (t>=33&&t<=36) return ['HEALTHY','s-ok'];
  if (t>=30&&t<33) return ['COOL','s-warn'];
  if (t>36&&t<=37) return ['WARM','s-warn'];
  if (t<30) return ['COLD','s-warn'];
  return ['HOT','s-warn'];
}
function humStatus(h){
  if (h==null||isNaN(h)) return ['NO DATA','s-mute'];
  if (h>=50&&h<=65) return ['HEALTHY','s-ok'];
  if (h>=40&&h<=75) return ['DECENT','s-watch'];
  if (h>85) return ['CONDENSATION RISK','s-bad'];
  if (h>75) return ['HUMID','s-warn'];
  if (h<30) return ['VERY DRY','s-bad'];
  return ['DRY','s-warn'];
}
function vpdStatus(v){
  if (v==null||isNaN(v)) return ['NO DATA','s-mute'];
  if (v>=0.4&&v<=0.8) return ['BROOD-FRIENDLY','s-ok'];
  if (v>0.8&&v<=1.2) return ['BALANCED','s-watch'];
  if (v>1.2&&v<=2.0) return ['DRYING STRESS','s-warn'];
  if (v>2.0) return ['HIGH STRESS','s-bad'];
  return ['SATURATED','s-warn'];
}
function dewMarginStatus(m){
  if (m==null||isNaN(m)) return ['NO DATA','s-mute'];
  if (m>5) return ['SAFE','s-ok'];
  if (m>1) return ['WATCH','s-watch'];
  return ['CONDENSATION','s-bad'];
}
function dbStatus(d){
  if (d==null||isNaN(d)) return ['NO DATA','s-mute'];
  if (d>-15&&d<0) return ['LOUD','s-warn'];
  if (d>=-30&&d<=-15) return ['NORMAL BUZZ','s-ok'];
  if (d>=-50&&d<-30) return ['QUIET','s-watch'];
  if (d<-50) return ['SILENT','s-bad'];
  return ['—','s-mute'];
}
function wingStatus(w){
  if (w==null||isNaN(w)) return ['NO DATA','s-mute'];
  if (w<150) return ['NO BEE SIGNAL','s-mute'];
  if (w>=200&&w<=300) return ['WORKER BUZZ','s-ok'];
  if (w>300&&w<=450) return ['QUEEN BAND','s-watch'];
  if (w>450) return ['ELEVATED','s-warn'];
  return ['LOW','s-watch'];
}
function qiStatus(q){
  if (q==null||isNaN(q)) return ['NO DATA','s-mute'];
  if (q>=0.15&&q<=0.50) return ['QUEENRIGHT','s-ok'];
  if (q<0.10) return ['LOW QUEEN SIGNAL','s-warn'];
  if (q>0.50) return ['UNUSUAL','s-watch'];
  return ['WATCH','s-watch'];
}
function centStatus(c){
  if (c==null||isNaN(c)) return ['NO DATA','s-mute'];
  if (c<150) return ['AMBIENT ONLY','s-mute'];
  if (c>=200&&c<=400) return ['HEALTHY','s-ok'];
  if (c>400&&c<=600) return ['ELEVATED','s-watch'];
  if (c>600) return ['STRESSED','s-warn'];
  return ['LOW','s-watch'];
}
function batteryStatus(v,pct){
  if (v==null||isNaN(v)||pct<0) return ['NOT WIRED','s-mute'];
  if (v<3.0) return ['CRITICAL','s-bad'];
  if (v<3.4) return ['LOW','s-warn'];
  if (v<3.7) return ['OK','s-watch'];
  return ['GOOD','s-ok'];
}
function chargeStatus(c){
  if (c==='charging') return ['CHARGING','s-ok','Charging from solar / USB'];
  if (c==='not_charging') return ['NOT CHARGING','s-warn','Charger present but not delivering current'];
  if (c==='discharging') return ['DISCHARGING','s-watch','Battery voltage falling'];
  if (c==='steady') return ['STEADY','s-watch','Voltage flat — no charge or discharge detected'];
  if (c==='not_wired') return ['NOT WIRED','s-mute','Wire voltage divider to GPIO 35 to enable'];
  return ['UNKNOWN','s-mute','Waiting for trend data (a few minutes after boot)'];
}
function healthStatus(h){
  if (h==null||h<0) return ['NO DATA','s-mute'];
  if (h>=80) return ['HEALTHY','s-ok'];
  if (h>=60) return ['WATCH','s-watch'];
  if (h>=40) return ['STRESSED','s-warn'];
  return ['CRITICAL','s-bad'];
}
function stabStatus(s){
  if (s==null||isNaN(s)) return ['NO DATA','s-mute'];
  if (s<0.5) return ['HEALTHY','s-ok'];
  if (s<1.0) return ['WATCH','s-watch'];
  if (s<2.0) return ['WEAK','s-warn'];
  return ['FAILING','s-bad'];
}
function fanStatus(f){
  if (f===true) return ['FANNING','s-watch'];
  return ['IDLE','s-mute'];
}
var SUMMARIES={
  HEALTHY:'All readings in normal range. Colony appears stable.',
  WARM:'Brood temperature is slightly above optimal. Watch for sustained heat.',
  COOL:'Brood temperature is slightly below optimal. Watch for sustained cold.',
  HUMID:'Humidity is elevated. Increased risk of mold and condensation.',
  DRY:'Humidity is low. Brood may be at risk of dehydration.',
  OVERHEATING:'CRITICAL: Brood temperature exceeds 38°C. Immediate action required.',
  FREEZING:'CRITICAL: Brood temperature below 6°C. Cluster cold-stressed.',
  CONDENSATION_RISK:'CRITICAL: Air temperature is at the dewpoint. Water droplets forming.',
  SWARM_WARNING:'WARNING: Rapid temperature rise detected — possible pre-swarm activity.',
  SILENT_HIVE:'WARNING: Acoustic activity below baseline. Colony quiet.',
  NO_DATA:'Waiting for sensor data.'
};
function stateClass(s){
  if (s==='HEALTHY') return 's-ok';
  if (['OVERHEATING','FREEZING','CONDENSATION_RISK','SWARM_WARNING'].indexOf(s)>=0) return 's-bad';
  if (s==='NO_DATA') return 's-mute';
  return 's-warn';
}
function setHero(j){
  var s=j.state||'NO_DATA';
  document.getElementById('state').textContent=s.replace(/_/g,' ');
  document.getElementById('summary').textContent=SUMMARIES[s]||'Status unknown.';
  var pill=document.getElementById('state_pill');
  pill.textContent=s.replace(/_/g,' ');
  pill.className='hero-pill '+stateClass(s);
  var dot=document.getElementById('dot');
  dot.style.background=getComputedStyle(document.documentElement).getPropertyValue('--'+(stateClass(s).replace('s-','')||'mute'));
  dot.style.boxShadow='0 0 8px '+dot.style.background;
  document.getElementById('m_health').textContent=(j.health_score!=null&&j.health_score>=0)?(j.health_score+'/100'):'—';
  document.getElementById('m_brood').textContent=(typeof j.brood==='number')?j.brood.toFixed(1)+'°C':'—';
  document.getElementById('m_hum').textContent=(typeof j.hum==='number')?j.hum.toFixed(0)+'%':'—';
  document.getElementById('m_act').textContent=(typeof j.sound==='number')?j.sound.toFixed(0)+' dBFS':'—';
  var bm=(j.battery_pct!=null&&j.battery_pct>=0)?(j.battery_pct+'%'):'—';
  if (j.charge==='charging') bm+=' ⚡';
  document.getElementById('m_batt').textContent=bm;
  document.getElementById('m_ts').textContent=j.timestamp||'—';
}
function tick(force){
  // force=true → /readnow forces a fresh sensor read on the device first.
  // Auto-refresh uses /data which serves the cached 15s-interval read.
  var url=force?'/readnow':'/data';
  fetch(url).then(r=>r.json()).then(j=>{
    setHero(j);
    document.getElementById('ts2').textContent=j.timestamp||'—';
    document.getElementById('ip').textContent='IP: '+location.host;
    document.getElementById('foot').textContent=j.timestamp||'';
    document.getElementById('brood').textContent=fmt(j.brood,1);
    document.getElementById('broodF').textContent=(typeof j.brood==='number')?(j.brood*9/5+32).toFixed(1)+' °F':'—';
    document.getElementById('hum').textContent=fmt(j.hum,1);
    document.getElementById('dewSub').textContent='Dewpoint: '+fmt(j.dewpoint,1)+' °C';
    document.getElementById('db').textContent=fmt(j.sound,1);
    document.getElementById('wingSub').textContent='Wingbeat: '+fmt(j.wingbeat,0)+' Hz';
    document.getElementById('vpd').textContent=fmt(j.vpd,2);
    document.getElementById('dew').textContent=fmt(j.dewpoint,1);
    var margin=(typeof j.brood==='number'&&typeof j.dewpoint==='number')?(j.brood-j.dewpoint):NaN;
    document.getElementById('dewmargin').textContent=isNaN(margin)?'—':margin.toFixed(1);
    document.getElementById('rms').textContent=fmt(j.rms,0);
    document.getElementById('wing').textContent=fmt(j.wingbeat,0);
    document.getElementById('qi').textContent=fmt(j.queen_index,2);
    document.getElementById('cent').textContent=fmt(j.centroid,0);
    document.getElementById('bl').textContent=sci(j.band_low);
    document.getElementById('bw').textContent=sci(j.band_worker);
    document.getElementById('bq').textContent=sci(j.band_queen);
    document.getElementById('ba').textContent=sci(j.band_agit);
    var s;
    s=broodStatus(j.brood);     setBadge('brood_badge',s[0],s[1]);
    s=humStatus(j.hum);         setBadge('hum_badge',s[0],s[1]);
    s=dbStatus(j.sound);        setBadge('db_badge',s[0],s[1]);
    s=vpdStatus(j.vpd);         setBadge('vpd_badge',s[0],s[1]);
    s=dewMarginStatus(margin);  setBadge('dew_badge',s[0],s[1]);
    s=wingStatus(j.wingbeat);   setBadge('wing_badge',s[0],s[1]);
    s=qiStatus(j.queen_index);  setBadge('qi_badge',s[0],s[1]);
    s=centStatus(j.centroid);   setBadge('cent_badge',s[0],s[1]);
    // Battery + charging
    document.getElementById('batt_pct').textContent=(j.battery_pct!=null&&j.battery_pct>=0)?j.battery_pct:'—';
    document.getElementById('batt_v').textContent=(typeof j.battery_v==='number')?j.battery_v.toFixed(2)+' V':'Not wired';
    s=batteryStatus(j.battery_v,j.battery_pct);
    setBadge('batt_badge',s[0],s[1]);
    var ch=chargeStatus(j.charge);
    document.getElementById('charge_text').textContent=ch[0];
    setBadge('charge_badge',ch[0],ch[1]);
    // Advanced indicators
    var health=j.health_score;
    document.getElementById('health').textContent=(health!=null&&health>=0)?health:'—';
    s=healthStatus(health);
    setBadge('health_badge',s[0],s[1]);
    document.getElementById('stab').textContent=fmt(j.temp_stability,2);
    s=stabStatus(j.temp_stability);
    setBadge('stab_badge',s[0],s[1]);
    document.getElementById('fan').textContent=j.fanning?'YES':'No';
    s=fanStatus(j.fanning);
    setBadge('fan_badge',s[0],s[1]);
    document.getElementById('events').textContent=j.event_count||0;
  });
}
function sparkSVG(values,color){
  if (!values.length) return '';
  var w=400,h=36,p=2;
  var lo=Math.min.apply(null,values),hi=Math.max.apply(null,values);
  if (lo===hi){hi=lo+1;}
  var n=values.length;
  var pts=values.map(function(v,i){
    var x=p+(i/(n-1||1))*(w-2*p);
    var y=h-p-((v-lo)/(hi-lo))*(h-2*p);
    return x.toFixed(1)+','+y.toFixed(1);
  }).join(' ');
  var lastX=p+((n-1)/(n-1||1))*(w-2*p);
  var lastY=h-p-((values[n-1]-lo)/(hi-lo))*(h-2*p);
  return '<svg class="spark-svg" viewBox="0 0 '+w+' '+h+'" preserveAspectRatio="none">'
    +'<polyline points="'+pts+'" fill="none" stroke="'+color+'" stroke-width="1.6" stroke-linejoin="round" stroke-linecap="round"/>'
    +'<circle cx="'+lastX.toFixed(1)+'" cy="'+lastY.toFixed(1)+'" r="2.4" fill="'+color+'"/></svg>';
}
function sparkRow(label,values,fmt,unit,color){
  if (!values.length){return '';}
  var last=values[values.length-1];
  var first=values[0];
  var delta=last-first;
  var dStr=(delta>=0?'+':'')+delta.toFixed(1)+unit;
  return '<div class="spark-row">'
    +'<div class="spark-label">'+label+'</div>'
    +sparkSVG(values,color)
    +'<div class="spark-num">'+fmt(last)+unit+'<span class="delta">'+dStr+' 24h</span></div>'
    +'</div>';
}
function sparkTick(){
  fetch('/recent').then(r=>r.json()).then(j=>{
    document.getElementById('sd_rows').textContent=(j.total||0)+' rows';
    var temp=[],hum=[],snd=[];
    j.rows.forEach(function(r){
      if (typeof r.b==='number') temp.push(r.b);
      if (typeof r.h==='number') hum.push(r.h);
      if (typeof r.s==='number') snd.push(r.s);
    });
    var host=document.getElementById('sparks');
    var info=document.getElementById('window_info');
    if (!j.rows.length){
      host.innerHTML='<div class="empty">Waiting for SD log data… (one row written every 15 min)</div>';
      info.textContent='';
      return;
    }
    host.innerHTML=
      sparkRow('Brood Temp',temp,function(v){return v.toFixed(1)},' °C','#5DCAA5')
      +sparkRow('Humidity',hum,function(v){return v.toFixed(0)},' %','#7CC7FF')
      +sparkRow('Sound',snd,function(v){return v.toFixed(0)},' dBFS','#E2A04B');
    info.textContent='Showing '+j.rows.length+' points across last 2 weeks · '
      +(j.total||0)+' total rows on SD · download CSV for the full history';
  }).catch(function(){
    document.getElementById('sparks').innerHTML='<div class="empty">SD not available yet.</div>';
  });
}
function formatTimestamp(name){
  // audio_YYYYMMDD_HHMMSS.wav -> YYYY-MM-DD HH:MM:SS
  var m=name.match(/(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})/);
  if (m) return m[1]+'-'+m[2]+'-'+m[3]+' '+m[4]+':'+m[5]+':'+m[6];
  // audio_up_NNNN.wav (no RTC) -> Boot+Ns
  var u=name.match(/audio_up_(\d+)/);
  if (u) return 'Boot+'+u[1]+'s';
  return name;
}
function audioTick(){
  fetch('/audiolist').then(r=>r.json()).then(j=>{
    var host=document.getElementById('audio_list');
    if (!j.files||!j.files.length){
      host.innerHTML='<div class="empty">No audio clips yet. First clip records 30s after boot, then once per hour.</div>';
      return;
    }
    var html='';
    j.files.forEach(function(f){
      var kb=(f.size/1024).toFixed(0);
      var url='/audio?f='+encodeURIComponent(f.name);
      html+='<div class="audio-item">'
        +'<div class="audio-name">'+formatTimestamp(f.name)+'</div>'
        +'<audio controls preload="none" src="'+url+'"></audio>'
        +'<a class="audio-dl" href="'+url+'" download="'+f.name+'">⬇ Download</a>'
        +'<div class="audio-size">'+kb+' KB</div>'
        +'</div>';
    });
    html+='<div class="help-text" style="margin-top:10px">Showing '+j.files.length+' most recent · '+j.total+' total on SD · Older clips auto-deleted (keeping latest 72 = 3 days)</div>';
    host.innerHTML=html;
  }).catch(function(){});
}
function logTick(){
  fetch('/log').then(r=>r.json()).then(j=>{
    var box=document.getElementById('logbox');
    if (!j.lines||!j.lines.length){box.textContent='(no events yet)';return;}
    box.innerHTML=j.lines.map(function(l){return '<div class="row">'+l.replace(/</g,'&lt;')+'</div>';}).join('');
    box.scrollTop=box.scrollHeight;
  }).catch(function(){});
}
function wipeData(){
  if (!confirm('Delete hive_log.csv and all audio files from the SD card? This cannot be undone.')) return;
  var s=document.getElementById('wipe_status');
  s.textContent='Wiping...';
  fetch('/wipe',{method:'POST'}).then(r=>r.text()).then(function(t){
    s.textContent=t;
    setTimeout(function(){sparkTick();audioTick();logTick();},1000);
  }).catch(function(){s.textContent='Failed';});
}
function setWifi(e){
  e.preventDefault();
  var f=e.target;
  var data=new URLSearchParams();
  data.append('ssid',f.ssid.value);
  data.append('pass',f.pass.value||'');
  var s=document.getElementById('wifi_status');
  s.textContent='Saving and rebooting...';
  fetch('/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:data}).then(r=>r.text()).then(function(t){
    s.textContent=t+' (You will need to reconnect to the new network to find the device.)';
  }).catch(function(){s.textContent='Saved (connection dropped during reboot, which is expected).';});
}
function recordNow(){
  var btn=document.getElementById('rec_btn');
  var status=document.getElementById('rec_status');
  btn.disabled=true;
  status.textContent='Recording 3 sec...';
  fetch('/record').then(r=>r.text()).then(function(t){
    status.textContent=t;
    setTimeout(function(){audioTick();logTick();status.textContent='Done. New clip in list.';btn.disabled=false;},3500);
  }).catch(function(){status.textContent='Failed';btn.disabled=false;});
}
tick();
sparkTick();
audioTick();
logTick();
setInterval(tick,60000);
setInterval(sparkTick,300000);
setInterval(audioTick,300000);
setInterval(logTick,30000);
</script>
)RAWHTML";

// =============================================================================
// HTTP handlers
// =============================================================================
void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleData();   // forward decl so handleReadNow can call it

// Force a synchronous sensor read, then return current values. Used by the
// dashboard's Refresh button so a manual click always reflects what the
// sensors see right now, not the last 15-second cache.
void handleReadNow() {
  readSensorsOnce();
  lastSensorReadMs = millis();
  handleData();
}

void handleData() {
  String out = "{";
  out += "\"timestamp\":\"" + isoTimestamp() + "\",";
  out += "\"brood\":"; out += (isnan(currentBroodC) ? "null" : String(currentBroodC, 2));
  out += ",\"entrance\":"; out += (isnan(currentEntranceC) ? "null" : String(currentEntranceC, 2));
  out += ",\"gradient\":"; out += (isnan(currentGradientC) ? "null" : String(currentGradientC, 2));
  out += ",\"hum\":"; out += (isnan(currentHum) ? "null" : String(currentHum, 1));
  out += ",\"dewpoint\":"; out += (isnan(currentDewpointC) ? "null" : String(currentDewpointC, 2));
  out += ",\"vpd\":"; out += (isnan(currentVpdKpa) ? "null" : String(currentVpdKpa, 3));
  out += ",\"sound\":"; out += (isnan(currentDbfs) ? "null" : String(currentDbfs, 1));
  out += ",\"rms\":"; out += (isnan(currentRms) ? "null" : String(currentRms, 2));
  out += ",\"queen_index\":"; out += (isnan(currentQueenIndex) ? "null" : String(currentQueenIndex, 3));
  out += ",\"centroid\":"; out += (isnan(currentCentroidHz) ? "null" : String(currentCentroidHz, 1));
  out += ",\"wingbeat\":"; out += (isnan(currentWingbeatHz) ? "null" : String(currentWingbeatHz, 1));
  out += ",\"band_low\":"; out += (isnan(currentBandLow) ? "null" : String(currentBandLow, 0));
  out += ",\"band_worker\":"; out += (isnan(currentBandWorker) ? "null" : String(currentBandWorker, 0));
  out += ",\"band_queen\":"; out += (isnan(currentBandQueen) ? "null" : String(currentBandQueen, 0));
  out += ",\"band_agit\":"; out += (isnan(currentBandAgit) ? "null" : String(currentBandAgit, 0));
  out += ",\"battery_v\":"; out += (isnan(currentBatteryV) ? "null" : String(currentBatteryV, 3));
  out += ",\"battery_pct\":"; out += String(currentBatteryPct);
  out += ",\"charge\":\"" + currentChargeStatus + "\"";
  out += ",\"temp_stability\":"; out += (isnan(currentTempStability) ? "null" : String(currentTempStability, 3));
  out += ",\"health_score\":"; out += String(currentHealthScore);
  out += ",\"fanning\":"; out += (currentlyFanning ? "true" : "false");
  out += ",\"event_count\":"; out += String(eventCount);
  out += ",\"state\":\"" + currentState + "\"";
  out += "}";
  server.send(200, "application/json", out);
}

// Return a downsampled view of the last 2 weeks of CSV data for sparklines.
// Two-pass streaming approach to keep RAM use bounded regardless of file size:
//   Pass 1: count total data rows.
//   Pass 2: compute stride to pick ~TARGET_POINTS evenly across last WINDOW_ROWS.
// Full CSV is still available via /download. Long-term retention is the SD card.
void handleRecent() {
  if (!sdAvailable) {
    server.send(503, "application/json", "{\"rows\":[],\"reason\":\"no_sd\"}");
    return;
  }
  const int WINDOW_ROWS   = 1344;   // 2 weeks at 15-min cadence (14*96)
  const int TARGET_POINTS = 168;    // ~one point per 2 hours over 14 days

  // Pass 1: count
  File f1 = SD.open(LOG_FILE, FILE_READ);
  if (!f1) { server.send(500, "application/json", "{\"rows\":[],\"reason\":\"no_file\"}"); return; }
  long totalRows = 0;
  bool header = true;
  String line;
  while (f1.available()) {
    char c = f1.read();
    if (c == '\n' || c == '\r') {
      if (line.length() > 0) { if (header) header = false; else totalRows++; }
      line = "";
    } else line += c;
  }
  if (line.length() > 0 && !header) totalRows++;
  f1.close();

  long windowStart = totalRows > WINDOW_ROWS ? (totalRows - WINDOW_ROWS) : 0;
  long inWindow    = totalRows - windowStart;
  long stride      = inWindow > TARGET_POINTS ? (inWindow / TARGET_POINTS) : 1;

  // Pass 2: stream, skipping pre-window rows, then taking every stride-th row.
  File f2 = SD.open(LOG_FILE, FILE_READ);
  if (!f2) { server.send(500, "application/json", "{\"rows\":[],\"reason\":\"no_file2\"}"); return; }
  String out = "{\"window_days\":14,\"total\":" + String(totalRows)
             + ",\"shown\":0,\"rows\":[";
  long rowIdx = -1;   // -1 = still in header
  long shown  = 0;
  line = "";
  bool first = true;
  bool hdr = true;
  while (f2.available()) {
    char c = f2.read();
    if (c != '\n' && c != '\r') { line += c; continue; }
    if (line.length() == 0) continue;
    if (hdr) { hdr = false; line = ""; continue; }
    rowIdx++;
    if (rowIdx < windowStart) { line = ""; continue; }
    long localIdx = rowIdx - windowStart;
    if (localIdx % stride != 0) { line = ""; continue; }
    // Parse the row we're keeping.
    int c1 = line.indexOf(',');               if (c1 < 0) { line = ""; continue; }
    int c2 = line.indexOf(',', c1 + 1);       if (c2 < 0) { line = ""; continue; }
    int c3 = line.indexOf(',', c2 + 1);       if (c3 < 0) { line = ""; continue; }
    int c4 = line.indexOf(',', c3 + 1);       if (c4 < 0) { line = ""; continue; }
    int c5 = line.indexOf(',', c4 + 1);       if (c5 < 0) { line = ""; continue; }
    int c6 = line.indexOf(',', c5 + 1);       if (c6 < 0) { line = ""; continue; }
    String ts    = line.substring(0, c1);
    String brood = line.substring(c1 + 1, c2);
    String hum   = line.substring(c4 + 1, c5);
    String snd   = line.substring(c5 + 1, c6);
    if (!first) out += ",";
    first = false;
    out += "{\"t\":\"" + ts + "\","
         + "\"b\":" + (brood.length() ? brood : "null") + ","
         + "\"h\":" + (hum.length()   ? hum   : "null") + ","
         + "\"s\":" + (snd.length()   ? snd   : "null") + "}";
    shown++;
    line = "";
  }
  f2.close();
  out += "],\"stride\":" + String(stride) + "}";
  // Patch the "shown" placeholder for the client header (cheap, just append; clients use rows.length).
  server.send(200, "application/json", out);
}

// List recent audio clips in SD root (filename prefix "audio_", sorted chrono).
void handleAudioList() {
  if (!sdAvailable) {
    server.send(503, "application/json", "{\"files\":[],\"reason\":\"no_sd\"}");
    return;
  }
  File dir = SD.open("/");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    server.send(200, "application/json", "{\"files\":[]}");
    return;
  }
  const int MAX_FILES = 256;
  String names[MAX_FILES];
  uint32_t sizes[MAX_FILES];
  int count = 0;
  File e;
  while ((e = dir.openNextFile())) {
    String n = e.name();
    if (n.startsWith("audio_") && n.endsWith(".wav") && count < MAX_FILES) {
      names[count] = n;
      sizes[count] = e.size();
      count++;
    }
    e.close();
  }
  dir.close();
  // Sort by name (timestamp-prefixed) descending so newest first
  for (int i = 1; i < count; i++) {
    String kn = names[i]; uint32_t ks = sizes[i]; int j = i - 1;
    while (j >= 0 && names[j] < kn) {
      names[j+1] = names[j]; sizes[j+1] = sizes[j]; j--;
    }
    names[j+1] = kn; sizes[j+1] = ks;
  }
  // Cap to the most recent 24 for the dashboard
  int show = count < 24 ? count : 24;
  String out = "{\"files\":[";
  for (int i = 0; i < show; i++) {
    String fname = names[i];
    if (fname.startsWith("/")) fname = fname.substring(1);
    if (i) out += ",";
    out += "{\"name\":\"" + fname + "\",\"size\":" + String(sizes[i]) + "}";
  }
  out += "],\"total\":" + String(count) + "}";
  server.send(200, "application/json", out);
}

// Serve a single WAV file from SD root. Path: /audio?f=NAME.wav
void handleAudioFile() {
  if (!sdAvailable) { server.send(503, "text/plain", "SD card not available"); return; }
  if (!server.hasArg("f")) { server.send(400, "text/plain", "missing ?f="); return; }
  String fname = server.arg("f");
  // Basic safety: must be audio_*.wav, no path traversal
  if (fname.indexOf('/') >= 0 || fname.indexOf("..") >= 0
      || !fname.endsWith(".wav") || !fname.startsWith("audio_")) {
    server.send(400, "text/plain", "bad filename");
    return;
  }
  String path = "/" + fname;
  File f = SD.open(path, FILE_READ);
  if (!f) { server.send(404, "text/plain", "not found"); return; }
  server.sendHeader("Content-Disposition", "inline; filename=" + fname);
  server.streamFile(f, "audio/wav");
  f.close();
}

void handleDownload() {
  if (!sdAvailable) {
    server.send(503, "text/plain", "SD card not available");
    return;
  }
  File f = SD.open(LOG_FILE, FILE_READ);
  if (!f) {
    server.send(500, "text/plain", "Cannot open log file");
    return;
  }
  server.sendHeader("Content-Disposition", "attachment; filename=hive_log.csv");
  server.streamFile(f, "text/csv");
  f.close();
}

// =============================================================================
// Manual recording trigger — /record fires a 3-second WAV recording on demand
// =============================================================================
void handleRecord() {
  if (!sdAvailable) {
    server.send(503, "text/plain", "SD card not available");
    return;
  }
  server.send(200, "text/plain", "Recording started (3 sec). Refresh the audio list when done.");
  logmsg("Manual /record triggered");
  recordAudioClip();
}

// =============================================================================
// Wipe all SD data on demand (CSV log + every audio_*.wav file)
// =============================================================================
void handleWipe() {
  if (!sdAvailable) { server.send(503, "text/plain", "SD not available"); return; }
  int wiped = 0;
  SD.remove(LOG_FILE);
  File dir = SD.open("/");
  if (dir && dir.isDirectory()) {
    File e;
    while ((e = dir.openNextFile())) {
      String n = e.name();
      e.close();
      if (n.startsWith("audio_") && n.endsWith(".wav")) {
        if (SD.remove(String("/") + n)) wiped++;
      }
    }
    dir.close();
  }
  eventCount = 0;
  logmsg("SD wiped via /wipe: log + %d audio files", wiped);
  String reply = "Wiped: hive_log.csv + " + String(wiped) + " audio files";
  server.send(200, "text/plain", reply);
}

// =============================================================================
// Set WiFi credentials over the dashboard, persist to NVS, reboot
// =============================================================================
void handleSetWifi() {
  if (!server.hasArg("ssid")) { server.send(400, "text/plain", "missing ssid"); return; }
  String ssid = server.arg("ssid");
  String pass = server.hasArg("pass") ? server.arg("pass") : "";
  ssid.trim();
  if (ssid.length() == 0) { server.send(400, "text/plain", "ssid cannot be empty"); return; }
  wifiPrefs.begin("wifi", false);  // read-write
  wifiPrefs.putString("ssid", ssid);
  wifiPrefs.putString("pass", pass);
  wifiPrefs.end();
  logmsg("WiFi creds saved (%s). Rebooting...", ssid.c_str());
  server.send(200, "text/plain", "Saved. Rebooting in 1 second...");
  delay(1200);
  ESP.restart();
}

// =============================================================================
// Remote serial-log view — /log returns recent device events as JSON
// =============================================================================
void handleLog() {
  String out = "{\"lines\":[";
  int start = (logRingIdx - logRingCount + LOG_LINES) % LOG_LINES;
  for (int i = 0; i < logRingCount; i++) {
    String line = logRing[(start + i) % LOG_LINES];
    line.replace("\\", "\\\\");
    line.replace("\"", "\\\"");
    if (i) out += ",";
    out += "\"" + line + "\"";
  }
  out += "]}";
  server.send(200, "application/json", out);
}

// =============================================================================
// OTA firmware update (works on AP mode or STA — any device on the same WiFi)
// =============================================================================
// GET /update — serve a minimal upload form
void handleUpdatePage() {
  static const char OTA_HTML[] PROGMEM =
    "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<body style='font-family:system-ui;background:#0a0f1c;color:#e6edf3;padding:24px'>"
    "<h2>Beehive Keeper - Firmware Update</h2>"
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<p>Select the compiled <b>.bin</b> file:</p>"
    "<input type='file' name='firmware' accept='.bin'><br><br>"
    "<input type='submit' value='Upload &amp; Flash' "
    "style='padding:10px 18px;background:#7CC7FF;border:0;border-radius:8px;font-weight:600'>"
    "</form><p style='color:#7e8aa1;font-size:13px'>Device flashes the new firmware "
    "and reboots automatically. Do not power off during upload.</p></body>";
  server.send_P(200, "text/html", OTA_HTML);
}

// POST /update upload handler — receives the .bin in chunks and writes to flash
void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    Serial.printf("OTA start: %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
    esp_task_wdt_reset();   // OTA upload blocks loop(); keep watchdog fed
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) Serial.printf("OTA done: %u bytes\n", up.totalSize);
    else Update.printError(Serial);
  }
}

void handleId() {
  uint8_t mac_sta[6], mac_ap[6];
  esp_wifi_get_mac(WIFI_IF_STA, mac_sta);
  esp_wifi_get_mac(WIFI_IF_AP, mac_ap);
  char sta[18], ap[18];
  snprintf(sta, sizeof(sta), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac_sta[0],mac_sta[1],mac_sta[2],mac_sta[3],mac_sta[4],mac_sta[5]);
  snprintf(ap, sizeof(ap), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac_ap[0],mac_ap[1],mac_ap[2],mac_ap[3],mac_ap[4],mac_ap[5]);
  String json = String("{\"hostname\":\"") + WiFi.getHostname() + "\","
              + "\"sta_mac\":\"" + sta + "\","
              + "\"ap_mac\":\"" + ap + "\","
              + "\"sd\":" + (sdAvailable ? "true" : "false") + ","
              + "\"rtc\":" + (rtcAvailable ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

// =============================================================================
// WiFi management
// =============================================================================
// Load credentials from NVS, fall back to compiled-in defaults if none stored.
// activeSSID/activePASS are what we actually try to join with.
void loadWiFiCreds() {
  wifiPrefs.begin("wifi", true);  // read-only
  activeSSID = wifiPrefs.getString("ssid", "");
  activePASS = wifiPrefs.getString("pass", "");
  wifiPrefs.end();
  if (activeSSID.length() == 0) {
    activeSSID = String(STA_SSID);
    activePASS = String(STA_PASS);
    logmsg("WiFi: using compiled defaults (%s)", activeSSID.c_str());
  } else {
    logmsg("WiFi: using saved creds (%s)", activeSSID.c_str());
  }
}

void connectWiFi() {
  loadWiFiCreds();
#if FORCE_AP_MODE
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  logmsg("AP forced. IP: %s", WiFi.softAPIP().toString().c_str());
  return;
#else
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("Beehive-ESP32");
  WiFi.begin(activeSSID.c_str(), activePASS.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    setLED(true); delay(250);
    setLED(false); delay(250);
    esp_task_wdt_reset();
  }
  if (WiFi.status() == WL_CONNECTED) {
    setLED(true);
    logmsg("STA connected. IP: %s", WiFi.localIP().toString().c_str());
  } else {
    setLED(false);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    logmsg("AP started. IP: %s", WiFi.softAPIP().toString().c_str());
  }
#endif
}

// Retry STA from AP mode at this interval. If the device boots out of range
// of the STA network and falls back to AP, this lets it recover automatically
// when WiFi comes back, without needing a power-cycle.
const unsigned long AP_RETRY_STA_MS = 10UL * 60UL * 1000UL;  // 10 min
unsigned long lastApRetryMs = 0;

void checkWiFi() {
  unsigned long now = millis();
#if FORCE_AP_MODE
  return;  // user explicitly forced AP; never try STA
#endif
  if (WiFi.getMode() == WIFI_AP) {
    if (now - lastApRetryMs < AP_RETRY_STA_MS) return;
    lastApRetryMs = now;
    logmsg("AP fallback: attempting STA reconnect...");
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(activeSSID.c_str(), activePASS.c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
      delay(250);
      esp_task_wdt_reset();
    }
    if (WiFi.status() == WL_CONNECTED) {
      setLED(true);
      logmsg("STA reconnect OK: %s", WiFi.localIP().toString().c_str());
    } else {
      WiFi.mode(WIFI_AP);
      WiFi.softAP(AP_SSID, AP_PASS);
      logmsg("STA retry failed, back to AP");
    }
    return;
  }
  // STA mode: reconnect if disconnected
  if (WiFi.status() != WL_CONNECTED) {
    logmsg("WiFi dropped, reconnecting...");
    WiFi.disconnect();
    WiFi.begin(activeSSID.c_str(), activePASS.c_str());
  }
}

// =============================================================================
// Setup / Loop
// =============================================================================
void setup() {
  pinMode(LED_PIN, OUTPUT);
  setLED(false);
  Serial.begin(115200);
  delay(300);
  logmsg("=== Beehive Keeper v3.1 boot ===");

  // Watchdog (ESP32 core 3.x API)
  esp_task_wdt_config_t wdt_cfg = {
    .timeout_ms = WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  esp_task_wdt_reconfigure(&wdt_cfg);
  esp_task_wdt_add(NULL);

  // Sensors
  ds18b20.begin();
  Serial.print("DS18B20 devices found: ");
  Serial.println(ds18b20.getDeviceCount());
  dht.begin();
  setupI2S();

  // Battery ADC via ESP-IDF v5 oneshot API (legacy analog* APIs crash on boot
  // due to driver_ng vs legacy conflict; oneshot avoids that entirely).
  initBatteryADC();
#if USE_CHRG_PIN
  pinMode(CHRG_PIN, INPUT_PULLUP);
  Serial.printf("CHG pin %d: %s at boot\n", CHRG_PIN,
                digitalRead(CHRG_PIN) == LOW ? "charging" : "not_charging");
#endif

  // I2C / RTC
  Wire.begin(21, 22);
  if (rtc.begin()) {
    rtcAvailable = true;
    Serial.println("RTC OK");
    if (rtc.lostPower()) {
      Serial.println("RTC lost power, will sync via NTP when WiFi up.");
    }
  } else {
    Serial.println("RTC NOT FOUND");
  }

  // SD card
  initSD();

#if WIPE_OLD_DATA
  if (sdAvailable) {
    Serial.println("WIPE_OLD_DATA=1: deleting /hive_log.csv and audio_*.wav...");
    SD.remove(LOG_FILE);
    File dir = SD.open("/");
    if (dir && dir.isDirectory()) {
      File e;
      int wiped = 0;
      while ((e = dir.openNextFile())) {
        String n = e.name();
        e.close();
        if (n.startsWith("audio_") && n.endsWith(".wav")) {
          String path = String("/") + n;
          if (SD.remove(path)) wiped++;
        }
      }
      dir.close();
      Serial.printf("Wiped %d audio files. Set WIPE_OLD_DATA back to 0 next flash.\n", wiped);
    }
  }
#endif

  // WiFi
  connectWiFi();

  // NTP sync
  if (WiFi.status() == WL_CONNECTED) syncNTP();

  // Web server
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/readnow", handleReadNow);
  server.on("/recent", handleRecent);
  server.on("/audiolist", handleAudioList);
  server.on("/audio", handleAudioFile);
  server.on("/download", handleDownload);
  server.on("/id", handleId);
  server.on("/record", handleRecord);
  server.on("/log", handleLog);
  server.on("/wipe", HTTP_POST, handleWipe);
  server.on("/wifi", HTTP_POST, handleSetWifi);
  // OTA: GET serves the upload form, POST receives + flashes the .bin
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, []() {
    bool ok = !Update.hasError();
    server.send(200, "text/plain", ok ? "OK - rebooting with new firmware" : "FAILED");
    delay(800);
    ESP.restart();
  }, handleUpdateUpload);
  server.begin();
  Serial.println("Web server started");

  // First sensor read happens immediately so the dashboard isn't blank.
  readSensorsOnce();
  bootMs = millis();
  lastSensorReadMs = bootMs;
  // First full cycle (sensor + log + audio) fires once at t = 30s after boot,
  // handled in loop() via firstCycleFired flag.
}

void loop() {
  esp_task_wdt_reset();
  server.handleClient();

  unsigned long now = millis();

  // First-cycle bundle: fire sensor read + SD log + audio clip once at 30s
  // after boot. After this the normal cadences take over.
  if (!firstCycleFired && (now - bootMs) >= FIRST_CYCLE_DELAY_MS) {
    Serial.println("First cycle (30s after boot): sensor + log + audio");
    readSensorsOnce();
    logToSD();
    recordAudioClip();
    lastSensorReadMs = now;
    lastLogMs        = now;
    lastAudioMs      = now;
    firstCycleFired  = true;
  }

  // Periodic sensor read (every 60s)
  if (now - lastSensorReadMs >= SENSOR_INTERVAL_MS) {
    readSensorsOnce();
    lastSensorReadMs = now;
  }

  // SD log (every 15 min); also retry init if card was inserted after boot
  retrySDIfNeeded();
  if (now - lastLogMs >= LOG_INTERVAL_MS) {
    logToSD();
    lastLogMs = now;
  }

  // Hourly audio clip (3s WAV to /audio on SD)
  if (now - lastAudioMs >= AUDIO_INTERVAL_MS) {
    recordAudioClip();
    lastAudioMs = now;
  }

  // WiFi check (every 60s)
  if (now - lastWifiCheckMs >= WIFI_CHECK_MS) {
    checkWiFi();
    lastWifiCheckMs = now;
  }

  // NTP resync (every 6 hr)
  if (now - lastNtpSyncMs >= NTP_RESYNC_MS && WiFi.status() == WL_CONNECTED) {
    syncNTP();
  }

  delay(10);
}
