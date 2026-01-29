#include <WiFi.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_AHTX0.h>
#include <ScioSense_ENS160.h>
#include "time.h"

// ====== WiFi & Time config ======
const char* ssid      = "yourwifi";
const char* password  = "yourpassword";
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec      = -6 * 3600;   // GMT+7
const int   daylightOffset_sec = 0;
//added to synctime
static unsigned long lastSyncMs = 0;
//wifi timesync lapse period
const unsigned long SYNC_EVERY_MS = 1UL * 60UL * 60UL * 1000UL; // 1 hour


bool isSyncingTime = false;
unsigned long lastSyncBlinkMs = 0;
bool syncDotOn = false;
bool syncFailed = false;  // stays true until success



// ====== Pins ======
#define TFT_CS   9
#define TFT_DC   8
#define TFT_RST  7
#define TFT_MOSI 6
#define TFT_SCLK 5

#define ENC_A_PIN    10
#define ENC_B_PIN    20
#define ENC_BTN_PIN  21
#define KEY0_PIN     0

#define SDA_PIN 1
#define SCL_PIN 2

#define LED_PIN 4
#define BUZZ_PIN 3

// ====== TFT & Sensors ======
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST); // hardware SPI
Adafruit_AHTX0   aht;
ScioSense_ENS160 ens160(0x53);   // ENS160 I2C address 0x53

// ====== Colors ======
#define CYBER_BG      ST77XX_BLACK
#define CYBER_GREEN   0x07E0  
#define CYBER_ACCENT  0x07FF  
#define CYBER_LIGHT   0xFD20  
#define CYBER_BLUE    0x07FF  
#define CYBER_PINK    0xF81F

#define AQ_BAR_GREEN  0x07E0
#define AQ_BAR_YELLOW 0xFFE0
#define AQ_BAR_ORANGE 0xFD20
#define AQ_BAR_RED    0xF800
#define CYBER_DARK    0x4208

#ifndef PI
#define PI 3.1415926
#endif

// ====== Modes ======
enum UIMode {
  MODE_MENU = 0,
  MODE_CLOCK,
  MODE_POMODORO,
  MODE_ALARM,
  MODE_DVD,
  MODE_GAME
};
UIMode currentMode = MODE_CLOCK;       // khởi động vào CLOCK luôn
int menuIndex = 0;
const int MENU_ITEMS = 5;       // Monitor, Pomodoro, Alarm, DVD, Game

// ====== Pomodoro ======
enum PomodoroState {
  POMO_SELECT = 0,
  POMO_RUNNING,
  POMO_PAUSED,
  POMO_DONE
};
PomodoroState pomoState = POMO_SELECT;
int  pomoPresetIndex    = 0;                   // 0:5, 1:15, 2:30
const uint16_t pomoDurationsMin[3] = {5, 15, 30};
unsigned long pomoStartMillis = 0;
unsigned long pomoPausedMillis = 0;
int prevPomoRemainSec  = -1;
int prevPomoPreset     = -1;
int prevPomoStateInt   = -1;

// ====== Env values ======
float    curTemp = 0;
float    curHum  = 0;
uint16_t curTVOC = 0;
uint16_t curECO2 = 400;
unsigned long lastEnvRead = 0;

// ====== Clock vars ======
int    prevSecond  = -1;
String prevTimeStr = "";

// ====== Encoder & Buttons ======
int  lastEncA   = HIGH;
int  lastEncB   = HIGH;
bool lastEncBtn = HIGH;
bool lastKey0   = HIGH;
unsigned long lastBtnMs = 0;

// ====== Alarm ======
bool     alarmEnabled = false;
uint8_t  alarmHour    = 7;
uint8_t  alarmMinute  = 0;
bool     alarmRinging = false;
int      alarmSelectedField = 0; // 0 = hour, 1 = minute, 2 = enable
int      lastAlarmDayTriggered = -1;

// ====== Alert / LED Blink ======
enum AlertLevel {
  ALERT_NONE = 0,
  ALERT_CO2,
  ALERT_ALARM
};
AlertLevel currentAlertLevel = ALERT_NONE;
unsigned long lastLedToggleMs = 0;
bool ledState = false;

unsigned long lastCo2BlinkMs = 0;
bool co2BlinkOn = false;

// ========= Helper: encoder & button =========
int readEncoderStep() {
  int encA = digitalRead(ENC_A_PIN);
  int encB = digitalRead(ENC_B_PIN);
  int step = 0;
  if (encA != lastEncA) {
    if (encA == LOW) {
      if (encB == HIGH) step = +1;
      else              step = -1;
    }
  }
  lastEncA = encA;
  lastEncB = encB;
  return step;
}

bool checkButtonPressed(uint8_t pin, bool &lastState) {
  bool cur = digitalRead(pin);
  bool pressed = false;
  unsigned long now = millis();
  if (cur == LOW && lastState == HIGH && (now - lastBtnMs) > 150) {
    pressed = true;
    lastBtnMs = now;
  }
  lastState = cur;
  return pressed;
}

// ========= Alarm icon =========
void drawAlarmIcon() {
  int x = 148;
  tft.fillRect(x - 10, 0, 12, 12, CYBER_BG);
  if (!alarmEnabled) return;

  uint16_t c = CYBER_LIGHT; // cam
  tft.drawRoundRect(x - 9, 2, 10, 7, 2, c);
  tft.drawFastHLine(x - 8, 8, 8, c);
  tft.fillCircle(x - 4, 10, 1, c);
}

