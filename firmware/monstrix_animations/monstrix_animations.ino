/*
 * MONSTRIX Robot v2.13 - Animations + Day/Night + Web Remote + PlayStation + Hero FX
 * ESP32-C3 + OLED 0.96" (+ optional MPU6050)
 * Uses FluxGarage_RoboEyes v1.1.x
 *
 * Based on the MONSTRIX firmware by Igor Fedorov (Belyiiii),
 *   https://github.com/Belyiiii/monstrix-cod - CC BY-NC 4.0
 * Changes: rewritten and extended (animation engine, effects, web remote,
 *   day/night, PlayStation presence, wireless updates, two-robot support).
 * This file is shared under CC BY-NC 4.0 (non-commercial). See LICENSE and CREDITS.md.
 * The RoboEyes library (GPL-3.0, Dennis Hoelscher) is NOT included here:
 *   install it from the Arduino Library Manager.
 *
 * Gets the time over Wi-Fi, works out today's sunset and sunrise for your
 * location, and only plays sleep animations at night.
 *
 * TESTING: open Serial Monitor (115200 baud), type a key and press Enter.
 * Type ? to print the full list of keys.
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <time.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <HTTPUpdateServer.h>
#include <ArduinoJson.h>   // install "ArduinoJson" by Benoit Blanchon (v7) in Library Manager

// The eyes library MUST be included last: it defines one-letter names
// (N, E, S, W...) for eye directions that break the Wi-Fi security and
// JSON libraries if they're loaded after it.
#include <FluxGarage_RoboEyes.h>

// Forward declarations (so functions can be used before they're defined)
bool playKey(char key);
void endAnimation();
void printMenu();
void printStatus();
void connectWiFi();
void updateDayNight();
void startPsn();
void handlePsnEvents();
void pickNextBehaviour();
void pickGamingBehaviour();
void startWebServer();
void maintainWiFi();
void loadIdentity();
void startOta();
void announceOnNetwork();
void printNetwork();
String jsonEscape(const String& s);
void updateBrightness();
bool shouldDim();
void setPanel(int level, bool deep);
bool canPickNow(int i);

// ================== YOUR SETTINGS ==================
// Wi-Fi name and password live in secrets.h (same folder as this sketch),
// so you can replace this file with new versions without retyping them.
#include "secrets.h"

// Your location, used to calculate sunset and sunrise. Set it in secrets.h;
// these are only fallbacks (Sacramento area) if secrets.h doesn't say.
#ifndef MY_LATITUDE
#define MY_LATITUDE  38.58
#endif
#ifndef MY_LONGITUDE
#define MY_LONGITUDE -121.49           // west is negative
#endif
const float LATITUDE  = MY_LATITUDE;
const float LONGITUDE = MY_LONGITUDE;

// ---- Which robot is this? ----
// Both robots run this same sketch. Each board remembers its own identity
// (saved on the board the first time it runs this version), so you can send
// updates to either robot without changing anything in secrets.h.
#ifndef WHICH_ROBOT
#define WHICH_ROBOT 1
#endif
#ifndef SET_IDENTITY_ON_UPLOAD
#define SET_IDENTITY_ON_UPLOAD 0   // 1 = make this board take WHICH_ROBOT, replacing what it remembers
#endif
#ifndef OTA_PASSWORD
#define OTA_PASSWORD "monstrix-update"
#endif
struct RobotProfile { const char* name; const char* host; int rotation; };
const RobotProfile PROFILES[] = {
  { "Monstrix", "monstrix", 0 },   // 1
  { "Nova",     "nova",     2 },   // 2 (screen mounted upside down)
};
const int NUM_PROFILES = sizeof(PROFILES) / sizeof(PROFILES[0]);
int robotId = WHICH_ROBOT;
const char* robotName = PROFILES[0].name;
const char* robotHost = PROFILES[0].host;
int robotRotation = 0;
// Web page address on your Wi-Fi: http://<robotHost>.local
const char* HOSTNAME = PROFILES[0].host;

// Pacific time with automatic daylight saving
const char* TIMEZONE = "PST8PDT,M3.2.0,M11.1.0";

// Start "night" this many minutes after sunset (0 = right at sunset)
const int NIGHT_START_OFFSET_MIN = 0;
// End "night" this many minutes after sunrise
const int NIGHT_END_OFFSET_MIN = 0;

// PlayStation: how often to check what you're playing (seconds)
const int PSN_POLL_SECONDS = 15;   // PSN allows ~300 requests per 15 min; this uses about 60
// How often to check for new trophies while you're in a game (seconds)
const int PSN_TROPHY_SECONDS = 120;
// After this many minutes in one session he starts looking tired
const int PSN_TIRED_AFTER_MIN = 120;
// ...and only late at night: from this time until 6 AM (22:30 = 10:30 PM)
const int TIRED_FROM_HOUR = 22, TIRED_FROM_MIN = 30;

// Night dimming: after this time (until 6 AM) his eyes dim, but only when
// you're not gaming. While you're playing he stays at full brightness.
const int DIM_FROM_HOUR = 22, DIM_FROM_MIN = 30;
const int DIM_UNTIL_HOUR = 6;
const int BRIGHT_LEVEL = 0xCF;   // normal brightness (0-255)
const int DIM_LEVEL = 4;         // night brightness (0-255, lower = dimmer)
// ====================================================

// Set to 1 if you install the MPU6050 gyro later
#define USE_GYRO 0

#if USE_GYRO
#include <MPU6050_light.h>
#endif

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C

// Default eye size (RoboEyes defaults) - used to restore after animations
#define EYE_W 36
#define EYE_H 36

// Eye centres for the custom-drawn faces
#define LEFT_X 41
#define RIGHT_X 87
#define EYE_Y 32

// The display, with one extra trick: just before each frame is sent to the
// screen, beforeShow() can add the hero mask on top of the eyes.
void beforeShow();
class MonstrixDisplay : public Adafruit_SSD1306 {
 public:
  using Adafruit_SSD1306::Adafruit_SSD1306;
  void display() { beforeShow(); Adafruit_SSD1306::display(); }
};
MonstrixDisplay display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
RoboEyes<MonstrixDisplay> eyes(display);

#if USE_GYRO
MPU6050 mpu(Wire);
#endif

// ---------- Timing ----------
unsigned long lastEmotionChange = 0;
unsigned long lastAngryTime = 0;
const unsigned long EMOTION_INTERVAL = 6000;  // time between moods/animations
const unsigned long ANGRY_DURATION = 3000;
bool isAngry = false;

// ---------- Day/night state ----------
bool timeValid = false;         // true once the clock has synced over Wi-Fi
bool isNight = false;
int sunriseMin = 0;             // minutes after local midnight
int sunsetMin = 0;
int sunCalcDay = -1;            // day of year the sun times were calculated for
unsigned long lastTimeCheck = 0;
// Serial test override: 0 = automatic, 1 = force night, 2 = force day
byte nightOverride = 0;

// ---------- Custom-drawn faces ----------
// While one of these is active, the eyes library is paused and we draw
// the screen ourselves.
enum CustomFace { FACE_NONE, FACE_HEART, FACE_X, FACE_HAPPY_ARCS, FACE_GOOGLY };
CustomFace customFace = FACE_NONE;
unsigned long lastCustomFrame = 0;

// Snoring: freezes the eyes as they are (1-pixel lines) and draws
// floating Z's in the top-right corner
bool snoring = false;
unsigned long lastSnoreFrame = 0;

// FX pack: drawn hero effects. An effect lasts for the step that starts it.
enum FxType { FX_NONE, FX_CHARGE, FX_FLASH, FX_SCAN, FX_TEXT, FX_BURST, FX_HEAL, FX_STEAM, FX_DASH,
             FX_POPUP, FX_SPIN, FX_PEDESTAL, FX_BIGCUP, FX_FIREWORKS, FX_TICKER };
char fxText[13] = "";           // lettering for text effects
FxType fxType = FX_NONE;
void startFx(FxType type, const char* text);
void stopFx();
unsigned long fxStart = 0, fxDur = 1;
bool maskOn = false;            // hero domino mask over the eyes

// Animations queued to play one after another (e.g. the Marvel Rivals intro)
char chainQueue[8] = "";

// Googly eye pupil positions
int pupilLX = 0, pupilLY = 0, pupilRX = 0, pupilRY = 0;
unsigned long nextPupilL = 0, nextPupilR = 0;

// ---------- Animation engine ----------
// Each animation runs one step at a time without blocking.
// A step does something and returns how many ms to wait before the next
// step, or -1 when the animation is finished.
typedef int (*AnimFn)(int step);

AnimFn currentAnim = nullptr;
const char* currentAnimName = "";
int animStep = 0;
unsigned long nextStepTime = 0;

void startAnimation(AnimFn fn, const char* name) {
  // Pause automatic behaviour so it doesn't fight the animation
  eyes.setIdleMode(OFF, 2, 1);
  eyes.setAutoblinker(OFF, 3, 1);
  currentAnim = fn;
  currentAnimName = name;
  animStep = 0;
  nextStepTime = millis();
  Serial.print("Animation: ");
  Serial.println(name);
}

void endAnimation() {
  // Put everything back to normal
  customFace = FACE_NONE;
  snoring = false;
  fxType = FX_NONE;
  maskOn = false;
  eyes.setHFlicker(OFF, 0);
  eyes.setVFlicker(OFF, 0);
  eyes.setCuriosity(OFF);
  eyes.setCyclops(OFF);
  eyes.setSweat(OFF);
  eyes.setWidth(EYE_W, EYE_W);
  eyes.setHeight(EYE_H, EYE_H);
  eyes.setPosition(DEFAULT);
  eyes.setMood(DEFAULT);
  eyes.open();
  eyes.setIdleMode(ON, 2, 1);
  eyes.setAutoblinker(ON, 3, 1);
  currentAnim = nullptr;
  currentAnimName = "";
  lastEmotionChange = millis();
}

void updateAnimation() {
  if (currentAnim == nullptr || millis() < nextStepTime) return;
  fxType = FX_NONE;                       // an effect lasts only for its own step
  int wait = currentAnim(animStep++);
  if (wait < 0) { endAnimation(); return; }
  nextStepTime = millis() + wait;
  if (fxType != FX_NONE) { fxStart = millis(); fxDur = wait > 0 ? wait : 1; }
}

void startFx(FxType type, const char* text) {
  fxType = type;
  strncpy(fxText, text ? text : "", sizeof(fxText) - 1);
  fxText[sizeof(fxText) - 1] = 0;
}
void stopFx() { fxType = FX_NONE; }

// ---------- Drawing helpers for custom faces ----------

// A 3-pixel-thick line
void drawThickLine(int x0, int y0, int x1, int y1) {
  for (int o = -1; o <= 1; o++) {
    display.drawLine(x0 + o, y0, x1 + o, y1, SSD1306_WHITE);
    display.drawLine(x0, y0 + o, x1, y1 + o, SSD1306_WHITE);
  }
}

void drawHeart(int cx, int cy, int r) {
  display.fillCircle(cx - r + 1, cy - 4, r, SSD1306_WHITE);
  display.fillCircle(cx + r - 1, cy - 4, r, SSD1306_WHITE);
  display.fillTriangle(cx - 2 * r + 2, cy - 1,
                       cx + 2 * r - 2, cy - 1,
                       cx, cy + 2 * r, SSD1306_WHITE);
}

void drawX(int cx, int cy) {
  drawThickLine(cx - 12, cy - 12, cx + 12, cy + 12);
  drawThickLine(cx + 12, cy - 12, cx - 12, cy + 12);
}

void drawHappyArc(int cx, int cy) {   // a ^ shape
  drawThickLine(cx - 14, cy + 6, cx, cy - 8);
  drawThickLine(cx, cy - 8, cx + 14, cy + 6);
}

void drawGooglyEye(int cx, int cy, int px, int py) {
  display.fillCircle(cx, cy, 17, SSD1306_WHITE);           // eyeball
  display.fillCircle(cx + px, cy + py, 7, SSD1306_BLACK);  // pupil
}

void updateGooglyPupils() {
  unsigned long now = millis();
  // Each pupil jumps to a random spot at its own random times
  if (now > nextPupilL) {
    pupilLX = random(-9, 10); pupilLY = random(-9, 10);
    nextPupilL = now + random(120, 400);
  }
  if (now > nextPupilR) {
    pupilRX = random(-9, 10); pupilRY = random(-9, 10);
    nextPupilR = now + random(120, 400);
  }
}

void drawCustomFace() {
  if (millis() - lastCustomFrame < 33) return;   // ~30 fps
  lastCustomFrame = millis();

  display.clearDisplay();
  switch (customFace) {
    case FACE_HEART: {
      int r = ((millis() / 300) % 2) ? 9 : 8;   // heartbeat pulse
      drawHeart(LEFT_X, EYE_Y, r);
      drawHeart(RIGHT_X, EYE_Y, r);
      break;
    }
    case FACE_X:
      drawX(LEFT_X, EYE_Y);
      drawX(RIGHT_X, EYE_Y);
      break;
    case FACE_HAPPY_ARCS:
      drawHappyArc(LEFT_X, EYE_Y);
      drawHappyArc(RIGHT_X, EYE_Y);
      break;
    case FACE_GOOGLY:
      updateGooglyPupils();
      drawGooglyEye(LEFT_X, EYE_Y, pupilLX, pupilLY);
      drawGooglyEye(RIGHT_X, EYE_Y, pupilRX, pupilRY);
      break;
    default:
      break;
  }
  display.display();
}

// Floating Z's. Only the top-right corner is redrawn, so the sleeping
// eyes the library last drew stay exactly where they are.
void drawSnore() {
  if (millis() - lastSnoreFrame < 50) return;   // ~20 fps
  lastSnoreFrame = millis();

  display.fillRect(88, 0, 40, 27, SSD1306_BLACK);   // clear the Z area
  display.setTextColor(SSD1306_WHITE);

  float t = (millis() % 2400) / 2400.0;   // one Z cycle every 2.4 s
  for (int i = 0; i < 3; i++) {
    float p = t + i / 3.0;                // three Z's, evenly staggered
    if (p >= 1.0) p -= 1.0;
    int x = 92 + (int)(p * 22);           // drift up and to the right
    int y = 19 - (int)(p * 19);
    display.setTextSize(p > 0.6 ? 2 : 1); // grow as they rise
    display.setCursor(x, y);
    display.print('Z');
  }
  display.setTextSize(1);
  display.display();
}

// ---------- FX pack ----------
const byte BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
uint8_t eyeBuf[SCREEN_WIDTH * SCREEN_HEIGHT];
uint8_t tmpBuf[SCREEN_WIDTH * SCREEN_HEIGHT];
uint8_t haloBuf[SCREEN_WIDTH * SCREEN_HEIGHT];
GFXcanvas1 maskCanvas(SCREEN_WIDTH, SCREEN_HEIGHT);

// Raw screen-memory access in screen coordinates (follows the robot's rotation)
inline void rotXY(int& x, int& y) {
  if (robotRotation == 2) { x = SCREEN_WIDTH - 1 - x; y = SCREEN_HEIGHT - 1 - y; }
}
inline bool bufPx(const uint8_t* b, int x, int y) { rotXY(x, y); return b[x + (y >> 3) * SCREEN_WIDTH] & (1 << (y & 7)); }
inline void setBufPx(uint8_t* b, int x, int y, bool on) {
  rotXY(x, y);
  uint8_t bit = 1 << (y & 7);
  if (on) b[x + (y >> 3) * SCREEN_WIDTH] |= bit; else b[x + (y >> 3) * SCREEN_WIDTH] &= ~bit;
}

// Draws a domino hero mask around whatever is on screen (the eyes),
// keeping a 2-pixel gap around them. left/top/right/h = the eyes' box.
void maskComposite(int left, int top, int right, int h) {
  uint8_t* b = display.getBuffer();
  const int SCR_W = SCREEN_WIDTH, SCR_H = SCREEN_HEIGHT;
  for (int y = 0; y < SCR_H; y++) for (int x = 0; x < SCR_W; x++) eyeBuf[y * SCR_W + x] = bufPx(b, x, y);
  for (int y = 0; y < SCR_H; y++) for (int x = 0; x < SCR_W; x++) {          // grow eyes by 2 px (halo)
    uint8_t v = 0;
    for (int dx = -2; dx <= 2 && !v; dx++) { int xx = x + dx; if (xx >= 0 && xx < SCR_W) v = eyeBuf[y * SCR_W + xx]; }
    tmpBuf[y * SCR_W + x] = v;
  }
  for (int y = 0; y < SCR_H; y++) for (int x = 0; x < SCR_W; x++) {
    uint8_t v = 0;
    for (int dy = -2; dy <= 2 && !v; dy++) { int yy = y + dy; if (yy >= 0 && yy < SCR_H) v = tmpBuf[yy * SCR_W + x]; }
    haloBuf[y * SCR_W + x] = v;
  }
  int cy = top + h / 2, bt = cy - 15, bl = left - 8, br = right + 8;
  maskCanvas.fillScreen(0);
  maskCanvas.fillRoundRect(bl, bt, br - bl, 30, 12, 1);
  maskCanvas.fillTriangle(bl + 6, bt + 2, bl - 9, bt - 7, bl + 2, bt + 16, 1);   // mask tips
  maskCanvas.fillTriangle(br - 6, bt + 2, br + 9, bt - 7, br - 2, bt + 16, 1);
  for (int y = 0; y < SCR_H; y++) for (int x = 0; x < SCR_W; x++) {
    bool on = eyeBuf[y * SCR_W + x] || (maskCanvas.getPixel(x, y) && !haloBuf[y * SCR_W + x]);
    setBufPx(b, x, y, on);
  }
}

// Called just before every frame goes to the screen
void beforeShow() {
  if (!maskOn || fxType != FX_NONE || customFace != FACE_NONE || snoring) return;
  uint8_t* b = display.getBuffer();
  int minX = SCREEN_WIDTH, minY = SCREEN_HEIGHT, maxX = -1, maxY = -1;
  for (int y = 0; y < SCREEN_HEIGHT; y++) for (int x = 0; x < SCREEN_WIDTH; x++) if (bufPx(b, x, y)) {
    if (x < minX) minX = x; if (x > maxX) maxX = x; if (y < minY) minY = y; if (y > maxY) maxY = y;
  }
  if (maxX < 0) return;
  maskComposite(minX, minY, maxX + 1, maxY - minY + 1);
}

// ---- Trophy celebration helpers (same drawings as the Eye Lab) ----
float easeOutF(float x) { return 1 - pow(1 - x, 3); }
float easeInF(float x) { return x * x * x; }
float clamp01F(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

void sparkleAt(int x, int y, int s) {
  display.drawFastHLine(x - s, y, 2 * s + 1, SSD1306_WHITE);
  display.drawFastVLine(x, y - s, 2 * s + 1, SSD1306_WHITE);
}

// A trophy cup, 32 x 23 pixels at scale 1, top-centred at (cx, top)
void drawCup(Adafruit_GFX& g, int cx, int top, float sc) {
  auto k = [&](float v) { return (int)(v * sc); };
  for (int r = k(3); r <= k(4); r++) { g.drawCircle(cx - k(12), top + k(4), r, 1); g.drawCircle(cx + k(12), top + k(4), r, 1); }
  g.fillTriangle(cx - k(11), top, cx + k(11), top, cx + k(5), top + k(11), 1);
  g.fillTriangle(cx - k(11), top, cx + k(5), top + k(11), cx - k(5), top + k(11), 1);
  g.fillCircle(cx, top + k(8), k(6), 1);
  g.fillRect(cx - k(2), top + k(12), k(5), k(5), 1);
  g.fillRect(cx - k(7), top + k(17), k(15), k(3), 1);
  g.fillRect(cx - k(9), top + k(20), k(19), k(3), 1);
  int sy = top + k(5), arm = sc > 1 ? 2 : 1;            // star emblem
  for (int i = -arm; i <= arm; i++) { g.drawPixel(cx + i, sy, 0); g.drawPixel(cx, sy + i, 0); }
}

// Diagonal glint sweeping across whatever is lit inside a box
void shineBox(int x0, int y0, int w, int h, float phase, int width) {
  float d = -h + phase * (w + 2 * h);
  for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) {
    float v = (x - x0) + (y - y0) - d;
    if (v >= 0 && v < width && x >= 0 && y >= 0 && x < SCREEN_WIDTH && y < SCREEN_HEIGHT && display.getPixel(x, y))
      display.drawPixel(x, y, SSD1306_BLACK);
  }
}

// Big trophy (scale 2) drawn scaled around (cx, cy)
void drawScaledCup(int cx, int cy, float kx, float ky) {
  maskCanvas.fillScreen(0);
  drawCup(maskCanvas, 64, 9, 2);
  if (kx < 0.01) kx = 0.01; if (ky < 0.01) ky = 0.01;
  for (int y = 0; y < SCREEN_HEIGHT; y++) for (int x = 0; x < SCREEN_WIDTH; x++) {
    int sx = (int)(cx + (x - cx) / kx), sy = (int)(cy + (y - cy) / ky);
    if (sx >= 0 && sy >= 0 && sx < SCREEN_WIDTH && sy < SCREEN_HEIGHT && maskCanvas.getPixel(sx, sy))
      display.drawPixel(x, y, SSD1306_WHITE);
  }
}

void drawRays(int cx, int cy, float rot) {
  for (int i = 0; i < 12; i++) {
    float a = rot + i * PI / 6;
    display.fillTriangle(cx, cy, cx + cos(a) * 100, cy + sin(a) * 100,
                         cx + cos(a + 0.16) * 100, cy + sin(a + 0.16) * 100, SSD1306_WHITE);
  }
}

void drawConfetti(int n, float speed) {
  double ms = millis();
  for (int i = 0; i < n; i++) {
    int x = (i * 37 + (int)(sin(ms / 300.0 + i) * 3) + 256) % SCREEN_WIDTH;
    int y = (int)fmod(ms * speed / (8 + i % 5) + i * 23, 72) - 6;
    int flip = ((unsigned long)(ms / 200) + i) % 3;
    if (flip == 0) display.fillRect(x, y, 2, 1, SSD1306_WHITE);
    else if (flip == 1) display.fillRect(x, y, 1, 2, SSD1306_WHITE);
    else display.drawPixel(x, y, SSD1306_WHITE);
  }
}

void drawFireworks(Adafruit_GFX& g, int count) {
  const int xs[5] = {20, 64, 108, 40, 90}, tops[5] = {16, 10, 18, 22, 12};
  for (int i = 0; i < count; i++) {
    float u = ((millis() + i * 230) % 1100) / 1100.0;
    int x = xs[i], yb = tops[i];
    if (u < 0.4) g.drawFastVLine(x, (int)(64 - (64 - yb) * (u / 0.4)), 4, 1);
    else if (u < 0.92) {
      float rr = 2 + 14 * easeOutF((u - 0.4) / 0.52);
      for (int k = 0; k < 10; k++) {
        float a = k * PI / 5;
        g.drawPixel(x + cos(a) * rr, yb + sin(a) * rr, 1);
        if (u < 0.7) g.drawPixel(x + cos(a) * (rr - 2), yb + sin(a) * (rr - 2), 1);
      }
    }
  }
}

void drawEyePairX(int cx, int cy, int w, int h, int gap, int r) {
  display.fillRoundRect(cx - gap / 2 - w, cy - h / 2, w, h, r, SSD1306_WHITE);
  display.fillRoundRect(cx + gap / 2, cy - h / 2, w, h, r, SSD1306_WHITE);
}

void drawEyePair(int cx, int cy, int w, int h) {
  display.fillRoundRect(cx - 5 - w, cy - h / 2, w, h, 8, SSD1306_WHITE);
  display.fillRoundRect(cx + 5, cy - h / 2, w, h, 8, SSD1306_WHITE);
}

float easeBackF(float t) { const float c1 = 1.7, c3 = c1 + 1; return 1 + c3 * pow(t - 1, 3) + c1 * pow(t - 1, 2); }

void drawFx() {
  if (millis() - lastCustomFrame < 33) return;   // ~30 fps
  lastCustomFrame = millis();
  float t = (millis() - fxStart) / (float)fxDur;
  if (t > 1) t = 1;
  display.clearDisplay();

  switch (fxType) {
    case FX_CHARGE: {                // energy pulls in to a growing orb
      float rot = millis() / 400.0;
      for (int i = 0; i < 16; i++) {
        float a = i * PI / 8 + rot;
        float ro = 80 * (1 - 0.75 * t) - (i % 3) * 6, ri = ro - 10;
        if (ri > 6) display.drawLine(64 + cos(a) * ro, 32 + sin(a) * ro * 0.6,
                                     64 + cos(a) * ri, 32 + sin(a) * ri * 0.6, SSD1306_WHITE);
      }
      int r = (int)(2 + 13 * pow(t, 1.4)) + ((((millis() / 60) % 2) && t > 0.8) ? 1 : 0);
      display.fillCircle(64, 32, r, SSD1306_WHITE);
      if (t > 0.3) display.drawCircle(64, 32, r + 4 + (millis() / 120) % 3, SSD1306_WHITE);
      break;
    }
    case FX_FLASH: {                 // white flash that dithers away
      float th = (1 - t) * 16;
      for (int y = 0; y < SCREEN_HEIGHT; y++) for (int x = 0; x < SCREEN_WIDTH; x++)
        if (BAYER[(y & 3) * 4 + (x & 3)] < th) display.drawPixel(x, y, SSD1306_WHITE);
      break;
    }
    case FX_BURST: {                 // comic starburst with lettering
      float g = t / 0.3; if (g > 1) g = 1;
      float grow = easeBackF(g);
      bool alt = (millis() / 90) % 2;
      float R = 29 * grow + (alt ? 1.5 : 0); if (R < 0) R = 0;
      float rot = alt ? 0.08 : 0;
      float bx[28], by[28];
      for (int i = 0; i < 28; i++) {
        float a = i * PI / 14 + rot, rad = (i % 2) ? R * 0.64 : R;
        bx[i] = 64 + cos(a) * rad * 2.05; by[i] = 32 + sin(a) * rad;
      }
      for (int i = 0; i < 28; i++)
        display.fillTriangle(64, 32, bx[i], by[i], bx[(i + 1) % 28], by[(i + 1) % 28], SSD1306_WHITE);
      if (t > 0.25 && t < 0.75) for (int i = 0; i < 10; i++) {   // action lines
        float a = i * PI / 5 + 0.3;
        display.drawLine(64 + cos(a) * R * 2.3, 32 + sin(a) * R * 1.15,
                         64 + cos(a) * R * 3, 32 + sin(a) * R * 1.5, SSD1306_WHITE);
      }
      int len = strlen(fxText);
      if (grow > 0.55 && len) {
        int z = 3;
        while (z > 1 && len * 6 * z - z > 84) z--;
        int w = len * 6 * z - z;
        display.setTextSize(z);
        display.setTextColor(SSD1306_BLACK);
        display.setCursor(64 - w / 2, 32 - (7 * z) / 2);
        display.print(fxText);
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
      }
      break;
    }
    case FX_HEAL: {                  // plus signs float up around happy eyes
      drawEyePair(64, 34, 34, 28);
      display.fillRoundRect(64 - 5 - 34 - 1, 34 + 14 - 10, 36, 28, 8, SSD1306_BLACK);
      display.fillRoundRect(64 + 5 - 1, 34 + 14 - 10, 36, 28, 8, SSD1306_BLACK);
      const int xs[6] = {8, 20, 108, 120, 14, 114};
      for (int i = 0; i < 6; i++) {
        float ph = fmod(millis() / 1400.0 + i / 6.0, 1.0);
        int x = xs[i], y = (int)(62 - ph * 66), sz = ph > 0.5 ? 3 : 2, big = sz > 2 ? 1 : 0;
        display.fillRect(x - sz, y, 2 * sz + 1, 1 + big, SSD1306_WHITE);
        display.fillRect(x, y - sz, 1 + big, 2 * sz + 1, SSD1306_WHITE);
      }
      break;
    }
    case FX_STEAM: {                 // fuming: angry eyes with steam puffs
      drawEyePair(64, 34, 34, 30);
      int lx = 64 - 5 - 34, rx = 64 + 5, ty = 34 - 15;
      display.fillTriangle(lx, ty - 1, lx + 34, ty - 1, lx + 34, ty + 13, SSD1306_BLACK);
      display.fillTriangle(rx, ty - 1, rx + 34, ty - 1, rx, ty + 13, SSD1306_BLACK);
      for (int i = 0; i < 4; i++) {
        float ph = fmod(millis() / 900.0 + i / 4.0, 1.0);
        int side = (i % 2) ? 1 : -1;
        if (ph < 0.85) display.drawCircle(64 + side * (54 + ph * 6), (int)(16 - ph * 18), (int)(1 + ph * 4), SSD1306_WHITE);
      }
      break;
    }
    case FX_DASH: {                  // speed streaks behind him
      int ox = (int)(sin(millis() / 120.0) * 2);
      for (int i = 0; i < 7; i++) {
        int y = 16 + i * 5, off = ((millis() / 25) + i * 13) % 40, len = 8 + (i * 5) % 14;
        display.drawFastHLine(20 - off - len + ox, y, len, SSD1306_WHITE);
      }
      drawEyePair(70 + ox, 32, 34, 30);
      break;
    }
    case FX_POPUP: {                 // trophy notification slides down
      int y = 4;
      if (t < 0.18) y = (int)(-28 + 32 * easeOutF(t / 0.18));
      else if (t > 0.85) y = (int)(4 - 32 * easeInF((t - 0.85) / 0.15));
      drawEyePairX(64, 48, 22, 14, 8, 5);              // he looks up at it
      display.fillRoundRect(8, y, 112, 26, 5, SSD1306_WHITE);
      display.fillRoundRect(10, y + 2, 108, 22, 4, SSD1306_BLACK);
      drawCup(display, 26, y + 2, 1);
      if (t > 0.25 && t < 0.6) shineBox(10, y + 2, 32, 23, (t - 0.25) / 0.35, 3);
      int len = strlen(fxText), z = 2;
      while (z > 1 && len * 6 * z - z > 72) z--;
      display.setTextSize(z);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(46, y + 13 - (7 * z) / 2);
      display.print(fxText);
      display.setTextSize(1);
      const int sx[6] = {12, 116, 30, 98, 8, 120}, sy[6] = {34, 36, 58, 60, 50, 52};
      for (int i = 0; i < 6; i++) if (((millis() / 130) + i) % 3 == 0) sparkleAt(sx[i], sy[i], i % 2 ? 2 : 1);
      break;
    }
    case FX_SPIN: {                  // trophy spins, then a ring of stars bursts out
      float k = 1;
      if (t < 0.55) { k = fabs(cos(t / 0.55 * PI * 3)); if (k < 0.1) k = 0.1; }
      drawScaledCup(64, 32, k, 1);
      if (t > 0.55) {
        float r = 10 + 60 * easeOutF((t - 0.55) / 0.45);
        for (int i = 0; i < 10; i++) { float a = i * PI / 5 + 0.2; sparkleAt(64 + cos(a) * r * 1.5, 32 + sin(a) * r, 2); }
      }
      break;
    }
    case FX_PEDESTAL: {              // trophy rises on a pedestal, rays and confetti
      int top = (int)(70 - 62 * easeOutF(clamp01F(t / 0.4)));
      if (t > 0.35) { drawRays(64, top + 10, millis() / 900.0); display.fillCircle(64, top + 11, 21, SSD1306_BLACK); }
      drawCup(display, 64, top, 1);
      display.fillRect(50, top + 24, 29, 4, SSD1306_WHITE);
      display.fillRect(53, top + 29, 23, 40, SSD1306_WHITE);
      display.fillRect(56, top + 32, 17, 40, SSD1306_BLACK);
      if (t > 0.3) drawConfetti((int)(10 + 30 * clamp01F((t - 0.3) / 0.3)), 1);
      break;
    }
    case FX_BIGCUP: {                // huge trophy grows in with rays and fireworks
      float in = clamp01F(t / 0.35), g = easeBackF(in);
      if (t > 0.3) { drawRays(64, 30, -(millis() / 700.0)); display.fillCircle(64, 30, 28, SSD1306_BLACK); }
      drawScaledCup(64, 32, g, g);
      if (t > 0.4) {                 // fireworks in the corners only
        maskCanvas.fillScreen(0);
        drawFireworks(maskCanvas, 5);
        for (int y = 0; y < SCREEN_HEIGHT; y++) for (int x = 0; x < SCREEN_WIDTH; x++)
          if ((x < 30 || x > 98) && maskCanvas.getPixel(x, y)) display.drawPixel(x, y, SSD1306_WHITE);
      }
      break;
    }
    case FX_FIREWORKS: {             // fireworks and confetti everywhere
      drawFireworks(display, 5);
      drawConfetti(36, 1.4);
      break;
    }
    case FX_TICKER: {                // trophy count rolls up by one
      int now = atoi(fxText); if (now <= 0) now = 1;
      char a[8], b[8];
      snprintf(a, sizeof(a), "%d", now - 1);
      snprintf(b, sizeof(b), "%d", now);
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(41, 6);
      display.print("TROPHIES");
      float p = easeOutF(clamp01F((t - 0.25) / 0.3));
      maskCanvas.fillScreen(0);
      maskCanvas.setTextSize(3);
      maskCanvas.setTextColor(1);
      int wa = strlen(a) * 18 - 3, wb = strlen(b) * 18 - 3;
      maskCanvas.setCursor(64 - wa / 2, (int)(26 - p * 26)); maskCanvas.print(a);
      maskCanvas.setCursor(64 - wb / 2, (int)(26 + (1 - p) * 26)); maskCanvas.print(b);
      for (int y = 20; y < 52; y++) for (int x = 0; x < SCREEN_WIDTH; x++)
        if (maskCanvas.getPixel(x, y)) display.drawPixel(x, y, SSD1306_WHITE);
      if (p >= 1 && (millis() / 140) % 2) { sparkleAt(28, 30, 2); sparkleAt(100, 40, 2); sparkleAt(96, 22, 1); }
      break;
    }
    case FX_TEXT: {                  // big word slams in from the right with speed lines
      int len = strlen(fxText), z = 4;
      while (z > 1 && len * 6 * z - z > 120) z--;
      int w = len * 6 * z - z;
      float in = t / 0.22; if (in > 1) in = 1;
      float ease = 1 - pow(1 - in, 3);
      int x = (int)(64 - w / 2 + 120 * (1 - ease));
      int y = 32 - (7 * z) / 2;
      for (int i = 0; i < 5; i++) {
        int ly = y + 2 + i * ((7 * z - 4) / 4);
        int lx = x + w + 4 + (int)(((millis() / 30) + i * 7) % 12);
        display.drawFastHLine(lx, ly, 10 + (i % 2) * 8, SSD1306_WHITE);
      }
      display.setTextSize(z);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(x, y);
      display.print(fxText);
      display.setTextSize(1);
      break;
    }
    case FX_SCAN: {                  // scanner sweeps down and suits him up
      drawEyePair(64, 32, 36, 36);
      maskComposite(23, 14, 105, 36);
      int sy = (int)(-2 + 68 * t);
      for (int y = max(0, sy); y < SCREEN_HEIGHT; y++) for (int x = 0; x < SCREEN_WIDTH; x++) {
        bool inEye = ((x >= 23 && x < 59) || (x >= 69 && x < 105)) && y >= 14 && y < 50;
        if (!inEye) display.drawPixel(x, y, SSD1306_BLACK);
      }
      display.drawFastHLine(0, sy, SCREEN_WIDTH, SSD1306_WHITE);
      display.drawFastHLine(0, sy + 1, SCREEN_WIDTH, SSD1306_WHITE);
      for (int x = 0; x < SCREEN_WIDTH; x += 3) display.drawPixel(x, sy - 4, SSD1306_WHITE);
      break;
    }
    default: break;
  }
  display.display();
}

// ---------- Original animations ----------

// Wink: one eye shuts to a line and holds while the other stays wide open
int animWink(int step) {
  switch (step) {
    case 0: eyes.setMood(DEFAULT); eyes.setPosition(DEFAULT); return 200;
    case 1: eyes.setHeight(EYE_H, 1); return 800;        // right eye shut, held
    case 2: eyes.setHeight(EYE_H, EYE_H); return 300;    // pops back open
    case 3: eyes.setMood(HAPPY); return 1000;            // cheeky happy face after
    default: return -1;
  }
}

// Look Around: glances left, right, up
int animLookAround(int step) {
  switch (step) {
    case 0: eyes.setPosition(W); return 900;
    case 1: eyes.setPosition(E); return 900;
    case 2: eyes.setPosition(N); return 700;
    case 3: eyes.setPosition(DEFAULT); return 500;
    default: return -1;
  }
}

// Side-Eye: deadpan, unimpressed "...really?" look
int animSideEye(int step) {
  switch (step) {
    case 0: eyes.setMood(DEFAULT); eyes.setHeight(16, 22); return 600;
    case 1: eyes.setPosition(E); return 1800;
    case 2: eyes.setPosition(DEFAULT); eyes.setHeight(14, 14); return 900;
    case 3: eyes.setPosition(E); eyes.setHeight(10, 18); return 2000;
    case 4: eyes.setHeight(EYE_H, EYE_H); eyes.setPosition(DEFAULT); return 300;
    case 5: eyes.blink(); return 500;
    default: return -1;
  }
}

// Surprised: eyes go big, then blink
int animSurprised(int step) {
  switch (step) {
    case 0: eyes.setWidth(42, 42); eyes.setHeight(48, 48); return 1400;
    case 1: eyes.blink(); return 500;
    case 2: eyes.setWidth(EYE_W, EYE_W); eyes.setHeight(EYE_H, EYE_H); return 400;
    default: return -1;
  }
}

// Sleepy: eyes squint down to 1-pixel lines and he snores Z's
int animSleepy(int step) {
  switch (step) {
    case 0: eyes.setMood(DEFAULT); eyes.setPosition(S); eyes.setHeight(28, 28); return 900;
    case 1: eyes.setHeight(20, 20); return 900;
    case 2: eyes.setHeight(12, 12); return 1000;
    case 3: eyes.setHeight(6, 6);   return 1000;
    case 4: eyes.setHeight(1, 1);   return 1200;   // 1-pixel sleepy lines
    case 5: snoring = true;         return 6000;   // Zzz...
    case 6: snoring = false; eyes.setHeight(10, 10); return 800;  // half wakes up
    case 7: eyes.setHeight(1, 1);   return 1000;   // drifts off again
    case 8: snoring = true;         return 3500;   // Zzz...
    case 9: snoring = false; eyes.setPosition(DEFAULT);
            eyes.setHeight(EYE_H, EYE_H); return 500;   // wakes up
    case 10: eyes.blink();          return 500;
    default: return -1;
  }
}

// Scared: small eyes, shivering
int animScared(int step) {
  switch (step) {
    case 0: eyes.setWidth(26, 26); eyes.setHeight(30, 30); eyes.setHFlicker(ON, 2); return 2000;
    case 1: eyes.setPosition(SW); return 800;
    case 2: eyes.setPosition(SE); return 800;
    default: return -1;
  }
}

// Excited: happy eyes bouncing
int animExcited(int step) {
  switch (step) {
    case 0: eyes.setMood(HAPPY); eyes.setVFlicker(ON, 2); return 2200;
    case 1: eyes.setVFlicker(OFF, 0); eyes.anim_laugh(); return 1200;
    default: return -1;
  }
}

// Dizzy: eyes spin in a circle twice, then blink
const byte circlePath[8] = {N, NE, E, SE, S, SW, W, NW};
int animDizzy(int step) {
  if (step < 16) {
    eyes.setPosition(circlePath[step % 8]);
    return 130;
  }
  if (step == 16) {
    eyes.setPosition(DEFAULT);
    eyes.blink();
    return 800;
  }
  return -1;
}

// Curious: one eye grows as he looks to each side
int animCurious(int step) {
  switch (step) {
    case 0: eyes.setCuriosity(ON); eyes.setPosition(W); return 1500;
    case 1: eyes.setPosition(E); return 1500;
    case 2: eyes.setPosition(DEFAULT); return 600;
    default: return -1;
  }
}

// Grumpy: angry glare with a sweat drop and a little shake
int animGrumpy(int step) {
  switch (step) {
    case 0: eyes.setMood(ANGRY); eyes.setSweat(ON); return 2500;
    case 1: eyes.setHFlicker(ON, 1); return 800;
    case 2: eyes.setHFlicker(OFF, 0); return 600;
    default: return -1;
  }
}

// Focus: eyes merge into one big cyclops eye
int animFocus(int step) {
  switch (step) {
    case 0: eyes.setCyclops(ON); return 2500;
    case 1: eyes.blink(); return 600;
    default: return -1;
  }
}

int animLaugh(int step) { if (step == 0) { eyes.anim_laugh(); return 1500; } return -1; }

// ---------- NEW silly faces ----------

// Double-take: glances away, looks back, realises, eyes go huge
int animDoubleTake(int step) {
  switch (step) {
    case 0: eyes.setPosition(E); return 800;                 // casual glance
    case 1: eyes.setPosition(DEFAULT); return 350;           // looks back...
    case 2: eyes.setPosition(E); return 200;                 // ...wait, what?
    case 3: eyes.setPosition(DEFAULT);
            eyes.setWidth(44, 44); eyes.setHeight(52, 52); return 1400;  // WHAT?!
    case 4: eyes.setWidth(EYE_W, EYE_W); eyes.setHeight(EYE_H, EYE_H); return 300;
    case 5: eyes.blink(); return 500;
    default: return -1;
  }
}

// Mismatched: one huge eye, one tiny eye, then they swap
int animMismatched(int step) {
  switch (step) {
    case 0: eyes.setWidth(44, 20); eyes.setHeight(48, 18); return 1500;
    case 1: eyes.setWidth(20, 44); eyes.setHeight(18, 48); return 1500;
    case 2: eyes.setWidth(EYE_W, EYE_W); eyes.setHeight(EYE_H, EYE_H); return 300;
    case 3: eyes.blink(); return 500;
    default: return -1;
  }
}

// Slow blink of judgment: stares, closes eyes very slowly, opens slowly
int animJudgment(int step) {
  const int heights[] = {30, 24, 18, 12, 6};
  if (step == 0) { eyes.setPosition(DEFAULT); eyes.setMood(DEFAULT); return 1000; } // stare
  if (step >= 1 && step <= 5) { eyes.setHeight(heights[step - 1], heights[step - 1]); return 350; }
  if (step == 6) { eyes.close(); return 1200; }                                    // ...
  if (step == 7) { eyes.open(); eyes.setHeight(6, 6); return 350; }
  if (step >= 8 && step <= 11) { int h = heights[11 - step]; eyes.setHeight(h, h); return 350; }
  if (step == 12) { eyes.setHeight(26, 26); return 1500; }                         // unimpressed stare
  return -1;
}

// Pancake: squashed flat like he got bonked, then pops back
int animPancake(int step) {
  switch (step) {
    case 0: eyes.setWidth(50, 50); eyes.setHeight(10, 10); eyes.setPosition(S); return 1600;
    case 1: eyes.setWidth(EYE_W, EYE_W); eyes.setHeight(EYE_H, EYE_H);
            eyes.setPosition(DEFAULT); return 300;
    case 2: eyes.blink(); return 500;
    default: return -1;
  }
}

// Nervous: tall skinny eyes darting around
int animNervous(int step) {
  switch (step) {
    case 0: eyes.setWidth(18, 18); eyes.setHeight(52, 52); return 600;
    case 1: eyes.setPosition(W); return 450;
    case 2: eyes.setPosition(E); return 450;
    case 3: eyes.setPosition(W); return 450;
    case 4: eyes.setPosition(DEFAULT); return 600;
    default: return -1;
  }
}

// Bounce: happy eyes hopping like he's dancing
int animBounce(int step) {
  if (step == 0) { eyes.setMood(HAPPY); return 200; }
  if (step >= 1 && step <= 8) {
    eyes.setPosition((step % 2) ? N : DEFAULT);
    return 220;
  }
  return -1;
}

// Heart eyes: beating hearts, then a happy face
int animHeartEyes(int step) {
  switch (step) {
    case 0: customFace = FACE_HEART; return 3000;
    case 1: customFace = FACE_NONE; eyes.setMood(HAPPY); return 1000;
    default: return -1;
  }
}

// X_X: knocked out, then slowly comes round
int animKnockedOut(int step) {
  switch (step) {
    case 0: customFace = FACE_X; return 2500;
    case 1: customFace = FACE_NONE; eyes.close(); return 700;
    case 2: eyes.open(); eyes.setHeight(12, 12); return 600;
    case 3: eyes.setHeight(EYE_H, EYE_H); return 400;
    case 4: eyes.blink(); return 500;
    default: return -1;
  }
}

// ^ ^ : cute, content happy face
int animCute(int step) {
  switch (step) {
    case 0: customFace = FACE_HAPPY_ARCS; return 2500;
    default: return -1;
  }
}

// Googly eyes: pupils bounce around randomly
int animGoogly(int step) {
  switch (step) {
    case 0: customFace = FACE_GOOGLY; return 3000;
    case 1: customFace = FACE_NONE; eyes.blink(); return 500;
    default: return -1;
  }
}

// Fake sleep: pretends to sleep, peeks at you with one eye, snaps shut
int animFakeSleep(int step) {
  switch (step) {
    case 0: eyes.setMood(DEFAULT); eyes.setPosition(DEFAULT); eyes.setHeight(14, 14); return 700;
    case 1: eyes.setHeight(1, 1);   return 1000;   // "asleep"
    case 2: snoring = true;         return 3000;   // Zzz...
    case 3: snoring = false; eyes.setHeight(1, 16); return 400;   // one eye cracks open...
    case 4: eyes.setPosition(E);    return 900;    // ...peeks at you
    case 5: eyes.setPosition(DEFAULT); eyes.setHeight(1, 1); return 500;  // snap shut!
    case 6: snoring = true;         return 2500;   // "still asleep" Zzz...
    case 7: snoring = false; eyes.setHeight(EYE_H, EYE_H); return 500;
    case 8: eyes.blink();           return 500;
    default: return -1;
  }
}

// Sneeze: ah... ah... ah... CHOO!
int animSneeze(int step) {
  switch (step) {
    case 0: eyes.setHeight(24, 24); eyes.setPosition(N); return 400;   // ah...
    case 1: eyes.setHeight(32, 32); return 250;
    case 2: eyes.setHeight(16, 16); return 450;                        // ah...
    case 3: eyes.setHeight(28, 28); return 250;
    case 4: eyes.setHeight(8, 8);   return 600;                        // ah...
    case 5: eyes.setHeight(1, 1); eyes.setPosition(S); return 350;     // squeeze shut
    case 6: eyes.setPosition(DEFAULT);
            eyes.setWidth(42, 42); eyes.setHeight(48, 48); return 800;  // CHOO!
    case 7: eyes.setWidth(EYE_W, EYE_W); eyes.setHeight(EYE_H, EYE_H); return 300;
    case 8: eyes.blink();           return 500;
    default: return -1;
  }
}

int animConfused(int step) { if (step == 0) { eyes.anim_confused(); return 1500; } return -1; }

// ---------- Gaming animations ----------

// Game Start: "WHOA, a game!" then an excited bounce
int animGameStart(int step) {
  if (step == 0) { eyes.setWidth(42, 42); eyes.setHeight(48, 48); return 700; }
  if (step == 1) { eyes.setWidth(EYE_W, EYE_W); eyes.setHeight(EYE_H, EYE_H);
                   eyes.setMood(HAPPY); return 300; }
  if (step >= 2 && step <= 7) { eyes.setPosition((step % 2) ? N : DEFAULT); return 200; }
  if (step == 8) { eyes.setPosition(DEFAULT); return 400; }
  return -1;
}

// Game Face: focused, slightly squinted eyes darting around the "screen"
int animGameFace(int step) {
  switch (step) {
    case 0: eyes.setMood(DEFAULT); eyes.setHeight(24, 24); return 500;
    case 1: eyes.setPosition(W);  return 600;
    case 2: eyes.setPosition(E);  return 500;
    case 3: eyes.setPosition(NW); return 400;
    case 4: eyes.setPosition(NE); return 500;
    case 5: eyes.setPosition(DEFAULT); return 400;
    case 6: eyes.setPosition(W);  return 300;
    case 7: eyes.setPosition(E);  return 300;
    case 8: eyes.setPosition(DEFAULT); return 600;
    default: return -1;
  }
}

// ---- Trophy celebrations, one per tier ----
char trophyCountStr[8] = "1";   // your new trophy total, shown by the ticker

// Bronze: trophy pop-up with a glint and sparkles, then the count rolls up
int animBronzeTrophy(int step) {
  switch (step) {
    case 0: startFx(FX_POPUP, "BRONZE"); return 2600;
    case 1: startFx(FX_TICKER, trophyCountStr); return 1500;
    case 2: eyes.setMood(HAPPY); return 700;
    default: return -1;
  }
}

// Silver: pop-up, the trophy spins and bursts a ring of stars
int animSilverTrophy(int step) {
  switch (step) {
    case 0: startFx(FX_POPUP, "SILVER"); return 2300;
    case 1: startFx(FX_SPIN, ""); return 1500;
    case 2: startFx(FX_TICKER, trophyCountStr); return 1500;
    case 3: eyes.setMood(HAPPY); return 700;
    default: return -1;
  }
}

// Gold: pop-up, the trophy rises on a pedestal with rays and confetti
int animGoldTrophy(int step) {
  switch (step) {
    case 0: startFx(FX_POPUP, "GOLD"); return 2200;
    case 1: startFx(FX_PEDESTAL, ""); return 2800;
    case 2: startFx(FX_TICKER, trophyCountStr); return 1500;
    case 3: eyes.setMood(HAPPY); return 800;
    default: return -1;
  }
}

// Platinum: the full show
int animPlatinumTrophy(int step) {
  switch (step) {
    case 0: startFx(FX_FLASH, ""); return 400;
    case 1: startFx(FX_POPUP, "PLATINUM"); return 2000;
    case 2: startFx(FX_BIGCUP, ""); return 2800;       // huge trophy, rays, fireworks
    case 3: startFx(FX_TEXT, "PLATINUM!"); return 1300;
    case 4: startFx(FX_FIREWORKS, ""); return 2600;    // fireworks and confetti everywhere
    case 5: startFx(FX_TICKER, trophyCountStr); return 1500;
    case 6: customFace = FACE_HEART; return 2200;
    case 7: customFace = FACE_NONE; eyes.setMood(HAPPY); return 900;
    default: return -1;
  }
}

// Tired Gamer: long session - sweaty, droopy, "one more game..."
int animTiredGamer(int step) {
  switch (step) {
    case 0: eyes.setSweat(ON); eyes.setHeight(22, 22); eyes.setPosition(S); return 1500;
    case 1: eyes.setHeight(14, 14); return 1300;
    case 2: eyes.blink(); return 400;
    case 3: eyes.setHeight(22, 22); eyes.setPosition(DEFAULT); return 1200;
    default: return -1;
  }
}

// Power Up: energy gathers, FLASH, fierce eyes
int animPowerUp(int step) {
  switch (step) {
    case 0: startFx(FX_CHARGE, ""); return 1700;   // energy gathers
    case 1: startFx(FX_FLASH, ""); return 500;     // FLASH
    case 2: eyes.setMood(ANGRY); eyes.setHeight(22, 22); return 1500;   // fierce
    case 3: eyes.setWidth(EYE_W, EYE_W); eyes.setHeight(EYE_H, EYE_H); eyes.setMood(DEFAULT); return 400;
    default: return -1;
  }
}

// Suit Up: a scanner sweeps down and puts his hero mask on
int animSuitUp(int step) {
  switch (step) {
    case 0: startFx(FX_SCAN, ""); return 1400;
    case 1: maskOn = true; return 600;
    case 2: maskOn = true; eyes.setMood(ANGRY); eyes.setHeight(26, 26); eyes.setPosition(W); return 900;
    case 3: eyes.setPosition(E); return 900;
    case 4: eyes.setPosition(DEFAULT); return 600;
    case 5: eyes.setWidth(EYE_W, EYE_W); eyes.setHeight(EYE_H, EYE_H); eyes.setMood(DEFAULT); eyes.blink(); return 500;
    default: return -1;
  }
}

// Hero Mode: masked eyes glancing around, ready for battle
int animHeroMode(int step) {
  switch (step) {
    case 0: maskOn = true; return 200;
    case 1: eyes.setPosition(W); return 900;
    case 2: eyes.setPosition(E); return 900;
    case 3: eyes.setPosition(DEFAULT); eyes.setHeight(26, 26); return 500;
    case 4: eyes.setMood(HAPPY); eyes.setWidth(EYE_W, EYE_W); eyes.setHeight(EYE_H, EYE_H); return 900;
    case 5: eyes.blink(); return 600;
    default: return -1;
  }
}

// Intense Stare: narrowed, locked-in eyes (his main look while gaming)
int animIntenseStare(int step) {
  switch (step) {
    case 0: eyes.setPosition(DEFAULT); eyes.setMood(ANGRY); eyes.setHeight(20, 20); return 500;
    case 1: eyes.setHeight(26, 26); return 1600;   // locked in
    case 2: eyes.setPosition(W); return 450;       // quick glance
    case 3: eyes.setPosition(DEFAULT); return 1400;
    case 4: eyes.setPosition(E); return 450;
    case 5: eyes.setPosition(DEFAULT); return 1200;
    case 6: eyes.setWidth(EYE_W, EYE_W); eyes.setHeight(EYE_H, EYE_H); eyes.setMood(DEFAULT); return 400;
    default: return -1;
  }
}

// ---- Marvel Rivals rotation (played at random while you're in Marvel Rivals) ----

// Ultimate Ready: ult charges up, ULT! burst, fierce eyes
int animUltimateReady(int step) {
  switch (step) {
    case 0: startFx(FX_CHARGE, ""); return 1000;      // ult charging...
    case 1: startFx(FX_BURST, "ULT!"); return 900;    // ...ready!
    case 2: eyes.setMood(ANGRY); eyes.setHeight(22, 22); return 1500;
    case 3: eyes.setWidth(EYE_W, EYE_W); eyes.setHeight(EYE_H, EYE_H); eyes.setMood(DEFAULT); return 400;
    default: return -1;
  }
}

// Healing: plus signs float up around happy eyes
int animHealing(int step) {
  switch (step) {
    case 0: startFx(FX_HEAL, ""); return 2400;
    case 1: eyes.setMood(HAPPY); return 700;
    default: return -1;
  }
}

// Combo: bursts counting up, 2X, 3X, COMBO!
int animCombo(int step) {
  switch (step) {
    case 0: startFx(FX_BURST, "2X"); return 450;
    case 1: startFx(FX_BURST, "3X"); return 450;
    case 2: startFx(FX_BURST, "COMBO!"); return 800;
    case 3: eyes.setMood(HAPPY); eyes.setVFlicker(ON, 2); return 900;
    case 4: eyes.setVFlicker(OFF, 0); return 300;
    default: return -1;
  }
}

// Fuming: angry eyes with steam puffs
int animFuming(int step) {
  switch (step) {
    case 0: startFx(FX_STEAM, ""); return 2400;       // that last round...
    case 1: eyes.setMood(ANGRY); eyes.setHeight(26, 26); return 800;
    case 2: eyes.setWidth(EYE_W, EYE_W); eyes.setHeight(EYE_H, EYE_H); eyes.setMood(DEFAULT); return 400;
    default: return -1;
  }
}

// Dash: speed streaks behind him
int animDash(int step) {
  switch (step) {
    case 0: startFx(FX_DASH, ""); return 1600;
    case 1: eyes.blink(); return 500;
    default: return -1;
  }
}

// Marvel Rivals intro: Power Up -> Suit Up -> Hero Mode, back to back
int animRivalsIntro(int step) {
  if (step == 0) { strcpy(chainQueue, "PSM"); return 0; }
  return -1;
}

// GG: you got off PlayStation - "GG" slams in, then a relaxed, satisfied look
int animGG(int step) {
  switch (step) {
    case 0: startFx(FX_TEXT, "GG"); return 1800;
    case 1: eyes.setMood(HAPPY); eyes.setHeight(28, 28); eyes.setPosition(N); return 1400;
    case 2: eyes.blink(); return 400;
    case 3: eyes.setHeight(EYE_H, EYE_H); eyes.setPosition(DEFAULT); return 800;
    default: return -1;
  }
}

// ---------- Animation list ----------
// The last column decides when it can be picked at random:
//   ANYTIME = day or night
//   NIGHT   = only after sunset (and before sunrise)
//   NEVER   = only plays when you type its key in the Serial Monitor
#define NEVER   0
#define ANYTIME 1
#define NIGHT   2
#define GAMING  3   // only played while you're in a PlayStation game

struct Anim { char key; const char* name; AnimFn fn; byte when; };

const Anim animations[] = {
  {'1', "Wink",           animWink,       ANYTIME},
  {'2', "Look Around",    animLookAround, ANYTIME},
  {'3', "Side-Eye",       animSideEye,    ANYTIME},
  {'4', "Surprised",      animSurprised,  ANYTIME},
  {'6', "Scared",         animScared,     ANYTIME},
  {'7', "Excited",        animExcited,    ANYTIME},
  {'8', "Dizzy",          animDizzy,      ANYTIME},
  {'9', "Curious",        animCurious,    ANYTIME},
  {'0', "Grumpy",         animGrumpy,     ANYTIME},
  {'f', "Focus",          animFocus,      ANYTIME},
  {'l', "Laugh",          animLaugh,      ANYTIME},
  {'d', "Double-Take",    animDoubleTake, ANYTIME},
  {'b', "Bounce",         animBounce,     ANYTIME},
  {'h', "Heart Eyes",     animHeartEyes,  ANYTIME},
  {'s', "Sneeze",         animSneeze,     ANYTIME},
  // Sleep animations - only after sunset
  {'5', "Sleepy",         animSleepy,     NIGHT},
  {'z', "Fake Sleep",     animFakeSleep,  NIGHT},
  // Gaming - played automatically by PlayStation gaming mode
  {'a', "Game Start",     animGameStart,  GAMING},
  {'q', "Game Face",      animGameFace,   GAMING},
  {'w', "Bronze Trophy",  animBronzeTrophy,   GAMING},
  {'v', "Silver Trophy",  animSilverTrophy,   GAMING},
  {'o', "Gold Trophy",    animGoldTrophy,     GAMING},
  {'y', "Platinum Trophy", animPlatinumTrophy, GAMING},
  {'r', "Tired Gamer",    animTiredGamer, GAMING},
  {'e', "GG",             animGG,         GAMING},
  {'i', "Intense Stare",  animIntenseStare, GAMING},
  {'R', "Rivals Intro",   animRivalsIntro,  GAMING},
  {'P', "Power Up",       animPowerUp,      GAMING},
  {'S', "Suit Up",        animSuitUp,       GAMING},
  {'M', "Hero Mode",      animHeroMode,     GAMING},
  {'U', "Ultimate Ready", animUltimateReady, GAMING},
  {'+', "Healing",        animHealing,      GAMING},
  {'Y', "Combo",          animCombo,        GAMING},
  {'G', "Fuming",         animFuming,       GAMING},
  {'D', "Dash",           animDash,         GAMING},
  // Test-only (not in random behaviour)
  {'x', "Knocked Out",    animKnockedOut, NEVER},
  {'m', "Mismatched",     animMismatched, NEVER},
  {'j', "Judgment Blink", animJudgment,   NEVER},
  {'p', "Pancake",        animPancake,    NEVER},
  {'n', "Nervous",        animNervous,    NEVER},
  {'u', "Cute ^ ^",       animCute,       NEVER},
  {'g', "Googly",         animGoogly,     NEVER},
  {'c', "Confused",       animConfused,   NEVER},
};
const int NUM_ANIMS = sizeof(animations) / sizeof(animations[0]);

void printMenu() {
  Serial.print("---- "); Serial.print(robotName); Serial.println(" animations ----");
  for (int i = 0; i < NUM_ANIMS; i++) {
    Serial.print("  ");
    Serial.print(animations[i].key);
    Serial.print("  ");
    Serial.println(animations[i].name);
  }
  Serial.println("  t  Show time, sunset/sunrise and day/night");
  Serial.println("  !  Show Wi-Fi status and web address");
  Serial.println("  N  Cycle test mode: auto -> force night -> force day");
  Serial.println("  ?  Show this list");
}

// ---------- Time and sunset ----------

// Why the last Wi-Fi drop happened (shown by the ! key)
volatile int wifiDrops = 0;
volatile int lastDropReason = 0;

void connectWiFi() {
  Serial.print("Connecting to Wi-Fi");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // Wi-Fi power saving makes some routers (mesh especially) drop him
  WiFi.setAutoReconnect(true);
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    wifiDrops++;
    lastDropReason = info.wifi_sta_disconnected.reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  // Many ESP32-C3 SuperMini boards won't connect at full power. Lowering it fixes that.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    eyes.update();          // keep the eyes alive while we wait
    delay(16);
    if ((millis() - start) % 1000 < 16) Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" connected!");
    configTzTime(TIMEZONE, "pool.ntp.org", "time.nist.gov");
  } else {
    Serial.println(" not yet. He'll keep trying in the background.");
  }
}

float degToRad(float d) { return d * PI / 180.0; }
float radToDeg(float r) { return r * 180.0 / PI; }

// Minutes the local clock is ahead of UTC (handles daylight saving)
int utcOffsetMinutes() {
  time_t now = time(nullptr);
  struct tm lt, gt;
  localtime_r(&now, &lt);
  gmtime_r(&now, &gt);
  int diff = (lt.tm_hour - gt.tm_hour) * 60 + (lt.tm_min - gt.tm_min);
  int dayDiff = (lt.tm_year != gt.tm_year) ? (lt.tm_year - gt.tm_year)
                                           : (lt.tm_yday - gt.tm_yday);
  return diff + dayDiff * 1440;
}

// Sunrise or sunset as minutes after local midnight.
// Standard sunrise/sunset algorithm (accurate to a couple of minutes).
// Returns -1 if the sun doesn't rise or set that day.
int sunEventMinutes(int dayOfYear, bool sunrise) {
  const float zenith = 90.833;   // official sunrise/sunset
  float lngHour = LONGITUDE / 15.0;
  float t = dayOfYear + ((sunrise ? 6.0 : 18.0) - lngHour) / 24.0;

  float M = 0.9856 * t - 3.289;
  float L = M + 1.916 * sin(degToRad(M)) + 0.020 * sin(degToRad(2 * M)) + 282.634;
  L = fmod(L + 360.0, 360.0);

  float RA = radToDeg(atan(0.91764 * tan(degToRad(L))));
  RA = fmod(RA + 360.0, 360.0);
  float Lquadrant  = floor(L / 90.0) * 90.0;
  float RAquadrant = floor(RA / 90.0) * 90.0;
  RA = (RA + (Lquadrant - RAquadrant)) / 15.0;

  float sinDec = 0.39782 * sin(degToRad(L));
  float cosDec = cos(asin(sinDec));
  float cosH = (cos(degToRad(zenith)) - sinDec * sin(degToRad(LATITUDE)))
               / (cosDec * cos(degToRad(LATITUDE)));
  if (cosH > 1 || cosH < -1) return -1;

  float H = sunrise ? 360.0 - radToDeg(acos(cosH)) : radToDeg(acos(cosH));
  H /= 15.0;

  float T = H + RA - 0.06571 * t - 6.622;
  float UT = fmod(T - lngHour + 48.0, 24.0);

  int local = (int)(UT * 60.0 + 0.5) + utcOffsetMinutes();
  return ((local % 1440) + 1440) % 1440;
}

void printTime(int minutes) {
  int h = minutes / 60, m = minutes % 60;
  int h12 = h % 12 == 0 ? 12 : h % 12;
  Serial.print(h12);
  Serial.print(m < 10 ? ":0" : ":");
  Serial.print(m);
  Serial.print(h < 12 ? " AM" : " PM");
}

// Checks the clock and decides whether it's night
void updateDayNight() {
  struct tm now;
  if (!getLocalTime(&now, 0)) {     // clock not synced yet
    timeValid = false;
  } else {
    if (!timeValid) Serial.println("Clock synced!");
    timeValid = true;

    // Recalculate sunrise and sunset once per day
    if (now.tm_yday != sunCalcDay) {
      sunCalcDay = now.tm_yday;
      sunriseMin = sunEventMinutes(now.tm_yday + 1, true);
      sunsetMin  = sunEventMinutes(now.tm_yday + 1, false);
      Serial.print("Today: sunrise ");
      printTime(sunriseMin);
      Serial.print(", sunset ");
      printTime(sunsetMin);
      Serial.println();
    }
  }

  bool wasNight = isNight;
  if (nightOverride == 1) {
    isNight = true;
  } else if (nightOverride == 2 || !timeValid || sunriseMin < 0 || sunsetMin < 0) {
    isNight = false;
  } else {
    int nowMin = now.tm_hour * 60 + now.tm_min;
    int nightStart = sunsetMin + NIGHT_START_OFFSET_MIN;
    int nightEnd   = sunriseMin + NIGHT_END_OFFSET_MIN;
    isNight = (nowMin >= nightStart) || (nowMin < nightEnd);
  }

  if (isNight != wasNight) {
    Serial.println(isNight ? "It's night - sleep animations on" : "It's day - sleep animations off");
  }
}

void printStatus() {
  struct tm now;
  if (getLocalTime(&now, 0)) {
    Serial.print("Time: ");
    printTime(now.tm_hour * 60 + now.tm_min);
    Serial.print("  |  Sunrise: ");
    printTime(sunriseMin);
    Serial.print("  |  Sunset: ");
    printTime(sunsetMin);
    Serial.println();
  } else {
    Serial.println("Clock not synced (check Wi-Fi name and password).");
  }
  Serial.print("Mode: ");
  Serial.print(nightOverride == 0 ? "automatic" : nightOverride == 1 ? "forced night" : "forced day");
  Serial.print("  |  Currently: ");
  Serial.println(isNight ? "NIGHT" : "DAY");
}

// ---------- Playing animations (shared by Serial Monitor and web page) ----------
bool playKey(char key) {
  for (int i = 0; i < NUM_ANIMS; i++) {
    if (animations[i].key == key) {
      if (currentAnim) endAnimation();
      startAnimation(animations[i].fn, animations[i].name);
      return true;
    }
  }
  return false;
}

// ---------- PlayStation Network ----------
// Uses the same (unofficial) PSN requests as the psn-api and PSNAWP libraries.
// Runs in its own background task so the eyes never freeze while it waits on Sony.

#define PSN_NOT_SET_UP   0
#define PSN_CONNECTING   1
#define PSN_OK           2
#define PSN_NEEDS_CODE   3   // sign-in code expired or wrong - paste a new one
#define PSN_ERROR        4   // temporary problem (network, Sony servers)

const char* PSN_AUTH_URL  = "https://ca.account.sony.com/api/authz/v3/oauth";
const char* PSN_CLIENT_ID = "09515159-7237-4370-9b40-3806e67c0891";
const char* PSN_BASIC_AUTH =
  "Basic MDk1MTUxNTktNzIzNy00MzcwLTliNDAtMzgwNmU2N2MwODkxOnVjUGprYTV0bnRCMktxc1A=";
const char* PSN_REDIRECT  = "com.scee.psxandroid.scecompcall://redirect";
const char* PSN_SCOPE     = "psn:mobile.v2.core psn:clientapp";

Preferences prefs;
SemaphoreHandle_t psnLock;

// Shared with the main loop and web page (always read/write inside psnLock)
volatile int psnState = PSN_NOT_SET_UP;
bool psnOnline = false;
String psnGame = "";           // "" = not in a game
String psnIcon = "";           // cover art URL
String psnPlatform = "";
int psnTrophies = -1;          // total earned, -1 = unknown
int psnPlatinums = -1;
int psnBronze = -1, psnSilver = -1, psnGold = -1;
volatile int evTrophyTier = 0;   // 1 bronze, 2 silver, 3 gold, 4 platinum
unsigned long psnSessionStart = 0;
String psnNewCode = "";        // NPSSO pasted on the web page, waiting to be used

// Events for the main loop
volatile bool evGameStart = false, evGameEnd = false;
volatile bool evWentOffline = false;   // console turned off / you signed out
bool psnWasOnline = false;
int psnOfflinePolls = 0;

// Background-task-only state
String psnNpsso, psnRefresh, psnAccess, psnAccountId;
unsigned long psnAccessExpires = 0;
int psnEmptyPolls = 0;
String psnLastGame = "";
unsigned long psnLastEnd = 0;

// One HTTPS request. Returns the HTTP status code (or a negative error).
int psnHttp(const char* method, const String& url, const String& body,
            const String& auth, const String& cookie, String& response, String* location) {
  WiFiClientSecure client;
  client.setInsecure();   // skips certificate checks - fine for a desk robot
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, url)) return -1;
  if (location) {
    const char* keys[] = {"Location"};
    http.collectHeaders(keys, 1);
  }
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (auth.length()) http.addHeader("Authorization", auth);
  if (cookie.length()) http.addHeader("Cookie", cookie);
  int code;
  if (strcmp(method, "POST") == 0) {
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    code = http.POST(body);
  } else {
    code = http.GET();
  }
  if (location) *location = http.header("Location");
  response = (code > 0) ? http.getString() : "";
  http.end();
  return code;
}

String urlEncode(const String& s) {
  String out;
  const char* hex = "0123456789ABCDEF";
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') out += c;
    else { out += '%'; out += hex[(c >> 4) & 15]; out += hex[c & 15]; }
  }
  return out;
}

// Reads access/refresh tokens from a token response
bool psnReadTokens(const String& json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  const char* access = doc["access_token"];
  if (!access) return false;
  psnAccess = access;
  psnAccessExpires = millis() + (unsigned long)(doc["expires_in"] | 3600) * 1000UL;
  const char* refresh = doc["refresh_token"];
  if (refresh && strlen(refresh) > 0) {
    psnRefresh = refresh;
    prefs.putString("refresh", psnRefresh);
  }
  return true;
}

// Step 1: trade the NPSSO sign-in code for tokens
bool psnLoginWithNpsso() {
  if (psnNpsso.length() == 0) return false;
  String url = String(PSN_AUTH_URL) + "/authorize?access_type=offline&client_id=" + PSN_CLIENT_ID +
               "&redirect_uri=" + urlEncode(PSN_REDIRECT) + "&response_type=code&scope=" +
               urlEncode(PSN_SCOPE);
  String resp, location;
  psnHttp("GET", url, "", "", "npsso=" + psnNpsso, resp, &location);

  int at = location.indexOf("?code=");
  if (at < 0) {
    Serial.println("PSN: sign-in code was rejected. Paste a new one on the web page.");
    return false;
  }
  String code = location.substring(at + 6);
  int amp = code.indexOf('&');
  if (amp >= 0) code = code.substring(0, amp);

  String body = "code=" + urlEncode(code) + "&redirect_uri=" + urlEncode(PSN_REDIRECT) +
                "&grant_type=authorization_code&token_format=jwt";
  int status = psnHttp("POST", String(PSN_AUTH_URL) + "/token", body, PSN_BASIC_AUTH, "", resp, nullptr);
  if (status != 200 || !psnReadTokens(resp)) {
    Serial.printf("PSN: token request failed (%d)\n", status);
    return false;
  }
  Serial.println("PSN: signed in!");
  return true;
}

// Step 2 (every hour): get a fresh access token with the saved refresh token
bool psnRefreshTokens() {
  if (psnRefresh.length() == 0) return false;
  String body = "refresh_token=" + urlEncode(psnRefresh) +
                "&grant_type=refresh_token&token_format=jwt&scope=" + urlEncode(PSN_SCOPE);
  String resp;
  int status = psnHttp("POST", String(PSN_AUTH_URL) + "/token", body, PSN_BASIC_AUTH, "", resp, nullptr);
  return status == 200 && psnReadTokens(resp);
}

bool psnEnsureToken() {
  if (psnAccess.length() && (long)(psnAccessExpires - millis()) > 60000) return true;
  if (psnRefreshTokens()) return true;
  if (psnLoginWithNpsso()) return true;
  return false;
}

bool psnEnsureAccountId() {
  if (psnAccountId.length()) return true;
  String resp;
  int status = psnHttp("GET", "https://dms.api.playstation.com/api/v1/devices/accounts/me",
                       "", "Bearer " + psnAccess, "", resp, nullptr);
  if (status != 200) return false;
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return false;
  const char* id = doc["accountId"];
  if (!id) return false;
  psnAccountId = id;
  prefs.putString("acct", psnAccountId);
  return true;
}

// Asks PSN what you're doing right now
bool psnPollPresence() {
  String resp;
  int status = psnHttp("GET",
    "https://m.np.playstation.com/api/userProfile/v1/internal/users/" + psnAccountId +
    "/basicPresences?type=primary", "", "Bearer " + psnAccess, "", resp, nullptr);
  if (status == 401) { psnAccess = ""; return false; }   // token expired - refresh next time
  if (status != 200) return false;

  JsonDocument doc;
  if (deserializeJson(doc, resp)) return false;
  JsonObject bp = doc["basicPresence"];
  const char* online = bp["primaryPlatformInfo"]["onlineStatus"] | "";
  if (online[0] == 0) online = bp["onlineStatus"] | "offline";
  const char* platform = bp["primaryPlatformInfo"]["platform"] | "";
  const char* title = bp["gameTitleInfoList"][0]["titleName"] | "";
  const char* icon = bp["gameTitleInfoList"][0]["conceptIconUrl"] | "";
  if (icon[0] == 0) icon = bp["gameTitleInfoList"][0]["npTitleIconUrl"] | "";

  bool nowOnline = strcmp(online, "online") == 0;
  String game = nowOnline ? String(title) : String("");

  xSemaphoreTake(psnLock, portMAX_DELAY);
  psnOnline = nowOnline;
  // Two offline checks in a row = you got off PlayStation
  if (nowOnline) { psnWasOnline = true; psnOfflinePolls = 0; }
  else if (psnWasOnline && ++psnOfflinePolls >= 2) {
    Serial.println("PSN: console went offline");
    psnWasOnline = false;
    evWentOffline = true;
  }
  psnPlatform = platform;
  if (game.length()) {
    psnEmptyPolls = 0;
    if (game != psnGame) {
      Serial.println("PSN: now playing " + game);
      // New session only if it's a different game or you'd stopped for 10+ minutes
      if (psnGame.length() == 0 &&
          (game != psnLastGame || psnLastEnd == 0 || millis() - psnLastEnd > 10UL * 60UL * 1000UL))
        psnSessionStart = millis();
      psnLastGame = game;
      psnGame = game;
      psnIcon = icon;
      evGameStart = true;
    }
  } else if (psnGame.length()) {
    // Wait for two empty checks in a row so a hiccup doesn't end the session
    if (++psnEmptyPolls >= 2) {
      Serial.println("PSN: stopped playing " + psnGame);
      psnLastEnd = millis();
      psnGame = "";
      psnIcon = "";
      evGameEnd = true;
    }
  }
  xSemaphoreGive(psnLock);
  return true;
}

// Checks your trophy count so he can celebrate new ones
void psnPollTrophies() {
  String resp;
  int status = psnHttp("GET",
    "https://m.np.playstation.com/api/trophy/v1/users/" + psnAccountId + "/trophySummary",
    "", "Bearer " + psnAccess, "", resp, nullptr);
  if (status != 200) return;
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return;
  JsonObject e = doc["earnedTrophies"];
  int bronze = e["bronze"] | 0, silver = e["silver"] | 0, gold = e["gold"] | 0, plat = e["platinum"] | 0;
  int total = bronze + silver + gold + plat;

  xSemaphoreTake(psnLock, portMAX_DELAY);
  if (psnTrophies >= 0 && total > psnTrophies) {
    // Celebrate the best tier you just earned
    int tier = 1;
    if (plat > psnPlatinums) tier = 4;
    else if (gold > psnGold) tier = 3;
    else if (silver > psnSilver) tier = 2;
    Serial.printf("PSN: new trophy! tier %d (%d total)\n", tier, total);
    evTrophyTier = tier;
  }
  psnTrophies = total;
  psnPlatinums = plat; psnBronze = bronze; psnSilver = silver; psnGold = gold;
  xSemaphoreGive(psnLock);
}

void psnTask(void* param) {
  unsigned long lastPoll = 0, lastTrophy = 0;
  bool first = true;
  for (;;) {
    // A new code was pasted on the web page
    xSemaphoreTake(psnLock, portMAX_DELAY);
    String newCode = psnNewCode;
    psnNewCode = "";
    xSemaphoreGive(psnLock);
    if (newCode.length()) {
      psnNpsso = newCode;
      prefs.putString("npsso", psnNpsso);
      psnRefresh = ""; psnAccess = "";
      prefs.remove("refresh");
      psnState = PSN_CONNECTING;
      first = true;
    }

    bool haveLogin = psnNpsso.length() || psnRefresh.length();
    if (!haveLogin) {
      psnState = PSN_NOT_SET_UP;
    } else if (WiFi.status() == WL_CONNECTED &&
               (first || millis() - lastPoll > (unsigned long)PSN_POLL_SECONDS * 1000UL)) {
      first = false;
      lastPoll = millis();
      if (!psnEnsureToken()) {
        psnState = PSN_NEEDS_CODE;
      } else if (!psnEnsureAccountId() || !psnPollPresence()) {
        psnState = PSN_ERROR;
      } else {
        psnState = PSN_OK;
        bool inGame;
        xSemaphoreTake(psnLock, portMAX_DELAY);
        inGame = psnGame.length() > 0;
        xSemaphoreGive(psnLock);
        if (psnTrophies < 0 ||
            (inGame && millis() - lastTrophy > (unsigned long)PSN_TROPHY_SECONDS * 1000UL)) {
          lastTrophy = millis();
          psnPollTrophies();
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void startPsn() {
  psnLock = xSemaphoreCreateMutex();
  prefs.begin("psn", false);
  psnNpsso = prefs.getString("npsso", "");
  psnRefresh = prefs.getString("refresh", "");
  psnAccountId = prefs.getString("acct", "");
  xTaskCreate(psnTask, "psn", 16384, nullptr, 1, nullptr);
}

// ---------- Gaming mode (main loop side) ----------
bool gaming = false;
char gameCategory = 'g';    // h = horror, s = sports/racing, f = shooter, c = chill, g = general

// Picks a style of reaction from words in the game's title
char categorizeGame(String title) {
  title.toLowerCase();
  const char* horror[] = {"resident evil", "silent hill", "dead space", "outlast", "five nights",
                          "alan wake", "until dawn", "the last of us", "evil within", "dying light",
                          "dead by daylight", "the quarry", "amnesia", "phasmophobia", "callisto"};
  const char* sports[] = {"fc 2", "fifa", "ea sports", "nba", "madden", "mlb", "nhl", "gran turismo",
                          "f1", "rocket league", "need for speed", "forza", "racing", "tennis",
                          "golf", "wwe", "ufc", "crash team", "nascar"};
  const char* shooter[] = {"call of duty", "battlefield", "apex", "fortnite", "overwatch", "destiny",
                           "valorant", "helldivers", "rainbow six", "warzone", "halo", "doom",
                           "the finals", "marathon", "borderlands", "titanfall"};
  const char* chill[] = {"minecraft", "stardew", "astro", "lego", "no man's sky", "sims", "roblox",
                         "sackboy", "ratchet", "spyro", "crash bandicoot", "fall guys", "it takes two",
                         "split fiction", "overcooked", "unravel", "journey"};
  for (const char* k : horror)  if (title.indexOf(k) >= 0) return 'h';
  for (const char* k : sports)  if (title.indexOf(k) >= 0) return 's';
  for (const char* k : shooter) if (title.indexOf(k) >= 0) return 'f';
  for (const char* k : chill)   if (title.indexOf(k) >= 0) return 'c';
  return 'g';
}

// Checks for new PlayStation events and reacts
void handlePsnEvents() {
  bool start = false, end = false, offline = false;
  int tier = 0;
  String game;
  xSemaphoreTake(psnLock, portMAX_DELAY);
  start = evGameStart; end = evGameEnd; tier = evTrophyTier; offline = evWentOffline;
  evGameStart = evGameEnd = evWentOffline = false;
  evTrophyTier = 0;
  game = psnGame;
  if (psnTrophies >= 0) snprintf(trophyCountStr, sizeof(trophyCountStr), "%d", psnTrophies);   // ticker shows your real total
  xSemaphoreGive(psnLock);

  // PSN sometimes stops reporting your game for a moment (loading screens,
  // matchmaking, a network hiccup). A gap shorter than this doesn't end the session.
  const unsigned long GRACE_MS = 10UL * 60UL * 1000UL;
  static String lastGame = "";
  static unsigned long gameGoneAt = 0;   // when PSN stopped reporting your game (0 = it's there)

  if (start) {
    bool resumed = gaming && game == lastGame;   // same game came back: no new intro
    gameGoneAt = 0;
    lastGame = game;
    if (resumed) {
      Serial.println("PSN: still playing " + game);
    } else {
      gaming = true;
      gameCategory = categorizeGame(game);
      chainQueue[0] = 0;
      String lower = game;
      lower.toLowerCase();
      if (lower.indexOf("marvel rivals") >= 0) {
        gameCategory = 'r';     // Marvel Rivals gets its own rotation
        playKey('R');           // Power Up -> Suit Up -> Hero Mode
      }
      else playKey('a');
    }
  } else if (tier) {
    chainQueue[0] = 0;
    const char keys[5] = {0, 'w', 'v', 'o', 'y'};   // bronze, silver, gold, platinum
    playKey(keys[tier]);
  }

  // Game no longer reported, but the console is still on: stay in gaming mode
  // quietly. No GG here, since this can happen mid-session.
  if (end && !offline && gaming) gameGoneAt = millis();

  // Gone for longer than the grace period: leave gaming mode quietly
  if (gaming && gameGoneAt && millis() - gameGoneAt > GRACE_MS) {
    Serial.println("PSN: no game for a while, leaving gaming mode");
    gaming = false;
    gameGoneAt = 0;
    lastGame = "";
  }

  // Console went offline = you got off PlayStation: end gaming mode with one GG
  if (offline) {
    bool wasGaming = gaming || lastGame.length();
    gaming = false;
    gameGoneAt = 0;
    lastGame = "";
    chainQueue[0] = 0;
    static unsigned long lastGG = 0;
    if (wasGaming && (lastGG == 0 || millis() - lastGG > GRACE_MS)) {
      lastGG = millis();
      playKey('e');
    }
  }
}

// What he does while you're gaming, based on the kind of game
void pickGamingBehaviour() {
  unsigned long sessionMin;
  xSemaphoreTake(psnLock, portMAX_DELAY);
  sessionMin = (millis() - psnSessionStart) / 60000UL;
  xSemaphoreGive(psnLock);

  // Tired Gamer: only after 2+ hours of play AND late at night (10:30 PM to 6 AM)
  bool lateNight = false;
  struct tm now;
  if (timeValid && getLocalTime(&now, 0)) {
    int m = now.tm_hour * 60 + now.tm_min;
    lateNight = m >= TIRED_FROM_HOUR * 60 + TIRED_FROM_MIN || m < 6 * 60;
  }
  if (lateNight && sessionMin >= (unsigned long)PSN_TIRED_AFTER_MIN && random(0, 100) < 25) {
    playKey('r');   // Tired Gamer
    return;
  }
  int roll = random(0, 100);
  if (roll < 50) {             // half the time: the intense stare
    playKey('i');
    return;
  }
  if (roll < 60) {             // sometimes just watch with a normal face
    eyes.setMood(DEFAULT);
    return;
  }

  // Animation keys to choose from for each kind of game
  const char* pool;
  switch (gameCategory) {
    case 'r': pool = "U+YGDq"; break;   // Marvel Rivals: ult ready, healing, combo, fuming, dash, game face
    case 'h': pool = "q62d4";  break;   // horror: game face, scared, look around, double-take, surprised
    case 's': pool = "q7b4l";  break;   // sports/racing: game face, excited, bounce, surprised, laugh
    case 'f': pool = "qq04f";  break;   // shooter: lots of game face, grumpy, surprised, focus
    case 'c': pool = "921bq";  break;   // chill: curious, look around, wink, bounce, game face
    default:  pool = "qq794";  break;   // general: game face, curious, excited, surprised
  }
  playKey(pool[random(0, strlen(pool))]);
}

// ---------- Identity ----------
void loadIdentity() {
  Preferences idp;
  idp.begin("robot", false);
  int saved = idp.getInt("id", 0);
  if (saved < 1 || saved > NUM_PROFILES || SET_IDENTITY_ON_UPLOAD) {
    saved = (WHICH_ROBOT >= 1 && WHICH_ROBOT <= NUM_PROFILES) ? WHICH_ROBOT : 1;
    idp.putInt("id", saved);
    Serial.print("Identity saved on this board: ");
  } else {
    Serial.print("This board remembers it is: ");
  }
  idp.end();
  robotId = saved;
  robotName = PROFILES[saved - 1].name;
  robotHost = PROFILES[saved - 1].host;
  robotRotation = PROFILES[saved - 1].rotation;
  HOSTNAME = robotHost;
  Serial.println(robotName);
}

// ---------- Wireless updates ----------
void drawUpdateScreen(const char* title, int percent) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(4, 10);
  display.print(title);
  if (percent >= 0) {
    display.drawRect(4, 30, 120, 12, SSD1306_WHITE);
    display.fillRect(6, 32, (116 * percent) / 100, 8, SSD1306_WHITE);
    display.setCursor(52, 48);
    display.print(percent);
    display.print("%");
  }
  display.display();
}

void startOta() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.setMdnsEnabled(false);          // announced by announceOnNetwork() instead
  ArduinoOTA.onStart([]() {
    Serial.println("Wireless update starting...");
    fxType = FX_NONE; customFace = FACE_NONE; snoring = false; maskOn = false;
    drawUpdateScreen("Updating...", 0);
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    static int last = -1;
    int pct = total ? (done * 100) / total : 0;
    if (pct != last) { last = pct; drawUpdateScreen("Updating...", pct); }
  });
  ArduinoOTA.onEnd([]() { drawUpdateScreen("Done! Restarting", 100); Serial.println("Update done, restarting"); });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("Update failed (error %u)\n", e);
    drawUpdateScreen("Update failed", -1);
  });
  ArduinoOTA.begin();
}

// ---------- Web page ----------
WebServer server(80);
HTTPUpdateServer httpUpdater;
bool webStarted = false;

const char PAGE_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<link rel="manifest" href="/manifest.json">
<meta name="theme-color" content="#0a0d12">
<title>Robot</title>
<style>
  :root {
    --bg: #0a0d12; --panel: #131820; --panel2: #1a212c; --line: #253041;
    --text: #e8eef6; --muted: #8494a8; --glow: #7ff3ff; --night: #9d8cff;
    --extra: #ffb45e;
  }
  * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
  body {
    margin: 0; background: var(--bg); color: var(--text);
    font: 15px/1.4 -apple-system, system-ui, "Segoe UI", Roboto, sans-serif;
    padding: max(16px, env(safe-area-inset-top)) 16px 40px;
  }
  .wrap { max-width: 560px; margin: 0 auto; }
  header { display: flex; align-items: baseline; justify-content: space-between; margin: 4px 2px 14px; }
  h1 { font-size: 22px; letter-spacing: 4px; margin: 0; font-weight: 800; }
  #conn { font-size: 12px; color: var(--muted); }
  #conn.off { color: #ff6b6b; }

  /* The robot's screen */
  .bezel {
    background: #050608; border: 1px solid var(--line); border-radius: 18px;
    padding: 18px; box-shadow: 0 0 40px rgba(127,243,255,.08) inset;
  }
  canvas {
    display: block; width: 100%; aspect-ratio: 2 / 1;
    image-rendering: pixelated; image-rendering: crisp-edges;
    filter: drop-shadow(0 0 6px rgba(127,243,255,.55));
  }
  .now { text-align: center; color: var(--muted); font-size: 13px; margin-top: 10px; min-height: 18px; }
  .now b { color: var(--glow); font-weight: 600; }

  /* Status */
  .stats { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; margin: 14px 0; }
  .stat { background: var(--panel); border: 1px solid var(--line); border-radius: 12px; padding: 10px 12px; }
  .stat small { display: block; color: var(--muted); font-size: 11px; text-transform: uppercase; letter-spacing: 1px; }
  .stat span { font-size: 16px; font-weight: 600; }

  /* Day / night switch */
  .seg { display: grid; grid-template-columns: repeat(3, 1fr); background: var(--panel); border: 1px solid var(--line);
         border-radius: 12px; padding: 4px; gap: 4px; margin-bottom: 22px; }
  .seg button { border: 0; background: transparent; color: var(--muted); padding: 10px; border-radius: 9px;
                font: inherit; font-weight: 600; }
  .seg button.on { background: var(--panel2); color: var(--text); box-shadow: 0 0 0 1px var(--line); }

  /* Animation buttons */
  h2 { font-size: 12px; color: var(--muted); text-transform: uppercase; letter-spacing: 1.5px; margin: 22px 2px 10px; }
  h2 i { font-style: normal; text-transform: none; letter-spacing: 0; opacity: .8; }
  .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 8px; }
  .anim {
    position: relative; text-align: left; border: 1px solid var(--line); background: var(--panel);
    color: var(--text); border-radius: 12px; padding: 14px 14px; font: inherit; font-weight: 600;
    transition: transform .08s, border-color .2s, background .2s;
  }
  .anim:active { transform: scale(.97); }
  .anim kbd { position: absolute; right: 10px; top: 50%; transform: translateY(-50%);
              font: 11px ui-monospace, monospace; color: var(--muted); border: 1px solid var(--line);
              border-radius: 5px; padding: 1px 6px; }
  .anim.night { border-left: 3px solid var(--night); }
  .anim.extra { border-left: 3px solid var(--extra); }
  .anim.anytime { border-left: 3px solid var(--glow); }
  .anim.playing { border-color: var(--glow); background: #12252b; }
  .anim.gaming { border-left: 3px solid var(--ps); }

  /* PlayStation card */
  :root { --ps: #4c8dff; }
  .ps { background: var(--panel); border: 1px solid var(--line); border-radius: 14px; padding: 14px;
        margin-bottom: 14px; }
  .ps-head { display: flex; align-items: center; justify-content: space-between; margin-bottom: 10px; }
  .ps-head b { font-size: 12px; color: var(--muted); text-transform: uppercase; letter-spacing: 1.5px; }
  .pill { font-size: 12px; font-weight: 600; padding: 3px 9px; border-radius: 99px;
          background: var(--panel2); color: var(--muted); border: 1px solid var(--line); }
  .pill.on { color: #7dffa8; border-color: #285c3b; background: #12251a; }
  .pill.warn { color: var(--extra); border-color: #5c4528; }
  .game { display: flex; gap: 12px; align-items: center; }
  .game img { width: 64px; height: 64px; border-radius: 10px; object-fit: cover; background: var(--panel2); flex: none; }
  .game .t { font-weight: 700; font-size: 16px; line-height: 1.25; }
  .game .s { color: var(--muted); font-size: 13px; margin-top: 3px; }
  .idle { color: var(--muted); font-size: 14px; }
  .setup ol { margin: 6px 0 10px; padding-left: 20px; color: var(--muted); font-size: 13px; }
  .setup ol li { margin: 4px 0; }
  .setup a { color: var(--ps); }
  .setup .row { display: flex; gap: 8px; }
  .setup input { flex: 1; min-width: 0; background: var(--bg); border: 1px solid var(--line); color: var(--text);
                 border-radius: 10px; padding: 10px 12px; font: 14px ui-monospace, monospace; }
  .setup button { border: 0; background: var(--ps); color: #fff; font: inherit; font-weight: 700;
                  border-radius: 10px; padding: 0 16px; }
  .setup .msg { font-size: 13px; margin-top: 8px; color: var(--muted); min-height: 18px; }
  details summary { color: var(--muted); font-size: 13px; margin-top: 12px; cursor: pointer; }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1 id="robotName">&nbsp;</h1>
    <div id="conn">connecting...</div>
  </header>

  <div class="bezel"><canvas id="scr" width="128" height="64"></canvas></div>
  <div class="now" id="now"></div>

  <div class="stats">
    <div class="stat"><small>Time</small><span id="time">--</span></div>
    <div class="stat"><small>Sunset</small><span id="sunset">--</span></div>
    <div class="stat"><small>Now</small><span id="phase">--</span></div>
  </div>

  <div class="ps">
    <div class="ps-head"><b>PlayStation</b><span class="pill" id="psPill">--</span></div>
    <div id="psBody"></div>
    <div class="setup" id="psSetup" hidden>
      <ol>
        <li>Sign in at <a href="https://www.playstation.com" target="_blank">playstation.com</a> in your browser.</li>
        <li>In the same browser, open <a href="https://ca.account.sony.com/api/v1/ssocookie" target="_blank">this page</a> and copy what it shows.</li>
        <li>Paste it below. Don't sign out of playstation.com afterwards, or the code stops working.</li>
      </ol>
      <div class="row">
        <input id="npsso" placeholder="Paste sign-in code" autocomplete="off" autocapitalize="off" spellcheck="false">
        <button id="psSave">Save</button>
      </div>
      <div class="msg" id="psMsg"></div>
    </div>
    <details id="psRedo" hidden><summary>Update sign-in code</summary></details>
  </div>

  <div class="seg" id="mode">
    <button data-m="0">Auto</button>
    <button data-m="1">Night</button>
    <button data-m="2">Day</button>
  </div>

  <h2>Anytime</h2>
  <div class="grid" id="g1"></div>
  <h2>After sunset <i>(random only at night)</i></h2>
  <div class="grid" id="g2"></div>
  <h2>Gaming <i>(plays while you're on PlayStation)</i></h2>
  <div class="grid" id="g3"></div>
  <h2>Extras <i>(never random)</i></h2>
  <div class="grid" id="g0"></div>
</div>

<script>
const cv = document.getElementById('scr'), ctx = cv.getContext('2d');
const img = ctx.createImageData(128, 64);
const $ = id => document.getElementById(id);
let playing = '';

// Draw the robot's screen from the raw 1024-byte display buffer
function draw(buf) {
  const d = img.data;
  for (let y = 0; y < 64; y++) {
    for (let x = 0; x < 128; x++) {
      const sx = flip ? 127 - x : x, sy = flip ? 63 - y : y;      // upside-down robots
      const on = (buf[sx + (sy >> 3) * 128] >> (sy & 7)) & 1;
      const i = (y * 128 + x) * 4;
      if (on) { d[i] = 200; d[i+1] = 250; d[i+2] = 255; }
      else    { d[i] = 5;   d[i+1] = 6;   d[i+2] = 8;   }
      d[i+3] = 255;
    }
  }
  ctx.putImageData(img, 0, 0);
}

function screenLoop() {
  fetch('/screen', { cache: 'no-store' })
    .then(r => r.arrayBuffer())
    .then(b => { if (b.byteLength === 1024) draw(new Uint8Array(b)); setConn(true); })
    .catch(() => setConn(false))
    .finally(() => setTimeout(screenLoop, 120));
}

function setConn(ok) {
  $('conn').textContent = ok ? 'live' : 'offline';
  $('conn').className = ok ? '' : 'off';
}

// Show this robot's own name in the header and browser tab
let flip = false;
fetch('/status', { cache: 'no-store' }).then(r => r.json()).then(s => {
  if (s.name) { $('robotName').textContent = s.name.toUpperCase(); document.title = s.name; }
  flip = s.rot === 2;
}).catch(() => {});

function statusLoop() {
  fetch('/status', { cache: 'no-store' })
    .then(r => r.json())
    .then(s => {
      $('time').textContent = s.synced ? s.time : '--';
      $('sunset').textContent = s.synced ? s.sunset : '--';
      $('phase').textContent = s.night ? 'Night' : 'Day';
      document.querySelectorAll('#mode button').forEach(b =>
        b.classList.toggle('on', +b.dataset.m === s.mode));
      playing = s.anim;
      $('now').innerHTML = s.anim ? 'Playing <b>' + s.anim + '</b>' : '';
      document.querySelectorAll('.anim').forEach(b =>
        b.classList.toggle('playing', b.dataset.n === s.anim));
      if (s.psn) showPsn(s.psn);
    })
    .catch(() => {})
    .finally(() => setTimeout(statusLoop, 1000));
}

// ---- PlayStation card ----
const esc = t => String(t).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
let lastPsnKey = '';
function showPsn(p) {
  const pill = $('psPill'), setup = $('psSetup'), redo = $('psRedo');
  const labels = ['Not set up', 'Connecting...', p.online ? 'Online' : 'Offline', 'Needs new code', 'Retrying...'];
  pill.textContent = labels[p.state] || '--';
  pill.className = 'pill' + (p.state === 2 && p.online ? ' on' : (p.state === 3 ? ' warn' : ''));

  const needSetup = p.state === 0 || p.state === 3;
  if (needSetup) setup.hidden = false;
  else if (!redo.open) setup.hidden = true;
  redo.hidden = needSetup;

  // Only redraw the game info when it changes
  const key = [p.state, p.online, p.game, p.icon, p.trophies, p.session].join('|');
  if (key === lastPsnKey) return;
  lastPsnKey = key;
  let html = '';
  if (p.state === 2 && p.game) {
    const mins = p.session >= 0 ? (p.session >= 60 ? Math.floor(p.session / 60) + 'h ' + (p.session % 60) + 'm'
                                                     : p.session + 'm') : '';
    html = '<div class="game">' + (p.icon ? '<img src="' + esc(p.icon) + '" alt="">' : '') +
      '<div><div class="t">' + esc(p.game) + '</div><div class="s">' +
      [p.platform, mins ? 'Playing for ' + mins : '', p.trophies >= 0 ? p.trophies + ' trophies' : '']
        .filter(Boolean).map(esc).join(' &middot; ') + '</div></div></div>';
  } else if (p.state === 2) {
    html = '<div class="idle">' + (p.online ? 'Online, not in a game.' : 'Console is offline.') +
           (p.trophies >= 0 ? ' &middot; ' + p.trophies + ' trophies' : '') + '</div>';
  } else if (p.state === 3) {
    html = '<div class="idle">The sign-in code expired. Paste a new one to reconnect.</div>';
  } else if (p.state === 4) {
    html = '<div class="idle">Having trouble reaching PlayStation. Trying again...</div>';
  }
  $('psBody').innerHTML = html;
}

$('psRedo').addEventListener('toggle', () => { $('psSetup').hidden = !$('psRedo').open; });

$('psSave').onclick = () => {
  let v = $('npsso').value.trim();
  const m = v.match(/"npsso"\s*:\s*"([^"]+)"/);   // accept the whole page's text too
  if (m) v = m[1];
  if (v.length < 20) { $('psMsg').textContent = "That doesn't look like a sign-in code."; return; }
  $('psMsg').textContent = 'Connecting...';
  fetch('/psn', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                  body: 'npsso=' + encodeURIComponent(v) })
    .then(r => { $('psMsg').textContent = r.ok ? 'Saved! He will connect in a few seconds.' : 'Something went wrong, try again.';
                 if (r.ok) { $('npsso').value = ''; $('psRedo').open = false; } })
    .catch(() => { $('psMsg').textContent = "Couldn't reach Monstrix."; });
};

function play(k) {
  fetch('/play?k=' + encodeURIComponent(k)).then(() => setTimeout(statusLoopOnce, 100));
}
function statusLoopOnce() {
  fetch('/status', { cache: 'no-store' }).then(r => r.json()).then(s => {
    $('now').innerHTML = s.anim ? 'Playing <b>' + s.anim + '</b>' : '';
    document.querySelectorAll('.anim').forEach(b =>
      b.classList.toggle('playing', b.dataset.n === s.anim));
  }).catch(() => {});
}

document.querySelectorAll('#mode button').forEach(b =>
  b.onclick = () => fetch('/mode?m=' + b.dataset.m).then(statusLoopOnce));

// Build the buttons from the robot's own animation list
fetch('/list').then(r => r.json()).then(list => {
  const cls = ['extra', 'anytime', 'night', 'gaming'];
  list.forEach(a => {
    const b = document.createElement('button');
    b.className = 'anim ' + cls[a.w];
    b.dataset.n = a.n;
    b.innerHTML = a.n + '<kbd>' + a.k + '</kbd>';
    b.onclick = () => play(a.k);
    $('g' + a.w).appendChild(b);
  });
});

screenLoop();
statusLoop();
</script>
</body>
</html>
)rawliteral";

void fmtTime(int minutes, char* out) {
  if (minutes < 0) { strcpy(out, "--"); return; }
  int h = minutes / 60, m = minutes % 60;
  int h12 = (h % 12 == 0) ? 12 : h % 12;
  sprintf(out, "%d:%02d %s", h12, m, h < 12 ? "AM" : "PM");
}

void handleManifest() {
  String m = "{\"name\":\"";
  m += robotName;
  m += "\",\"short_name\":\"";
  m += robotName;
  m += "\",\"start_url\":\"/\",\"display\":\"standalone\",\"background_color\":\"#0a0d12\",\"theme_color\":\"#0a0d12\"}";
  server.send(200, "application/manifest+json", m);
}

void handleRoot() {
  server.send_P(200, "text/html", PAGE_HTML);
}

// Sends the raw screen buffer so the page can show a live mirror of his face
void handleScreen() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "application/octet-stream",
                (const char*)display.getBuffer(), SCREEN_WIDTH * SCREEN_HEIGHT / 8);
}

