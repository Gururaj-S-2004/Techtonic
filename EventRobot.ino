#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <ESP32Servo.h>
#include <driver/i2s.h>
#include <math.h>
#include <WiFi.h>

// ============================================================================
// WIRE PROTOCOL (USB Serial, 921600 baud, matches robot_backend/serial_link.py)
// ----------------------------------------------------------------------------
// ESP32 -> laptop (ASCII lines, '\n'-terminated unless noted):
//   STATUS:<text>              informational, laptop just logs it
//     STATUS:LISTEN_READY      signals the laptop to start recording from its
//                               own microphone (no audio captured on ESP32)
//     STATUS:GREETING_DONE     greeting animation + tone complete
//     STATUS:IDLE              interaction finished, back to idle
//   TRIGGER:BUTTON              button pressed while idle
//   TRIGGER:PROXIMITY           someone detected in range while idle
//   ERROR:<text>                something went wrong on the ESP32 side
//   AUDIO_START:<len>:<crc32>   begins a raw PCM16LE mono 16kHz payload of
//                                exactly <len> bytes, immediately followed by
//                                those <len> raw bytes (NOT text, no
//                                delimiters inside), then a bare line:
//   AUDIO_END                   payload complete; CRC32 (IEEE 802.3 poly,
//                                same as Python's zlib.crc32) covers exactly
//                                those <len> bytes
//
// laptop -> ESP32 (ASCII lines):
//   COMMAND:GREET                play wave + greeting tone (local, no audio)
//   COMMAND:LISTEN               ESP32 signals LISTEN_READY; laptop records
//                                 from its own mic, sends STATUS:RECORDING_DONE
//                                 when finished - no audio is streamed from
//                                 ESP32 in this direction
//   COMMAND:PROCESSING           purely a status hint ("Thinking...")
//   COMMAND:SPEAK                followed immediately by one
//                                 AUDIO_START:<len>:<crc32> / bytes / AUDIO_END
//                                 frame that the ESP32 should play out loud
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

// Microphone: audio is now captured by the laptop's own mic.
// The INMP441 I2S mic has been removed. No mic pins are needed on the ESP32.

// MAX98357A amplifier (I2S output, uses I2S peripheral #1)
#define PIN_SPK_BCLK 1     // BCLK
#define PIN_SPK_LRC  2     // LRC / WS
#define PIN_SPK_DOUT 38    // DIN on the MAX98357A. Tie its SD pin high (always on) or to a spare GPIO.