//===========Sync icon ===========
void drawSyncDotIcon() {
 
 int x = 148;
  tft.fillRect(x - 24, 0, 12, 12, CYBER_BG);
  
  //if (!isSyncingTime) return;
  if (!isSyncingTime && !syncFailed) return;

  uint16_t c = isSyncingTime ? CYBER_GREEN : ST77XX_RED;

  //uint16_t c = CYBER_GREEN; // cam
  // draw a circle centered nicely in that 12x12 box
  tft.drawCircle(x - 16, 6, 3, c);     // outline circle
  tft.fillCircle(x - 16, 6, 2, c);     // filled inside (optional)
}


// ========= WiFi & Time =========
bool connectWiFiAndSyncTime(bool showUI) {

  isSyncingTime = true;
  drawSyncDotIcon();

  if (showUI) {
    tft.fillScreen(CYBER_BG);
    tft.setTextColor(CYBER_LIGHT);
    tft.setTextSize(1);
    tft.setCursor(10, 55);
    tft.print("Connecting WiFi");
  }

  
  WiFi.begin(ssid, password);

  uint8_t retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(300);
    if (showUI) tft.print(".");
    retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    if (showUI) {
      tft.fillScreen(CYBER_BG);
      tft.setCursor(10, 55);
      tft.print("Syncing time...");
      delay(800);
    }

    //Added to control wakeup sync
    struct tm timeinfo;
    bool ok = getLocalTime(&timeinfo, 5000);  // up to 5s

    // Disconnect WiFi to save power
    WiFi.disconnect(true);   // true = erase old connection (optional)
    WiFi.mode(WIFI_OFF);

    if (ok) syncFailed = false;
    else    syncFailed = true;

    isSyncingTime = false;
    drawSyncDotIcon(); // clears it

    return ok;
  } else {

    if (showUI) {
      tft.fillScreen(CYBER_BG);
      tft.setCursor(10, 55);
      tft.setTextColor(ST77XX_RED);
      tft.print("WiFi FAILED!");
      delay(1000);
    }

    syncFailed = true;
    isSyncingTime = false;
    drawSyncDotIcon(); // clears it
    
    // Disconnect WiFi to save power
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return false;
  }
}

String getTimeStr(char type) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "--";
  char buf[8];
  if (type == 'H') strftime(buf, sizeof(buf), "%H", &timeinfo);
  else if (type == 'M') strftime(buf, sizeof(buf), "%M", &timeinfo);
  else if (type == 'S') strftime(buf, sizeof(buf), "%S", &timeinfo);
  return String(buf);
}

// ========= Env Sensors =========
void updateEnvSensors(bool force = false) {
  unsigned long now = millis();
  if (!force && (now - lastEnvRead) < 5000) return;
  lastEnvRead = now;

  sensors_event_t hum, temp;
  if (aht.getEvent(&hum, &temp)) {
    curTemp = temp.temperature;
    curHum  = hum.relative_humidity;
  }

  ens160.set_envdata(curTemp, curHum);
  ens160.measure();                      // blocking

  uint16_t newTVOC = ens160.getTVOC();
  uint16_t newCO2  = ens160.geteCO2();

  if (newTVOC != 0xFFFF) curTVOC = newTVOC;
  if (newCO2  != 0xFFFF) curECO2 = newCO2;

  Serial.print("ENS160: TVOC=");
  Serial.print(curTVOC);
  Serial.print(" eCO2=");
  Serial.println(curECO2);
}

// ========= Clock UI =========

// Grid layout
const int GRID_L   = 4;
const int GRID_R   = 156;
const int GRID_TOP = 56;
const int GRID_MID = 80;
const int GRID_BOT = 104;
const int GRID_MID_X = (GRID_L + GRID_R) / 2;

// Y cho label / value (text size 1, cao ~8px)
const int TOP_LABEL_Y    = GRID_TOP + 4;   // 60
const int TOP_VALUE_Y    = GRID_TOP + 15;  // 71
const int BOTTOM_LABEL_Y = GRID_MID + 4;   // 84
const int BOTTOM_VALUE_Y = GRID_MID + 15;  // 95

// bar layout
const int BAR_MARGIN_X = 2;
const int BAR_GAP      = 2;
const int BAR_Y        = 110;
const int BAR_H        = 6;
const int BAR_W        = (160 - 2 * BAR_MARGIN_X - 3 * BAR_GAP) / 4; // 37

// In text căn giữa trong khoảng [x0, x1]
void printCenteredText(const String &txt,
                       int x0, int x1,
                       int y,
                       uint16_t color,
                       uint16_t bg,
                       uint8_t size = 1) {
  int16_t bx, by;
  uint16_t w, h;
  tft.setTextSize(size);
  tft.getTextBounds(txt, 0, 0, &bx, &by, &w, &h);
  int x = x0 + ((x1 - x0) - (int)w) / 2;
  tft.setTextColor(color, bg);
  tft.setCursor(x, y);
  tft.print(txt);
}

uint16_t colorForCO2(uint16_t eco2) {
  if (eco2 <= 800)  return AQ_BAR_GREEN;
  if (eco2 <= 1200) return AQ_BAR_YELLOW;
  if (eco2 <= 1800) return AQ_BAR_ORANGE;
  return AQ_BAR_RED;
}

