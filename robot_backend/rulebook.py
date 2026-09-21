"""Local keyword search over data/rulebook.json - no network, no LLM.

Used to ground the LLM: we hand the top-matching rule(s) to llm.py as
context rather than trusting the model to know event-specific facts.
"""
from __future__ import annotations

import json
import logging
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

logger = logging.getLogger("robot_backend.rulebook")

_TOKEN_RE = re.compile(r"[a-z0-9']+")

STOPWORDS = {
    "a", "an", "the", "is", "are", "was", "were", "be", "been",
    "what", "which", "who", "whom", "where", "when", "how", "why",
    "for", "to", "of", "in", "on", "at", "by", "with", "from", "into",
    "tell", "me", "about", "can", "you", "i", "do", "does", "did",
    "and", "or", "as", "if", "this", "that", "there", "some", "any"
}


def _tokenize(text: str) -> set[str]:
    tokens = set(_TOKEN_RE.findall(text.lower()))
    content_tokens = {t for t in tokens if t not in STOPWORDS}
    return content_tokens if content_tokens else tokens


@dataclass
class Rule:
    id: str
    keywords: list[str]
    answer: str
    question_hint: str = ""

    @property
    def keyword_tokens(self) -> set[str]:
        tokens: set[str] = set()
        for kw in self.keywords:
            tokens |= _tokenize(kw)
        return tokens


@dataclass
class RuleMatch:
    rule: Rule
    score: int


class Rulebook:
    def __init__(self, rules: list[Rule], event_name: str = ""):
        self.rules = rules
        self.event_name = event_name

    @classmethod
    def load(cls, path: str | Path) -> "Rulebook":
        path = Path(path)
        with path.open("r", encoding="utf-8") as f:
            data = json.load(f)

        rules = [
            Rule(
                id=r["id"],
                keywords=r.get("keywords", []),
                answer=r["answer"],
                question_hint=r.get("question_hint", ""),
            )
            for r in data.get("rules", [])
        ]
        logger.info("Loaded %d rulebook entries from %s", len(rules), path)
        return cls(rules=rules, event_name=data.get("event_name", ""))

    def search(self, query: str, top_k: int = 3, min_score: int = 1) -> list[RuleMatch]:
        """Scores each rule by how many of its keyword tokens appear in the
        query (simple token-overlap count - fine for a small, curated
        rulebook; swap in TF-IDF/embeddings if the rulebook grows large)."""
        query_tokens = _tokenize(query)
        if not query_tokens:
            return []

        scored: list[RuleMatch] = []
        for rule in self.rules:
            overlap = len(query_tokens & rule.keyword_tokens)
            if overlap >= min_score:
                scored.append(RuleMatch(rule=rule, score=overlap))

        scored.sort(key=lambda m: m.score, reverse=True)
        return scored[:top_k]

    def best_match(self, query: str) -> Optional[RuleMatch]:
        matches = self.search(query, top_k=1)
        return matches[0] if matches else None
