#include <WiFi.h>
#include <WebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "driver/i2s.h"
#include "esp_wifi.h"
#include "DHT.h"

// ---------- Wi-Fi (STA then AP fallback) ----------
const char* STA_SSID = "PDS_Guest";   // open network OK
const char* STA_PASS = "";            // leave blank for open network
const char* AP_SSID  = "Beehive-AP";  // AP fallback
const char* AP_PASS  = "beehive123";

// ---------- LED status ----------
#define LED_PIN 2
#define LED_ACTIVE_HIGH 1

// ---------- DS18B20 ----------
#define ONE_WIRE_BUS 4
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);

// ---------- DHT11 (Humidity) ----------
#define DHT_PIN 14
#define DHT_TYPE DHT11
DHT dht(DHT_PIN, DHT_TYPE);

// ---------- SPH0645 (I2S mic) ----------
#define I2S_WS   25
#define I2S_SCK  26
#define I2S_SD   32
#define I2S_PORT I2S_NUM_0
#define SAMPLE_RATE 16000
#define SAMPLES 1024

// ---------- Globals ----------
WebServer server(80);

float currentTempC = NAN;
float currentHum   = NAN;
float currentDbfs  = NAN;
float currentRms   = NAN;

inline void setLED(bool on) {
  digitalWrite(LED_PIN, (LED_ACTIVE_HIGH ? (on ? HIGH : LOW) : (on ? LOW : HIGH)));
}

float dbfsFromRMS(double rms) {
  const double FS = 8388607.0;
  if (rms <= 0.0) return -120.0;
  return 20.0 * log10(rms / FS);
}

void setupI2S() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num = I2S_SCK,
    .ws_io_num  = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_SD
  };
  i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_PORT, &pins);
  i2s_set_clk(I2S_PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_32BIT, I2S_CHANNEL_MONO);
}

String uptimeString() {
  unsigned long ms  = millis();
  unsigned long sec = ms / 1000;
  unsigned long min = sec / 60;
  unsigned long hr  = min / 60;
  char buf[40];
  snprintf(buf, sizeof(buf), "Uptime %luh %lum %lus", hr, min % 60, sec % 60);
  return String(buf);
}

void readSensorsOnce() {
  // Temperature
  ds18b20.requestTemperatures();
  currentTempC = ds18b20.getTempCByIndex(0);

  // Humidity
  currentHum = dht.readHumidity();

  // Sound (RMS and dBFS)
  static int32_t buf[SAMPLES];
  size_t br = 0;
  i2s_read(I2S_PORT, (void*)buf, sizeof(buf), &br, portMAX_DELAY);
  const size_t n = br / sizeof(int32_t);
  double s2 = 0.0;
  for (size_t i = 0; i < n; i++) {
    int32_t s = buf[i] >> 14;
    s2 += (double)s * (double)s;
  }
  const double rms = (n ? sqrt(s2 / n) : 0.0);
  currentRms  = (float)rms;
  currentDbfs = dbfsFromRMS(rms);
}

