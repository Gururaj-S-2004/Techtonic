"""LLM answer generation via Groq's OpenAI-compatible chat completions API.

This is the ONLY step in the pipeline allowed to call a paid/cloud service -
STT (stt.py), TTS (tts.py), and rulebook search (rulebook.py) are all local
and offline. Keep it that way: don't add other network calls here.
"""
from __future__ import annotations

import logging
import time

import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

import config
from rulebook import RuleMatch

logger = logging.getLogger("robot_backend.llm")

GROQ_CHAT_URL = "https://api.groq.com/openai/v1/chat/completions"

SYSTEM_PROMPT = (
    "You are TECHTONIC, a friendly and witty personal assistant robot at the TECHTONIC 2026 tech fest at MCC. "
    "Your personality: warm, cheerful, confident, and helpful — like a smart friend who happens to know everything about the event. "
    "Rules: "
    "1. Always answer every question — never say you don't know or ask the visitor to ask someone else. "
    "2. For event-specific questions, use only the provided event facts. "
    "3. For general/personal questions (greetings, weather, jokes, your name, how you feel, etc.), answer naturally and in character as a robot assistant. "
    "4. Keep every reply to ONE short sentence, maximum 15 words. "
    "5. No markdown, no bullet points, no lists — plain spoken words only. "
    "6. Be warm and engaging, not robotic. "
    "7. Output ONLY the answer sentence — no reasoning, no preamble, no explanation."
)


class LLMError(Exception):
    pass


def _clean_answer(text: str) -> str:
    """Strip chain-of-thought reasoning that some models leak into content.

    Handles patterns like:
      - <think>...</think> XML blocks
      - Multi-paragraph reasoning ending with the actual answer
      - 'So answer: "..."' or 'Answer: ...' preamble lines
    Returns just the final clean sentence.
    """
    import re
    # Strip <think>...</think> blocks (some models use these)
    text = re.sub(r'<think>.*?</think>', '', text, flags=re.DOTALL).strip()
    # If the model wrote 'So answer: ...' or 'Answer: ...' take only what follows
    answer_match = re.search(r'(?:so answer|final answer|answer)[:\s]+["\u201c]?(.+)["\u201d]?', text, re.IGNORECASE)
    if answer_match:
        return answer_match.group(1).strip().strip('"\u201c\u201d')
    # If there are multiple lines/paragraphs, take the last non-empty line
    lines = [l.strip() for l in text.splitlines() if l.strip()]
    if len(lines) > 1:
        # Last line is usually the actual answer in reasoning models
        return lines[-1].strip('"\u201c\u201d')
    return text.strip('"\u201c\u201d')


def _build_context(matches: list[RuleMatch]) -> str:
    if not matches:
        return "(no matching event facts found)"
    lines = [f"- {m.rule.answer}" for m in matches]
    return "\n".join(lines)


def answer_question(question: str, matches: list[RuleMatch]) -> str:
    if not config.GROQ_API_KEY:
        raise LLMError("GROQ_API_KEY is not set - add it to robot_backend/.env")

    context = _build_context(matches)
    user_prompt = (
        f"Event facts:\n{context}\n\n"
        f'Visitor asked: "{question}"\n\n'
        "Answer in ONE concise, apt sentence under 12 words."
    )

    payload = {
        "model": config.GROQ_MODEL,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": user_prompt},
        ],
        "temperature": 0.4,
        "max_tokens": 120,
    }
    # 'Connection: close' forces a fresh TCP handshake every call.
    # On Windows, reusing keep-alive connections through a firewall or
    # antivirus proxy causes ConnectionResetError 10054 on later requests.
    headers = {
        "Authorization": f"Bearer {config.GROQ_API_KEY}",
        "Connection": "close",
    }

    last_exc: Exception | None = None
    for attempt in range(1, 4):          # up to 3 attempts
        try:
            # Fresh session per attempt - avoids reusing a broken socket.
            session = requests.Session()
            adapter = HTTPAdapter(max_retries=Retry(total=0))  # we handle retries ourselves
            session.mount("https://", adapter)
            resp = session.post(GROQ_CHAT_URL, json=payload, headers=headers, timeout=25)
            resp.raise_for_status()
            last_exc = None
            break                        # success — stop retrying
        except requests.RequestException as e:
            last_exc = e
            if attempt < 3:
                logger.warning("Groq attempt %d failed (%s), retrying in 2s...", attempt, e)
                time.sleep(2)
        finally:
            session.close()
    if last_exc is not None:
        raise LLMError(f"Groq request failed after 3 attempts: {last_exc}") from last_exc

    data = resp.json()
    try:
        msg = data["choices"][0]["message"]
        text = (msg.get("content") or "").strip()
        # Some reasoning models return an empty 'content' and put the
        # answer in a separate 'reasoning' field - fall back to it.
        if not text:
            text = (msg.get("reasoning") or "").strip()
    except (KeyError, IndexError) as e:
        raise LLMError(f"Unexpected Groq response shape: {data}") from e

    text = _clean_answer(text)
    logger.info("LLM answer: %r", text)
    return text
