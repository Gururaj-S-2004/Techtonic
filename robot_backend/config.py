"""Central config, loaded from environment variables / a local .env file.

Copy .env.example to .env and fill in real values - .env is gitignored and
should never contain a real API key that gets committed.
"""
from __future__ import annotations

import os
from pathlib import Path

from dotenv import load_dotenv

BASE_DIR = Path(__file__).resolve().parent

load_dotenv(BASE_DIR / ".env")


def _env(name: str, default: str | None = None) -> str:
    value = os.getenv(name, default)
    if value is None:
        raise RuntimeError(f"Missing required environment variable: {name}")
    return value


def _env_int(name: str, default: int) -> int:
    return int(os.getenv(name, str(default)))


def _env_float(name: str, default: float) -> float:
    return float(os.getenv(name, str(default)))


# -- Serial link --------------------------------------------------------
SERIAL_PORT = _env("SERIAL_PORT", "COM3")
SERIAL_BAUD = _env_int("SERIAL_BAUD", 921600)
SERIAL_COMMAND_TIMEOUT_S = _env_float("SERIAL_COMMAND_TIMEOUT_S", 20.0)

# -- Speech-to-text (faster-whisper, offline) ----------------------------
WHISPER_MODEL_SIZE = _env("WHISPER_MODEL_SIZE", "small.en")
WHISPER_DEVICE = _env("WHISPER_DEVICE", "cpu")
WHISPER_COMPUTE_TYPE = _env("WHISPER_COMPUTE_TYPE", "int8")

# -- Text-to-speech (Piper, offline) -------------------------------------
PIPER_MODEL_PATH = _env("PIPER_MODEL_PATH", str(BASE_DIR / "voices" / "en_US-lessac-medium.onnx"))

# -- Rulebook -------------------------------------------------------------
RULEBOOK_PATH = _env("RULEBOOK_PATH", str(BASE_DIR / "data" / "rulebook.json"))

# -- LLM (Groq - the only cloud/paid step) --------------------------------
GROQ_API_KEY = os.getenv("GROQ_API_KEY", "")
GROQ_MODEL = _env("GROQ_MODEL", "compound-mini")

# -- Audio format shared with Nexa.ino ------------------------------
AUDIO_SAMPLE_RATE = _env_int("AUDIO_SAMPLE_RATE", 16000)
AUDIO_SAMPLE_WIDTH_BYTES = 2  # 16-bit PCM, must match Nexa.ino

# Brief dramatic pause after COMMAND:SPEAK (answer already shown on the
# TFT) and before the voice actually starts - makes the robot feel like
# it's "considering" the answer instead of blurting it out instantly,
# which reads as more engaging to a watching crowd. Set to 0 to disable.
PRE_SPEAK_DELAY_S = _env_float("PRE_SPEAK_DELAY_S", 1.0)

# Trailing silence padded onto the end of every spoken answer so the audio
# device's output stream has time to fully drain before playback stops -
# without it, the last syllable of the answer can get clipped on some
# Windows audio backends.
TTS_TRAILING_SILENCE_S = _env_float("TTS_TRAILING_SILENCE_S", 0.6)

# -- Laptop microphone (replaces INMP441 on the ESP32) ---------------------
# How many seconds to capture from the laptop mic after COMMAND:LISTEN.
MIC_RECORD_SECONDS = _env_float("MIC_RECORD_SECONDS", 5.0)
# sounddevice device index or partial name; None = OS default input.
# Set MIC_DEVICE to an integer index (e.g. "1") or leave empty for default.
_mic_device_raw = os.getenv("MIC_DEVICE", "")
MIC_DEVICE: int | str | None = (
    int(_mic_device_raw) if _mic_device_raw.isdigit()
    else (_mic_device_raw if _mic_device_raw else None)
)

LOG_LEVEL = _env("LOG_LEVEL", "INFO")
