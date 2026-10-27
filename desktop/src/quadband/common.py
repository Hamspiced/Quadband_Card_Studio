"""Shared validation. SPDX-License-Identifier: GPL-3.0-only"""
import json
import re
from pathlib import Path


class CardError(Exception):
    """An actionable device, validation or verification error."""


class NoCard(CardError):
    pass


class AuthenticationError(CardError):
    pass


def require(condition, message):
    if not condition:
        raise CardError(message)


def hex_bytes(text, count=None, maximum=4096):
    compact = re.sub(r"[\s:\-]", "", text)
    require(len(compact) % 2 == 0 and re.fullmatch(r"[0-9a-fA-F]*", compact),
            "Enter complete hexadecimal bytes; spaces are optional.")
    value = bytes.fromhex(compact)
    require(len(value) <= maximum and (count is None or len(value) == count),
            f"Enter exactly {count} bytes." if count is not None else "The data is too large.")
    return value


def load_json(path, limit=65536):
    # Bound the actual read, including when a file changes after stat().
    with Path(path).open("rb") as stream:
        data = stream.read(limit + 1)
    require(len(data) <= limit, "This card file is too large.")
    try:
        return json.loads(data)
    except (ValueError, UnicodeError) as exc:
        raise CardError("This is not a valid Quadband card file.") from exc


def save_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
