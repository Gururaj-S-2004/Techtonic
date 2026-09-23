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
#define PIN_BUTTON   4     // push button, other leg to GND, uses internal pull-up
#define PIN_TRIG     5     // HC-SR04 TRIG
#define PIN_ECHO     6     // HC-SR04 ECHO (use a resistor divider: sensor is 5V logic)

// Actuators / indicators
#define PIN_SERVO    7     // servo signal wire

// 1.8" TFT SPI 128x160 (ST7735)
#define PIN_TFT_RST   8    // RES / RESET
#define PIN_TFT_DC    9    // DC / A0 / RS
#define PIN_TFT_CS   10    // CS
#define PIN_TFT_MOSI 11    // SDA / MOSI / DIN
#define PIN_TFT_SCLK 12    // SCL / SCK / CLK
// Note: Connect TFT BLK/LED to 3.3V, VCC to 3.3V (or 5V), GND to GND

// Audio: all audio is now played by the laptop's own speaker.
// The MAX98357A I2S amplifier has been removed. No speaker pins are needed.

// Microphone: audio is captured by the laptop's own mic.
// The INMP441 I2S mic has been removed. No mic pins are needed on the ESP32.

// ============================================================================
// 1.8" TFT SPI CONFIG (ST7735 128x160)
// ============================================================================
#define TFT_WIDTH    160
#define TFT_HEIGHT   128
// Pass the SPI class explicitly to ensure it uses the custom pins on ESP32-S3
Adafruit_ST7735 tft = Adafruit_ST7735(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
bool tftReady = false;

// ============================================================================
// SERVO CONFIG
// ============================================================================
Servo handServo;
#define SERVO_REST_ANGLE 0
#define SERVO_WAVE_ANGLE 90

// ============================================================================
// ULTRASONIC CONFIG
// ============================================================================
#define TRIGGER_DISTANCE_CM   60UL     // start interaction if someone is closer than this
#define ULTRASONIC_TIMEOUT_US 30000UL  // ~5 m max range

// ============================================================================
// SERIAL TIMEOUT
// ============================================================================
#define SERIAL_CMD_TIMEOUT_MS  20000       // how long to wait for a laptop command

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
const unsigned long TRIGGER_COOLDOWN_MS = 3000; // ignore new triggers right after one finishes

// Ultrasonic measurement pacing (HC-SR04 requires >= 60ms between pings to prevent echo flooding)
unsigned long lastPingTime = 0;
const unsigned long PING_INTERVAL_MS = 100;
long lastReportedDistance = -999;

// Forward declaration for display helper
void showStatus(const char *title, const char *body, uint16_t titleColor = 0);
void showIdleDistance(long dist);

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  // 32 KB RX buffer: at 921600 baud (~92 KB/s) this gives ~350ms of headroom.
  Serial.setRxBufferSize(32768);
  Serial.begin(SERIAL_BAUD);
  unsigned long bootStart = millis();
  while (!Serial && millis() - bootStart < 3000) { delay(10); }
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
  if (currentState == STATE_IDLE) {
    // Drain and ignore any stray laptop command while idle (keeps protocol
    // in sync if the backend restarts mid-session and resends COMMAND:IDLE).
    if (Serial.available()) {
      String stray = Serial.readStringUntil('\n');
      stray.trim();
      // nothing to act on here; idle already implies COMMAND:IDLE state
    }

    if (millis() - lastInteractionTime < TRIGGER_COOLDOWN_MS) return;

    bool buttonTrigger = digitalRead(PIN_BUTTON) == LOW;      // active-low button

    // Ping ultrasonic sensor with a clean 100ms interval (prevents transducer flooding)
    bool proximityTrigger = false;
    if (millis() - lastPingTime >= PING_INTERVAL_MS) {
      lastPingTime = millis();
      long dist = readDistanceCM();

      // Only refresh screen if distance changes by >= 2cm to keep display smooth
      if (abs(dist - lastReportedDistance) >= 2 || (dist <= TRIGGER_DISTANCE_CM) != (lastReportedDistance <= TRIGGER_DISTANCE_CM)) {
        lastReportedDistance = dist;
        showIdleDistance(dist);
      }

      if (dist > 0 && dist <= TRIGGER_DISTANCE_CM) {
        proximityTrigger = true;
      }
    }

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
  if (cmd != "GREET") {
    sendError("Expected GREET, got: " + cmd);
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
    unsigned long cmdTimeout = (currentState == STATE_SPEAKING)
        ? SERIAL_CMD_TIMEOUT_MS * 4   // up to 80s for very long TTS
        : SERIAL_CMD_TIMEOUT_MS;

    cmd = waitForCommand(cmdTimeout);

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

    cmd = waitForCommand(SERIAL_CMD_TIMEOUT_MS);
    if (cmd == "PROCESSING") {
      cmd = waitForCommand(SERIAL_CMD_TIMEOUT_MS);
    }
    if (cmd.startsWith("DISPLAY_Q:")) {
      cmd = waitForCommand(SERIAL_CMD_TIMEOUT_MS);
    }
    if (cmd.startsWith("DISPLAY_A:")) {
      lastAnswer = cmd.substring(10);
      showStatus("Answer:", lastAnswer.c_str(), ST7735_CYAN);
      cmd = waitForCommand(SERIAL_CMD_TIMEOUT_MS);
    }

    if (cmd == "SPEAK") {
      // Laptop plays TTS on its own speaker. Answer stays on TFT.
      // We just update state and fall back to the top of the loop,
      // where we wait (with extended timeout) for LISTEN or IDLE.
      currentState = STATE_SPEAKING;
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
  handServo.write(SERVO_REST_ANGLE);
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
    if (elapsed >= timeoutMs) return "";
    // Poll in 100ms slices so we check the overall deadline frequently.
    unsigned long slice = timeoutMs - elapsed;
    if (slice > 100) slice = 100;
    String line = readLineBlocking(slice);
    if (line.length() == 0) continue;
    if (line.startsWith("COMMAND:")) {
      return line.substring(8);
    }
    // Non-COMMAND lines (STATUS echoes, stray bytes) are discarded; keep waiting.
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
  if (duration == 0) return 9999; // no echo received = nothing in range

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
// ============================================================================
void setupTFT() {
  // Pass MISO=-1 (not used), MOSI and SCLK as defined, CS managed by Adafruit.
  // 27 MHz is safe for ST7735 on short PCB traces; reduces per-frame latency.
  SPI.begin(PIN_TFT_SCLK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
  SPI.setFrequency(27000000);
  // Initialization for 1.8" TFT SPI 128x160 (ST7735)
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1); // landscape mode: 160 width x 128 height
  tft.fillScreen(ST7735_BLACK);
  tftReady = true;
}

void showStatus(const char *title, const char *body, uint16_t titleColor) {
  if (!tftReady) return;

  if (titleColor == 0) {
    if (strcmp(title, "Ready") == 0) titleColor = ST7735_GREEN;
    else if (strcmp(title, "Hello!") == 0) titleColor = ST7735_CYAN;
    else if (strcmp(title, "Listening...") == 0) titleColor = ST7735_GREEN;
    else if (strcmp(title, "Thinking...") == 0) titleColor = ST7735_YELLOW;
    else if (strcmp(title, "Answering...") == 0) titleColor = ST7735_CYAN;
    else titleColor = ST7735_WHITE;
  }

  tft.fillScreen(ST7735_BLACK);

  // Top header banner
  tft.fillRect(0, 0, 160, 18, ST7735_BLUE);
  tft.setTextSize(1);
  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(44, 5);
  tft.print("EVENT ROBOT");

  // Title in thematic color
  tft.setTextSize(1);
  tft.setTextColor(titleColor);
  tft.setCursor(5, 22);
  tft.println(title);

  // Divider line
  tft.drawFastHLine(5, 33, 150, 0x4208); // subtle gray divider line

  // Body message
  //
  // Choose text size based on the LONGEST pipe-separated segment, not the
  // total body length.  At textSize=2 each character is 12px wide; with a
  // 5px left margin on a 160px-wide display only 12 characters fit before
  // GFX auto-wraps the line internally.
  // The Python side word-wraps at 24 chars, so multi-sentence answers
  // are 13-24 chars and use textSize=1 (6px/char, ~25 chars fit).
  // Status screens ("Ready", "Hello!", "Listening...", "Thinking...")
  // have segments <= 12 chars and use textSize=2 (big, clear font).
  int maxSegLen = 0, segLen = 0;
  for (const char *pp = body; *pp; pp++) {
    if (*pp == '|' || *pp == '\n') {
      if (segLen > maxSegLen) maxSegLen = segLen;
      segLen = 0;
    } else {
      segLen++;
    }
  }
  if (segLen > maxSegLen) maxSegLen = segLen;

  int textSize    = (maxSegLen > 12) ? 1 : 2;
  int lineSpacing = (textSize == 1) ? 10 : 16;

  tft.setTextSize(textSize);
  tft.setTextColor(ST7735_WHITE);

  // Body starts at y=34.
  // At textSize=2 with lineSpacing=16, 5 lines occupy y=34, 50, 66, 82, 98.
  // The 5th line ends at y=111 <= 127 (fits completely within TFT height).
  // For Ready screen: 4 lines end at y=95, well above the sensor area (y=107..127).
  int cursorY = 34;
  tft.setCursor(5, cursorY);

  int maxCursorY = (textSize == 2) ? 102 : 118;

  const char *p = body;
  while (*p) {
    if (*p == '|' || *p == '\n') {
      cursorY += lineSpacing;
      if (cursorY > maxCursorY) break; // clamp: don't overflow display or sensor zone
      tft.setCursor(5, cursorY);
    } else {
      tft.print(*p);
    }
    p++;
  }
}

void showIdleDistance(long dist) {
  if (!tftReady) return;
  // Clear only the bottom sensor status area (y=107 to 127) to avoid screen flicker
  // and preserve all 4 lines of textSize=2 body text above (ends at y=102).
  tft.fillRect(0, 107, 160, 21, ST7735_BLACK);
  tft.setCursor(5, 114);
  tft.setTextSize(1);

  if (dist >= 9999) {
    tft.setTextColor(ST7735_RED);
    tft.print("Sensor: No echo/out");
  } else if (dist <= TRIGGER_DISTANCE_CM) {
    tft.setTextColor(ST7735_GREEN);
    tft.print("Sensor: NEAR (");
    tft.print(dist);
    tft.print("cm)");
  } else {
    tft.setTextColor(ST7735_CYAN);
    tft.print("Sensor: ");
    tft.print(dist);
    tft.print(" cm");
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
    if (line == "STATUS:RECORDING_DONE") return;
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