void initClockStaticUI() {
  tft.fillScreen(CYBER_BG);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);

  tft.setCursor(4, 4);
  tft.print("Monitor");

  tft.setCursor(4, 44);
  tft.print("Air Quality:");

  tft.drawFastHLine(GRID_L, GRID_TOP, GRID_R - GRID_L, ST77XX_WHITE);
  tft.drawFastHLine(GRID_L, GRID_MID, GRID_R - GRID_L, ST77XX_WHITE);
  tft.drawFastHLine(GRID_L, GRID_BOT, GRID_R - GRID_L, ST77XX_WHITE);
  tft.drawFastVLine(GRID_MID_X, GRID_TOP, GRID_BOT - GRID_TOP, ST77XX_WHITE);

  // Label căn giữa trong từng ô
  printCenteredText("HUMI", GRID_L,     GRID_MID_X, TOP_LABEL_Y,    ST77XX_WHITE, CYBER_BG, 1);
  printCenteredText("TEMP", GRID_MID_X, GRID_R,     TOP_LABEL_Y,    ST77XX_WHITE, CYBER_BG, 1);
  printCenteredText("TVOC", GRID_L,     GRID_MID_X, BOTTOM_LABEL_Y, ST77XX_WHITE, CYBER_BG, 1);
  printCenteredText("CO2",  GRID_MID_X, GRID_R,     BOTTOM_LABEL_Y, ST77XX_WHITE, CYBER_BG, 1);

  int x = BAR_MARGIN_X;
  tft.fillRect(x,                          BAR_Y, BAR_W, BAR_H, AQ_BAR_GREEN);
  tft.fillRect(x + (BAR_W + BAR_GAP),      BAR_Y, BAR_W, BAR_H, AQ_BAR_YELLOW);
  tft.fillRect(x + 2 * (BAR_W + BAR_GAP),  BAR_Y, BAR_W, BAR_H, AQ_BAR_ORANGE);
  tft.fillRect(x + 3 * (BAR_W + BAR_GAP),  BAR_Y, BAR_W, BAR_H, AQ_BAR_RED);

  drawAlarmIcon();
}

void drawClockTime(String hourStr, String minStr, String secStr) {
  String cur = hourStr + ":" + minStr + ":" + secStr;
  if (cur == prevTimeStr) return;
  prevTimeStr = cur;

  int16_t x1, y1;
  uint16_t w, h;
  tft.setTextSize(3);
  tft.getTextBounds(cur, 0, 0, &x1, &y1, &w, &h);
  int x = (160 - w) / 2;
  int y = 18;

  tft.setTextColor(CYBER_LIGHT, CYBER_BG);
  tft.setCursor(x, y);
  tft.print(cur);

  tft.setTextColor(CYBER_ACCENT, CYBER_BG);
  tft.setCursor(x, y);
  tft.print(cur);
}

void drawCO2Value(uint16_t eco2, uint16_t col) {
  char co2Buf[12];
  sprintf(co2Buf, "%4uppm", eco2);
  printCenteredText(String(co2Buf),
                    GRID_MID_X, GRID_R,
                    BOTTOM_VALUE_Y,
                    col, CYBER_BG, 1);
}

void drawEnvDynamic(float temp, float hum, uint16_t tvoc, uint16_t eco2) {
  uint16_t colHUMI = CYBER_ACCENT;
  uint16_t colTEMP = CYBER_LIGHT;
  uint16_t colTVOC = CYBER_GREEN;
  uint16_t colCO2  = colorForCO2(eco2);

  // HUMI
  char humBuf[8];
  sprintf(humBuf, "%2.0f%%", hum);
  printCenteredText(String(humBuf),
                    GRID_L, GRID_MID_X,
                    TOP_VALUE_Y,
                    colHUMI, CYBER_BG, 1);

  // TEMP
  char tempBuf[10];
  sprintf(tempBuf, "%2.1fC", temp);
  printCenteredText(String(tempBuf),
                    GRID_MID_X, GRID_R,
                    TOP_VALUE_Y,
                    colTEMP, CYBER_BG, 1);

  // TVOC (mg/m3)
  float tvoc_mg = tvoc / 1000.0f;
  char tvocBuf[16];
  sprintf(tvocBuf, "%.3fmg/m3", tvoc_mg);
  printCenteredText(String(tvocBuf),
                    GRID_L, GRID_MID_X,
                    BOTTOM_VALUE_Y,
                    colTVOC, CYBER_BG, 1);

  // CO2 (ppm)
  drawCO2Value(eco2, colCO2);

  // Thanh màu + tam giác
  uint8_t level = 1;
  if (eco2 > 1800) level = 4;
  else if (eco2 > 1200) level = 3;
  else if (eco2 > 800)  level = 2;

  tft.fillRect(0, BAR_Y + BAR_H + 1, 160, 8, CYBER_BG);
  int centerX = BAR_MARGIN_X + (BAR_W / 2) + (level - 1) * (BAR_W + BAR_GAP);
  int tipY    = BAR_Y + BAR_H + 2;
  tft.fillTriangle(centerX,     tipY - 4,
                   centerX - 4, tipY + 2,
                   centerX + 4, tipY + 2,
                   ST77XX_WHITE);
}