// ============================================================================
// 1.8" TFT SPI CONFIG (ST7735 128x160)
// ============================================================================
#define TFT_WIDTH    160
#define TFT_HEIGHT   128
// Pass the SPI class explicitly to ensure it uses the custom pins on ESP32-S3
Adafruit_ST7735 tft = Adafruit_ST7735(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
bool tftReady = false;
bool tftBusy = false;  // true while audio is playing - blocks TFT SPI to prevent bus contention

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
// AUDIO CONFIG
// ============================================================================
#define SAMPLE_RATE          16000
#define I2S_SPK_PORT         I2S_NUM_0    // only one I2S port needed now (speaker)
#define CHUNK_SAMPLES         512          // mono samples per I2S write chunk (32ms at 16kHz)
// DMA depth: 4 buffers × 512 stereo samples = 128ms of pipeline depth.
// Keeps drain time short (128ms + 50ms margin = ~180ms) so the TFT SPI
// transaction in returnToIdle() never overlaps with active I2S DMA.
#define I2S_DMA_BUF_COUNT    4
#define I2S_DMA_BUF_LEN      CHUNK_SAMPLES  // stereo samples per DMA buffer
#define SERIAL_CMD_TIMEOUT_MS  20000       // how long to wait for a laptop command/audio

// CRC32 (IEEE 802.3, same polynomial/algorithm as Python's zlib.crc32).
// Incremental API so multi-chunk streams can be verified without buffering
// the whole payload: crc32Init() -> crc32Update() per chunk -> crc32Final().
static const uint32_t CRC32_POLY = 0xEDB88320UL;
uint32_t crc32Init() { return 0xFFFFFFFFUL; }
uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc >> 1) ^ (CRC32_POLY & (~(crc & 1) + 1));
    }
  }
  return crc;
}
uint32_t crc32Final(uint32_t crc) { return crc ^ 0xFFFFFFFFUL; }

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
  // The laptop sends audio in paced 1KB chunks but Windows timer granularity
  // can cause bursts; a large buffer absorbs them without dropping bytes.
  Serial.setRxBufferSize(32768);
  Serial.begin(SERIAL_BAUD);
  unsigned long bootStart = millis();
  while (!Serial && millis() - bootStart < 3000) { delay(10); }
  delay(300);

  setupTFT();
  setupServo();
  setupTriggers();
  setupSpeakerI2S();

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
  // Turn off Wi-Fi during interaction to prevent audio interference/glitches
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
  playGreetingTone();
  drainAudioPipeline(); // flush DMA before any subsequent TFT SPI access
  sendStatus("GREETING_DONE");

  // --- 2. Wait for COMMAND:LISTEN, then capture + stream mic audio ---
  cmd = waitForCommand(SERIAL_CMD_TIMEOUT_MS);
  if (cmd != "LISTEN") {
    sendError("Expected LISTEN, got: " + cmd);
    returnToIdle();
    return;
  }
  currentState = STATE_LISTENING;
  showStatus("Listening...", "Speak your\nquestion\nnow");
  streamMicToBackend();

  // --- 3. Optional COMMAND:PROCESSING status hint, then wait for reply ---
  currentState = STATE_WAITING_RESPONSE;
  showStatus("Thinking...", "Please wait");

  // Save the displayed text so we can re-show it on the TFT after audio
  // finishes. Without this, returnToIdle() immediately overwrites the
  // answer with "Ready" the moment playback ends, making the TFT appear
  // de-synced from the Python terminal which still shows the answer.
  String lastAnswer = "";

  cmd = waitForCommand(SERIAL_CMD_TIMEOUT_MS);
  if (cmd == "PROCESSING") {
    cmd = waitForCommand(SERIAL_CMD_TIMEOUT_MS);
  }

  if (cmd.startsWith("DISPLAY_Q:")) {
    // Question is displayed in the laptop terminal only, not on the TFT
    cmd = waitForCommand(SERIAL_CMD_TIMEOUT_MS);
  }

  if (cmd.startsWith("DISPLAY_A:")) {
    lastAnswer = cmd.substring(10);
    showStatus("Answer:", lastAnswer.c_str(), ST7735_CYAN);
    cmd = waitForCommand(SERIAL_CMD_TIMEOUT_MS);
  }

  if (cmd == "SPEAK") {
    currentState = STATE_SPEAKING;
    // tftBusy blocks showStatus()/showIdleDistance() so SPI never runs
    // while I2S DMA is active (both use GDMA on ESP32-S3; simultaneous
    // transfers corrupt each other, causing the crackle+glitch symptom).
    tftBusy = true;
    if (!receiveAndPlayAudio()) {
      sendError("Failed to receive/play TTS audio");
    }
    drainAudioPipeline(); // wait for DMA to empty *before* clearing the flag
    tftBusy = false;      // only now is it safe to write to TFT

    // Re-show the answer after audio finishes so the TFT stays in sync
    // with the terminal. Without this the display jumps straight to
    // "Ready" the moment the last DMA buffer drains, while the Python
    // backend log still shows the answer text.
    if (lastAnswer.length() > 0) {
      showStatus("Answer:", lastAnswer.c_str(), ST7735_CYAN);
      delay(4000); // keep the answer visible for 4 seconds
    }
  } else if (cmd == "IDLE") {
    // backend gave up / had nothing to say - but still show the answer
    // if we got one (e.g. TTS failed), so the TFT is never blank.
    if (lastAnswer.length() > 0) {
      showStatus("Answer:", lastAnswer.c_str(), ST7735_CYAN);
      delay(4000);
    }
  } else {
    sendError("Expected SPEAK or IDLE, got: " + cmd);
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
  if (!tftReady || tftBusy) return;  // skip during audio - I2S DMA and SPI must not overlap

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
  if (!tftReady || tftBusy) return;  // skip during audio - I2S DMA and SPI must not overlap
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
  // readLineBlocking() already polls Serial.available() internally with 2ms
  // granularity — no outer Serial.available() wrapper is needed, and the
  // old nested timeout clocks were not coordinated with each other.
  unsigned long start = millis();
  while (millis() - start < SERIAL_CMD_TIMEOUT_MS) {
    String line = readLineBlocking(200);
    if (line == "STATUS:RECORDING_DONE") return;
    // Ignore stray lines (STATUS echoes, etc.) and keep waiting.
  }
  sendError("Timed out waiting for STATUS:RECORDING_DONE from laptop mic");
}