void handleList() {
  String json = "[";
  for (int i = 0; i < NUM_ANIMS; i++) {
    if (i) json += ",";
    json += "{\"k\":\"";
    json += animations[i].key;
    json += "\",\"n\":\"";
    json += animations[i].name;
    json += "\",\"w\":";
    json += animations[i].when;
    json += "}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleStatus() {
  char nowStr[12] = "--", riseStr[12], setStr[12];
  struct tm now;
  if (getLocalTime(&now, 0)) fmtTime(now.tm_hour * 60 + now.tm_min, nowStr);
  fmtTime(timeValid ? sunriseMin : -1, riseStr);
  fmtTime(timeValid ? sunsetMin : -1, setStr);

  String json = "{\"synced\":";
  json += timeValid ? "true" : "false";
  json += ",\"time\":\"";    json += nowStr;
  json += "\",\"sunrise\":\""; json += riseStr;
  json += "\",\"sunset\":\"";  json += setStr;
  json += "\",\"night\":";   json += isNight ? "true" : "false";
  json += ",\"mode\":";      json += nightOverride;
  json += ",\"rot\":";       json += robotRotation;
  json += ",\"name\":\"";    json += jsonEscape(robotName);
  json += "\",\"anim\":\"";    json += currentAnimName;
  json += "\"";

  // PlayStation info
  xSemaphoreTake(psnLock, portMAX_DELAY);
  json += ",\"psn\":{\"state\":"; json += psnState;
  json += ",\"online\":";  json += psnOnline ? "true" : "false";
  json += ",\"platform\":\""; json += jsonEscape(psnPlatform);
  json += "\",\"game\":\"";   json += jsonEscape(psnGame);
  json += "\",\"icon\":\"";   json += jsonEscape(psnIcon);
  json += "\",\"trophies\":"; json += psnTrophies;
  json += ",\"session\":";
  json += psnGame.length() ? (long)((millis() - psnSessionStart) / 60000UL) : -1L;
  json += "}";
  xSemaphoreGive(psnLock);

  json += "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

String jsonEscape(const String& s) {
  String out;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((unsigned char)c < 0x20) out += ' ';
    else out += c;
  }
  return out;
}

// Receives the PSN sign-in code pasted on the web page
void handlePsnCode() {
  String code = server.arg("npsso");
  code.trim();
  if (code.length() < 20) {
    server.send(400, "text/plain", "That doesn't look like a sign-in code.");
    return;
  }
  xSemaphoreTake(psnLock, portMAX_DELAY);
  psnNewCode = code;
  xSemaphoreGive(psnLock);
  psnState = PSN_CONNECTING;
  server.send(200, "text/plain", "ok");
}

void handlePlay() {
  if (!server.hasArg("k") || server.arg("k").length() == 0) {
    server.send(400, "text/plain", "missing key");
    return;
  }
  chainQueue[0] = 0;
  if (playKey(server.arg("k")[0])) server.send(200, "text/plain", "ok");
  else server.send(404, "text/plain", "unknown animation");
}

void handleMode() {
  int m = server.arg("m").toInt();
  if (m < 0 || m > 2) m = 0;
  nightOverride = m;
  updateDayNight();
  server.send(200, "text/plain", "ok");
}

// Announces http://<name>.local and the wireless-update port on the network.
// Called again after every reconnect, because the announcement is lost when Wi-Fi drops.
bool mdnsStarted = false;
void announceOnNetwork() {
  if (mdnsStarted) return;                   // the .local name survives Wi-Fi dropouts by itself
  if (MDNS.begin(HOSTNAME)) {
    mdnsStarted = true;
    MDNS.addService("http", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "name", robotName);
    MDNS.enableArduino(3232, true);          // shows up as a network port in Arduino IDE
  } else {
    Serial.println("Couldn't start the .local name - use the number address below.");
  }
}

void startWebServer() {
  if (WiFi.status() != WL_CONNECTED || webStarted) return;
  announceOnNetwork();
  startOta();

  server.on("/", handleRoot);
  server.on("/manifest.json", handleManifest);
  server.on("/screen", handleScreen);
  server.on("/list", handleList);
  server.on("/status", handleStatus);
  server.on("/play", handlePlay);
  server.on("/mode", handleMode);
  server.on("/psn", HTTP_POST, handlePsnCode);
  httpUpdater.setup(&server, "/update", "admin", OTA_PASSWORD);   // browser upload page
  server.begin();
  webStarted = true;

  Serial.print("Web page ready: http://");
  Serial.print(HOSTNAME);
  Serial.print(".local  or  http://");
  Serial.println(WiFi.localIP());
  Serial.println("Wireless updates ready (Arduino IDE network port, or /update in a browser)");
}

// Keeps him on the network: retries Wi-Fi, starts the web page once it's up,
// and re-announces the .local name after a dropout.
void maintainWiFi() {
  static unsigned long lastCheck = 0, downSince = 0;
  static bool wasConnected = true;           // setup() already handled the first connect
  if (millis() - lastCheck < 1000) return;   // check once a second
  lastCheck = millis();
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    Serial.print("Wi-Fi connected: "); Serial.println(WiFi.localIP());
    configTzTime(TIMEZONE, "pool.ntp.org", "time.nist.gov");
    if (!webStarted) startWebServer();
  }
  if (!connected && wasConnected) {
    Serial.print("Wi-Fi lost (");
    Serial.print(WiFi.disconnectReasonName((wifi_err_reason_t)lastDropReason));
    Serial.println(") - reconnecting...");
    downSince = millis();
  }
  // The Wi-Fi library reconnects on its own. Only give it a fresh start if it's
  // still down after 30 s (never interrupt an attempt that's in progress).
  if (!connected && millis() - downSince > 30000) {
    downSince = millis();
    Serial.println("Still no Wi-Fi - starting a fresh connection");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
  }
  if (connected && !webStarted) startWebServer();
  wasConnected = connected;
}

