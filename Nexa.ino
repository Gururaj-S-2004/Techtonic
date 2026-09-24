#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <ESP32Servo.h>
#include <WiFi.h>

// ============================================================================
// WIRE PROTOCOL (USB Serial, 921600 baud, matches robot_backend/serial_link.py)
// -----------------------------------------------------------------------------
// ESP32 -> laptop (ASCII lines, '\n'-terminated unless noted):
//   STATUS:<text>              informational, laptop just logs it
//     STATUS:LISTEN_READY      signals the laptop to start recording from its
//                               own microphone (no audio captured on ESP32)
//     STATUS:GREETING_DONE     greeting animation complete
//     STATUS:IDLE              interaction finished, back to idle
//   TRIGGER:BUTTON              button pressed while idle
//   TRIGGER:PROXIMITY           someone detected in range while idle
//   ERROR:<text>                something went wrong on the ESP32 side
//
// laptop -> ESP32 (ASCII lines):
//   COMMAND:GREET                play wave animation (local, no audio)
//   COMMAND:LISTEN               ESP32 signals LISTEN_READY; laptop records
//                                 from its own mic, sends STATUS:RECORDING_DONE
//                                 when finished
//   COMMAND:PROCESSING           purely a status hint ("Thinking...")
//   COMMAND:SPEAK                laptop will play TTS audio on its own speaker;
//                                 ESP32 just updates the display and waits for
//                                 COMMAND:IDLE
//   COMMAND:IDLE                 return to idle / reset
//   STATUS:RECORDING_DONE        laptop finished capturing from its own mic
//
// Both sides must agree on this exactly - see robot_backend/serial_link.py.
// ============================================================================

#define SERIAL_BAUD 921600

// ============================================================================
// PIN DEFINITIONS (ESP32-S3 DevKit - avoids strapping pins 0/3/45/46 and
// the native-USB pins 19/20)
// ============================================================================
// Triggers
#define PIN_BUTTON 4 // push button, other leg to GND, uses internal pull-up
#define PIN_TRIG 5   // HC-SR04 TRIG
#define PIN_ECHO 6 // HC-SR04 ECHO (use a resistor divider: sensor is 5V logic)

// Actuators / indicators
#define PIN_SERVO 7 // servo signal wire

// 1.8" TFT SPI 128x160 (ST7735)
#define PIN_TFT_RST 8   // RES / RESET
#define PIN_TFT_DC 9    // DC / A0 / RS
#define PIN_TFT_CS 10   // CS
#define PIN_TFT_MOSI 11 // SDA / MOSI / DIN
#define PIN_TFT_SCLK 12 // SCL / SCK / CLK
// Note: Connect TFT BLK/LED to 3.3V, VCC to 3.3V (or 5V), GND to GND

// Audio: all audio is now played by the laptop's own speaker.
// The MAX98357A I2S amplifier has been removed. No speaker pins are needed.

// Microphone: audio is captured by the laptop's own mic.
// The INMP441 I2S mic has been removed. No mic pins are needed on the ESP32.

