# Nexa — AI Event-Kiosk Robot

> **TECHTONIC 2026 · MCC**  
> Nexa stands at your booth, greets visitors, listens to their questions, and answers aloud — hands-free, no staff needed.

---

## What Is Nexa?

Nexa is a physical kiosk robot built for tech fests and exhibitions. When a visitor walks within **60 cm**, Nexa:

1. 🤚 **Waves** (servo) and speaks **"Welcome to Techtonic Fest!"**
2. 👂 **Listens** to their spoken question (laptop mic)
3. 🧠 **Thinks** — offline STT → local rulebook → cloud LLM → offline TTS
4. 🔊 **Speaks** the answer aloud and shows it on the TFT display
5. 🔁 **Loops** — asks for more questions until silence
6. 👋 **Says goodbye** and resets when the visitor walks away

---

## Hardware

| Component | Part | Notes |
|---|---|---|
| Microcontroller | ESP32-S3 DevKit | Main body: sensors + display + servo |
| Distance sensor | HC-SR04 | Triggers on ≤ 60 cm |
| Servo | Standard 5V | Waves on greeting, tracks proximity at all times |
| Display | 1.8″ TFT ST7735 128×160 | Shows "NEXA BOT" header + status/answers |
| Brain | Windows laptop | STT, LLM, TTS — all USB-connected |
| Mic | Laptop built-in mic | Records visitor questions |
| Speaker | Laptop speaker | Plays TTS audio |

### Pin Map (ESP32-S3)

| Signal | Pin |
|---|---|
| Button | GPIO 4 |
| HC-SR04 TRIG | GPIO 5 |
| HC-SR04 ECHO | GPIO 6 |
| Servo | GPIO 7 |
| TFT RST | GPIO 8 |
| TFT DC | GPIO 9 |
| TFT CS | GPIO 10 |
| TFT MOSI | GPIO 11 |
| TFT SCLK | GPIO 12 |

---

## Architecture

```
Visitor
  | walks within 60 cm
  v
[ESP32 - Nexa.ino]
  |  TRIGGER:PROXIMITY --------------------------------------------.
  |                                                                 |
  | <-- COMMAND:GREET -------------------------------------------- |
  |  servo waves, TFT shows "Hello!"                               |
  |  STATUS:GREETING_DONE ---------------------------------------- |
  |                                                    [laptop - robot_backend/]
  | <-- COMMAND:LISTEN ------------------------------ main.py speaks welcome (TTS)
  |  STATUS:LISTEN_READY ---------------------------- mic.py records 6s
  |  <-- STATUS:RECORDING_DONE                        stt.py transcribes (Whisper)
  |                                                    rulebook.py searches facts
  | <-- COMMAND:PROCESSING                             llm.py calls Groq API
  | <-- COMMAND:DISPLAY_A:<answer>                     tts.py synthesizes audio
  | <-- COMMAND:SPEAK -------------------------------- laptop plays audio on speaker
  |
  |  [loop back to LISTEN for next question]
  |
  |  [visitor leaves > 60 cm]
  |  STATUS:PERSON_LEFT ----------------------------- laptop speaks goodbye
  |  COMMAND:IDLE ----------------------------------------------------.
  |  [reset, wait for next visitor]
```

---

## Proximity and Servo Behaviour

The ultrasonic sensor runs **continuously** in every state (idle, greeting, listening, thinking, speaking):

| Distance | Servo | TFT Status |
|---|---|---|
| <= 60 cm | ON (90 deg) | `NEAR (Xcm)` in gold |
| > 60 cm | OFF (0 deg) | `Xcm` in dim gold |
| No echo | OFF | `No echo/out` in red |

If the visitor walks away **during** an active session, Nexa immediately:
- Shows "Bye! See you soon!" on TFT
- Sends `STATUS:PERSON_LEFT` to the laptop
- Laptop speaks the goodbye message and resets to idle

---

## Software Stack

| Layer | Technology |
|---|---|
| Firmware | Arduino C++ (`Nexa.ino`) |
| Speech-to-Text | `faster-whisper` (`small.en` model, CPU, offline) |
| Knowledge base | Local JSON rulebook (`data/rulebook.json`) |
| LLM | Groq API - `llama-3.1-8b-instant` (~1-2s latency) |
| Text-to-Speech | Piper (`en_US-lessac-medium.onnx`, offline) |
| Serial link | Python `pyserial` at 921600 baud |

---

## Project Structure

