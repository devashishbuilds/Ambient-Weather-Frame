#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>

// ------------------------- User configuration -------------------------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* LOCATION_QUERY = "YOUR_CITY, YOUR_COUNTRY";

// ----------------------------- Hardware ------------------------------
constexpr uint8_t SERVO_PIN = 10;
constexpr uint8_t LED_PIN = 9;
constexpr uint8_t BOTTOM_SENSOR_PIN = 21;
constexpr uint8_t TOP_SENSOR_PIN = 20;
constexpr uint8_t NUM_LEDS = 10;

constexpr int SERVO_UP_US = 2000;
constexpr int SERVO_DOWN_US = 1000;
constexpr int SERVO_STOP_US = 1500;
constexpr int SERVO_FRAME_US = 20000;  // 50 Hz

// Measured calibration for this mechanism.
constexpr float SENSOR_SPAN_MM = 263.0f;
constexpr float FULL_UP_TIME_MS = 44604.0f;
constexpr float FULL_DOWN_TIME_MS = 47397.0f;
constexpr float UP_MS_PER_MM = FULL_UP_TIME_MS / SENSOR_SPAN_MM;
constexpr float DOWN_MS_PER_MM = FULL_DOWN_TIME_MS / SENSOR_SPAN_MM;

constexpr float SENSOR_BACKOFF_MM = 5.0f;
constexpr float NORMAL_BOTTOM_MM = 5.0f;
constexpr float NORMAL_TOP_MM = SENSOR_SPAN_MM - SENSOR_BACKOFF_MM;
constexpr float MOVE_DEADBAND_MM = 1.5f;

constexpr int RAIN_THRESHOLD_PERCENT = 40;
constexpr unsigned long POSITION_UPDATE_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long WEATHER_UPDATE_MS = 3UL * 60UL * 60UL * 1000UL;

// ----------------------------- State ---------------------------------
Adafruit_NeoPixel sun(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
WebServer server(80);

float latitude = NAN;
float longitude = NAN;
float positionMM = NORMAL_BOTTOM_MM;
float targetMM = NORMAL_BOTTOM_MM;

time_t sunriseUTC = 0;
time_t sunsetUTC = 0;
int rainProbability = 0;
int weatherCode = -1;
bool thunderstormExpected = false;
bool homed = false;

unsigned long lastPositionUpdate = 0;
unsigned long lastWeatherUpdate = 0;

// --------------------------- Sensor I/O -------------------------------
bool bottomTriggered() { return digitalRead(BOTTOM_SENSOR_PIN) == HIGH; }
bool topTriggered() { return digitalRead(TOP_SENSOR_PIN) == HIGH; }

// ---------------------------- LEDs -----------------------------------
void setSun(bool on) {
  for (uint8_t i = 0; i < NUM_LEDS; ++i)
    sun.setPixelColor(i, on ? sun.Color(255, 120, 10) : 0);
  sun.setBrightness(100);
  sun.show();
}

// ---------------------------- Servo ----------------------------------
// Continuous-rotation servo pulse output without the Servo library.
void servoPulse(int pulseUS) {
  digitalWrite(SERVO_PIN, HIGH);
  delayMicroseconds(pulseUS);
  digitalWrite(SERVO_PIN, LOW);
  delayMicroseconds(SERVO_FRAME_US - pulseUS);
}

void stopServo() {
  for (int i = 0; i < 3; ++i) servoPulse(SERVO_STOP_US);
}

// Runs until the requested duration expires or a physical endpoint is hit.
// Position is updated from calibrated runtime; endpoint sensors override it.
void moveTimed(bool up, unsigned long durationMS) {
  const unsigned long start = millis();
  const unsigned long hardLimit = durationMS + 2000;

  while (millis() - start < durationMS && millis() - start < hardLimit) {
    if (up && topTriggered()) {
      stopServo();
      positionMM = SENSOR_SPAN_MM;
      return;
    }
    if (!up && bottomTriggered()) {
      stopServo();
      positionMM = 0.0f;
      return;
    }
    servoPulse(up ? SERVO_UP_US : SERVO_DOWN_US);
  }

  stopServo();
  const float elapsed = millis() - start;
  positionMM += up ? elapsed / UP_MS_PER_MM : -elapsed / DOWN_MS_PER_MM;
  positionMM = constrain(positionMM, 0.0f, SENSOR_SPAN_MM);
}

void backOffEndpoint() {
  if (positionMM <= 0.1f) {
    moveTimed(true, SENSOR_BACKOFF_MM * UP_MS_PER_MM);
    positionMM = NORMAL_BOTTOM_MM;
  } else if (positionMM >= SENSOR_SPAN_MM - 0.1f) {
    moveTimed(false, SENSOR_BACKOFF_MM * DOWN_MS_PER_MM);
    positionMM = NORMAL_TOP_MM;
  }
}

bool homeMechanism() {
  const unsigned long timeout = millis() + 75000UL;

  while (!bottomTriggered() && millis() < timeout)
    servoPulse(SERVO_DOWN_US);

  stopServo();
  if (!bottomTriggered()) return false;

  positionMM = 0.0f;
  backOffEndpoint();
  homed = true;
  return true;
}

void moveTo(float requestedMM) {
  if (!homed) return;

  requestedMM = constrain(requestedMM, NORMAL_BOTTOM_MM, NORMAL_TOP_MM);
  float delta = requestedMM - positionMM;
  if (fabs(delta) <= MOVE_DEADBAND_MM) return;

  const bool up = delta > 0;
  const unsigned long duration = fabs(delta) * (up ? UP_MS_PER_MM : DOWN_MS_PER_MM);
  moveTimed(up, duration);

  // If a sensor corrected our position, release it and optionally finish the move.
  if (bottomTriggered() || topTriggered()) {
    backOffEndpoint();
    delta = requestedMM - positionMM;
    if (fabs(delta) > MOVE_DEADBAND_MM) {
      const bool secondUp = delta > 0;
      moveTimed(secondUp, fabs(delta) * (secondUp ? UP_MS_PER_MM : DOWN_MS_PER_MM));
    }
  }
}

// ------------------------- Network helpers ----------------------------
String urlEncode(const String& text) {
  String out;
  char buf[4];
  for (char c : text) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += c;
    else {
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      out += buf;
    }
  }
  return out;
}