// ========= Menu UI =========
void drawMenu() {
  tft.fillScreen(CYBER_BG);

  tft.setTextSize(1);
  tft.setTextColor(CYBER_LIGHT);
  tft.setCursor(10, 10);
  tft.print("MODE SELECT");

  const char* items[MENU_ITEMS] = {
    "Monitor",
    "Pomodoro",
    "Alarm",
    "DVD",
    "Space Attack"
  };

  for (int i = 0; i < MENU_ITEMS; i++) {
    int y = 32 + i * 18;
    if (i == menuIndex) {
      tft.fillRect(6, y - 2, 148, 14, CYBER_ACCENT);
      tft.setTextColor(CYBER_BG);
    } else {
      tft.fillRect(6, y - 2, 148, 14, CYBER_BG);
      tft.setTextColor(ST77XX_WHITE);
    }
    tft.setCursor(12, y);
    tft.print(items[i]);
  }
  drawAlarmIcon();
}

// ========= Pomodoro =========
uint16_t pomoColorFromFrac(float f) {
  if (f < 0.33f) return AQ_BAR_GREEN;
  if (f < 0.66f) return AQ_BAR_YELLOW;
  if (f < 0.85f) return AQ_BAR_ORANGE;
  return AQ_BAR_RED;
}

// Vòng Pomodoro 270°
void drawPomodoroRing(float progress) {
  int cx = 80;
  int cy = 64;
  int rOuter = 55;
  int rInner = 44;

  // Vẽ cung 270°: từ -225° đến +45° (chừa hở đáy)
  const float startDeg = -225.0f;
  const float endDeg   =   45.0f;
  const float spanDeg  = endDeg - startDeg;   // 270°

  for (float deg = startDeg; deg <= endDeg; deg += 6.0f) {
    float frac = (deg - startDeg) / spanDeg;   // 0..1
    uint16_t col = (frac <= progress) ? pomoColorFromFrac(frac) : CYBER_DARK;

    float rad = deg * PI / 180.0f;
    int xOuter = cx + cos(rad) * rOuter;
    int yOuter = cy + sin(rad) * rOuter;
    int xInner = cx + cos(rad) * rInner;
    int yInner = cy + sin(rad) * rInner;
    tft.drawLine(xInner, yInner, xOuter, yOuter, col);
  }
}

void drawPomodoroScreen(bool forceStatic) {
  bool needStatic = forceStatic;
  if (prevPomoPreset != pomoPresetIndex || prevPomoStateInt != (int)pomoState) {
    needStatic = true;
    prevPomoPreset   = pomoPresetIndex;
    prevPomoStateInt = (int)pomoState;
  }

  if (needStatic) {
    tft.fillScreen(CYBER_BG);
    tft.fillRect(0, 0, 160, 16, CYBER_BG);
    tft.setTextSize(1);
    tft.setCursor(8, 4);
    tft.setTextColor(CYBER_LIGHT);
    tft.print("POMODORO");
    drawAlarmIcon();
  }

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE, CYBER_BG);
  tft.fillRect(100, 0, 60, 16, CYBER_BG);
  tft.setCursor(108, 4);
  tft.printf("%2d min", pomoDurationsMin[pomoPresetIndex]);

  unsigned long durationMs = pomoDurationsMin[pomoPresetIndex] * 60UL * 1000UL;
  unsigned long elapsed = 0;
  if (pomoState == POMO_RUNNING)      elapsed = millis() - pomoStartMillis;
  else if (pomoState == POMO_PAUSED)  elapsed = pomoPausedMillis - pomoStartMillis;
  else if (pomoState == POMO_DONE)    elapsed = durationMs;
  if (elapsed > durationMs) elapsed = durationMs;

  float progress = (durationMs > 0) ? (float)elapsed / durationMs : 0.0f;
  drawPomodoroRing(progress);

  int cx = 80;
  int cy = 64;
  tft.fillCircle(cx, cy, 38, CYBER_BG);

  unsigned long remain = durationMs - elapsed;
  uint16_t rmMin = remain / 60000UL;
  uint8_t  rmSec = (remain / 1000UL) % 60;

  char buf[8];
  sprintf(buf, "%02d:%02d", rmMin, rmSec);

  int16_t x1, y1;
  uint16_t w, h;
  tft.setTextSize(3);
  tft.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor(cx - w / 2, cy - 8);
  tft.setTextColor(ST77XX_WHITE, CYBER_BG);
  tft.print(buf);

  // Footer: chỉ hiện Paused / Completed
  tft.fillRect(0, 96, 160, 32, CYBER_BG);
  tft.setTextSize(1);
  tft.setCursor(8, 100);
  tft.setTextColor(CYBER_GREEN, CYBER_BG);
  if (pomoState == POMO_PAUSED)      tft.print("Paused");
  else if (pomoState == POMO_DONE)   tft.print("Completed");
}

