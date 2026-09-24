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
    """Runs one full visitor session (multiple questions) until silence is detected.
    After each answered question the session loops back to LISTEN.
    The session ends only when the visitor is silent for MIC_RECORD_SECONDS seconds."""
    trigger_source = link.wait_for_trigger()
    logger.info("Trigger received: %s", trigger_source)

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

        # After speaking: the while loop sends COMMAND:LISTEN next round —
        # NO COMMAND:IDLE here, so the ESP32 stays in the session.



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
