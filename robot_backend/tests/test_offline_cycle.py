"""Full trigger -> greet -> listen -> processing -> speak -> idle cycle,
run against a fake in-memory serial pair (tests/fake_stream.py) instead of
real hardware. STT/TTS/LLM are monkeypatched so this test is fast, fully
offline, and doesn't need any ML model downloaded - it exists purely to
prove main.py and serial_link.py agree on the wire protocol with each
other (and, by inspection, with EventRobot.ino's matching comments).

Run with: pytest tests/test_offline_cycle.py -v
"""
from __future__ import annotations

import struct
import sys
import threading
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import llm  # noqa: E402
import main  # noqa: E402
import mic  # noqa: E402
import rulebook as rulebook_mod  # noqa: E402
import stt  # noqa: E402
import tts  # noqa: E402
from serial_link import SerialLink  # noqa: E402
from tests.fake_stream import FakeSerialPair  # noqa: E402


def _fake_pcm(n_samples: int, value: int = 1000) -> bytes:
    return struct.pack(f"<{n_samples}h", *([value] * n_samples))


def _fake_device(device_link: SerialLink, question_pcm: bytes, results: dict) -> None:
    """Plays the role of EventRobot.ino for one full interaction."""
    device_link.send_line("TRIGGER:BUTTON")

    line = device_link.readline(timeout=5)
    assert line == "COMMAND:GREET", line
    device_link.send_line("STATUS:GREETING_DONE")

    line = device_link.readline(timeout=5)
    assert line == "COMMAND:LISTEN", line
    device_link.send_line("STATUS:LISTEN_READY")

    line = device_link.readline(timeout=5)
    assert line == "STATUS:RECORDING_DONE", line

    line = device_link.readline(timeout=10)
    if line == "COMMAND:PROCESSING":
        line = device_link.readline(timeout=20)

    if line == "COMMAND:SPEAK":
        results["reply_pcm"] = device_link.read_audio_frame(timeout=10)
    elif line == "COMMAND:IDLE":
        results["reply_pcm"] = None
    else:
        raise AssertionError(f"unexpected command: {line}")

    device_link.send_line("STATUS:IDLE")


def test_full_interaction_cycle(monkeypatch):
    question_pcm = _fake_pcm(1600)  # 0.1s of fake "question" audio
    reply_pcm = _fake_pcm(800, value=-500)  # fake "answer" audio

    monkeypatch.setattr(mic, "record", lambda: question_pcm)
    monkeypatch.setattr(stt, "transcribe", lambda pcm: "where is registration")
    monkeypatch.setattr(
        llm, "answer_question", lambda question, matches: "Registration is at the front desk."
    )
    monkeypatch.setattr(tts, "synthesize", lambda text: reply_pcm)

    rulebook = rulebook_mod.Rulebook(
        rules=[
            rulebook_mod.Rule(
                id="reg",
                keywords=["registration", "register"],
                answer="Registration is at the front desk.",
            )
        ]
    )

    pair = FakeSerialPair()
    host_link = SerialLink(pair.host, default_timeout=5)
    device_link = SerialLink(pair.device, default_timeout=5)

    results: dict = {}
    device_thread = threading.Thread(
        target=_fake_device, args=(device_link, question_pcm, results), daemon=True
    )
    device_thread.start()

    main.run_interaction(host_link, rulebook)

    device_thread.join(timeout=10)
    assert not device_thread.is_alive(), "fake device thread hung or crashed"
    assert results.get("reply_pcm") == reply_pcm


def test_no_speech_goes_idle(monkeypatch):
    """Empty transcript -> backend should send COMMAND:IDLE, not SPEAK."""
    question_pcm = _fake_pcm(1600)

    monkeypatch.setattr(mic, "record", lambda: question_pcm)
    monkeypatch.setattr(stt, "transcribe", lambda pcm: "")
    monkeypatch.setattr(
        llm, "answer_question", lambda *a, **k: pytest.fail("LLM should not be called")
    )

    rulebook = rulebook_mod.Rulebook(rules=[])

    pair = FakeSerialPair()
    host_link = SerialLink(pair.host, default_timeout=5)
    device_link = SerialLink(pair.device, default_timeout=5)

    results: dict = {}
    device_thread = threading.Thread(
        target=_fake_device, args=(device_link, question_pcm, results), daemon=True
    )
    device_thread.start()

    main.run_interaction(host_link, rulebook)

    device_thread.join(timeout=10)
    assert not device_thread.is_alive()
    assert results.get("reply_pcm") is None