// ========= Alarm UI =========
void drawAlarmScreen(bool full) {
  if (full) {
    tft.fillScreen(CYBER_BG);
    tft.setTextSize(1);
    tft.setCursor(8, 4);
    tft.setTextColor(CYBER_LIGHT);
    tft.print("ALARM");
    drawAlarmIcon();
  }

  char hBuf[3];
  char mBuf[3];
  sprintf(hBuf, "%02d", alarmHour);
  sprintf(mBuf, "%02d", alarmMinute);

  tft.setTextSize(3);

  String disp = String(hBuf) + ":" + String(mBuf);
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(disp, 0, 0, &x1, &y1, &w, &h);
  int x = (160 - w) / 2;
  int y = 30;

  tft.setCursor(x, y);
  tft.setTextColor(alarmSelectedField == 0 ? CYBER_LIGHT : ST77XX_WHITE, CYBER_BG);
  tft.print(hBuf);

  tft.setTextColor(ST77XX_WHITE, CYBER_BG);
  tft.print(":");

  tft.setTextColor(alarmSelectedField == 1 ? CYBER_LIGHT : ST77XX_WHITE, CYBER_BG);
  tft.print(mBuf);

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE, CYBER_BG);
  tft.fillRect(20, 80, 120, 24, CYBER_BG);
  tft.setCursor(30, 84);
  tft.print("Alarm:");
  tft.setCursor(80, 84);
  uint16_t enColor = alarmSelectedField == 2
                     ? CYBER_LIGHT
                     : (alarmEnabled ? CYBER_GREEN : ST77XX_RED);
  tft.setTextColor(enColor, CYBER_BG);
  tft.print(alarmEnabled ? "ON " : "OFF");
}

void drawAlarmRingingScreen() {
  tft.fillScreen(ST77XX_RED);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE, ST77XX_RED);
  tft.setCursor(30, 40);
  tft.print("ALARM!");
}

// ========= Alarm logic =========
void checkAlarmTrigger() {
  if (!alarmEnabled || alarmRinging) return;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  if (timeinfo.tm_hour == alarmHour &&
      timeinfo.tm_min  == alarmMinute &&
      timeinfo.tm_sec  == 0) {

    alarmRinging = true;
    lastAlarmDayTriggered = timeinfo.tm_mday;
    currentMode = MODE_ALARM;
    drawAlarmRingingScreen();
  }
}

// ========= Alert visual + audio =========
void updateAlertStateAndLED() {
  if (alarmRinging) currentAlertLevel = ALERT_ALARM;
  else if (curECO2 > 1800) currentAlertLevel = ALERT_CO2;
  else currentAlertLevel = ALERT_NONE;

  unsigned long now = millis();

  unsigned long interval;
  if (currentAlertLevel == ALERT_ALARM)      interval = 120;
  else if (currentAlertLevel == ALERT_CO2)   interval = 250;
  else                                       interval = 1000;

  if (interval != 1000){
      if (now - lastLedToggleMs > interval) {
        lastLedToggleMs = now;
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState ? HIGH : LOW);
      }
  } else {digitalWrite(LED_PIN, LOW);}

  if (currentAlertLevel == ALERT_CO2) {
    if (now - lastCo2BlinkMs > 350) {
      lastCo2BlinkMs = now;
      co2BlinkOn = !co2BlinkOn;
      uint16_t baseCol = colorForCO2(curECO2);
      uint16_t col = co2BlinkOn ? baseCol : CYBER_DARK;
      drawCO2Value(curECO2, col);
      tone(BUZZ_PIN, 1800, 80);
    }
  }
}

// ========= GAME: Space Attack (simple TFT version) =========

// court layout
const int COURT_X = 8;
const int COURT_Y = 16;
const int COURT_W = 144;
const int COURT_H = 96;

const int PADDLE_W = 18;
const int PADDLE_H = 4;
const int PADDLE_Y = COURT_Y + COURT_H - 8;

const int ALIEN_ROWS = 3;
const int ALIEN_COLS = 8;
bool alienAlive[ALIEN_ROWS][ALIEN_COLS];
int  alienOffsetX = 0;   // pixel offset
int  alienDir     = 1;   // 1:right, -1:left
unsigned long lastAlienMoveMs = 0;
unsigned long alienMoveInterval = 250;

bool  bulletActive = false;
int   bulletX = 0, bulletY = 0;
unsigned long lastBulletMoveMs = 0;
unsigned long bulletInterval = 25;

int  playerX = COURT_X + (COURT_W - PADDLE_W) / 2;
int  gameScore = 0;
bool gameOver  = false;
bool gameInited = false;

void initGame() {
  gameScore = 0;
  gameOver  = false;
  gameInited = true;

  // background
  tft.fillScreen(CYBER_BG);
  tft.drawRect(COURT_X, COURT_Y, COURT_W, COURT_H, CYBER_ACCENT);

  // aliens
  for (int r = 0; r < ALIEN_ROWS; r++) {
    for (int c = 0; c < ALIEN_COLS; c++) {
      alienAlive[r][c] = true;
    }
  }
  alienOffsetX = 0;
  alienDir     = 1;

  bulletActive = false;

  // title
  tft.setTextSize(1);
  tft.setTextColor(CYBER_LIGHT, CYBER_BG);
  tft.setCursor(10, 2);
  tft.print("SPACE ATTACK");

  // score
  tft.setCursor(110, 2);
  tft.print("0");

  drawAlarmIcon();
}

void drawScore() {
  tft.fillRect(100, 0, 60, 10, CYBER_BG);
  tft.setCursor(110, 2);
  tft.setTextSize(1);
  tft.setTextColor(CYBER_GREEN, CYBER_BG);
  tft.print(gameScore);
}

void drawPlayer() {
  tft.fillRect(COURT_X + 1, PADDLE_Y, COURT_W - 2, PADDLE_H, CYBER_BG);
  tft.fillRect(playerX, PADDLE_Y, PADDLE_W, PADDLE_H, CYBER_ACCENT);
}

