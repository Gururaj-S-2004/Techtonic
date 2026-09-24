"""Orchestrator: TRIGGER -> GREET -> LISTEN -> PROCESSING (STT -> rulebook
-> LLM -> TTS) -> SPEAK -> IDLE, looped forever.

Run with: python main.py
"""
from __future__ import annotations

import logging
import time
import textwrap

import numpy as np
import sounddevice as sd

import config
import llm
import mic
import rulebook as rulebook_mod
import stt
import tts
from serial_link import SerialLink, SerialLinkError, SerialTimeout

logger = logging.getLogger("robot_backend.main")

FALLBACK_ANSWER = "Sorry, my circuits are a bit foggy right now. Try again!"
WELCOME_MESSAGE = "Welcome to Techtonic Fest! I'm Nexa, your event assistant. Ask me anything!"
GOODBYE_MESSAGE = "Goodbye! Hope to see you again at Techtonic Fest!"


class PersonLeft(Exception):
    """Raised when the visitor walks away (>60cm) mid-session."""


def format_for_display(text: str, width: int = 12, max_lines: int = 5) -> str:
    """Cleans unicode chars and word-wraps text for the ESP32 TFT display in big font (textSize=2)."""
    text = text.replace('’', "'").replace('‘', "'").replace('“', '"').replace('”', '"').replace('—', '-').replace('–', '-')
    text = text.encode('ascii', 'ignore').decode('ascii')
    text = " ".join(text.split())
    lines = textwrap.wrap(text, width=width, break_long_words=True, break_on_hyphens=True)
    if len(lines) > max_lines:
        lines = lines[:max_lines]
    return "|".join(lines)



def run_interaction(link: SerialLink, rulebook: rulebook_mod.Rulebook) -> None:
    """Runs one full visitor session (multiple questions) until silence is
    detected OR the visitor walks away (>60cm).
    Raises PersonLeft if the visitor leaves mid-session so the caller can
    play a goodbye and then welcome the next person."""
    trigger_source = link.wait_for_trigger()
    logger.info("Trigger received: %s", trigger_source)

    # Speak the welcome greeting before anything else
    logger.info("Speaking welcome greeting")
    welcome_pcm, welcome_sr = tts.synthesize(WELCOME_MESSAGE)
    if welcome_pcm:
        _play_audio(welcome_pcm, welcome_sr)

    link.send_command("GREET")
    link.wait_for_status("GREETING_DONE", timeout=config.SERIAL_COMMAND_TIMEOUT_S)

    question_count = 0

    # Session loop: keep listening until silence (empty transcript) is detected.
    while True:
        link.send_command("LISTEN")
        link.wait_for_status("LISTEN_READY", timeout=config.SERIAL_COMMAND_TIMEOUT_S)
        pcm_audio = mic.record()                   # records for MIC_RECORD_SECONDS
        link.send_line("STATUS:RECORDING_DONE")
        logger.info("Recorded %.2fs of audio from laptop mic",
                    len(pcm_audio) / 2 / config.AUDIO_SAMPLE_RATE)

        processing_start = time.perf_counter()
        question = stt.transcribe(pcm_audio)

        if not question:
            # No speech detected — visitor is silent, end the session.
            logger.info("Silence detected — ending session after %d question(s).", question_count)
            link.send_command("IDLE")
            link.wait_for_status("IDLE", timeout=config.SERIAL_COMMAND_TIMEOUT_S)
            return

        question_count += 1
        logger.info("Visitor asked: %s", question)
        print(f"\n==========================================")
        print(f"  You said: {question}")
        print(f"==========================================\n")

        link.send_command("PROCESSING")

        answer = _generate_answer(question, rulebook)

        if not answer:
            # Fallback is always set, but guard anyway — end session.
            link.send_command("IDLE")
            link.wait_for_status("IDLE", timeout=config.SERIAL_COMMAND_TIMEOUT_S)
            return

        logger.info("Answer: %s", answer)
        print(f"\n==========================================")
        print(f"  Answer: {answer}")
        print(f"==========================================\n")

        clean_a = format_for_display(answer, width=12, max_lines=5)
        link.send_command(f"DISPLAY_A:{clean_a}")

        pcm_bytes, sample_rate = tts.synthesize(answer)
        if not pcm_bytes:
            # TTS failed — end session gracefully.
            link.send_command("IDLE")
            link.wait_for_status("IDLE", timeout=config.SERIAL_COMMAND_TIMEOUT_S)
            return

        logger.info(
            "Question -> answer ready in %.2fs (STT+rulebook+LLM+TTS)",
            time.perf_counter() - processing_start,
        )
        link.send_command("SPEAK")

        # Small deliberate pause before the voice starts - reads as the robot
        # "considering" its answer rather than blurting it out, which is more
        # engaging for a watching crowd. Set PRE_SPEAK_DELAY_S=0 to disable.
        if config.PRE_SPEAK_DELAY_S > 0:
            time.sleep(config.PRE_SPEAK_DELAY_S)

        # Play the TTS audio on the laptop's own speaker, start to finish.
        _play_audio(pcm_bytes, sample_rate)

        # After speaking, check if the person left while we were talking
        _raise_if_person_left(link)

        # After speaking: the while loop sends COMMAND:LISTEN next round —
        # NO COMMAND:IDLE here, so the ESP32 stays in the session.