// ============================================================================
// 1.8" TFT SPI CONFIG (ST7735 128x160)
// ============================================================================
#define TFT_WIDTH 160
#define TFT_HEIGHT 128
    // Pass the SPI class explicitly to ensure it uses the custom pins on
    // ESP32-S3
        Adafruit_ST7735 tft =
            Adafruit_ST7735(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
bool tftReady = false;

// ============================================================================
// TFT THEME (Nexa Bot colors: maroon + gold) & UI LAYOUT
// ============================================================================
#define RGB565(r, g, b)                                                        \
  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define COL_BG RGB565(12, 3, 6) // near-black maroon wash (screen background)
#define COL_HEADER RGB565(122, 12, 40) // Techtonic maroon (header bar)
#define COL_ACCENT                                                             \
  RGB565(255, 178, 36) // warm gold accent (titles, icons, dividers)
#define COL_ACCENT_DIM                                                         \
  RGB565(150, 96, 24) // muted gold (secondary icon details, dividers)
#define COL_TEXT RGB565(250, 238, 220) // warm cream (body text)

#define HEADER_H 20  // header bar height
#define BADGE_CX 146 // small always-on status icon, top-right of header
#define BADGE_CY 10
#define BADGE_SIZE 13
#define BIG_ICON_CX 30 // large status icon shown alongside short status text
#define BIG_ICON_CY 64
#define BIG_ICON_SIZE 40
#define TEXT_X 58            // text column start when the big icon is shown
#define ANIM_INTERVAL_MS 220 // status icon animation frame rate

// Animation state: which icon frame is showing, and whether the large body
// icon is active for the screen currently on display (set by showStatus()).
int animFrame = 0;
unsigned long lastAnimMs = 0;
bool bigIconActive = false;

// ============================================================================
// SERVO CONFIG
// ============================================================================
Servo handServo;
#define SERVO_REST_ANGLE 0
#define SERVO_WAVE_ANGLE 90

// ============================================================================
// ULTRASONIC CONFIG
// ============================================================================
#define TRIGGER_DISTANCE_CM                                                    \
  60UL // start interaction if someone is closer than this
#define ULTRASONIC_TIMEOUT_US 30000UL // ~5 m max range

// ============================================================================
// SERIAL TIMEOUT
// ============================================================================
#define SERIAL_CMD_TIMEOUT_MS 20000 // how long to wait for a laptop command
// The THINK phase (Groq LLM call in robot_backend/llm.py) retries up to 3x
// with a 25s timeout and 2s backoff between attempts - up to ~79s worst
// case - before Python ever sends DISPLAY_A/SPEAK back. If this window is
// too short, the ESP32 gives up and silently reverts to the idle "Ready"
// screen while Python is still mid-answer, desyncing the TFT from the
// terminal/audio for the rest of that session. Must stay >= Python's worst
// case LLM+TTS latency; see llm.py's retry loop before shrinking this.
#define SERIAL_THINK_TIMEOUT_MS 90000

// ============================================================================
// STATE MACHINE
// ============================================================================
enum SystemState {
  STATE_IDLE,
  STATE_GREETING,
  STATE_LISTENING,
  STATE_WAITING_RESPONSE,
  STATE_SPEAKING,
  STATE_ERROR
};
SystemState currentState = STATE_IDLE;

unsigned long lastInteractionTime = 0;
const unsigned long TRIGGER_COOLDOWN_MS =
    3000; // ignore new triggers right after one finishes

// Ultrasonic measurement pacing (HC-SR04 requires >= 60ms between pings to
// prevent echo flooding)
unsigned long lastPingTime = 0;
const unsigned long PING_INTERVAL_MS = 100;
long lastReportedDistance = -999;

// Persistent servo state driven by proximity (runs in ALL states)
bool servoOn = false;
// Set to true inside updateServoFromProximity() when the person walks away
// during an active session — cleared when we return to idle.
bool personLeftDuringSession = false;

// Forward declarations
void showStatus(const char *title, const char *body, uint16_t titleColor = 0);
void showIdleDistance(long dist);
void updateAnimation();
void updateServoFromProximity();

// Called everywhere we poll so the servo always tracks the sensor.
void updateServoFromProximity() {
  if (millis() - lastPingTime < PING_INTERVAL_MS)
    return;
  lastPingTime = millis();
  long dist = readDistanceCM();
  bool shouldBeOn = (dist > 0 && dist <= TRIGGER_DISTANCE_CM);
  if (shouldBeOn != servoOn) {
    servoOn = shouldBeOn;
    handServo.write(servoOn ? SERVO_WAVE_ANGLE : SERVO_REST_ANGLE);
    // If person walks away during an active interaction, signal the backend
    if (!servoOn && currentState != STATE_IDLE) {
      personLeftDuringSession = true;
      sendStatus("PERSON_LEFT");
    }
  }
  // Also update the idle distance display when idle
  if (currentState == STATE_IDLE) {
    if (abs(dist - lastReportedDistance) >= 2 ||
        (dist <= TRIGGER_DISTANCE_CM) != (lastReportedDistance <= TRIGGER_DISTANCE_CM)) {
      lastReportedDistance = dist;
      showIdleDistance(dist);
    }
  }
}


// ============================================================================
// SETUP
// ============================================================================
void setup() {
  // 32 KB RX buffer: at 921600 baud (~92 KB/s) this gives ~350ms of headroom.
  Serial.setRxBufferSize(32768);
  Serial.begin(SERIAL_BAUD);
  unsigned long bootStart = millis();
  while (!Serial && millis() - bootStart < 3000) {
    delay(10);
  }
  delay(300);

  setupTFT();
  setupServo();
  setupTriggers();

  showStatus("Ready", "Press button\nor stand\nclose to\nstart");
  sendStatus("READY");

  // Turn on Wi-Fi to draw extra current and prevent power bank auto-shutdown
  WiFi.mode(WIFI_STA);
}

// ============================================================================
// MAIN LOOP
// Idle: poll triggers AND watch for an out-of-band COMMAND: line from the
// laptop (e.g. it may want to push COMMAND:IDLE on reconnect). Once
// triggered, run the whole interaction start-to-finish (blocking) then
// return to idle. Only one visitor is served at a time.
// ============================================================================
void loop() {
  // Servo tracks proximity in ALL states (runs every 100ms regardless of
  // what the interaction state machine is doing)
  updateServoFromProximity();

  if (currentState == STATE_IDLE) {
    // Drain and ignore any stray laptop command while idle (keeps protocol
    // in sync if the backend restarts mid-session and resends COMMAND:IDLE).
    if (Serial.available()) {
      String stray = Serial.readStringUntil('\n');
      stray.trim();
      // nothing to act on here; idle already implies COMMAND:IDLE state
    }

    updateAnimation(); // keep the idle status icon breathing while waiting for
                       // a visitor

    if (millis() - lastInteractionTime < TRIGGER_COOLDOWN_MS)
      return;

    bool buttonTrigger = digitalRead(PIN_BUTTON) == LOW; // active-low button

    // Proximity trigger (display update now handled inside
    // updateServoFromProximity above)
    bool proximityTrigger =
        (lastReportedDistance > 0 && lastReportedDistance <= TRIGGER_DISTANCE_CM);

    if (buttonTrigger || proximityTrigger) {
      runInteraction(buttonTrigger ? "BUTTON" : "PROXIMITY");
      lastInteractionTime = millis();
      lastReportedDistance = -999;
    }
  }
}

// ============================================================================
// FULL INTERACTION SEQUENCE
// ============================================================================
void runInteraction(const char *triggerSource) {
  // Turn off Wi-Fi during interaction to prevent radio interference
  WiFi.mode(WIFI_OFF);

  Serial.print("TRIGGER:");
  Serial.println(triggerSource);

  // --- 1. Wait for COMMAND:GREET from the backend ---
  String cmd = waitForCommand(SERIAL_CMD_TIMEOUT_MS);
  if (cmd == "PERSON_LEFT" || cmd != "GREET") {
    if (cmd != "PERSON_LEFT") sendError("Expected GREET, got: " + cmd);
    returnToIdle();
    return;
  }
  currentState = STATE_GREETING;
  showStatus("Hello!", "Ask your\nquestion\nafter beep");
  waveServo();
  sendStatus("GREETING_DONE");

  // ============================================================
  // SESSION LOOP — runs one Q&A round per iteration.
  // Python sends COMMAND:LISTEN to start each round.
  // Python sends COMMAND:IDLE when silence is detected (session over).
  // ============================================================
  String lastAnswer = "";
  int questionCount = 0;

  while (true) {
    // At the top of each round, wait for LISTEN (continue) or IDLE (end).
    // After a SPEAK the laptop plays audio first — use a long timeout so we
    // don't give up while audio is still playing.
    unsigned long cmdTimeout =
        (currentState == STATE_SPEAKING)
            ? SERIAL_CMD_TIMEOUT_MS * 4 // up to 80s for very long TTS
            : SERIAL_CMD_TIMEOUT_MS;

    cmd = waitForCommand(cmdTimeout);

    if (cmd == "PERSON_LEFT") {
      // Visitor walked away - end session cleanly
      showStatus("Bye!", "See you\nsoon!");
      delay(800);
      break;
    }

    if (cmd == "IDLE") {
      // Laptop detected silence — session is over.
      break;
    }

    if (cmd != "LISTEN") {
      sendError("Expected LISTEN or IDLE, got: " + cmd);
      break;
    }

    // --- LISTEN phase ---
    questionCount++;
    currentState = STATE_LISTENING;
    if (questionCount == 1) {
      showStatus("Listening...", "Speak your\nquestion\nnow");
    } else {
      showStatus("Listening...", "Ask another\nquestion");
    }
    streamMicToBackend();

    // --- THINK phase ---
    currentState = STATE_WAITING_RESPONSE;
    showStatus("Thinking...", "Please wait");
    lastAnswer = "";

    // Everything from here until SPEAK/IDLE arrives is bounded by the LLM's
    // (and then TTS's) worst-case latency, not the usual quick-reply budget
    // - see SERIAL_THINK_TIMEOUT_MS above.
    cmd = waitForCommand(SERIAL_THINK_TIMEOUT_MS);
    if (cmd == "PROCESSING" || cmd == "PERSON_LEFT") {
      if (cmd == "PERSON_LEFT") break;
      cmd = waitForCommand(SERIAL_THINK_TIMEOUT_MS);
    }
    if (cmd.startsWith("DISPLAY_Q:") || cmd == "PERSON_LEFT") {
      if (cmd == "PERSON_LEFT") break;
      cmd = waitForCommand(SERIAL_THINK_TIMEOUT_MS);
    }
    if (cmd.startsWith("DISPLAY_A:") || cmd == "PERSON_LEFT") {
      if (cmd == "PERSON_LEFT") break;
      lastAnswer = cmd.substring(10);
      // Switch state before drawing so the badge icon (mic/thinking-dots)
      // doesn't linger stale over the answer text - the answer being shown
      // is already the "about to speak" moment from the visitor's POV, even
      // though the actual COMMAND:SPEAK hasn't arrived yet.
      currentState = STATE_SPEAKING;
      showStatus("Answer:", lastAnswer.c_str());
      cmd = waitForCommand(SERIAL_THINK_TIMEOUT_MS);
    }

    if (cmd == "SPEAK") {
      // Laptop plays TTS on its own speaker. Answer stays on TFT.
      // currentState is already STATE_SPEAKING (set when the answer was
      // displayed above); just fall back to the top of the loop, where we
      // wait (with extended timeout) for LISTEN or IDLE.
    } else if (cmd == "IDLE") {
      // Session ended mid-round (TTS failed, or no answer).
      break;
    } else {
      sendError("Expected SPEAK or IDLE, got: " + cmd);
      break;
    }
  }

  returnToIdle();
}

void returnToIdle() {
  currentState = STATE_IDLE;
  personLeftDuringSession = false; // clear for next session
  // Servo position is now managed entirely by updateServoFromProximity();
  // do NOT force it here — if someone is still within 60cm when the
  // interaction ends the servo should stay on.
  showStatus("Ready", "Press button\nor stand\nclose to\nstart");
  lastReportedDistance = -999;
  sendStatus("IDLE");

  // Turn on Wi-Fi to draw extra current and prevent power bank auto-shutdown
  WiFi.mode(WIFI_STA);
}

// ============================================================================
// SERIAL LINE HELPERS
// ============================================================================
void sendStatus(const String &text) {
  Serial.print("STATUS:");
  Serial.println(text);
}

void sendError(const String &text) {
  Serial.print("ERROR:");
  Serial.println(text);
}

// Blocks until a "COMMAND:<word>" line arrives (or timeout), returns <word>.
// Returns "" on timeout. Any non-COMMAND line received while waiting is
// ignored (defensive against stray STATUS echoes etc.).
String waitForCommand(unsigned long timeoutMs) {
  // Use readLineBlocking() in 100ms slices instead of Serial.readStringUntil().
  // Serial.readStringUntil() has a hidden 1-second internal timeout (from
  // Serial.setTimeout() default): if the '\n' doesn't arrive within that
  // window it returns a truncated line that fails the "COMMAND:" check and
  // is silently discarded, desyncing the ESP32 from the Python backend.
  unsigned long start = millis();
  while (true) {
    unsigned long elapsed = millis() - start;
    if (elapsed >= timeoutMs)
      return "";
    // Poll in 100ms slices so we check the overall deadline frequently.
    unsigned long slice = timeoutMs - elapsed;
    if (slice > 100)
      slice = 100;
    String line = readLineBlocking(slice);
    updateAnimation(); // step the status icon while we block waiting on the
                       // laptop
    updateServoFromProximity(); // keep servo tracking sensor during wait
    // If the person walked away, surface it immediately to the caller
    if (personLeftDuringSession)
      return "PERSON_LEFT";
    if (line.length() == 0)
      continue;
    if (line.startsWith("COMMAND:")) {
      return line.substring(8);
    }
    // Non-COMMAND lines (STATUS echoes, stray bytes) are discarded; keep
    // waiting.
  }
}

// ============================================================================
// TRIGGER HARDWARE: button + HC-SR04
// ============================================================================
void setupTriggers() {
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);
}

