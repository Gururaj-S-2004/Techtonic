"""Speech-to-text via faster-whisper (offline, CPU by default).

Input is always raw PCM16LE mono at config.AUDIO_SAMPLE_RATE, captured from
the laptop's own microphone by mic.py (mic.record()) and passed directly to
transcribe() - no audio is streamed from the ESP32.
"""
from __future__ import annotations

import logging

import numpy as np

import config

logger = logging.getLogger("robot_backend.stt")

_model = None


def _get_model():
    global _model
    if _model is None:
        from faster_whisper import WhisperModel

        logger.info(
            "Loading faster-whisper model=%s device=%s compute_type=%s (first call, may take a while)",
            config.WHISPER_MODEL_SIZE,
            config.WHISPER_DEVICE,
            config.WHISPER_COMPUTE_TYPE,
        )
        _model = WhisperModel(
            config.WHISPER_MODEL_SIZE,
            device=config.WHISPER_DEVICE,
            compute_type=config.WHISPER_COMPUTE_TYPE,
        )
    return _model


def pcm16_to_float32(pcm_bytes: bytes) -> np.ndarray:
    samples = np.frombuffer(pcm_bytes, dtype="<i2")
    return samples.astype(np.float32) / 32768.0


def transcribe(pcm_bytes: bytes) -> str:
    """Returns the best-effort transcript text (empty string if nothing
    intelligible was captured)."""
    if not pcm_bytes:
        return ""

    audio = pcm16_to_float32(pcm_bytes)
    model = _get_model()
    segments, info = model.transcribe(
        audio,
        language="en",
        vad_filter=True,
        beam_size=5,
    )
    text = " ".join(seg.text.strip() for seg in segments).strip()
    logger.info("STT (%.2fs audio): %r", len(audio) / config.AUDIO_SAMPLE_RATE, text)
    return text