void drawAliens() {
  // clear area
  tft.fillRect(COURT_X + 1, COURT_Y + 1, COURT_W - 2, COURT_H - 16, CYBER_BG);

  int cellW = COURT_W / ALIEN_COLS;
  int cellH = 12;

  for (int r = 0; r < ALIEN_ROWS; r++) {
    for (int c = 0; c < ALIEN_COLS; c++) {
      if (!alienAlive[r][c]) continue;
      int x = COURT_X + 4 + alienOffsetX + c * cellW;
      int y = COURT_Y + 4 + r * cellH;
      tft.fillRect(x, y, 8, 6, CYBER_GREEN);
      tft.drawRect(x, y, 8, 6, CYBER_DARK);
    }
  }
}

void drawBullet() {
  // clear old bullet track
  tft.fillRect(COURT_X + 1, COURT_Y + 1, COURT_W - 2, COURT_H - 2, CYBER_BG);
  drawAliens();
  drawPlayer();

  if (bulletActive) {
    tft.drawFastVLine(bulletX, bulletY, 4, CYBER_PINK);
  }
}

void updateGameLogic(int encStep, bool firePressed, bool backPressed) {
  if (backPressed) {
    gameInited = false;
    currentMode = MODE_MENU;
    drawMenu();
    return;
  }

  if (gameOver) {
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_WHITE, CYBER_BG);
    tft.setCursor(35, 60);
    tft.print("GAME OVER");
    if (firePressed || encStep != 0) {
      initGame();
      drawAliens();
      drawPlayer();
    }
    return;
  }

  // move player by encoder
  if (encStep != 0) {
    playerX += encStep * 4;  // nhanh tay hơn
    if (playerX < COURT_X + 2) playerX = COURT_X + 2;
    if (playerX + PADDLE_W > COURT_X + COURT_W - 2)
      playerX = COURT_X + COURT_W - 2 - PADDLE_W;
    drawPlayer();
  }

  // fire
  if (firePressed && !bulletActive) {
    bulletActive = true;
    bulletX = playerX + PADDLE_W / 2;
    bulletY = PADDLE_Y - 2;
    tone(BUZZ_PIN, 1800, 40);
  }

  unsigned long now = millis();

  // move aliens
  if (now - lastAlienMoveMs > alienMoveInterval) {
    lastAlienMoveMs = now;
    alienOffsetX += alienDir * 4;

    int leftMost = INT16_MAX;
    int rightMost = INT16_MIN;

    int cellW = COURT_W / ALIEN_COLS;
    for (int c = 0; c < ALIEN_COLS; c++) {
      for (int r = 0; r < ALIEN_ROWS; r++) {
        if (!alienAlive[r][c]) continue;
        int x = alienOffsetX + c * cellW;
        if (x < leftMost) leftMost = x;
        if (x > rightMost) rightMost = x;
      }
    }

    if (leftMost + 4 < 0)  alienDir = 1;
    if (rightMost + 12 > COURT_W) alienDir = -1;

    drawAliens();
  }

  // move bullet
  if (bulletActive && now - lastBulletMoveMs > bulletInterval) {
    lastBulletMoveMs = now;
    bulletY -= 4;
    if (bulletY < COURT_Y + 2) {
      bulletActive = false;
    } else {
      // check collision
      int cellW = COURT_W / ALIEN_COLS;
      int cellH = 12;

      for (int r = 0; r < ALIEN_ROWS; r++) {
        for (int c = 0; c < ALIEN_COLS; c++) {
          if (!alienAlive[r][c]) continue;
          int alienX = COURT_X + 4 + alienOffsetX + c * cellW;
          int alienY = COURT_Y + 4 + r * cellH;
          if (bulletX >= alienX && bulletX <= alienX + 8 &&
              bulletY >= alienY && bulletY <= alienY + 6) {
            alienAlive[r][c] = false;
            bulletActive = false;
            gameScore += 10;
            tone(BUZZ_PIN, 2200, 60);
            drawScore();
          }
        }
      }
    }
    drawBullet();
  }

  // check win / lose
  bool anyAlive = false;
  for (int r = 0; r < ALIEN_ROWS; r++) {
    for (int c = 0; c < ALIEN_COLS; c++) {
      if (alienAlive[r][c]) {
        anyAlive = true;
        int cellH = 12;
        int alienY = COURT_Y + 4 + r * cellH;
        if (alienY + 6 >= PADDLE_Y) {
          gameOver = true;
        }
      }
    }
  }
  if (!anyAlive) {
    gameOver = true;
  }
}

// ========= DVD screensaver =========

bool dvdInited = false;
int  dvdX, dvdY;
int  dvdVX = 2;
int  dvdVY = 1;
int  dvdW  = 50;
int  dvdH  = 18;
unsigned long lastDvdMs = 0;
unsigned long dvdInterval = 35;

uint16_t dvdColors[] = {
  ST77XX_WHITE,
  CYBER_ACCENT,
  CYBER_LIGHT,
  CYBER_GREEN,
  CYBER_PINK,
  ST77XX_YELLOW
};
int dvdColorIndex = 0;

void drawDvdLogo(int x, int y, uint16_t c) {
  // x,y: top-left
  tft.fillRoundRect(x, y, dvdW, dvdH, 4, c);
  tft.drawRoundRect(x, y, dvdW, dvdH, 4, CYBER_BG);

  // chữ DVD ở giữa
  tft.setTextSize(1);
  tft.setTextColor(CYBER_BG, c);
  int16_t bx, by;
  uint16_t w, h;
  tft.getTextBounds("DVD", 0, 0, &bx, &by, &w, &h);
  int tx = x + (dvdW - (int)w) / 2;
  int ty = y + (dvdH - (int)h) / 2 + 1;
  tft.setCursor(tx, ty);
  tft.print("DVD");
}