def _raise_if_person_left(link: SerialLink) -> None:
    """Peeks at buffered serial lines (non-blocking) and raises PersonLeft if
    STATUS:PERSON_LEFT is in the incoming stream. Safe to call at any await
    point between serial commands."""
    person_left = False
    for _ in range(5):
        try:
            line = link.readline(timeout=0.05)
        except SerialTimeout:
            break  # no more buffered data
        if line == "STATUS:PERSON_LEFT":
            logger.info("Person left mid-session — aborting")
            person_left = True
        elif line.startswith("STATUS:"):
            logger.info("[ESP32] %s", line[len("STATUS:"):])
    if person_left:
        raise PersonLeft()


def _drain_until_idle(link: SerialLink, timeout: float = 5.0) -> None:
    """Reads and discards all serial lines until STATUS:IDLE arrives or timeout.
    Also consumes any stale STATUS:PERSON_LEFT lines so they cannot poison the
    next session's _raise_if_person_left check."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            line = link.readline(timeout=0.3)
            logger.debug("drain: %r", line)
            if line == "STATUS:IDLE":
                return
            # Explicitly consume PERSON_LEFT so it never leaks into the next session
            if line == "STATUS:PERSON_LEFT":
                logger.debug("drain: consumed stale PERSON_LEFT")
        except SerialTimeout:
            continue


def _play_audio(pcm_bytes: bytes, sample_rate: int) -> None:
    """Plays raw PCM16LE mono audio on the laptop's default output device,
    fully and clearly from start to end.

    Appends trailing silence before playing so the last word is never cut
    off by the audio device's hardware output buffer drain. Without this,
    sounddevice.play(blocking=True) can return while the final ~50-100ms
    is still queued in the device driver, clipping the end of the sentence.
    """
    if not pcm_bytes:
        return
    samples = np.frombuffer(pcm_bytes, dtype="<i2").astype(np.float32) / 32768.0
    # Silence tail — long enough to outlast any hardware buffer latency.
    pad_len = int(sample_rate * config.TTS_TRAILING_SILENCE_S)
    if pad_len:
        samples = np.concatenate([samples, np.zeros(pad_len, dtype=np.float32)])
    sd.play(samples, samplerate=sample_rate, blocking=True, latency="high")
    sd.wait()  # belt-and-suspenders: drain any remaining OS-level buffer


def _generate_answer(question: str, rulebook: rulebook_mod.Rulebook) -> str:
    if not question:
        logger.warning("Empty transcript - nothing to answer")
        return ""

    matches = rulebook.search(question)
    try:
        return llm.answer_question(question, matches)
    except llm.LLMError as e:
        logger.error("LLM call failed: %s", e)
        return FALLBACK_ANSWER


def main() -> None:
    logging.basicConfig(
        level=getattr(logging, config.LOG_LEVEL.upper(), logging.INFO),
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
    )

    logger.info("Loading rulebook from %s", config.RULEBOOK_PATH)
    rulebook = rulebook_mod.Rulebook.load(config.RULEBOOK_PATH)

    while True:
        try:
            logger.info("Connecting to %s @ %d baud ...", config.SERIAL_PORT, config.SERIAL_BAUD)
            link = SerialLink.open(config.SERIAL_PORT, config.SERIAL_BAUD)
            link.send_command("IDLE")  # sync ESP32 state on (re)connect
        except Exception as e:
            logger.error("Failed to open serial port: %s - retrying in 5s", e)
            time.sleep(5)
            continue

        logger.info("Connected. Waiting for a visitor...")
        while True:
            try:
                run_interaction(link, rulebook)
            except PersonLeft:
                # Person walked away — say goodbye, sync ESP32 to idle,
                # then wait a beat before accepting the next visitor so we
                # don't immediately re-trigger on the same person.
                logger.info("Session ended: visitor left. Speaking goodbye.")
                # Tell ESP32 to reset to idle
                try:
                    link.send_command("IDLE")
                except Exception:
                    pass
                # Speak goodbye
                try:
                    bye_pcm, bye_sr = tts.synthesize(GOODBYE_MESSAGE)
                    if bye_pcm:
                        _play_audio(bye_pcm, bye_sr)
                except Exception as e:
                    logger.warning("Goodbye TTS failed: %s", e)
                # Drain serial until STATUS:IDLE so stale TRIGGER/STATUS lines
                # from the just-ended session don't immediately re-fire.
                logger.info("Draining serial buffer after session end...")
                _drain_until_idle(link)
                # Brief cooldown: give the sensor time to confirm the person
                # is truly gone before we accept another proximity trigger.
                logger.info("Cooldown 3s before next visitor...")
                time.sleep(3)
                logger.info("Ready for next visitor.")
            except (SerialTimeout, SerialLinkError) as e:
                # A single bad cycle (bad CRC, a stray line, a slow visitor)
                # - stay on the same connection and just wait for the next
                # trigger. Reopening the port here would toggle DTR and
                # reset the ESP32 mid-demo, which is far worse than one
                # missed interaction.
                logger.error("Protocol error, resuming on same connection: %s", e)
            except OSError as e:
                # The port itself is gone (cable unplugged, board reset) -
                # this is the case that actually needs a reconnect.
                logger.error("Serial link failed, reconnecting: %s", e)
                try:
                    link.close()
                except Exception:
                    pass
                time.sleep(2)
                break


if __name__ == "__main__":
    main()
