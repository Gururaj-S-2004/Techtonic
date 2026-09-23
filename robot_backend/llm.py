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

# Used when the rulebook found matching event facts for the question -
# stay strictly grounded in those facts, never invent event details.
EVENT_SYSTEM_PROMPT = (
    "You are a friendly event-kiosk robot for TECHTONIC 2026. "
    "Answer the visitor's question in ONE short, crisp, apt sentence (maximum 10 to 12 words). "
    "Your response is shown on a small screen and spoken aloud, so be direct and concise. "
    "No markdown, no lists, no unnecessary filler words. "
    "Only use the event facts provided below. If they don't actually answer the "
    "question, say: 'Please ask a staff member for help.' Never invent facts."
)

# Used when the question has nothing to do with the event (no rulebook
# match) - answer normally and helpfully like any friendly assistant would,
# instead of refusing or deflecting to a staff member.
GENERAL_SYSTEM_PROMPT = (
    "You are a friendly, upbeat event-kiosk robot at TECHTONIC 2026, chatting "
    "with a visitor. Their question isn't about the event itself, so just "
    "answer it naturally and helpfully, like a warm general-purpose assistant. "
    "Reply in ONE short, natural sentence (maximum 15 words). "
    "Your response is shown on a small screen and spoken aloud, so be direct, "
    "warm, and concise. No markdown, no lists, no unnecessary filler words."
)


class LLMError(Exception):
    pass


def _build_context(matches: list[RuleMatch]) -> str:
    lines = [f"- {m.rule.answer}" for m in matches]
    return "\n".join(lines)


def answer_question(question: str, matches: list[RuleMatch]) -> str:
    if not config.GROQ_API_KEY:
        raise LLMError("GROQ_API_KEY is not set - add it to robot_backend/.env")

    if matches:
        # Event-related question: ground the answer strictly in the
        # matching rulebook facts.
        system_prompt = EVENT_SYSTEM_PROMPT
        context = _build_context(matches)
        user_prompt = (
            f"Event facts:\n{context}\n\n"
            f'Visitor asked: "{question}"\n\n'
            "Answer in ONE concise, apt sentence under 12 words, using only the facts above."
        )
    else:
        # Not related to the event - answer as a friendly general AI
        # instead of refusing or sending the visitor to a staff member.
        system_prompt = GENERAL_SYSTEM_PROMPT
        user_prompt = f'Visitor asked: "{question}"\n\nAnswer in ONE short, friendly sentence.'

    payload = {
        "model": config.GROQ_MODEL,
        "messages": [
            {"role": "system", "content": system_prompt},
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