// Returns distance in cm, or a large number if out of range/timeout.
long readDistanceCM() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  unsigned long duration = pulseIn(PIN_ECHO, HIGH, ULTRASONIC_TIMEOUT_US);
  if (duration == 0)
    return 9999; // no echo received = nothing in range

  return (long)(duration * 0.0343 / 2.0); // speed of sound conversion
}

// ============================================================================
// SERVO
// ============================================================================
void setupServo() {
  ESP32PWM::allocateTimer(0);
  handServo.setPeriodHertz(50);
  handServo.attach(PIN_SERVO, 500, 2400);
  handServo.write(SERVO_REST_ANGLE);
}

void waveServo() {
  for (int i = 0; i < 3; i++) {
    handServo.write(SERVO_WAVE_ANGLE);
    delay(300);
    handServo.write(SERVO_REST_ANGLE);
    delay(300);
  }
}

// ============================================================================
// TFT DISPLAY - pure status feedback, no laptop involvement
// Themed maroon/gold (Techtonic event colors) with an animated status icon
// that reflects currentState (idle pulse / wave hello / mic / thinking dots /
// speaker). See STATUS ICONS section below for the icon artwork itself.
// ============================================================================
void setupTFT() {
  // Pass MISO=-1 (not used), MOSI and SCLK as defined, CS managed by Adafruit.
  // 27 MHz is safe for ST7735 on short PCB traces; reduces per-frame latency.
  SPI.begin(PIN_TFT_SCLK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
  SPI.setFrequency(27000000);
  // Initialization for 1.8" TFT SPI 128x160 (ST7735)
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1); // landscape mode: 160 width x 128 height
  tft.fillScreen(COL_BG);
  tftReady = true;
}