// ============================================================================
// SPEAKER (MAX98357A) - I2S output
// Note: this is now the ONLY I2S peripheral in use (I2S_SPK_PORT = I2S_NUM_0).
//       The mic I2S peripheral (#1) has been removed along with the INMP441.
// ============================================================================
void setupSpeakerI2S() {
  i2s_config_t spkConfig = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT, // MAX98357A wants a stereo frame
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    // I2S_DMA_BUF_LEN is in stereo samples (CHUNK_SAMPLES*2).
    // 8 buffers at that size gives ~128ms of audio pipeline depth,
    // eliminating DMA underruns that cause crackling.
    .dma_buf_count = I2S_DMA_BUF_COUNT,
    .dma_buf_len = I2S_DMA_BUF_LEN,
    .use_apll = true,   // use APLL for a cleaner clock - reduces jitter/noise
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  i2s_pin_config_t spkPins = {
    .bck_io_num = PIN_SPK_BCLK,
    .ws_io_num = PIN_SPK_LRC,
    .data_out_num = PIN_SPK_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_SPK_PORT, &spkConfig, 0, NULL);
  i2s_set_pin(I2S_SPK_PORT, &spkPins);
  i2s_zero_dma_buffer(I2S_SPK_PORT);
}

// Software volume scale: 0.0 (silent) to 1.0 (full).
// Reducing below 1.0 cuts peak current draw from the MAX98357A, which
// reduces power rail droops that cause TFT SPI corruption (glitching).
// Increase back to 1.0 once decoupling capacitors are installed.
#define AUDIO_VOLUME_SCALE  0.70f

