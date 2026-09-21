"""LLM answer generation via Groq's OpenAI-compatible chat completions API.

This is the ONLY step in the pipeline allowed to call a paid/cloud service -
STT (stt.py), TTS (tts.py), and rulebook search (rulebook.py) are all local
and offline. Keep it that way: don't add other network calls here.
"""
from __future__ import annotations

import logging

import requests

import config
from rulebook import RuleMatch

logger = logging.getLogger("robot_backend.llm")

GROQ_CHAT_URL = "https://api.groq.com/openai/v1/chat/completions"

SYSTEM_PROMPT = (
    "You are a friendly event-kiosk robot for TECHTONIC 2026. "
    "Answer the visitor's question in ONE short, crisp, apt sentence (maximum 10 to 12 words). "
    "Your response is shown on a small screen and spoken aloud, so be direct and concise. "
    "No markdown, no lists, no unnecessary filler words. "
    "Only use the event facts provided below. If you don't know, say: "
    "'Please ask a staff member for help.' Never invent facts."
)


class LLMError(Exception):
    pass


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
        "temperature": 0.3,
        "max_tokens": 150,
    }
    headers = {"Authorization": f"Bearer {config.GROQ_API_KEY}"}

    try:
        resp = requests.post(GROQ_CHAT_URL, json=payload, headers=headers, timeout=15)
        resp.raise_for_status()
    except requests.RequestException as e:
        raise LLMError(f"Groq request failed: {e}") from e

    data = resp.json()
    try:
        text = data["choices"][0]["message"]["content"].strip()
    except (KeyError, IndexError) as e:
        raise LLMError(f"Unexpected Groq response shape: {data}") from e

    logger.info("LLM answer: %r", text)
    return text