// Longest pipe/newline-separated segment in body — decides layout below.
int longestSegment(const char *body) {
  int maxSegLen = 0, segLen = 0;
  for (const char *pp = body; *pp; pp++) {
    if (*pp == '|' || *pp == '\n') {
      if (segLen > maxSegLen)
        maxSegLen = segLen;
      segLen = 0;
    } else {
      segLen++;
    }
  }
  if (segLen > maxSegLen)
    maxSegLen = segLen;
  return maxSegLen;
}

// Prints body text at textSize=1, wrapping to a new line on '|' or '\n',
// clamped to maxY so it never overflows the display or the sensor zone.
void drawWrappedBody(int x, int y0, int lineSpacing, int maxY, const char *body,
                     uint16_t color) {
  tft.setTextSize(1);
  tft.setTextColor(color);
  int cursorY = y0;
  tft.setCursor(x, cursorY);
  const char *p = body;
  while (*p) {
    if (*p == '|' || *p == '\n') {
      cursorY += lineSpacing;
      if (cursorY > maxY)
        break;
      tft.setCursor(x, cursorY);
    } else {
      tft.print(*p);
    }
    p++;
  }
}

void showStatus(const char *title, const char *body, uint16_t titleColor) {
  if (!tftReady)
    return;

  if (titleColor == 0)
    titleColor = COL_ACCENT; // unified gold branding for all titles

  tft.fillScreen(COL_BG);

  // Header bar with gold underline accent
  tft.fillRect(0, 0, TFT_WIDTH, HEADER_H, COL_HEADER);
  tft.drawFastHLine(0, HEADER_H, TFT_WIDTH, COL_ACCENT);
  tft.setTextSize(1);
  tft.setTextColor(COL_ACCENT);
  tft.setCursor(44, 6);
  tft.print("  NEXA BOT");
  tft.setCursor(45, 6); // 1px overdraw = cheap faux-bold
  tft.print("  NEXA BOT");

  // Reset animation so the icon redraws immediately at frame 0 for this screen
  animFrame = 0;
  lastAnimMs = millis();

  // Short status text ("Ready" / "Hello!" / "Listening..." / "Thinking...")
  // gets a big animated icon beside a narrower text column. Long text
  // (wrapped answers, ~13-24 chars/segment from the Python side) uses the
  // full width instead, since it needs the room.
  bool compact = longestSegment(body) <= 12;

  if (compact) {
    bigIconActive = true;
    drawIconForState(currentState, BIG_ICON_CX, BIG_ICON_CY, BIG_ICON_SIZE, 0,
                     COL_BG);

    tft.setTextSize(1);
    tft.setTextColor(titleColor);
    tft.setCursor(TEXT_X, 26);
    tft.print(title);
    tft.setCursor(TEXT_X + 1, 26);
    tft.print(title);

    tft.drawFastHLine(TEXT_X, 37, TFT_WIDTH - TEXT_X - 4, COL_ACCENT_DIM);
    drawWrappedBody(TEXT_X, 42, 13, 100, body, COL_TEXT);
  } else {
    bigIconActive = false;

    tft.setTextSize(1);
    tft.setTextColor(titleColor);
    tft.setCursor(5, 24);
    tft.print(title);

    tft.drawFastHLine(5, 35, TFT_WIDTH - 10, COL_ACCENT_DIM);
    drawWrappedBody(5, 39, 10, 118, body, COL_TEXT);
  }

  // Small always-on status badge, top-right of the header
  drawIconForState(currentState, BADGE_CX, BADGE_CY, BADGE_SIZE, 0, COL_HEADER);
}

