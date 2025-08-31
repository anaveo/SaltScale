#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Ticker.h>
#include <HX711.h>
#include <OneButton.h>
#include <Preferences.h>
#include "soc/rtc.h"

#define BEEPER_PIN 27
#define BUTTON_PIN 26
#define LDR_ANALOG_PIN 35
#define LDR_DIGITAL_PIN 34
#define LDR_ENABLE_PIN 33
#define OLED_ENABLE_PIN 19
#define HX711_DOUT 12
#define HX711_SCK 13

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
#define WEIGHT_THRESHOLD 25.0

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
HX711 scale;
OneButton button(BUTTON_PIN, true, true);
Preferences prefs;

Ticker debounceTicker;
Ticker weightTicker;
Ticker displayTicker;

volatile bool ldrState = false;
volatile bool ldrChanged = false;
bool oledOn = false;
float lastWeight = 0.0;
float calibration_factor = 13563.6; // Default
float offset = 61927.00; // Default
bool calibrating = false;

void IRAM_ATTR ldrISR() {
  if (calibrating) return;
  detachInterrupt(digitalPinToInterrupt(LDR_DIGITAL_PIN));
  debounceTicker.once_ms(50, checkLdrState);
}

void checkLdrState() {
  bool currentState = !digitalRead(LDR_DIGITAL_PIN);
  if (currentState != ldrState) {
    ldrState = currentState;
    ldrChanged = true;
    Serial.print("LDR state changed to: ");
    Serial.println(ldrState);
  }
  attachInterrupt(digitalPinToInterrupt(LDR_DIGITAL_PIN), ldrISR, CHANGE);
}

void readWeight() {
  if (calibrating) return;
  float weight = scale.get_units(10);
  Serial.print("Weight: ");
  Serial.print(weight);
  Serial.println(" lbs");
  lastWeight = weight;

  if (weight < WEIGHT_THRESHOLD) {
    tone(BEEPER_PIN, 1000, 250);
  }
}

void updateDisplay() {
  if (calibrating) return;
  float weight = lastWeight;
  int lbs = round(weight);

  if (!oledOn) return;

  display.clearDisplay();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  const char* title = weight < WEIGHT_THRESHOLD ? "SALT LEVEL LOW" : "SALT LEVEL";
  int16_t titleWidth = strlen(title) * 6;
  int16_t titleX = max(0, (SCREEN_WIDTH - titleWidth) / 2);
  display.setCursor(titleX, 0);
  display.print(title);

  char buf[6];
  snprintf(buf, sizeof(buf), "%d", lbs);
  display.setTextSize(4);
  int16_t lbsWidth = strlen(buf) * 24;
  int16_t lbsX = max(0, (SCREEN_WIDTH - lbsWidth) / 2);
  display.setCursor(lbsX, 16);
  static bool flashState = true;
  static unsigned long lastFlash = 0;
  if (weight < WEIGHT_THRESHOLD && millis() - lastFlash >= 500) {
    flashState = !flashState;
    lastFlash = millis();
  }
  if (weight >= WEIGHT_THRESHOLD || flashState) {
    display.print(buf);
  }

  display.setTextSize(2);
  const char* unit = "lbs";
  int16_t unitWidth = strlen(unit) * 12;
  int16_t unitX = max(0, (SCREEN_WIDTH - unitWidth) / 2);
  display.setCursor(unitX, 48);
  if (weight >= WEIGHT_THRESHOLD || flashState) {
    display.print(unit);
  }

  display.display();
}

void displayCalibrationStep(const char* message, int line) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  int16_t msgWidth = strlen(message) * 6;
  int16_t msgX = max(0, (SCREEN_WIDTH - msgWidth) / 2);
  display.setCursor(msgX, line);
  display.print(message);
  display.display();
}