void initDvd() {
  dvdInited = true;
  tft.fillScreen(CYBER_BG);

  // header
  tft.setTextSize(1);
  tft.setTextColor(CYBER_LIGHT, CYBER_BG);
  tft.setCursor(8, 4);
  tft.print("DVD");

  drawAlarmIcon();

  // vùng chạy logo: chừa header 16px
  dvdX = 40;
  dvdY = 40;
  dvdVX = 2;
  dvdVY = 1;
  lastDvdMs = millis();
  dvdColorIndex = 0;

  drawDvdLogo(dvdX, dvdY, dvdColors[dvdColorIndex]);
}

void updateDvd(int encStep, bool encPressed, bool backPressed) {
  (void)encPressed;

  if (backPressed) {
    dvdInited = false;
    currentMode = MODE_MENU;
    drawMenu();
    return;
  }

  // encoder chỉnh nhẹ tốc độ ngang
  if (encStep != 0) {
    int speed = abs(dvdVX) + encStep;
    if (speed < 1) speed = 1;
    if (speed > 4) speed = 4;
    dvdVX = (dvdVX >= 0 ? 1 : -1) * speed;
  }

  unsigned long now = millis();
  if (now - lastDvdMs < dvdInterval) return;
  lastDvdMs = now;

  // xóa logo cũ
  tft.fillRoundRect(dvdX, dvdY, dvdW, dvdH, 4, CYBER_BG);

  // update vị trí
  dvdX += dvdVX;
  dvdY += dvdVY;

  // biên trong vùng màn
  int left   = 0;
  int right  = 160 - dvdW;
  int top    = 16;
  int bottom = 128 - dvdH;

  bool hitX = false;
  bool hitY = false;

  if (dvdX <= left) {
    dvdX = left;
    dvdVX = -dvdVX;
    hitX = true;
  } else if (dvdX >= right) {
    dvdX = right;
    dvdVX = -dvdVX;
    hitX = true;
  }

  if (dvdY <= top) {
    dvdY = top;
    dvdVY = -dvdVY;
    hitY = true;
  } else if (dvdY >= bottom) {
    dvdY = bottom;
    dvdVY = -dvdVY;
    hitY = true;
  }

  // nếu đập vào góc (cả X & Y cùng va chạm) => đổi màu
  if (hitX && hitY) {
    dvdColorIndex++;
    if (dvdColorIndex >= (int)(sizeof(dvdColors) / sizeof(dvdColors[0]))) {
      dvdColorIndex = 0;
    }
    tone(BUZZ_PIN, 1500, 80);
  }

  drawDvdLogo(dvdX, dvdY, dvdColors[dvdColorIndex]);
}

// ========= SETUP =========
void setup() {
  Serial.begin(115200);
  delay(1500);

  pinMode(ENC_A_PIN,   INPUT_PULLUP);
  pinMode(ENC_B_PIN,   INPUT_PULLUP);
  pinMode(ENC_BTN_PIN, INPUT_PULLUP);
  pinMode(KEY0_PIN,    INPUT_PULLUP);

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZ_PIN, OUTPUT);

  Wire.begin(SDA_PIN, SCL_PIN);
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(CYBER_BG);

  connectWiFiAndSyncTime(true);
  lastSyncMs = millis();

  if (!aht.begin()) Serial.println("AHT21 not found");
  if (!ens160.begin()) Serial.println("ENS160 begin FAIL");
  else ens160.setMode(ENS160_OPMODE_STD);

  updateEnvSensors(true);

  // khởi động vào Monitor (clock)
  initClockStaticUI();
  prevTimeStr = "";
  drawClockTime(getTimeStr('H'), getTimeStr('M'), getTimeStr('S'));
  drawEnvDynamic(curTemp, curHum, curTVOC, curECO2);
}