void showIdleDistance(long dist) {
  if (!tftReady)
    return;
  // Clear only the bottom sensor status area (y=107 to 127) to avoid screen
  // flicker and preserve the body text/icon above.
  tft.fillRect(0, 107, 160, 21, COL_BG);
  tft.setCursor(5, 114);
  tft.setTextSize(1);

  if (dist >= 9999) {
    tft.setTextColor(ST7735_RED);
    tft.print("Sensor: No echo/out");
  } else if (dist <= TRIGGER_DISTANCE_CM) {
    tft.setTextColor(COL_ACCENT);
    tft.print("Sensor: NEAR (");
    tft.print(dist);
    tft.print("cm)");
  } else {
    tft.setTextColor(COL_ACCENT_DIM);
    tft.print("Sensor: ");
    tft.print(dist);
    tft.print(" cm");
  }
}

// ============================================================================
// STATUS ICONS - small hand-drawn, animated glyphs (no bitmap assets needed)
// that visually represent currentState: idle pulse, waving hello, mic while
// listening, bouncing dots while thinking, speaker while speaking. Each one
// clears its own square area first, so calling it repeatedly with a new
// `frame` animates it in place.
// ============================================================================
void iconClear(int cx, int cy, int boxSize, uint16_t bg) {
  tft.fillRect(cx - boxSize / 2, cy - boxSize / 2, boxSize, boxSize, bg);
}

