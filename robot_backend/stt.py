"""Speech-to-text via faster-whisper (offline, CPU by default).

Input is always raw PCM16LE mono at config.AUDIO_SAMPLE_RATE, captured from
the laptop's own microphone by mic.py (mic.record()) and passed directly to
transcribe() - no audio is streamed from the ESP32.
"""
from __future__ import annotations

import logging
import time

import numpy as np

import config

logger = logging.getLogger("robot_backend.stt")

_model = None


def _get_model():
    global _model
    if _model is None:
        from faster_whisper import WhisperModel

        logger.info(
            "Loading faster-whisper model=%s device=%s compute_type=%s cpu_threads=%d (first call, may take a while)",
            config.WHISPER_MODEL_SIZE,
            config.WHISPER_DEVICE,
            config.WHISPER_COMPUTE_TYPE,
            config.WHISPER_CPU_THREADS,
        )
        _model = WhisperModel(
            config.WHISPER_MODEL_SIZE,
            device=config.WHISPER_DEVICE,
            compute_type=config.WHISPER_COMPUTE_TYPE,
            cpu_threads=config.WHISPER_CPU_THREADS,
        )
    return _model


def pcm16_to_float32(pcm_bytes: bytes) -> np.ndarray:
    samples = np.frombuffer(pcm_bytes, dtype="<i2")
    return samples.astype(np.float32) / 32768.0


# Whisper often mishears uncommon proper nouns.  Add corrections here as
# new mishearings are discovered during the event.
# Keys are case-insensitive regex patterns; values are the correct replacements.
_NAME_CORRECTIONS: list[tuple[str, str]] = [
    # Robot name  (Nixa / Nexar / Naxer / Naxa / Nexa → Nexa)
    (r"\bNi[xks]a\b",      "Nexa"),
    (r"\bNax[ae]r?\b",     "Nexa"),
    (r"\bNexar\b",         "Nexa"),
    # Fest name
    (r"\bTech[- ]?tonic\b", "Techtonic"),
    (r"\bTek[- ]?tonic\b",  "Techtonic"),
]


def _fix_names(text: str) -> str:
    """Correct common Whisper mishearings of event-specific proper nouns."""
    import re
    for pattern, replacement in _NAME_CORRECTIONS:
        text = re.sub(pattern, replacement, text, flags=re.IGNORECASE)
    return text


def transcribe(pcm_bytes: bytes) -> str:
    """Returns the best-effort transcript text (empty string if nothing
    intelligible was captured)."""
    if not pcm_bytes:
        return ""

    audio = pcm16_to_float32(pcm_bytes)
    model = _get_model()
    t0 = time.perf_counter()
    segments, info = model.transcribe(
        audio,
        language="en",
        vad_filter=True,
        beam_size=config.WHISPER_BEAM_SIZE,
    )
    text = " ".join(seg.text.strip() for seg in segments).strip()
    text = _fix_names(text)
    logger.info(
        "STT (%.2fs audio) took %.2fs: %r",
        len(audio) / config.AUDIO_SAMPLE_RATE,
        time.perf_counter() - t0,
        text,
    )
    return text