void printNetwork() {
  Serial.print("Wi-Fi: ");
  Serial.println(WiFi.status() == WL_CONNECTED ? "connected" : "NOT connected");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("  Address: http://"); Serial.println(WiFi.localIP());
    Serial.print("  Name:    http://"); Serial.print(HOSTNAME);
    Serial.println(mdnsStarted ? ".local" : ".local (not announced)");
    Serial.print("  Signal:  "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
  }
  Serial.print("  Web page running: "); Serial.println(webStarted ? "yes" : "no");
  Serial.print("  Wi-Fi drops since power-on: "); Serial.println(wifiDrops);
  if (wifiDrops > 0) {
    Serial.print("  Last drop reason: "); Serial.print(lastDropReason); Serial.print(" ");
    Serial.println(WiFi.disconnectReasonName((wifi_err_reason_t)lastDropReason));
  }
  Serial.print("  Last restart cause: ");
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  Serial.println("power on"); break;
    case ESP_RST_SW:       Serial.println("software restart (e.g. after an update)"); break;
    case ESP_RST_BROWNOUT: Serial.println("BROWNOUT - power dipped (cable/charger too weak)"); break;
    case ESP_RST_PANIC:    Serial.println("crash"); break;
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: Serial.println("watchdog (froze)"); break;
    default:               Serial.println((int)esp_reset_reason()); break;
  }
  Serial.print("  Running for: "); Serial.print(millis() / 60000); Serial.println(" min");
}

