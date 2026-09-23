# Kibi-Kibi backend

Runs on the laptop, talks to the ESP32-S3 kiosk over USB serial (921600
baud). Orchestrates: wait for TRIGGER -> COMMAND:GREET -> COMMAND:LISTEN
(receive question audio) -> COMMAND:PROCESSING (offline STT -> local
rulebook search -> Groq LLM -> offline TTS) -> COMMAND:SPEAK (send answer
audio) -> COMMAND:IDLE.

The exact line/byte framing is documented in two places that must always
agree: the `WIRE PROTOCOL` comment block at the top of `../KibiKibi.ino`,
and the module docstring in `serial_link.py`.

Everything is offline/free except the LLM call (`llm.py`, via Groq's API) -
STT is `faster-whisper`, TTS is `Piper`, and rulebook lookup is local
keyword search over `data/rulebook.json`.

## Setup

```
py -3.11 -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt
```

`faster-whisper` (via `ctranslate2`) and `piper-tts` don't reliably have
prebuilt wheels for brand-new Python releases - if your default `python`
is newer than 3.11/3.12, install with `py -3.11` explicitly, as above.

Copy the environment template and fill it in:

```
copy .env.example .env
```

Then edit `.env`:
- `SERIAL_PORT` - the COM port the ESP32 enumerates as (Device Manager).
- `GROQ_API_KEY` - paste your Groq API key here. **Only in `.env`, never in
  a file you'd commit** - `.env` should be gitignored.
- `PIPER_MODEL_PATH` - path to a downloaded Piper voice `.onnx` file (see
  below).

## Downloading a Piper voice

Piper voices are published on Hugging Face
(`rhasspy/piper-voices`). For `en_US-lessac-medium`:

```
mkdir voices
curl -L -o voices/en_US-lessac-medium.onnx ^
  https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx
curl -L -o voices/en_US-lessac-medium.onnx.json ^
  https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx.json
```

Both the `.onnx` and its matching `.onnx.json` config file are required in
the same directory.

## Filling in the rulebook

Edit `data/rulebook.json`. Every `TODO` field needs replacing:

```json
{
  "id": "unique-id",
  "keywords": ["words", "a visitor", "might", "say"],
  "question_hint": "example question this rule answers (not used by code, just for your own reference)",
  "answer": "the exact sentence(s) the robot should speak"
}
```

`rulebook.py` does simple keyword-overlap search - keep `keywords` lists
generous (include synonyms, informal phrasing) since there's no fuzzy
matching or embeddings involved.

## Running

```
python main.py
```

## Testing without hardware

```
pip install pytest
pytest tests/ -v
```

`tests/test_offline_cycle.py` runs a full trigger -> greet -> listen ->
processing -> speak -> idle cycle against an in-memory fake serial pair
(`tests/fake_stream.py`), with STT/TTS/LLM monkeypatched out. It exists to
catch wire-protocol desyncs between `main.py`/`serial_link.py` and
`KibiKibi.ino` without needing a board plugged in.

## Fragile points / things to double-check before a live demo

- **Serial port auto-reset**: opening a COM port to an ESP32 toggles DTR
  and reboots the board (Arduino-core default). `main.py` only reopens the
  port on an actual `OSError` (cable unplugged), not on ordinary protocol
  errors - don't "fix" a stuck interaction by restarting `main.py`
  repeatedly if the board is otherwise fine, since each restart reboots it.
- **Fixed 5-second listening window**: `KibiKibi.ino`'s `RECORD_SECONDS`
  is a fixed capture window, not silence-detected. A visitor who talks
  past 5 seconds gets truncated. If that's a problem live, raise
  `RECORD_SECONDS` in the .ino (and reflash) rather than trying to fix it
  from the backend.
- **CRC mismatches are logged, not retried**: both directions verify a
  CRC32 but there's no automatic re-send - a bad frame just aborts that one
  interaction (the visitor sees a "no response" style failure and gets
  cycled back to idle). Fine for a kiosk with light foot traffic; not
  designed for a lossy/long serial cable.
- **Mic gain shift (`>>14`) in KibiKibi.ino**: tuned by feel, not
  measured. If STT accuracy is poor, check `stt.py`'s log line for the
  transcript first - if it's consistently garbled/clipped, that shift
  amount is the first thing to revisit, not the Whisper model size.
- **Groq model name**: `GROQ_MODEL` in `.env.example` is a best-guess
  default (`llama-3.1-8b-instant`) - Groq's available model list changes;
  confirm the model is still served before the event, since an invalid
  model name will surface at request time as `LLMError`, not at startup.
