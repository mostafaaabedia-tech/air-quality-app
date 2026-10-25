/*
  Air Quality Monitor  —  ESP32 + MQ2 + MQ135 + LCD I2C + Buzzer + Firebase
  ---------------------------------------------------------------------------
  Talks to the Firebase Realtime Database directly over HTTPS (Firebase REST API).
  NO Firebase library is needed — that library's pre-built files don't work with
  ESP32 board package 3.x and give "bad reloc symbol index" / "undefined reference
  to br_..." errors.

  Libraries:  LiquidCrystal I2C   (everything else is built into the ESP32 package)
  Board:      ESP32 Dev Module

  Firebase data (same paths as before, so the website works unchanged):
    /house/lpg, /house/smoke, /house/co2    numbers, updated every second
    /house/danger                            true while gas levels are dangerous
    /house/silenced                          true after the website silenced the alarm
    /house/updated                           server time of the last update (website shows "offline" if old)
    /commands/silence_alarm                  website writes 1, ESP32 sets it back to 0
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <type_traits>

// ---- WiFi + Firebase ----
#define WIFI_SSID     "YOUR_WIFI_NAME"        // <--- your WiFi name (2.4 GHz)
#define WIFI_PASS     "YOUR_WIFI_PASSWORD"    // <--- your WiFi password
#define FIREBASE_HOST "https://airqualityproject-e49ae-default-rtdb.firebaseio.com"
#define FIREBASE_AUTH ""                      // <--- your database secret ("" if your rules are public)

// ---- Pins ----
#define MQ2_PIN     34
#define MQ135_PIN   35
#define BUZZER_PIN  13

LiquidCrystal_I2C lcd(0x27, 16, 2);           // try 0x3F if the LCD stays blank

// ---- Thresholds ----
const float LPG_DANGER   = 600.0;
const float SMOKE_DANGER = 300.0;
const float CO2_DANGER   = 1500.0;

// ---- Shared between the main loop and the network task ----
volatile float lpg = 0, smoke = 0, co2 = 400;
volatile bool  dangerState = false;     // gas levels are dangerous right now
volatile bool  silenced = false;        // website pressed "Silence" during this danger
volatile bool  silenceRequest = false;  // set by the network task, handled by the loop
volatile bool  online = false;          // last Firebase request worked

unsigned long lastLcdUpdate = 0;
bool lastShownDanger = false;

// Works with both versions of the LiquidCrystal_I2C library (one uses init(), the other begin())
template <typename T> struct HasInit {
  template <typename U> static char test(decltype(&U::init));
  template <typename U> static long test(...);
  static const bool value = sizeof(test<T>(0)) == 1;
};
template <typename T> void lcdStartImpl(T& l, std::true_type)  { l.init(); }
template <typename T> void lcdStartImpl(T& l, std::false_type) { l.begin(); }
template <typename T> void lcdStart(T& l) { lcdStartImpl(l, std::integral_constant<bool, HasInit<T>::value>()); }

// ================= Firebase over HTTPS =================
WiFiClientSecure tls;
HTTPClient http;

String fbUrl(const char* path) {
  String url = String(FIREBASE_HOST) + path + ".json";
  if (strlen(FIREBASE_AUTH) > 0) url += String("?auth=") + FIREBASE_AUTH;
  return url;
}

// method: "PATCH", "PUT" or "GET".  Returns the HTTP code, response in *reply (for GET).
int fbRequest(const char* method, const char* path, const String& body, String* reply = nullptr) {
  if (!http.begin(tls, fbUrl(path))) return -1;
  http.addHeader("Content-Type", "application/json");
  int code;
  if (strcmp(method, "PATCH") == 0)    code = http.PATCH(body);
  else if (strcmp(method, "PUT") == 0) code = http.PUT(body);
  else                                 code = http.GET();
  if (reply && code == 200) *reply = http.getString();
  http.end();
  return code;
}

// Runs on the other CPU core, so slow internet never freezes the LCD or the buzzer.
void networkTask(void*) {
  tls.setInsecure();          // skip certificate check (fine for a school project)
  http.setReuse(true);        // keep the connection open = much faster updates
  http.setTimeout(4000);

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      online = false;
      WiFi.reconnect();
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }
    unsigned long t0 = millis();

    // 1) send all readings in ONE request (the website gets one clean update)
    char body[220];
    snprintf(body, sizeof(body),
             "{\"lpg\":%.1f,\"smoke\":%.1f,\"co2\":%.1f,\"danger\":%s,\"silenced\":%s,"
             "\"updated\":{\".sv\":\"timestamp\"}}",
             (float)lpg, (float)smoke, (float)co2,
             dangerState ? "true" : "false", silenced ? "true" : "false");
    int code = fbRequest("PATCH", "/house", body);
    online = (code == 200);
    if (code != 200) Serial.printf("Firebase send failed: %d %s\n", code, http.errorToString(code).c_str());

    // 2) did the website press "Silence Alarm"?
    String reply;
    if (fbRequest("GET", "/commands/silence_alarm", "", &reply) == 200) {
      reply.trim();
      if (reply == "1") {
        Serial.println("Alarm silenced from web app!");
        silenceRequest = true;
        fbRequest("PUT", "/commands/silence_alarm", "0");
      }
    }

    // about one update per second
    unsigned long took = millis() - t0;
    if (took < 1000) vTaskDelay(pdMS_TO_TICKS(1000 - took));
  }
}

// ================= Sensors, buzzer, LCD =================
int readAvg(int pin) {                         // average 10 readings = steadier numbers
  long sum = 0;
  for (int i = 0; i < 10; i++) sum += analogRead(pin);
  return sum / 10;
}

void buzzerUpdate() {
  static bool on = false;
  bool want = dangerState && !silenced && ((millis() / 250) % 2 == 0);   // beep-beep-beep
  if (want && !on)  { tone(BUZZER_PIN, 2000); on = true; }
  if (!want && on)  { noTone(BUZZER_PIN);     on = false; }
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  noTone(BUZZER_PIN);
  analogReadResolution(12);

  lcdStart(lcd);
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("Air Quality Boot");
  lcd.setCursor(0, 1); lcd.print("Warming Up...   ");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);            // connects in the background while the sensors warm up
  delay(10000);                                // MQ sensors need to heat up
  Serial.println(WiFi.status() == WL_CONNECTED ? "WiFi connected! IP: " + WiFi.localIP().toString()
                                               : String("WiFi not connected yet (alarm still works offline)"));

  xTaskCreatePinnedToCore(networkTask, "net", 12288, nullptr, 1, nullptr, 0);

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("System Ready");
  delay(1200);
  lcd.clear();
}

void loop() {
  int mq2_raw   = readAvg(MQ2_PIN);
  int mq135_raw = readAvg(MQ135_PIN);

  // simple estimates from the raw readings (not calibrated ppm)
  lpg   = (mq2_raw / 4095.0) * 1000.0;
  smoke = (mq2_raw / 4095.0) * 800.0;
  co2   = 400.0 + ((mq135_raw / 4095.0) * 1600.0);

  bool danger = (lpg > LPG_DANGER) || (smoke > SMOKE_DANGER) || (co2 > CO2_DANGER);
  if (danger && !dangerState) Serial.println("!!! DANGER: gas level high");
  if (!danger) silenced = false;               // air is safe again: next danger will beep again
  dangerState = danger;

  if (silenceRequest) {                        // website pressed "Silence Alarm"
    silenceRequest = false;
    if (dangerState) silenced = true;
  }

  buzzerUpdate();

  // LCD once a second, overwrite instead of clear (no flicker)
  if (millis() - lastLcdUpdate > 1000) {
    lastLcdUpdate = millis();
    if (dangerState != lastShownDanger) { lcd.clear(); lastShownDanger = dangerState; }
    char line1[17], line2[17];
    if (dangerState) {
      snprintf(line1, sizeof(line1), "!! DANGER !!    ");
      snprintf(line2, sizeof(line2), silenced ? "Alarm silenced  " : "Evacuate Now!   ");
    } else {
      snprintf(line1, sizeof(line1), "LPG:%4.0f S:%4.0f ", (float)lpg, (float)smoke);
      snprintf(line2, sizeof(line2), "CO2:%5.0fppm  %c", (float)co2, online ? 'W' : '-');   // W = online
    }
    lcd.setCursor(0, 0); lcd.print(line1);
    lcd.setCursor(0, 1); lcd.print(line2);

    Serial.printf("LPG=%.1f | Smoke=%.1f | CO2=%.1f | DANGER=%d | silenced=%d | online=%d\n",
                  (float)lpg, (float)smoke, (float)co2, dangerState, silenced, online);
  }

  delay(50);
}