bool httpsGetJson(const String& url, JsonDocument& doc) {
  WiFiClientSecure client;
  client.setInsecure(); // Convenient for prototypes; validate CA certificates in production.

  HTTPClient http;
  if (!http.begin(client, url)) return false;
  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();
  return !error;
}

bool geocodeLocation() {
  JsonDocument doc;
  String url = "https://geocoding-api.open-meteo.com/v1/search?name=" +
               urlEncode(LOCATION_QUERY) + "&count=1&language=en&format=json";

  if (!httpsGetJson(url, doc) || !doc["results"][0]) return false;
  latitude = doc["results"][0]["latitude"].as<float>();
  longitude = doc["results"][0]["longitude"].as<float>();
  return true;
}

bool fetchWeather() {
  if (isnan(latitude) || isnan(longitude)) return false;

  JsonDocument doc;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(latitude, 6) +
               "&longitude=" + String(longitude, 6) +
               "&daily=sunrise,sunset,precipitation_probability_max,weather_code"
               "&timezone=auto&timeformat=unixtime&forecast_days=1";

  if (!httpsGetJson(url, doc)) return false;

  sunriseUTC = doc["daily"]["sunrise"][0].as<time_t>();
  sunsetUTC = doc["daily"]["sunset"][0].as<time_t>();
  rainProbability = doc["daily"]["precipitation_probability_max"][0] | 0;
  weatherCode = doc["daily"]["weather_code"][0] | -1;
  thunderstormExpected = weatherCode == 95 || weatherCode == 96 || weatherCode == 99;
  lastWeatherUpdate = millis();
  return sunriseUTC > 0 && sunsetUTC > sunriseUTC;
}

// ------------------------ Position calculation ------------------------
float calculateTarget(time_t now) {
  if (!sunriseUTC || !sunsetUTC || now <= sunriseUTC || now >= sunsetUTC)
    return NORMAL_BOTTOM_MM;

  const time_t midpoint = sunriseUTC + (sunsetUTC - sunriseUTC) / 2;

  if (now <= midpoint) {
    const float progress = float(now - sunriseUTC) / float(midpoint - sunriseUTC);
    return NORMAL_BOTTOM_MM + progress * (NORMAL_TOP_MM - NORMAL_BOTTOM_MM);
  }

  const float progress = float(now - midpoint) / float(sunsetUTC - midpoint);
  return NORMAL_TOP_MM - progress * (NORMAL_TOP_MM - NORMAL_BOTTOM_MM);
}

void updateSunPosition() {
  const time_t now = time(nullptr);
  if (now < 100000) return;

  targetMM = calculateTarget(now);
  const bool daylight = now >= sunriseUTC && now < sunsetUTC;
  setSun(daylight);
  moveTo(targetMM);
  lastPositionUpdate = millis();
}

// ----------------------- Minimal web status ---------------------------
void setupWebServer() {
  server.on("/", []() {
    String page = "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<h2>Ambient Weather Frame</h2>"
                  "<p>Position: " + String(positionMM, 1) + " mm</p>"
                  "<p>Target: " + String(targetMM, 1) + " mm</p>"
                  "<p>Rain probability: " + String(rainProbability) + "%</p>"
                  "<p>Rain expected: " + String(rainProbability >= RAIN_THRESHOLD_PERCENT ? "yes" : "no") + "</p>"
                  "<p>Thunderstorm: " + String(thunderstormExpected ? "yes" : "no") + "</p>";
    server.send(200, "text/html", page);
  });
  server.begin();
}

// ----------------------------- Setup ---------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(SERVO_PIN, OUTPUT);
  pinMode(BOTTOM_SENSOR_PIN, INPUT);
  pinMode(TOP_SENSOR_PIN, INPUT);

  sun.begin();
  setSun(false);
  stopServo();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) delay(250);

  configTime(0, 0, "pool.ntp.org", "time.google.com");
  geocodeLocation();
  fetchWeather();
  setupWebServer();

  if (homeMechanism()) updateSunPosition();
}

// ------------------------------ Loop ---------------------------------
void loop() {
  server.handleClient();

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(100);
    return;
  }

  if (millis() - lastWeatherUpdate >= WEATHER_UPDATE_MS) fetchWeather();
  if (millis() - lastPositionUpdate >= POSITION_UPDATE_MS) updateSunPosition();
}