// Idle "Ready" - a breathing radar-style pulse ring (invites people closer).
void iconReady(int cx, int cy, int size, int frame, uint16_t bg) {
  iconClear(cx, cy, size + 8, bg);
  int maxR = size / 2 - 2;
  int step = maxR / 4;
  if (step < 1)
    step = 1;
  int r = 5 + (frame % 4) * step;
  if (r > maxR)
    r = maxR;
  tft.drawCircle(cx, cy, r, COL_ACCENT);
  if (r > 3)
    tft.drawCircle(cx, cy, r - 1, COL_ACCENT_DIM);
  tft.fillCircle(cx, cy, 3, COL_ACCENT);
}

// Greeting - a small waving hand (palm + wiggling fingers + wrist).
void iconGreet(int cx, int cy, int size, int frame, uint16_t bg) {
  iconClear(cx, cy, size + 8, bg);
  int r = size / 5;
  if (r < 4)
    r = 4;
  int palmY = cy + r / 2;
  tft.fillCircle(cx, palmY, r, COL_ACCENT);
  int wiggle = (frame % 2 == 0) ? -1 : 2;
  for (int i = -1; i <= 1; i++) {
    int fx = cx + i * r;
    int topY = palmY - r - 6;
    tft.drawLine(fx, palmY - r, fx + wiggle, topY, COL_ACCENT);
    tft.drawLine(fx + 1, palmY - r, fx + 1 + wiggle, topY, COL_ACCENT);
  }
  tft.fillRect(cx - 3, palmY + r - 2, 6, 7, COL_ACCENT_DIM);
}

