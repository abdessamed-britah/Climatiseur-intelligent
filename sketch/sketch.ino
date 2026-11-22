/*
 * =====================================================================
 *  SMART AC CONTROLLER  -  ESP32 + DHT11/DHT22 + Relay + MQTT + Node-RED
 * =====================================================================
 *  IoT module project
 *
 *  What it does
 *  ------------
 *  - Reads temperature & humidity from a DHT sensor every 2 s
 *  - AUTO mode  : turns the AC (relay) ON when T > 30 °C and OFF when
 *                 T < 29 °C (1 °C hysteresis -> no rapid on/off "chattering")
 *  - MANUAL mode: AC is controlled from the Node-RED dashboard or the
 *                 push button on the board
 *  - Publishes JSON telemetry over MQTT, receives commands over MQTT
 *  - Threshold (setpoint) can be changed live from the dashboard and is
 *    saved in flash (NVS), so it survives a reboot
 *  - Last-Will "offline" message -> dashboard knows if the ESP32 is down
 *  - Fail-safe: if the sensor fails in AUTO mode, the AC is switched OFF
 *  - Compressor protection: minimum 5 s between automatic switchings
 *  - Local feedback: LCD 16x2 (I2C), LEDs and buzzer
 *
 *  Wiring (see diagram.json)
 *  -------------------------
 *   DHT  data   -> GPIO 15
 *   Relay IN    -> GPIO 26   (relay contact drives the blue "AC" LED = AC load)
 *   Red LED     -> GPIO 27   (over-temperature alarm)
 *   Green LED   -> GPIO 25   (MQTT connected)
 *   Buzzer      -> GPIO 14
 *   Push button -> GPIO 13   (toggle AC manually)
 *   LCD I2C     -> SDA 21, SCL 22
 * =====================================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <DHTesp.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>

// --------------------------- SENSOR CHOICE ---------------------------
// Wokwi only provides the DHT22 part. DHT11 and DHT22 have the same pinout
// and protocol, so on REAL hardware with a DHT11 just set USE_DHT11 to 1.
#define USE_DHT11 0

// ------------------------------ NETWORK ------------------------------
const char* WIFI_SSID     = "Wokwi-GUEST";   // Wokwi virtual WiFi
const char* WIFI_PASSWORD = "";

const char* MQTT_BROKER = "broker.hivemq.com"; // free public broker
const uint16_t MQTT_PORT = 1883;

// !!! CHANGE "student01" to your own name (same in the Node-RED flow) !!!
// Everyone on the public broker shares topics, so make yours unique.
#define TOPIC_ROOT "iot2026/smartac/abdessamed"

const char* T_TELEMETRY = TOPIC_ROOT "/telemetry";      // ESP32 -> JSON data
const char* T_STATUS    = TOPIC_ROOT "/status";         // online / offline (LWT)
const char* T_AC_STATE  = TOPIC_ROOT "/ac/state";       // ON / OFF (retained)
const char* T_EVENT     = TOPIC_ROOT "/event";          // JSON event log
const char* T_CMD_AC    = TOPIC_ROOT "/cmd/ac";         // dashboard -> ON / OFF
const char* T_CMD_MODE  = TOPIC_ROOT "/cmd/mode";       // dashboard -> AUTO / MANUAL
const char* T_CMD_SP    = TOPIC_ROOT "/cmd/setpoint";   // dashboard -> number (°C)
const char* T_CMD_ALL   = TOPIC_ROOT "/cmd/#";

// ------------------------------- PINS --------------------------------
const int PIN_DHT    = 15;
const int PIN_RELAY  = 26;
const int PIN_LED_ALARM = 27;
const int PIN_LED_MQTT  = 25;
const int PIN_BUZZER = 14;
const int PIN_BUTTON = 13;

// ---------------------------- PARAMETERS -----------------------------
const float DEFAULT_SETPOINT = 30.0;   // °C  -> the ">30 °C" requirement
const float HYSTERESIS       = 1.0;    // °C  -> AC OFF below 29 °C
const float SP_MIN = 16.0, SP_MAX = 40.0;

const unsigned long SENSOR_PERIOD_MS = 2000;  // DHT22 needs >= 2 s
const unsigned long MIN_SWITCH_MS    = 5000;  // compressor protection (3 min on a real AC)
const unsigned long RECONNECT_MS     = 5000;

// ------------------------------ OBJECTS ------------------------------
WiFiClient        wifiClient;
PubSubClient      mqtt(wifiClient);
DHTesp            dht;
LiquidCrystal_I2C lcd(0x27, 16, 2);
Preferences       prefs;

// ------------------------------- STATE -------------------------------
enum Mode { MODE_AUTO, MODE_MANUAL };
Mode  mode      = MODE_AUTO;
bool  acOn      = false;
float setpoint  = DEFAULT_SETPOINT;
float lastTemp  = NAN, lastHum = NAN, lastHeatIndex = NAN;
bool  sensorOk  = false;

unsigned long lastSensorRead   = 0;
unsigned long lastSwitchTime   = 0;
unsigned long lastReconnectTry = 0;
unsigned long acOnSince        = 0;
unsigned long acRuntimeMs      = 0;   // total time the AC has been ON
unsigned long switchCount      = 0;

// button debounce
int  lastButtonReading = HIGH;
int  buttonState       = HIGH;
unsigned long lastDebounce = 0;

// =====================================================================
//                               HELPERS
// =====================================================================
const char* modeName() { return mode == MODE_AUTO ? "AUTO" : "MANUAL"; }

unsigned long acRuntimeSeconds() {
  unsigned long ms = acRuntimeMs;
  if (acOn) ms += millis() - acOnSince;
  return ms / 1000;
}

void beep(int freq, int ms) {
  tone(PIN_BUZZER, freq, ms);
}

void publishEvent(const char* type, const char* detail) {
  if (!mqtt.connected()) return;
  char buf[200];
  snprintf(buf, sizeof(buf),
           "{\"type\":\"%s\",\"detail\":\"%s\",\"ac\":%s,\"mode\":\"%s\",\"temperature\":%.1f}",
           type, detail, acOn ? "true" : "false", modeName(),
           isnan(lastTemp) ? -99.0 : lastTemp);
  mqtt.publish(T_EVENT, buf);
  Serial.printf("[EVENT] %s - %s\n", type, detail);
}

void saveSettings() {
  prefs.putFloat("sp", setpoint);
  prefs.putUChar("mode", (uint8_t)mode);
}

// =====================================================================
//                         ACTUATOR (AC RELAY)
// =====================================================================
void setAC(bool on, const char* reason) {
  if (on == acOn) return;

  acOn = on;
  digitalWrite(PIN_RELAY, on ? HIGH : LOW);
  lastSwitchTime = millis();
  switchCount++;

  if (on) {
    acOnSince = millis();
    beep(1500, 120);
  } else {
    acRuntimeMs += millis() - acOnSince;
    beep(800, 120);
  }

  Serial.printf("[AC] %s  (%s)\n", on ? "ON" : "OFF", reason);
  if (mqtt.connected()) {
    mqtt.publish(T_AC_STATE, on ? "ON" : "OFF", true);   // retained
  }
  publishEvent(on ? "AC_ON" : "AC_OFF", reason);
}

void setMode(Mode m, const char* reason) {
  if (m == mode) return;
  mode = m;
  saveSettings();
  Serial.printf("[MODE] %s (%s)\n", modeName(), reason);
  publishEvent("MODE", modeName());
}

// =====================================================================
//                   AUTOMATIC CONTROL WITH HYSTERESIS
// =====================================================================
//    T > setpoint               -> AC ON
//    T < setpoint - hysteresis  -> AC OFF
//    between the two            -> keep current state (dead band)
void autoControl() {
  if (mode != MODE_AUTO) return;

  // Fail-safe: no valid reading -> do not run the AC blindly
  if (!sensorOk) {
    setAC(false, "fail-safe: sensor error");
    return;
  }

  // Protect the compressor from too frequent switching
  if (millis() - lastSwitchTime < MIN_SWITCH_MS && lastSwitchTime != 0) return;

  if (!acOn && lastTemp > setpoint) {
    char why[64];
    snprintf(why, sizeof(why), "auto: %.1fC > %.1fC", lastTemp, setpoint);
    setAC(true, why);
  } else if (acOn && lastTemp < setpoint - HYSTERESIS) {
    char why[64];
    snprintf(why, sizeof(why), "auto: %.1fC < %.1fC", lastTemp, setpoint - HYSTERESIS);
    setAC(false, why);
  }
}

// =====================================================================
//                                SENSOR
// =====================================================================
void readSensor() {
  TempAndHumidity v = dht.getTempAndHumidity();

  if (dht.getStatus() != DHTesp::ERROR_NONE || isnan(v.temperature) || isnan(v.humidity)) {
    if (sensorOk) publishEvent("SENSOR_ERROR", dht.getStatusString());
    sensorOk = false;
    Serial.printf("[DHT] read error: %s\n", dht.getStatusString());
    return;
  }

  sensorOk      = true;
  lastTemp      = v.temperature;
  lastHum       = v.humidity;
  lastHeatIndex = dht.computeHeatIndex(v.temperature, v.humidity, false);
}

// =====================================================================
//                              TELEMETRY
// =====================================================================
void publishTelemetry() {
  if (!mqtt.connected()) return;

  char buf[320];
  if (sensorOk) {
    snprintf(buf, sizeof(buf),
      "{\"temperature\":%.1f,\"humidity\":%.1f,\"heatIndex\":%.1f,"
      "\"ac\":%s,\"mode\":\"%s\",\"setpoint\":%.1f,\"hysteresis\":%.1f,"
      "\"alarm\":%s,\"sensorOk\":true,\"acRuntime\":%lu,\"switches\":%lu,"
      "\"rssi\":%d,\"uptime\":%lu}",
      lastTemp, lastHum, lastHeatIndex,
      acOn ? "true" : "false", modeName(), setpoint, HYSTERESIS,
      lastTemp > setpoint ? "true" : "false",
      acRuntimeSeconds(), switchCount, WiFi.RSSI(), millis() / 1000);
  } else {
    snprintf(buf, sizeof(buf),
      "{\"ac\":%s,\"mode\":\"%s\",\"setpoint\":%.1f,\"hysteresis\":%.1f,"
      "\"alarm\":false,\"sensorOk\":false,\"acRuntime\":%lu,\"switches\":%lu,"
      "\"rssi\":%d,\"uptime\":%lu}",
      acOn ? "true" : "false", modeName(), setpoint, HYSTERESIS,
      acRuntimeSeconds(), switchCount, WiFi.RSSI(), millis() / 1000);
  }

  mqtt.publish(T_TELEMETRY, buf);
  Serial.printf("[MQTT] -> %s\n", buf);
}

// =====================================================================
//                              LCD + LEDS
// =====================================================================
void updateLocalUI() {
  char line[17];

  lcd.setCursor(0, 0);
  if (sensorOk) {
    snprintf(line, sizeof(line), "T:%4.1f%cC H:%2.0f%% ", lastTemp, (char)223, lastHum);
  } else {
    snprintf(line, sizeof(line), "SENSOR ERROR!   ");
  }
  lcd.print(line);
  lcd.setCursor(15, 0);
  lcd.print(mqtt.connected() ? '*' : '!');   // * = cloud OK, ! = offline

  lcd.setCursor(0, 1);
  snprintf(line, sizeof(line), "AC:%-3s %-4s %4.1f",
           acOn ? "ON" : "OFF", mode == MODE_AUTO ? "AUTO" : "MAN", setpoint);
  lcd.print(line);

  digitalWrite(PIN_LED_ALARM, (sensorOk && lastTemp > setpoint) ? HIGH : LOW);
  digitalWrite(PIN_LED_MQTT, mqtt.connected() ? HIGH : LOW);
}

// =====================================================================
//                         MQTT COMMAND HANDLER
// =====================================================================
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  char msg[32];
  unsigned int n = min(length, (unsigned int)sizeof(msg) - 1);
  memcpy(msg, payload, n);
  msg[n] = '\0';
  String cmd = String(msg);
  cmd.trim();
  cmd.toUpperCase();

  Serial.printf("[MQTT] <- %s : %s\n", topic, msg);

  if (strcmp(topic, T_CMD_MODE) == 0) {
    if (cmd == "AUTO")        setMode(MODE_AUTO, "dashboard");
    else if (cmd == "MANUAL") setMode(MODE_MANUAL, "dashboard");
    autoControl();   // apply the rule immediately when back in AUTO
  }
  else if (strcmp(topic, T_CMD_AC) == 0) {
    // A manual ON/OFF order automatically switches to MANUAL mode,
    // otherwise the automatic rule would immediately override it.
    if (cmd == "ON" || cmd == "1" || cmd == "TRUE") {
      setMode(MODE_MANUAL, "manual AC command");
      setAC(true, "dashboard command");
    } else if (cmd == "OFF" || cmd == "0" || cmd == "FALSE") {
      setMode(MODE_MANUAL, "manual AC command");
      setAC(false, "dashboard command");
    }
  }
  else if (strcmp(topic, T_CMD_SP) == 0) {
    float v = atof(msg);
    if (v >= SP_MIN && v <= SP_MAX) {
      setpoint = v;
      saveSettings();
      char d[32];
      snprintf(d, sizeof(d), "setpoint %.1fC", setpoint);
      publishEvent("SETPOINT", d);
      autoControl();
    } else {
      publishEvent("REJECTED", "setpoint out of range 16-40");
    }
  }
  publishTelemetry();   // instant feedback to the dashboard
  updateLocalUI();
}

// =====================================================================
//                        WIFI / MQTT CONNECTION
// =====================================================================
void connectWiFi() {
  lcd.clear();
  lcd.print("WiFi connecting");
  Serial.print("[WiFi] Connecting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 6);   // channel 6 = faster on Wokwi
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.printf("\n[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
  lcd.clear();
  lcd.print("WiFi OK");
  lcd.setCursor(0, 1);
  lcd.print(WiFi.localIP());
  delay(800);
}

// Non-blocking: tries once every RECONNECT_MS, the control loop keeps running
// (the AC keeps working in AUTO mode even if the internet is down).
void maintainMqtt() {
  if (mqtt.connected()) return;
  if (millis() - lastReconnectTry < RECONNECT_MS && lastReconnectTry != 0) return;
  lastReconnectTry = millis();

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    return;
  }

  String clientId = "esp32-smartac-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.printf("[MQTT] Connecting to %s as %s ... ", MQTT_BROKER, clientId.c_str());

  // Last Will: the broker publishes "offline" if the ESP32 disappears
  if (mqtt.connect(clientId.c_str(), T_STATUS, 1, true, "offline")) {
    Serial.println("OK");
    mqtt.publish(T_STATUS, "online", true);
    mqtt.publish(T_AC_STATE, acOn ? "ON" : "OFF", true);
    mqtt.subscribe(T_CMD_ALL, 1);
    publishEvent("BOOT", "device connected");
    publishTelemetry();
  } else {
    Serial.printf("failed, rc=%d\n", mqtt.state());
  }
}

// =====================================================================
//                            PUSH BUTTON
// =====================================================================
void handleButton() {
  int reading = digitalRead(PIN_BUTTON);
  if (reading != lastButtonReading) lastDebounce = millis();

  if (millis() - lastDebounce > 50 && reading != buttonState) {
    buttonState = reading;
    if (buttonState == LOW) {           // pressed
      setMode(MODE_MANUAL, "push button");
      setAC(!acOn, "local push button");
      publishTelemetry();
      updateLocalUI();
    }
  }
  lastButtonReading = reading;
}

// =====================================================================
//                                SETUP
// =====================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== SMART AC CONTROLLER ===");

  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_LED_ALARM, OUTPUT);
  pinMode(PIN_LED_MQTT, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  digitalWrite(PIN_RELAY, LOW);           // AC OFF at boot (safe state)

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print(" SMART  AC  IoT ");
  lcd.setCursor(0, 1); lcd.print("ESP32 MQTT NodeR");

#if USE_DHT11
  dht.setup(PIN_DHT, DHTesp::DHT11);
#else
  dht.setup(PIN_DHT, DHTesp::DHT22);
#endif

  // Restore saved settings
  prefs.begin("smartac", false);
  setpoint = prefs.getFloat("sp", DEFAULT_SETPOINT);
  mode     = (Mode)prefs.getUChar("mode", MODE_AUTO);
  Serial.printf("[CFG] setpoint=%.1f mode=%s\n", setpoint, modeName());

  beep(1000, 80);
  delay(1200);

  connectWiFi();

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(30);

  lcd.clear();
}

// =====================================================================
//                                 LOOP
// =====================================================================
void loop() {
  maintainMqtt();
  mqtt.loop();
  handleButton();

  if (millis() - lastSensorRead >= SENSOR_PERIOD_MS) {
    lastSensorRead = millis();
    readSensor();
    autoControl();
    publishTelemetry();
    updateLocalUI();
  }

  // blink the green LED while disconnected from the broker
  if (!mqtt.connected()) {
    digitalWrite(PIN_LED_MQTT, (millis() / 300) % 2);
  }
}