// ---------- Setup ----------
void setup() {
  Wire.begin();
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // Don't wait for the Serial Monitor: without this he freezes when it's closed
  // or when he's powered without a computer listening.
  Serial.setTxTimeoutMs(0);
#endif
  loadIdentity();
  Serial.print(robotName);
  Serial.println(" (Monstrix v2.13) starting...");
  startPsn();   // loads any saved PlayStation sign-in and starts the background checker

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 allocation failed");
    return;
  }

  display.setRotation(robotRotation);    // Nova's screen is upside down (rotation 2)
  display.clearDisplay();
  display.display();

  eyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 60);
  eyes.setDisplayColors(0, 1);
  eyes.setAutoblinker(ON, 3, 1);
  eyes.setIdleMode(ON, 2, 1);
  eyes.setMood(DEFAULT);

#if USE_GYRO
  byte status = mpu.begin();
  if (status != 0) {
    Serial.print("MPU6050 connection failed. Status: ");
    Serial.println(status);
  } else {
    delay(1000);
    mpu.calcGyroOffsets();
    Serial.println("MPU6050 calibrated");
  }
#endif

  randomSeed(esp_random());
  connectWiFi();
  startWebServer();
  Serial.println("Ready!");
  printMenu();
}

// ---------- Main loop ----------
void loop() {
  // Either the eyes library, the snoring overlay or a custom face draws the screen
  if (fxType != FX_NONE) drawFx();             // hero effects
  else if (customFace != FACE_NONE) drawCustomFace();
  else if (snoring) drawSnore();   // eyes frozen, only the Z's move
  else eyes.update();

  updateAnimation();

  // Play the next queued animation (e.g. the Marvel Rivals intro)
  if (currentAnim == nullptr && chainQueue[0]) {
    char k = chainQueue[0];
    memmove(chainQueue, chainQueue + 1, strlen(chainQueue));
    playKey(k);
  }
  maintainWiFi();
  if (webStarted) { server.handleClient(); ArduinoOTA.handle(); }
  delay(16);

  unsigned long now = millis();

  // Check day/night every 30 seconds (and soon after start-up)
  if (now - lastTimeCheck > 30000 || (!timeValid && now - lastTimeCheck > 2000)) {
    lastTimeCheck = now;
    updateDayNight();
  }

  // React to PlayStation events (game started, trophy, game over)
  handlePsnEvents();

  // Dim at night when you're not gaming
  updateBrightness();

  // Serial Monitor test keys
  if (Serial.available()) {
    char key = Serial.read();
    if (key == '?') {
      printMenu();
    } else if (key == '!') {
      printNetwork();
    } else if (key == 't') {
      printStatus();
    } else if (key == 'N') {
      nightOverride = (nightOverride + 1) % 3;
      updateDayNight();
      printStatus();
    } else {
      chainQueue[0] = 0;
      playKey(key);
    }
  }

#if USE_GYRO
  mpu.update();
  float acc = abs(mpu.getAccX()) + abs(mpu.getAccY()) + abs(mpu.getAccZ());
  if (!isAngry && acc > 2.5) {
    if (currentAnim) endAnimation();
    Serial.println("Shake detected!");
    eyes.setMood(ANGRY);
    eyes.setSweat(ON);
    isAngry = true;
    lastAngryTime = now;
    return;
  }
  if (isAngry && (now - lastAngryTime > ANGRY_DURATION)) {
    isAngry = false;
    eyes.setSweat(OFF);
    eyes.setMood(DEFAULT);
    lastEmotionChange = now;
  }
#endif

  // Pick something new every EMOTION_INTERVAL (not while animating)
  if (!isAngry && currentAnim == nullptr && (now - lastEmotionChange > EMOTION_INTERVAL)) {
    lastEmotionChange = now;
    pickNextBehaviour();
  }
}