// Listening - a mic (capsule + cage + stand) with a pulsing cage ring and a
// blinking "recording" dot.
void iconMic(int cx, int cy, int size, int frame, uint16_t bg) {
  iconClear(cx, cy, size + 8, bg);
  int capW = size / 3;
  if (capW < 6)
    capW = 6;
  int capH = size / 2;
  int capX = cx - capW / 2;
  int capY = cy - size / 2 + 2;
  tft.fillRoundRect(capX, capY, capW, capH, capW / 2, COL_ACCENT);
  uint16_t cageColor = (frame % 2 == 0) ? COL_ACCENT_DIM : bg;
  tft.drawRoundRect(capX - 3, capY - 3, capW + 6, capH + 6, (capW + 6) / 2,
                    cageColor);
  int standTopY = capY + capH;
  int standH = size / 6;
  if (standH < 4)
    standH = 4;
  tft.drawFastVLine(cx, standTopY, standH, COL_ACCENT);
  int baseW = size / 3;
  tft.drawFastHLine(cx - baseW / 2, standTopY + standH, baseW, COL_ACCENT);
  if (frame % 2 == 0) {
    tft.fillCircle(capX + capW + 3, capY + 2, 2, COL_ACCENT);
  }
}

// Thinking - three dots, one "bouncing" higher at a time.
void iconThink(int cx, int cy, int size, int frame, uint16_t bg) {
  iconClear(cx, cy, size + 8, bg);
  int spacing = size / 4;
  if (spacing < 8)
    spacing = 8;
  int idx = frame % 3;
  for (int i = 0; i < 3; i++) {
    int dx = cx + (i - 1) * spacing;
    int dy = cy;
    int rad = 3;
    if (i == idx) {
      dy -= 4;
      rad = 4;
    }
    tft.fillCircle(dx, dy, rad, COL_ACCENT);
  }
}

// Speaking - a speaker cone with sound waves that pulse outward 1-3-then-off.
void iconSpeak(int cx, int cy, int size, int frame, uint16_t bg) {
  iconClear(cx, cy, size + 8, bg);
  int boxW = size / 5;
  if (boxW < 4)
    boxW = 4;
  int boxH = size / 3;
  int bx = cx - size / 3;
  int by = cy - boxH / 2;
  tft.fillRect(bx, by, boxW, boxH, COL_ACCENT);
  tft.fillTriangle(bx + boxW, by, bx + boxW, by + boxH, bx + boxW + size / 4,
                   cy, COL_ACCENT);
  int waves = frame % 4;
  int baseX = bx + boxW + size / 4 + 3;
  for (int i = 0; i < waves; i++) {
    int d = 4 + i * 5;
    tft.drawLine(baseX + d, cy - d, baseX + d + 3, cy, COL_ACCENT_DIM);
    tft.drawLine(baseX + d, cy + d, baseX + d + 3, cy, COL_ACCENT_DIM);
  }
}

