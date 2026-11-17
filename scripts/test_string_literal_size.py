"""MSVC refuses a string literal piece over 16380 bytes (error C2026); a wide TEXT() piece holds about 8190 characters.

The extension guides are long raw strings: split them into adjacent TEXT(R"MD(...)MD") pieces below the limit.
"""
from __future__ import annotations

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
RAW = re.compile(r'R"(\w*)\((.*?)\)\1"', re.S)
LIMIT_UTF16_UNITS = 8000  # below MSVC's 16380-byte piece limit for a wide literal, with room for escapes


class StringLiteralSize(unittest.TestCase):
    def test_every_raw_string_piece_fits_msvc(self):
        too_big = []
        for source in sorted(ROOT.glob("**/Source/**/*.cpp")) + sorted(ROOT.glob("**/Source/**/*.h")):
            if "ThirdParty" in source.parts or "Intermediate" in source.parts:
                continue
            text = source.read_text(encoding="utf-8", errors="replace")
            for match in RAW.finditer(text):
                units = len(match.group(2).encode("utf-16-le")) // 2
                if units > LIMIT_UTF16_UNITS:
                    line = text.count("\n", 0, match.start()) + 1
                    too_big.append(f"{source.relative_to(ROOT)}:{line}: {units} UTF-16 units")
        self.assertEqual(too_big, [], "split these raw strings into adjacent pieces (MSVC C2026)")


if __name__ == "__main__":
    unittest.main()