void displayCalibrationResult(float factor, float offset) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  char buf[32];
  snprintf(buf, sizeof(buf), "Factor: %.1f", factor);
  int16_t factorWidth = strlen(buf) * 6;
  int16_t factorX = max(0, (SCREEN_WIDTH - factorWidth) / 2);
  display.setCursor(factorX, 16);
  display.print(buf);
  snprintf(buf, sizeof(buf), "Offset: %.1f", offset);
  int16_t offsetWidth = strlen(buf) * 6;
  int16_t offsetX = max(0, (SCREEN_WIDTH - offsetWidth) / 2);
  display.setCursor(offsetX, 32);
  display.print(buf);
  display.display();
}

void calibrateScale() {
  calibrating = true;
  weightTicker.detach();
  displayTicker.detach();

  if (!oledOn) turnOnOled();

  displayCalibrationStep("Remove weight", 16);
  Serial.println("Calibration: Remove weight...");
  delay(5000);
  scale.set_scale();
  scale.tare();
  offset = scale.get_offset();
  Serial.print("Tare done. Offset: ");
  Serial.println(offset);

  displayCalibrationStep("Place 10 lbs weight", 16);
  Serial.println("Place 10 lbs weight...");
  delay(5000);
  float known_weight = 10.0; // CHANGE THIS TO YOUR KNOWN WEIGHT
  float raw = scale.get_units(10);
  calibration_factor = raw / known_weight;
  Serial.print("Calibration factor: ");
  Serial.println(calibration_factor);

  scale.set_scale(calibration_factor);
  scale.set_offset(offset);

  // Save to Preferences
  prefs.begin("scale_config", false);
  prefs.putFloat("cal_factor", calibration_factor);
  prefs.putFloat("offset", offset);
  prefs.end();

  displayCalibrationResult(calibration_factor, offset);
  Serial.println("Calibration complete. Rebooting...");
  delay(5000);
  ESP.restart();
}

void onLongPress() {
  if (!calibrating) {
    Serial.println("Long press detected, starting calibration...");
    calibrateScale();
  }
}

void turnOnOled() {
  if (oledOn) return;
  digitalWrite(OLED_ENABLE_PIN, HIGH);
  delay(100);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED initialization failed");
    return;
  }
  display.ssd1306_command(SSD1306_DISPLAYON);
  oledOn = true;
  if (!calibrating) updateDisplay();
  Serial.println("OLED turned on");
}

void turnOffOled() {
  if (!oledOn) return;
  display.clearDisplay();
  display.display();
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  digitalWrite(OLED_ENABLE_PIN, LOW);
  oledOn = false;
  Serial.println("OLED turned off");
}

void init_scale() {
  scale.begin(HX711_DOUT, HX711_SCK);
  prefs.begin("scale_config", true); // Read-only mode
  calibration_factor = prefs.getFloat("cal_factor", 13563.6); // Default if not found
  offset = prefs.getFloat("offset", 61927.00); // Default if not found
  prefs.end();
  scale.set_scale(calibration_factor);
  scale.set_offset(offset);
  Serial.print("Loaded calibration factor: ");
  Serial.println(calibration_factor);
  Serial.print("Loaded offset: ");
  Serial.println(offset);
}

void setup() {
  pinMode(BEEPER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LDR_ANALOG_PIN, INPUT);
  pinMode(LDR_DIGITAL_PIN, INPUT);
  pinMode(LDR_ENABLE_PIN, OUTPUT);
  pinMode(OLED_ENABLE_PIN, OUTPUT);

  Serial.begin(115200);
  delay(500);

  button.attachLongPressStop(onLongPress);
  button.setPressTicks(2000);

  init_scale();
  digitalWrite(LDR_ENABLE_PIN, HIGH);
  ldrState = !digitalRead(LDR_DIGITAL_PIN);
  attachInterrupt(digitalPinToInterrupt(LDR_DIGITAL_PIN), ldrISR, CHANGE);
  weightTicker.attach(60.0, readWeight);
  displayTicker.attach(1.0, updateDisplay);

  readWeight();
  if (ldrState) {
    turnOnOled();
  } else {
    turnOffOled();
  }
}

void loop() {
  button.tick();
  if (ldrChanged && !calibrating) {
    ldrChanged = false;
    if (ldrState) {
      turnOnOled();
    } else {
      turnOffOled();
    }
  }
}