```
event_robo_share/
|-- Nexa.ino                     ESP32-S3 firmware
|-- README.md                    This file
|-- robot_backend/
|   |-- main.py                  Orchestrator: trigger -> greet -> listen -> speak -> loop
|   |-- serial_link.py           USB serial protocol (ESP32 <-> laptop)
|   |-- llm.py                   Groq LLM call (only cloud step)
|   |-- stt.py                   Whisper offline STT
|   |-- tts.py                   Piper offline TTS
|   |-- mic.py                   Laptop mic recording
|   |-- rulebook.py              Event-facts keyword search
|   |-- config.py                All config from .env
|   |-- .env                     Local secrets (never commit)
|   |-- .env.example             Template
|   |-- data/
|   |   |-- rulebook.json        Event facts (Q&A pairs)
|   |-- voices/
|   |   |-- en_US-lessac-medium.onnx   Piper TTS voice model
|   |-- tests/
|       |-- test_offline_cycle.py      Protocol tests (no board needed)
```

---

## Quick Start

### 1 - Flash the ESP32

1. Install [Arduino IDE 2.x](https://www.arduino.cc/en/software)
2. Add the ESP32-S3 board package via Board Manager URL:
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
3. Install libraries: `Adafruit ST7735`, `Adafruit GFX`, `ESP32Servo`
4. Open **`Nexa.ino`**, select your board + COM port, click **Upload**

### 2 - Set Up the Python Backend

```powershell
cd robot_backend

# Create and activate venv
python -m venv .venv
.venv\Scripts\activate

# Install dependencies
pip install -r requirements.txt
```

### 3 - Configure .env

Copy `.env.example` to `.env` and fill in:

```ini
SERIAL_PORT=COM12
GROQ_API_KEY=gsk_...
GROQ_MODEL=llama-3.1-8b-instant
MIC_RECORD_SECONDS=6.0
PRE_SPEAK_DELAY_S=0
LOG_LEVEL=INFO
```

### 4 - Run

```powershell
python main.py
```

Nexa connects to the ESP32 and waits for a visitor.

---

## Wire Protocol (ESP32 <-> Laptop)

All messages are plain ASCII lines (`\n`-terminated). Both sides strip `\r\n`.

### ESP32 -> Laptop

| Message | Meaning |
|---|---|
| `TRIGGER:BUTTON` | Button pressed while idle |
| `TRIGGER:PROXIMITY` | Person detected within 60 cm while idle |
| `STATUS:READY` | Boot complete |
| `STATUS:LISTEN_READY` | ESP32 ready, start mic recording |
| `STATUS:GREETING_DONE` | Wave animation finished |
| `STATUS:PERSON_LEFT` | Visitor walked away mid-session |
| `STATUS:IDLE` | Session ended, back to idle |
| `ERROR:<text>` | Something went wrong |

### Laptop -> ESP32

| Command | Meaning |
|---|---|
| `COMMAND:GREET` | Play wave animation |
| `COMMAND:LISTEN` | Start the mic-ready phase |
| `COMMAND:PROCESSING` | Show "Thinking..." on TFT |
| `COMMAND:DISPLAY_A:<text>` | Show answer on TFT |
| `COMMAND:SPEAK` | Laptop will play TTS audio |
| `COMMAND:IDLE` | Reset to idle |
| `STATUS:RECORDING_DONE` | Laptop finished recording |

---

## Tuning and Config

| Variable | Default | Purpose |
|---|---|---|
| `MIC_RECORD_SECONDS` | `6.0` | How long to record per question |
| `TRIGGER_DISTANCE_CM` | `60` | Proximity threshold (set in `Nexa.ino`) |
| `PRE_SPEAK_DELAY_S` | `0` | Pause before TTS starts |
| `GROQ_MODEL` | `llama-3.1-8b-instant` | Model for quality/speed trade-off |
| `WHISPER_MODEL_SIZE` | `small.en` | Use `medium.en` for better accuracy |
| `TTS_TRAILING_SILENCE_S` | `0.6` | Prevents last syllable clipping |

---

## Running Tests (No Board Needed)

```powershell
cd robot_backend
pytest tests/
```

Simulates the full ESP32 <-> laptop protocol with an in-memory fake serial port.

---

## Latency Profile

| Step | Typical Time |
|---|---|
| Wake on proximity | instant (100 ms poll) |
| Welcome TTS synthesis | ~0.5 s |
| Mic recording | 6 s (fixed window) |
| Whisper STT | ~1-2 s (CPU, small.en) |
| Groq LLM (`llama-3.1-8b-instant`) | ~1-2 s |
| Piper TTS synthesis | ~0.3 s |
| **Total (typical)** | **~10-12 s end-to-end** |

---

## Known Limitations

- **Fixed recording window** - Nexa always records for `MIC_RECORD_SECONDS` even if the visitor finishes speaking early.
- **One visitor at a time** - Nexa serves one session fully before accepting the next trigger.
- **Laptop dependency** - All heavy processing (STT, LLM, TTS) runs on the connected laptop; the ESP32 is just the body.
- **English only** - Whisper `small.en` and the Piper voice are English-only.

---

*Built for TECHTONIC 2026 @ MCC · Powered by Groq, faster-whisper, Piper, and an ESP32-S3*
