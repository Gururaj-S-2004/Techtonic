"""Text-to-speech via Piper (offline, local .onnx voice model).

Piper voices typically synthesize at 22050 Hz; we resample to
config.AUDIO_SAMPLE_RATE (16000, matching EventRobot.ino's I2S speaker
config) with simple linear interpolation - good enough for spoken-word
audio and avoids pulling in scipy/audioop just for this.
"""
from __future__ import annotations

import logging

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


def _resample_linear(samples: np.ndarray, orig_sr: int, target_sr: int) -> np.ndarray:
    if orig_sr == target_sr or len(samples) == 0:
        return samples
    duration = len(samples) / orig_sr
    n_target = max(1, int(round(duration * target_sr)))
    x_old = np.linspace(0, duration, num=len(samples), endpoint=False)
    x_new = np.linspace(0, duration, num=n_target, endpoint=False)
    resampled = np.interp(x_new, x_old, samples.astype(np.float32))
    return resampled.astype(np.int16)


def synthesize(text: str) -> bytes:
    """Returns raw PCM16LE mono bytes at config.AUDIO_SAMPLE_RATE, ready to
    hand straight to SerialLink.send_audio_frame()."""
    if not text.strip():
        return b""

    voice = _get_voice()
    # voice.synthesize() yields one AudioChunk per sentence (mono int16 PCM
    # at the voice's native sample rate) - concatenate them all before
    # resampling once, rather than resampling per-sentence.
    chunks = list(voice.synthesize(text))
    if not chunks:
        return b""

    frame_rate = chunks[0].sample_rate
    samples = np.concatenate(
        [np.frombuffer(c.audio_int16_bytes, dtype="<i2") for c in chunks]
    )

    resampled = _resample_linear(samples, frame_rate, config.AUDIO_SAMPLE_RATE)
    pcm_bytes = resampled.astype("<i2").tobytes()
    logger.info(
        "TTS %r -> %.2fs audio (%dHz source, resampled to %dHz)",
        text,
        len(resampled) / config.AUDIO_SAMPLE_RATE,
        frame_rate,
        config.AUDIO_SAMPLE_RATE,
    )
    return pcm_bytes