// Picks the icon artwork for the current interaction state.
void drawIconForState(SystemState state, int cx, int cy, int size, int frame,
                      uint16_t bg) {
  switch (state) {
  case STATE_GREETING:
    iconGreet(cx, cy, size, frame, bg);
    break;
  case STATE_LISTENING:
    iconMic(cx, cy, size, frame, bg);
    break;
  case STATE_WAITING_RESPONSE:
    iconThink(cx, cy, size, frame, bg);
    break;
  case STATE_SPEAKING:
    iconSpeak(cx, cy, size, frame, bg);
    break;
  case STATE_IDLE:
  default:
    iconReady(cx, cy, size, frame, bg);
    break;
  }
}

// Steps the animated status icon(s) by one frame, throttled to
// ANIM_INTERVAL_MS. Cheap to call often (from loop() and every serial-wait
// poll) - it no-ops between frames. Always redraws the small header badge;
// also redraws the large body icon when showStatus() last used compact mode.
void updateAnimation() {
  if (!tftReady)
    return;
  unsigned long now = millis();
  if (now - lastAnimMs < ANIM_INTERVAL_MS)
    return;
  lastAnimMs = now;
  animFrame = (animFrame + 1) % 4;

  drawIconForState(currentState, BADGE_CX, BADGE_CY, BADGE_SIZE, animFrame,
                   COL_HEADER);
  if (bigIconActive) {
    drawIconForState(currentState, BIG_ICON_CX, BIG_ICON_CY, BIG_ICON_SIZE,
                     animFrame, COL_BG);
  }
}

// ============================================================================
// MICROPHONE - laptop mic (no local I2S mic hardware)
// ============================================================================
// Audio capture has moved entirely to the laptop. When the backend sends
// COMMAND:LISTEN, the ESP32 signals readiness with STATUS:LISTEN_READY, then
// blocks waiting for STATUS:RECORDING_DONE from the laptop (sent once the
// laptop has finished recording from its own microphone and is about to run
// STT). No audio data is transmitted from the ESP32 to the laptop at all.
void streamMicToBackend() {
  // Signal the laptop to start capturing from its own microphone.
  sendStatus("LISTEN_READY");

  // Wait until the laptop confirms it has finished recording.
  // The backend sends back "STATUS:RECORDING_DONE" as a plain line
  // (not a COMMAND: prefix) to distinguish it from the normal command flow.
  unsigned long start = millis();
  while (millis() - start < SERIAL_CMD_TIMEOUT_MS) {
    String line = readLineBlocking(200);
    updateAnimation();          // pulse the mic icon while the laptop records
    updateServoFromProximity(); // keep servo tracking sensor during mic wait
    // Person walked away mid-recording — abort immediately
    if (personLeftDuringSession) {
      // Backend is still recording from the laptop mic; unblock it first by
      // pretending recording is done, then the PERSON_LEFT status (already
      // sent by updateServoFromProximity) causes the backend to abort.
      sendStatus("RECORDING_DONE");
      return;
    }
    if (line == "STATUS:RECORDING_DONE")
      return;
    // Ignore stray lines (STATUS echoes, etc.) and keep waiting.
  }
  sendError("Timed out waiting for STATUS:RECORDING_DONE from laptop mic");
}

// ============================================================================
// SERIAL LINE UTILITIES
// ============================================================================

// Blocks until a full line (up to '\n') is available, or timeout. Trimmed,
// so a trailing '\r' from a '\r\n'-terminated sender (e.g. Python's
// pyserial writing "...\r\n", or Serial.println()'s own "\r\n") never
// leaks into line-equality checks.
String readLineBlocking(unsigned long timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    if (Serial.available()) {
      String line = Serial.readStringUntil('\n');
      line.trim();
      return line;
    }
    delay(2);
  }
  return "";
}