// ---------- Web UI (15-min refresh) ----------
static const char INDEX_HTML[] PROGMEM = R"RAWHTML(
<!doctype html>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Beehive Dashboard</title>
<style>
:root{--bg:#0b1320;--card:#0f1b32;--ink:#e6edf3;--accent:#7cc7ff;--grid:#223}
body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;background:var(--bg);color:var(--ink)}
header{padding:16px 20px;background:var(--card);border-bottom:1px solid var(--grid)}
h1{margin:0;font-size:18px}
.wrap{max-width:960px;margin:24px auto;padding:0 16px}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:16px}
.card{background:var(--card);border:1px solid var(--grid);border-radius:14px;padding:16px}
.label{opacity:.75;font-size:12px}
.value{font-size:28px;font-weight:700;margin:6px 0 10px}
.pill{display:inline-block;background:#11223a;border:1px solid #223;border-radius:999px;padding:6px 10px;font-size:12px;opacity:.85;margin-right:6px}
button{display:inline-block;background:#1b2b4b;color:#e6edf3;border:1px solid #223;border-radius:10px;padding:8px 12px;text-decoration:none;margin-right:8px;cursor:pointer}
.small{font-size:12px;opacity:.8}
</style>
<header><h1>Beehive Health — Live View (15-min refresh)</h1></header>
<div class="wrap">
  <div class="cards">
    <div class="card">
      <div class="label">Last Update</div>
      <div id="ts" class="value">—</div>
      <div class="pill" id="ip">IP: </div>
      <div class="small">Page auto refresh every 15 minutes. Use Capture Now for an instant refresh.</div>
    </div>

    <div class="card">
      <div class="label">Temperature (°C / °F)</div>
      <div id="tC" class="value">—</div>
      <div id="tF" class="value" style="font-size:22px">—</div>
    </div>

    <div class="card">
      <div class="label">Humidity (%)</div>
      <div id="hum" class="value">—</div>
    </div>

    <div class="card">
      <div class="label">Sound (dBFS) / RMS</div>
      <div id="db" class="value">—</div>
      <div id="rms" class="value" style="font-size:20px">—</div>
    </div>

    <div class="card">
      <div class="label">Power and Battery</div>
      <div class="value" id="battStatus" style="font-size:20px">
        Battery monitoring hardware not wired yet.
      </div>
      <div class="small">
        This placeholder will later show battery voltage, approximate percentage, and whether solar is charging.
      </div>
    </div>

    <div class="card">
      <div class="label">Controls</div>
      <button onclick="captureNow()">Capture Now</button>
    </div>
  </div>
</div>
<script>
(function(){
  function get(url, cb){
    var x=new XMLHttpRequest();
    x.open('GET', url, true);
    x.onreadystatechange=function(){
      if(x.readyState===4 && x.status===200){ cb(JSON.parse(x.responseText)); }
    };
    x.send();
  }

  function tick(){
    get('/data', function(j){
      document.getElementById('ip').textContent = 'IP: '+location.host;

      var ts  = document.getElementById('ts');
      var tC  = document.getElementById('tC');
      var tF  = document.getElementById('tF');
      var hum = document.getElementById('hum');
      var db  = document.getElementById('db');
      var rms = document.getElementById('rms');

      ts.textContent = j.timestamp || '—';

      if(typeof j.temp === 'number'){
        tC.textContent = j.temp.toFixed(2);
        tF.textContent = (j.temp*9/5+32).toFixed(2);
      } else {
        tC.textContent = '—';
        tF.textContent = '—';
      }

      hum.textContent = (typeof j.hum === 'number') ? j.hum.toFixed(1) : '—';
      db.textContent  = (typeof j.sound === 'number') ? j.sound.toFixed(1) : '—';
      rms.textContent = (typeof j.rms === 'number') ? Math.round(j.rms) : '—';
    });
  }

  window.captureNow = function(){
    // For this simplified version, just call /data again after a short delay
    setTimeout(tick, 500);
  };

  tick();
  setInterval(tick, 900000); // 15 minutes
})();
</script>
)RAWHTML";

// ---------- HTTP Handlers ----------
void handleRoot() {
  server.send(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleData() {
  readSensorsOnce();

  String out = "{";
  out += "\"timestamp\":\"" + uptimeString() + "\",";
  out += "\"temp\":";
  if (isnan(currentTempC)) out += "null"; else out += String(currentTempC, 2);
  out += ",\"hum\":";
  if (isnan(currentHum)) out += "null"; else out += String(currentHum, 1);
  out += ",\"sound\":";
  if (isnan(currentDbfs)) out += "null"; else out += String(currentDbfs, 1);
  out += ",\"rms\":";
  if (isnan(currentRms)) out += "null"; else out += String(currentRms, 2);
  out += "}";
  server.send(200, "application/json", out);
}

void handleId() {
  uint8_t mac_sta[6], mac_ap[6];
  esp_wifi_get_mac(WIFI_IF_STA, mac_sta);
  esp_wifi_get_mac(WIFI_IF_AP,  mac_ap);
  char sta[18], ap[18];
  snprintf(sta, sizeof(sta), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac_sta[0],mac_sta[1],mac_sta[2],mac_sta[3],mac_sta[4],mac_sta[5]);
  snprintf(ap,  sizeof(ap),  "%02X:%02X:%02X:%02X:%02X:%02X",
           mac_ap[0],mac_ap[1],mac_ap[2],mac_ap[3],mac_ap[4],mac_ap[5]);
  String json = String("{\"hostname\":\"") + WiFi.getHostname() + "\","
              + "\"sta_mac\":\"" + sta + "\","
              + "\"ap_mac\":\""  + ap  + "\"}";
  server.send(200, "application/json", json);
}

// ---------- Setup / Loop ----------
void setup() {
  pinMode(LED_PIN, OUTPUT);
  setLED(false);

  Serial.begin(115200);
  delay(300);

  ds18b20.begin();
  dht.begin();
  setupI2S();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("Beehive-ESP32");
  WiFi.begin(STA_SSID, STA_PASS);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
    setLED(true); delay(250);
    setLED(false); delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    setLED(true);
    Serial.print("STA connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    setLED(false);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.print("AP started. IP: ");
    Serial.println(WiFi.softAPIP());
  }

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/id", handleId);
  server.begin();
  Serial.println("Web server started");
}

void loop() {
  server.handleClient();
}