// Writes one chunk of mono 16-bit PCM samples out to the speaker,
// duplicating each sample into a stereo (L=R) frame.
void playMonoPCM(const int16_t *mono, size_t sampleCount) {
  static int16_t stereo[CHUNK_SAMPLES * 2];
  size_t offset = 0;
  while (offset < sampleCount) {
    size_t n = sampleCount - offset;
    if (n > CHUNK_SAMPLES) n = CHUNK_SAMPLES;
    for (size_t i = 0; i < n; i++) {
      int32_t s = (int32_t)(mono[offset + i] * AUDIO_VOLUME_SCALE);
      // Clamp to int16 range to prevent wrap-around distortion
      if (s >  32767) s =  32767;
      if (s < -32768) s = -32768;
      stereo[2 * i]     = (int16_t)s;
      stereo[2 * i + 1] = (int16_t)s;
    }
    size_t bytesWritten = 0;
    i2s_write(I2S_SPK_PORT, stereo, n * 2 * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
    offset += n;
  }
}

// Writes one buffer of silence then delays long enough for all DMA buffers
// to drain at the hardware level.  Call after every audio sequence (TTS reply
// and greeting tone) so the I2S GDMA is fully idle before returnToIdle()
// writes to the TFT - otherwise the SPI GDMA and I2S GDMA clash, producing
// the simultaneous crackle + display glitch observed during testing.
void drainAudioPipeline() {
  // One extra chunk of silence pushes any partial last DMA buffer through.
  static int16_t silence[CHUNK_SAMPLES] = {0};
  playMonoPCM(silence, CHUNK_SAMPLES);
  // Wait for all remaining DMA buffers to finish playing.
  // drain_ms = (buf_count * buf_len samples) / sample_rate
  const uint32_t drainMs =
      ((uint32_t)I2S_DMA_BUF_COUNT * I2S_DMA_BUF_LEN * 1000UL) / SAMPLE_RATE;
  delay(drainMs + 50); // +50 ms safety margin
}

// A short two-tone chirp played locally (no laptop round trip) right after a
// trigger, so the visitor gets an instant audible cue to start speaking.
void playGreetingTone() {
  static int16_t tone[CHUNK_SAMPLES];
  const float freqs[2] = {880.0f, 1320.0f};

  for (int t = 0; t < 2; t++) {
    uint32_t phaseCounter = 0;
    for (int rep = 0; rep < (SAMPLE_RATE / 4) / CHUNK_SAMPLES; rep++) {
      for (int i = 0; i < CHUNK_SAMPLES; i++) {
        float angle = 2.0f * PI * freqs[t] * ((float)phaseCounter / SAMPLE_RATE);
        tone[i] = (int16_t)(3000.0f * sinf(angle));
        phaseCounter++;
      }
      playMonoPCM(tone, CHUNK_SAMPLES);
    }
  }
}

// Reads one AUDIO_START:<len>:<crc32> / bytes / AUDIO_END frame from the
// laptop and plays it as it arrives (chunked, no giant single malloc).
bool receiveAndPlayAudio() {
  String header = readLineBlocking(SERIAL_CMD_TIMEOUT_MS);
  if (!header.startsWith("AUDIO_START:")) {
    sendError("Expected AUDIO_START, got: " + header);
    return false;
  }

  int firstColon = header.indexOf(':', 12);
  if (firstColon < 0) {
    sendError("Malformed AUDIO_START header");
    return false;
  }
  uint32_t length = (uint32_t)header.substring(12, firstColon).toInt();
  uint32_t expectedCrc = (uint32_t)strtoul(header.substring(firstColon + 1).c_str(), nullptr, 10);

  static uint8_t buf[CHUNK_SAMPLES * sizeof(int16_t)];
  uint32_t remaining = length;
  uint32_t runningCrc = crc32Init();

  while (remaining > 0) {
    uint32_t toRead = remaining < sizeof(buf) ? remaining : sizeof(buf);
    // Keep reads on 16-bit sample boundaries so playMonoPCM never sees a
    // half-sample; audio length is always even in this protocol anyway.
    toRead -= (toRead % sizeof(int16_t));
    if (toRead == 0) toRead = remaining; // final odd byte (shouldn't happen)

    if (!readExactBlocking(buf, toRead, SERIAL_CMD_TIMEOUT_MS)) {
      sendError("Timed out reading audio payload");
      return false;
    }
    runningCrc = crc32Update(runningCrc, buf, toRead);
    // Played as it arrives (can't buffer an unbounded reply in ~320KB of
    // SRAM); a CRC mismatch is only detectable *after* playback, so on
    // mismatch we just report it - useful for catching a desynced stream
    // during bring-up, but it can't undo audio already sent to the amp.
    playMonoPCM((const int16_t *)buf, toRead / sizeof(int16_t));
    remaining -= toRead;
  }

  // One blank-line separator (see streamMicToBackend()'s matching note),
  // then the actual "AUDIO_END" line.
  readLineBlocking(2000);
  String endLine = readLineBlocking(2000);
  if (endLine != "AUDIO_END") {
    sendError("Expected AUDIO_END, got: " + endLine);
    return false;
  }

  uint32_t actualCrc = crc32Final(runningCrc);
  if (actualCrc != expectedCrc) {
    sendError("TTS audio CRC mismatch - stream may be corrupted/desynced");
    return false;
  }
  return true;
}

// Blocks until a full line (up to '\n') is available, or timeout. Trimmed,
// so a trailing '\r' from a '\r\n'-terminated sender (e.g. Python's
// pyserial writing "...\r\n", or Serial.println()'s own "\r\n") never
// leaks into line-equality checks like `endLine != "AUDIO_END"`.
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

// Blocks until exactly `length` bytes are read into buffer, or timeout.
bool readExactBlocking(uint8_t *buffer, size_t length, unsigned long timeoutMs) {
  size_t received = 0;
  unsigned long start = millis();
  while (received < length) {
    if (millis() - start > timeoutMs) return false;
    int avail = Serial.available();
    if (avail <= 0) {
      delay(1);
      continue;
    }
    size_t toRead = (size_t)avail < (length - received) ? (size_t)avail : (length - received);
    int n = Serial.readBytes(buffer + received, toRead);
    if (n <= 0) continue;
    received += n;
  }
  return true;
}