// ========= LOOP =========
void loop() {
  //Removed this code because it was always checking for the wifi to be connected
  /* 
  static unsigned long lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 10000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.begin(ssid, password);
    }
  }*/

  //Added to synctime on period chosen in SYNC_EVERY_MS
  unsigned long nowMs = millis();

  if (nowMs - lastSyncMs >= SYNC_EVERY_MS) {
    bool ok = connectWiFiAndSyncTime(false);
    lastSyncMs = nowMs; // or only update if ok, your choice
  }

  int encStep     = readEncoderStep();
  bool encPressed = checkButtonPressed(ENC_BTN_PIN, lastEncBtn);
  bool k0Pressed  = checkButtonPressed(KEY0_PIN,    lastKey0);

  checkAlarmTrigger();
  updateAlertStateAndLED();

  switch (currentMode) {
    case MODE_MENU: {
      if (encStep != 0) {
        menuIndex += encStep;
        if (menuIndex < 0) menuIndex = MENU_ITEMS - 1;
        if (menuIndex >= MENU_ITEMS) menuIndex = 0;
        drawMenu();
      }
      if (encPressed) {
        if (menuIndex == 0) {
          currentMode = MODE_CLOCK;
          initClockStaticUI();
          prevTimeStr = "";
          updateEnvSensors(true);
          drawClockTime(getTimeStr('H'), getTimeStr('M'), getTimeStr('S'));
          drawEnvDynamic(curTemp, curHum, curTVOC, curECO2);
        } else if (menuIndex == 1) {
          currentMode = MODE_POMODORO;
          pomoState = POMO_SELECT;
          prevPomoRemainSec = -1;
          prevPomoPreset = -1;
          prevPomoStateInt = -1;
          drawPomodoroScreen(true);
        } else if (menuIndex == 2) {
          currentMode = MODE_ALARM;
          alarmSelectedField = 0;
          drawAlarmScreen(true);
        } else if (menuIndex == 3) {
          currentMode = MODE_DVD;
          dvdInited = false;
        } else if (menuIndex == 4) {
          currentMode = MODE_GAME;
          gameInited = false;
        }
      }
      break;
    }

    case MODE_CLOCK: {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        int sec = timeinfo.tm_sec;
        if (sec != prevSecond) {
          prevSecond = sec;
          drawClockTime(getTimeStr('H'), getTimeStr('M'), getTimeStr('S'));
          if (sec % 5 == 0) {
            updateEnvSensors(true);
            drawEnvDynamic(curTemp, curHum, curTVOC, curECO2);
          }
        }
      }
      if (k0Pressed) {
        currentMode = MODE_MENU;
        drawMenu();
      }
      break;
    }

    case MODE_POMODORO: {
      if (pomoState == POMO_SELECT && encStep != 0) {
        pomoPresetIndex += encStep;
        if (pomoPresetIndex < 0) pomoPresetIndex = 2;
        if (pomoPresetIndex > 2) pomoPresetIndex = 0;
        prevPomoRemainSec = -1;
        drawPomodoroScreen(true);
      }

      if (encPressed) {
        if (pomoState == POMO_SELECT || pomoState == POMO_DONE) {
          pomoState = POMO_RUNNING;
          pomoStartMillis = millis();
          prevPomoRemainSec = -1;
          drawPomodoroScreen(true);
        } else if (pomoState == POMO_RUNNING) {
          pomoState = POMO_PAUSED;
          pomoPausedMillis = millis();
          drawPomodoroScreen(true);
        } else if (pomoState == POMO_PAUSED) {
          unsigned long pauseDur = millis() - pomoPausedMillis;
          pomoStartMillis += pauseDur;
          pomoState = POMO_RUNNING;
          prevPomoRemainSec = -1;
          drawPomodoroScreen(true);
        }
      }

      if (pomoState == POMO_RUNNING || pomoState == POMO_PAUSED || pomoState == POMO_DONE) {
        unsigned long durationMs = pomoDurationsMin[pomoPresetIndex] * 60UL * 1000UL;
        unsigned long elapsed = 0;
        if (pomoState == POMO_RUNNING) elapsed = millis() - pomoStartMillis;
        else if (pomoState == POMO_PAUSED) elapsed = pomoPausedMillis - pomoStartMillis;
        else if (pomoState == POMO_DONE) elapsed = durationMs;

        if (elapsed > durationMs) {
          elapsed = durationMs;
          if (pomoState != POMO_DONE) {
            pomoState = POMO_DONE;
            tone(BUZZ_PIN, 2000, 500);
            drawPomodoroScreen(true);
          }
        }

        int remainSec = (durationMs - elapsed) / 1000UL;
        if (remainSec != prevPomoRemainSec) {
          prevPomoRemainSec = remainSec;
          drawPomodoroScreen(false);
        }
      }

      if (k0Pressed) {
        currentMode = MODE_MENU;
        drawMenu();
      }
      break;
    }

    case MODE_ALARM: {
      if (alarmRinging) {
        static unsigned long lastBeep = 0;
        if (millis() - lastBeep > 1000) {
          lastBeep = millis();
          tone(BUZZ_PIN, 2000, 400);
        }
        if (encPressed || k0Pressed) {
          alarmRinging = false;
          lastAlarmDayTriggered = -1;
          noTone(BUZZ_PIN);
          drawAlarmScreen(true);
        }
        break;
      }

      bool changed = false;
      if (encStep != 0) {
        if (alarmSelectedField == 0) {
          if (encStep > 0) alarmHour = (alarmHour + 1) % 24;
          else             alarmHour = (alarmHour + 23) % 24;
          changed = true;
        } else if (alarmSelectedField == 1) {
          if (encStep > 0) alarmMinute = (alarmMinute + 1) % 60;
          else             alarmMinute = (alarmMinute + 59) % 60;
          changed = true;
        } else if (alarmSelectedField == 2) {
          alarmEnabled = !alarmEnabled;
          changed = true;
        }

        if (changed) {
          lastAlarmDayTriggered = -1;
        }
      }

      if (encPressed) {
        alarmSelectedField = (alarmSelectedField + 1) % 3;
        changed = true;
      }

      if (k0Pressed) {
        currentMode = MODE_MENU;
        drawMenu();
        break;
      }

      if (changed) {
        drawAlarmScreen(false);
        drawAlarmIcon();
      }

      break;
    }

    case MODE_DVD: {
      if (!dvdInited) {
        initDvd();
      }
      updateDvd(encStep, encPressed, k0Pressed);
      break;
    }

    case MODE_GAME: {
      if (!gameInited) {
        initGame();
        drawAliens();
        drawPlayer();
      }
      updateGameLogic(encStep, encPressed, k0Pressed);
      break;
    }
  }
}