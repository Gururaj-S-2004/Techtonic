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

FALLBACK_ANSWER = "I'm not sure. Please ask a staff member."


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
    """Runs exactly one full visitor interaction. Any protocol-level error
    is allowed to propagate to the caller, which logs it and returns the
    robot to waiting for the next trigger - one bad cycle should never take
    down the whole kiosk."""
    trigger_source = link.wait_for_trigger()
    logger.info("Trigger received: %s", trigger_source)

    link.send_command("GREET")
    link.wait_for_status("GREETING_DONE", timeout=config.SERIAL_COMMAND_TIMEOUT_S)

    # Send LISTEN, wait for ESP32 to signal it's ready (green LED on the robot),
    # then record from the laptop's own microphone.
    link.send_command("LISTEN")
    link.wait_for_status("LISTEN_READY", timeout=config.SERIAL_COMMAND_TIMEOUT_S)
    pcm_audio = mic.record()              # captures from the laptop mic
    link.send_line("STATUS:RECORDING_DONE")  # tell ESP32 we're done
    logger.info("Recorded %.2fs of question audio from laptop mic",
                len(pcm_audio) / 2 / config.AUDIO_SAMPLE_RATE)

    link.send_command("PROCESSING")

    question = stt.transcribe(pcm_audio)
    if question:
        logger.info("Visitor asked: %s", question)
        print(f"\n==========================================")
        print(f"  You said: {question}")
        print(f"==========================================\n")
        # 'You said' is displayed only in the terminal, not on the TFT

    answer = _generate_answer(question, rulebook)

    if not answer:
        link.send_command("IDLE")
        return

    logger.info("Answer: %s", answer)
    print(f"\n==========================================")
    print(f"  Answer: {answer}")
    print(f"==========================================\n")

    clean_a = format_for_display(answer, width=12, max_lines=5)
    link.send_command(f"DISPLAY_A:{clean_a}")

    pcm_bytes, sample_rate = tts.synthesize(answer)
    if not pcm_bytes:
        link.send_command("IDLE")
        return

    # Signal ESP32 that we are about to speak (it updates the display and waits).
    link.send_command("SPEAK")

    # Small deliberate pause before the voice starts - reads as the robot
    # "considering" its answer rather than blurting it out, which is more
    # engaging for a watching crowd. Set PRE_SPEAK_DELAY_S=0 to disable.
    if config.PRE_SPEAK_DELAY_S > 0:
        time.sleep(config.PRE_SPEAK_DELAY_S)

    # Play the TTS audio on the laptop's own speaker, start to finish.
    _play_audio(pcm_bytes, sample_rate)

    # Tell the ESP32 we are done speaking so it can return to idle.
    link.send_command("IDLE")

    # ESP32 always ends runInteraction() with STATUS:IDLE - wait for it so
    # the serial buffer is clean before we go back to wait_for_trigger().
    link.wait_for_status("IDLE", timeout=config.SERIAL_COMMAND_TIMEOUT_S)



def _play_audio(pcm_bytes: bytes, sample_rate: int) -> None:
    """Plays raw PCM16LE mono audio on the laptop's default output device,
    fully and clearly from start to end. A little trailing silence is
    padded on so the output stream has time to drain before it stops -
    otherwise the last syllable can get clipped on some Windows audio
    backends."""
    if not pcm_bytes:
        return
    samples = np.frombuffer(pcm_bytes, dtype="<i2").astype(np.float32) / 32768.0
    pad_len = int(sample_rate * config.TTS_TRAILING_SILENCE_S)
    if pad_len:
        samples = np.concatenate([samples, np.zeros(pad_len, dtype=np.float32)])
    sd.play(samples, samplerate=sample_rate, blocking=False, latency="high")
    sd.wait()


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
