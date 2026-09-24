"""Text-to-speech via Piper (offline, local .onnx voice model).

Audio is now played directly on the laptop's own speaker via sounddevice.
We return the raw PCM at the voice's native sample rate (typically 22050 Hz)
so no resampling is needed, giving better audio quality than the old
16 kHz downsampled stream that was sent to the ESP32 amplifier.
"""
from __future__ import annotations

import logging
import time

import numpy as np

import config

logger = logging.getLogger("robot_backend.tts")

_voice = None


def _get_voice():
    global _voice
    if _voice is None:
        from piper import PiperVoice

        logger.info("Loading Piper voice from %s", config.PIPER_MODEL_PATH)
        _voice = PiperVoice.load(config.PIPER_MODEL_PATH)
    return _voice


def synthesize(text: str) -> tuple[bytes, int]:
    """Returns (raw PCM16LE mono bytes, sample_rate_hz) at the voice's native
    sample rate, ready to be played directly on the laptop speaker via
    sounddevice.  Returns (b"", 16000) on empty or failed synthesis."""
    if not text.strip():
        return b"", config.AUDIO_SAMPLE_RATE

    voice = _get_voice()
    t0 = time.perf_counter()
    # voice.synthesize() yields one AudioChunk per sentence (mono int16 PCM
    # at the voice's native sample rate) - concatenate them all.
    chunks = list(voice.synthesize(text))
    if not chunks:
        return b"", config.AUDIO_SAMPLE_RATE

    sample_rate = chunks[0].sample_rate
    samples = np.concatenate(
        [np.frombuffer(c.audio_int16_bytes, dtype="<i2") for c in chunks]
    )
    pcm_bytes = samples.astype("<i2").tobytes()
    logger.info(
        "TTS %r -> %.2fs audio at %dHz, synthesis took %.2fs",
        text,
        len(samples) / sample_rate,
        sample_rate,
        time.perf_counter() - t0,
    )
    return pcm_bytes, sample_rate
