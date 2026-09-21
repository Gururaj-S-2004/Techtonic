# EventRobot — AI Event-Kiosk Robot

A physical kiosk robot for events/exhibitions that greets visitors, listens
to their spoken questions, and answers out loud using a mix of **offline
speech recognition, a local knowledge base, a cloud LLM for phrasing, and
offline text-to-speech** — all running on an **ESP32-S3** (the "body":
sensors, mic, speaker, servo, display) paired with a **laptop** (the
"brain": all the heavy processing) connected over a single USB cable.

## Why this exists

At an event/exhibition booth, staff repeatedly answer the same handful of
questions ("Where's registration?", "What time does it start?", "Where's
the main hall?"). EventRobot stands at the booth, waves and greets people
who walk up or press a button, listens to their question, and answers it
using facts you provide about *your specific event* — freeing up staff and
giving the booth a novelty/attraction factor.

It is deliberately built to be **mostly free to run**: transcription (STT)
and speech synthesis (TTS) both run fully offline/locally on the laptop.
The **only paid/cloud call** is a single Groq LLM request per question,
used purely to phrase a natural-sounding answer from the facts you supply —
it is explicitly instructed never to invent facts about your event.

## What has been built so far

- **`EventRobot.ino`** — firmware for the ESP32-S3: reads the button and
  ultrasonic sensor, drives the waving servo, RGB status LED and OLED
  status screen, captures microphone audio and streams it to the laptop,
  and plays back the synthesized answer through the speaker amp. Runs a
  simple state machine (`IDLE → GREETING → LISTENING → WAITING_RESPONSE →
  SPEAKING → IDLE`) and talks to the laptop over USB serial using a small
  custom line/byte protocol.
- **`robot_backend/`** — the Python program that runs on the laptop:
  - `main.py` — orchestrates one full visitor interaction end-to-end, looped
    forever.
  - `serial_link.py` — the Python half of the wire protocol (must always
    stay in sync with the `WIRE PROTOCOL` comment block at the top of
    `EventRobot.ino`).
  - `stt.py` — offline speech-to-text using **faster-whisper**.
  - `rulebook.py` — local keyword search over `data/rulebook.json` (your
    event facts), used to ground the LLM so it doesn't hallucinate.
  - `llm.py` — sends the question + matched facts to **Groq** (OpenAI-compatible
    chat completions API) and gets back a short spoken-style answer. This is
    the only network/paid call in the whole pipeline.
  - `tts.py` — offline text-to-speech using **Piper**, resampled to match
    the ESP32's audio format.
  - `config.py` — all settings, loaded from a local `.env` file.
  - `data/rulebook.json` — the editable "knowledge base" of event facts.
  - `tests/` — an offline test (`test_offline_cycle.py`) that replays a
    full trigger→greet→listen→process→speak→idle cycle against a fake,
    in-memory serial connection (no hardware needed) to catch protocol
    mismatches between the firmware and the backend before they reach a
    live demo.

## How it works, end to end

```
 Visitor walks up / presses button
              │
              ▼
   ESP32 detects trigger (button or ultrasonic proximity)
              │  TRIGGER:BUTTON / TRIGGER:PROXIMITY  (over USB serial)
              ▼
   Laptop (main.py) sends COMMAND:GREET
              │
              ▼
   ESP32 waves servo, plays a greeting chirp, shows "Hello!" on OLED,
   RGB LED turns BLUE
              │  STATUS:GREETING_DONE
              ▼
   Laptop sends COMMAND:LISTEN
              │
              ▼
   ESP32 records 5s of mic audio (INMP441, I2S), RGB LED turns GREEN,
   streams it back as raw PCM16 audio (AUDIO_START/.../AUDIO_END)
              │
              ▼
   Laptop: faster-whisper transcribes the audio → text question
              │
              ▼
   Laptop: rulebook.py keyword-matches the question against your
   event facts (data/rulebook.json)
              │
              ▼
   Laptop: llm.py sends the question + matched facts to Groq's LLM,
   gets back a short, spoken-style answer (RGB LED YELLOW = "thinking")
              │
              ▼
   Laptop: tts.py (Piper) synthesizes the answer to speech, offline
              │
              ▼
   Laptop sends COMMAND:SPEAK + the audio frame
              │
              ▼
   ESP32 plays the answer through the speaker (MAX98357A), RGB LED CYAN
              │  STATUS:IDLE
              ▼
   Back to IDLE, ready for the next visitor
```

The laptop is the only side that talks to the internet (and only for the
single Groq call) — the ESP32 never connects to Wi-Fi/cloud, it only talks
to the laptop over the USB cable.

## Hardware — bill of materials

| Component | Purpose | Notes |
|---|---|---|
| ESP32-S3 DevKit board | Main controller ("body") | Any ESP32-S3 dev board with enough exposed GPIO; avoids strapping pins 0/3/45/46 and native-USB pins 19/20 |
| Push button | Manual trigger | Any momentary NO push button |
| HC-SR04 ultrasonic sensor | Proximity trigger (auto-greet when someone approaches) | 5V logic — needs a voltage divider on ECHO, see wiring below |
| SG90 (or similar) hobby servo | Waving "hand" gesture | Standard 3-wire hobby servo, 50Hz PWM |
| Common-cathode RGB LED (+ 3 resistors) | Visual status indicator | ~220–330Ω resistor per colour channel; if you only have a common-anode LED, either drive it through NPN transistors or flip the HIGH/LOW logic in `setRGB()` in the firmware |
| SSD1306 128×64 I2C OLED display | Status text ("Ready", "Listening...", etc.) | I2C address `0x3C` (default on most modules) |
| INMP441 I2S MEMS microphone | Captures the visitor's spoken question | I2S digital mic, tie its L/R select pin to GND |
| MAX98357A I2S class-D amplifier + small 4–8Ω speaker | Plays the greeting chirp and the spoken answer | I2S digital audio in, speaker out |
| USB cable (data-capable) | Power **and** the serial link to the laptop | Must be a real data cable, not charge-only |
| Laptop / PC | Runs the Python backend (STT, LLM call, TTS) | Windows, with a free USB port; see software setup below |

## Circuit / wiring connections

All pin numbers below are ESP32-S3 GPIO numbers, exactly as defined at the
top of `EventRobot.ino` (`#define PIN_...`). If you use a different board
layout, only the `#define` block needs to change — nothing else in the
firmware.

### Trigger inputs

| Signal | ESP32-S3 GPIO | Wiring |
|---|---|---|
| Push button | GPIO 4 | One leg → GPIO 4, other leg → GND. Uses the internal pull-up (`INPUT_PULLUP`) — no external resistor needed. Pressed = LOW. |
| HC-SR04 `TRIG` | GPIO 5 | Direct connection (TRIG is a 3.3V-compatible input on the sensor) |
| HC-SR04 `ECHO` | GPIO 6 | **Do not connect directly** — ECHO outputs 5V and the ESP32 is 3.3V-only on its GPIOs. Use a resistor divider, e.g. 1kΩ (ECHO→node) + 2kΩ (node→GND), and feed the midpoint node into GPIO 6. |
| HC-SR04 `VCC` | 5V rail | From the ESP32 board's 5V pin (or an external 5V supply) |
| HC-SR04 `GND` | GND | Common ground with the ESP32 |

### Actuators / indicators

| Signal | ESP32-S3 GPIO | Wiring |
|---|---|---|
| Servo signal | GPIO 7 | Servo signal wire → GPIO 7. Servo `+`/red → 5V, servo `-`/brown or black → GND. **Power the servo from a 5V source that can supply its stall current** (not directly from the ESP32's onboard 3.3V regulator) — share ground with the ESP32. |
| RGB LED — Red | GPIO 15 | GPIO 15 → resistor → LED red anode/cathode leg (see LED type note below) |
| RGB LED — Green | GPIO 16 | GPIO 16 → resistor → LED green leg |
| RGB LED — Blue | GPIO 17 | GPIO 17 → resistor → LED blue leg |
| RGB LED — common | — | Common-cathode leg → GND (firmware assumes common-cathode: HIGH = colour on). For a common-anode LED, wire the common leg to 3.3V/5V and either invert the logic in `setRGB()` or drive each channel through an NPN transistor. |

### 1.8" TFT SPI Display (ST7735 128x160 V1.1)

| Signal | ESP32-S3 Pin | Wiring |
|---|---|---|
| VCC | 3.3V (or 5V) | Power pin (3.3V recommended) |
| GND | GND | Common ground |
| CS | GPIO 10 | To TFT `CS` (Chip Select) |
| RESET / RES | GPIO 8 | To TFT `RESET` / `RES` |
| A0 / DC | GPIO 9 | To TFT `A0` / `DC` (Data/Command) |
| SDA / MOSI | GPIO 11 | To TFT `SDA` / `MOSI` / `DIN` (Data In) |
| SCL / SCK | GPIO 12 | To TFT `SCL` / `SCK` / `CLK` (SPI Clock) |
| LED / BLK | 3.3V | To TFT `LED` / `BLK` (Backlight power - must be wired to light up the display) |

### Microphone — INMP441 (I2S input, bus #0)

| Signal | ESP32-S3 GPIO | Wiring |
|---|---|---|
| SCK / BCLK | GPIO 40 | To INMP441 `SCK` |
| WS / LRCL | GPIO 41 | To INMP441 `WS` |
| SD (mic data out) | GPIO 42 | To INMP441 `SD` (this is mic **output**, wired into the ESP32's data-**in**) |
| L/R | — | Tie to GND (selects the left channel; matches the firmware's `I2S_CHANNEL_FMT_ONLY_LEFT`) |
| VDD | 3.3V | INMP441 is a 3.3V part |
| GND | GND | Common ground |

If recorded audio comes back silent or garbled once everything is wired
and flashed, try swapping the firmware's channel format to
`I2S_CHANNEL_FMT_ONLY_RIGHT` (one-line change, noted in the firmware) — some
boards/wiring combinations land the data on the other slot.

### Speaker amplifier — MAX98357A (I2S output, bus #1)

| Signal | ESP32-S3 GPIO | Wiring |
|---|---|---|
| BCLK | GPIO 1 | To MAX98357A `BCLK` |
| LRC / WS | GPIO 2 | To MAX98357A `LRC` |
| DIN | GPIO 38 | To MAX98357A `DIN` |
| SD | — | Tie high (always enabled) or to a spare GPIO if you want software mute control |
| Speaker out | — | MAX98357A's `+`/`-` speaker terminals → your 4–8Ω speaker |
| VIN | 5V (or 3–5.5V per datasheet) | Power rail |
| GND | GND | Common ground |

### Power notes

- Share a **common ground** across every module (ESP32, HC-SR04, servo,
  MAX98357A, OLED, mic, LED) — this is the single most common source of
  "flaky sensor" or "no audio" bugs.
- The servo and the amplifier can both draw meaningful current — if you see
  brownouts/resets when the servo moves or audio plays, power them from a
  dedicated 5V supply rather than solely through the ESP32 board's onboard
  regulator, and make sure that supply's ground is tied back to the ESP32's
  ground.
- The USB cable to the laptop provides both power to the ESP32 itself and
  the serial data link — it does not need to carry the higher current the
  servo/amp draw if you're powering those separately.

## Software setup — from a blank laptop to a working demo

### 1. Flash the ESP32-S3 firmware

1. Install the [Arduino IDE](https://www.arduino.cc/en/software) (2.x).
2. In **Boards Manager**, install the **esp32** board package (Espressif
   Systems), then select an **ESP32S3 Dev Module** board.
3. Install these libraries via **Library Manager**:
   - `Adafruit GFX Library`
   - `Adafruit SSD1306`
   - `ESP32Servo`
   - (the I2S driver used, `driver/i2s.h`, ships with the ESP32 core — no
     separate install needed)
4. Wire up the hardware exactly as in the tables above.
5. Open `EventRobot.ino`, select the correct COM port, and click **Upload**.
6. Open the Serial Monitor at **921600 baud** — after boot you should see
   `STATUS:READY` and the OLED should show "Ready / Press button or stand
   close".

### 2. Set up the laptop backend

All commands below are run from the `robot_backend/` folder.

```
py -3.11 -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt
```

> `faster-whisper` (via `ctranslate2`) and `piper-tts` don't reliably have
> prebuilt wheels for brand-new Python releases — if your default `python`
> is newer than 3.11/3.12, install with `py -3.11` explicitly as above.

Copy the environment template and fill it in:

```
copy .env.example .env
```

Then edit `.env`:

- `SERIAL_PORT` — the COM port the ESP32 enumerates as (check **Device
  Manager → Ports (COM & LPT)** on Windows).
- `GROQ_API_KEY` — get a free key from [console.groq.com](https://console.groq.com)
  and paste it here. **Only in `.env`, never in a file you'd commit** —
  make sure `.env` is gitignored.
- `PIPER_MODEL_PATH` — path to a downloaded Piper voice `.onnx` file (next
  step).

### 3. Download an offline TTS voice (Piper)

Piper voices are published on Hugging Face (`rhasspy/piper-voices`). For
`en_US-lessac-medium`:

```
mkdir voices
curl -L -o voices/en_US-lessac-medium.onnx ^
  https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx
curl -L -o voices/en_US-lessac-medium.onnx.json ^
  https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx.json
```

Both the `.onnx` file and its matching `.onnx.json` config must sit in the
same folder.

`faster-whisper`'s STT model (`small.en` by default, set via
`WHISPER_MODEL_SIZE` in `.env`) downloads itself automatically the first
time `main.py` runs — no manual step needed, just make sure the laptop has
internet on that first run.

### 4. Fill in your event's facts

Edit `robot_backend/data/rulebook.json` and replace every `TODO` field:

```json
{
  "id": "unique-id",
  "keywords": ["words", "a visitor", "might", "say"],
  "question_hint": "example question this rule answers (reference only, not used by code)",
  "answer": "the exact sentence(s) the robot should speak"
}
```

Keep `keywords` generous (synonyms, informal phrasing) — matching is a
simple keyword-overlap search, there's no fuzzy matching or embeddings
involved.

### 5. Connect the hardware and run it

1. Plug the ESP32-S3 into the laptop via USB.
2. Make sure nothing else (Arduino Serial Monitor, another program) is
   holding the COM port open.
3. From `robot_backend/`, with the venv active:

```
python main.py
```

You should see log lines for connecting to the serial port, then
`Connected. Waiting for a visitor...`. Press the button (or walk within
~60cm of the ultrasonic sensor) to trigger a full interaction.

### 6. Test the software without any hardware plugged in

```
pip install pytest
pytest tests/ -v
```

`tests/test_offline_cycle.py` runs a full trigger → greet → listen →
processing → speak → idle cycle against an in-memory fake serial pair, with
STT/TTS/LLM monkeypatched out. It exists to catch wire-protocol mismatches
between `main.py` / `serial_link.py` and `EventRobot.ino` without needing a
board plugged in — useful for developing away from the physical kiosk.

## The wire protocol (laptop ↔ ESP32)

Plain ASCII lines over USB serial at **921600 baud**. Documented in full,
and kept in sync, in two places: the `WIRE PROTOCOL` comment block at the
top of `EventRobot.ino`, and the module docstring in
`robot_backend/serial_link.py`.

**ESP32 → laptop:**
- `STATUS:<text>` — informational, laptop just logs it
- `TRIGGER:BUTTON` / `TRIGGER:PROXIMITY` — a visitor triggered an interaction
- `ERROR:<text>` — something went wrong on the ESP32 side
- `AUDIO_START:<len>:<crc32>` + `<len>` raw PCM16LE mono 16kHz bytes + `AUDIO_END`

**Laptop → ESP32:**
- `COMMAND:GREET` — play the wave + greeting chirp
- `COMMAND:LISTEN` — record the mic and stream it back
- `COMMAND:PROCESSING` — status hint only ("Thinking...")
- `COMMAND:SPEAK` + one audio frame — play the answer out loud
- `COMMAND:IDLE` — return to idle

If you ever change one side of this protocol, change the other and re-run
`pytest tests/ -v` before testing on real hardware.

## Known fragile points / things to double-check before a live event

- **Opening the serial port reboots the ESP32** (Arduino-core default DTR
  toggle) — `main.py` only reopens the port on an actual `OSError` (cable
  unplugged), not on ordinary protocol hiccups. Don't restart `main.py`
  repeatedly to "fix" a stuck interaction if the board itself is fine —
  each restart reboots it.
- **Fixed 5-second listening window** — `RECORD_SECONDS` in
  `EventRobot.ino` is a fixed capture window, not silence-detected. A
  visitor who talks past 5 seconds gets truncated. If that's a problem
  live, raise `RECORD_SECONDS` in the firmware and reflash.
- **CRC mismatches are logged, not retried** — both directions verify a
  CRC32 checksum but there's no automatic re-send; a bad frame just aborts
  that one interaction and the kiosk returns to idle. Fine for light foot
  traffic, not designed for a long/lossy cable.
- **Mic gain shift (`>>14`) in `EventRobot.ino`** — tuned by feel, not
  measured. If STT accuracy is poor, check the transcript logged by
  `stt.py` first; if it's consistently garbled or clipped, revisit that
  shift amount before reaching for a bigger Whisper model.
- **Groq model name** — `GROQ_MODEL` in `.env.example` is a best-guess
  default (`llama-3.1-8b-instant`); Groq's available model list changes
  over time. Confirm the model is still served before the event, since an
  invalid model name only surfaces at request time as an `LLMError`, not at
  startup.

## Project structure

```
event robo/
├── EventRobot.ino              ESP32-S3 firmware
└── robot_backend/              Laptop-side Python backend
    ├── main.py                 Orchestrator loop
    ├── serial_link.py          Wire-protocol transport
    ├── stt.py                  Offline speech-to-text (faster-whisper)
    ├── rulebook.py              Local keyword search over event facts
    ├── llm.py                  Groq LLM call (only cloud/paid step)
    ├── tts.py                  Offline text-to-speech (Piper)
    ├── config.py                Settings, loaded from .env
    ├── .env.example             Environment variable template
    ├── data/
    │   └── rulebook.json        Your event's facts (edit this!)
    ├── voices/                  Downloaded Piper .onnx voice models
    └── tests/
        ├── fake_stream.py       In-memory fake serial pair for testing
        └── test_offline_cycle.py  Full-cycle test, no hardware needed
```
