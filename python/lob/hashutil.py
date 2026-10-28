"""Shared 64-bit fingerprint.

This is a word-wise FNV-style mix, not cryptographic. Both engines must apply
the same operations in the same order. Values are mixed as unsigned 64-bit
integers, little-endian in spirit: the full word is xored, then multiplied.
"""

MASK = (1 << 64) - 1
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211


def mix(h: int, value: int) -> int:
    h ^= value & MASK
    h = (h * FNV_PRIME) & MASK
    return h
