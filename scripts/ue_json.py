"""Read Unreal Engine's JSON files, which are often JSONC: `//` and `/* */` comments, trailing commas.

UE's own JSON reader accepts them, and engine files use them (UE 5.5's Engine/Config/Apple/Apple_SDK.json
has `//` comments and trailing commas), so strict `json.loads` fails on them. Python stdlib only.
"""
from __future__ import annotations

import json
from pathlib import Path
import re

# A JSON string literal comes first in each alternation, so a `//` or `,]` inside a string is kept as-is.
_STRING = r'"(?:[^"\\]|\\.)*"'
_COMMENT = re.compile(_STRING + r"|//[^\r\n]*|/\*.*?\*/", re.S)
_TRAILING_COMMA = re.compile(_STRING + r"|,(?=\s*[}\]])")


def _keep_strings(pattern: re.Pattern, text: str, replacement: str) -> str:
    return pattern.sub(lambda m: m.group() if m.group().startswith('"') else replacement, text)


def loads(text: str):
    """json.loads that also accepts comments and trailing commas (comments go first: one may follow a comma)."""
    return json.loads(_keep_strings(_TRAILING_COMMA, _keep_strings(_COMMENT, text, " "), ""))


def read(path: Path):
    return loads(Path(path).read_text(encoding="utf-8-sig"))