// ---------- Night dimming ----------
int brightness = BRIGHT_LEVEL;      // current level, fades toward the target
bool deepDim = false;
unsigned long lastFadeStep = 0;

bool shouldDim() {
  if (gaming || !timeValid) return false;          // gaming = full power
  struct tm now;
  if (!getLocalTime(&now, 0)) return false;
  int m = now.tm_hour * 60 + now.tm_min;
  return m >= DIM_FROM_HOUR * 60 + DIM_FROM_MIN || m < DIM_UNTIL_HOUR * 60;
}

void setPanel(int level, bool deep) {
  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(level);
  if (deep != deepDim) {           // extra dimming the contrast setting alone can't reach
    display.ssd1306_command(SSD1306_SETPRECHARGE);
    display.ssd1306_command(deep ? 0x11 : 0xF1);
    display.ssd1306_command(SSD1306_SETVCOMDETECT);
    display.ssd1306_command(deep ? 0x00 : 0x40);
    deepDim = deep;
  }
}

// Fades smoothly over about 3 seconds instead of snapping
void updateBrightness() {
  static bool wasDim = false;
  static unsigned long lastCheck = 0;
  static bool dimNow = false;
  if (millis() - lastCheck > 1000) { lastCheck = millis(); dimNow = shouldDim(); }
  if (dimNow != wasDim) {
    Serial.println(dimNow ? "Dimming for the night" : "Back to full brightness");
    wasDim = dimNow;
  }
  int target = dimNow ? DIM_LEVEL : BRIGHT_LEVEL;
  if (brightness == target || millis() - lastFadeStep < 15) return;
  lastFadeStep = millis();
  if (!dimNow && deepDim) setPanel(brightness, false);   // undo deep dim before brightening
  brightness += (target > brightness) ? 1 : -1;          // one step every 15 ms
  setPanel(brightness, dimNow && brightness == target);   // deepest dim once fully faded
}

// ---------- Behaviour picker ----------
// 60% of the time: a simple mood (default or happy)
// 40% of the time: a random animation (ANYTIME ones, plus NIGHT ones after sunset)
bool canPickNow(int i) {
  if (animations[i].when == ANYTIME) return true;
  if (animations[i].when == NIGHT) return isNight;
  return false;
}

void pickNextBehaviour() {
  if (gaming) {          // PlayStation gaming mode takes over while you play
    pickGamingBehaviour();
    return;
  }
  if (random(0, 100) < 60) {
    if (random(0, 2) == 0) {
      eyes.setMood(DEFAULT);
      Serial.println("Mood: Default");
    } else {
      eyes.setMood(HAPPY);
      Serial.println("Mood: Happy");
    }
    return;
  }

  int count = 0;
  for (int i = 0; i < NUM_ANIMS; i++) if (canPickNow(i)) count++;
  int pick = random(0, count);
  for (int i = 0; i < NUM_ANIMS; i++) {
    if (!canPickNow(i)) continue;
    if (pick-- == 0) {
      startAnimation(animations[i].fn, animations[i].name);
      return;
    }
  }
}
