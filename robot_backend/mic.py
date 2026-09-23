"""Laptop microphone capture.

Records from the host machine's default (or configured) audio input device,
returning raw PCM16LE mono bytes at config.AUDIO_SAMPLE_RATE - the same format
that KibiKibi.ino previously streamed over serial from the INMP441.

Usage::

    pcm_bytes = mic.record()
    text = stt.transcribe(pcm_bytes)

Dependencies: sounddevice, numpy (both already pulled in by faster-whisper /
the existing requirements).  Install separately if needed:
    pip install sounddevice
"""
from __future__ import annotations

import logging

import numpy as np
import sounddevice as sd

import config

logger = logging.getLogger("robot_backend.mic")


def record(
    seconds: float | None = None,
    device: int | str | None = None,
) -> bytes:
    """Record *seconds* of audio from the laptop's microphone.

    Parameters
    ----------
    seconds:
        Recording duration in seconds.  Defaults to ``config.MIC_RECORD_SECONDS``.
    device:
        sounddevice device index or name.  ``None`` uses the OS default input.
        Override via ``config.MIC_DEVICE`` / the ``MIC_DEVICE`` env var.

    Returns
    -------
    bytes
        Raw PCM16LE mono audio at ``config.AUDIO_SAMPLE_RATE``.
    """
    duration = seconds if seconds is not None else config.MIC_RECORD_SECONDS
    dev = device if device is not None else config.MIC_DEVICE

    logger.info(
        "Recording %.1fs from laptop mic (device=%s, rate=%d Hz)",
        duration,
        dev if dev is not None else "default",
        config.AUDIO_SAMPLE_RATE,
    )

    # sounddevice returns float32 in [-1, 1] by default; we convert to int16.
    audio: np.ndarray = sd.rec(
        int(duration * config.AUDIO_SAMPLE_RATE),
        samplerate=config.AUDIO_SAMPLE_RATE,
        channels=1,
        dtype="float32",
        device=dev,
        blocking=True,
    )

    # Flatten to 1-D and clip to prevent int16 overflow from loud input.
    mono = np.clip(audio[:, 0], -1.0, 1.0)
    pcm = (mono * 32767).astype(np.int16)
    return pcm.tobytes()
