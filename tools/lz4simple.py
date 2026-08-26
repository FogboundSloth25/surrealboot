#!/usr/bin/env python3

"""
Tiny dependency-free LZ4 block compressor.

Produces a raw LZ4 block suitable for the matching decoder
used by surreal_boot.c.

This is intentionally simple:
- no frame format
- no external dependencies
- greedy match search
- maximum match distance: 65535
"""

WINDOW = 65535
MIN_MATCH = 4


def _emit_len(out: bytearray, length: int) -> None:
    while length >= 255:
        out.append(255)
        length -= 255

    out.append(length)


def _emit_sequence(
    out: bytearray,
    literals: bytes,
    offset: int | None,
    match_length: int,
) -> None:
    lit_len = len(literals)

    token_lit = min(lit_len, 15)
    token_match = min(max(match_length - 4, 0), 15)

    out.append((token_lit << 4) | token_match)

    if lit_len >= 15:
        _emit_len(out, lit_len - 15)

    out.extend(literals)

    if offset is None:
        return

    out.append(offset & 0xFF)
    out.append((offset >> 8) & 0xFF)

    if match_length - 4 >= 15:
        _emit_len(out, match_length - 4 - 15)


def compress(data: bytes) -> bytes:
    if not data:
        return bytes([0])

    out = bytearray()

    pos = 0
    anchor = 0
    size = len(data)

    # Simple hash table.
    table: dict[int, int] = {}

    def hash4(p: int) -> int:
        x = (
            data[p]
            | (data[p + 1] << 8)
            | (data[p + 2] << 16)
            | (data[p + 3] << 24)
        )

        return ((x * 2654435761) >> 16) & 0xFFFF

    while pos + MIN_MATCH <= size:

        h = hash4(pos)
        previous = table.get(h)
        table[h] = pos

        if previous is None:
            pos += 1
            continue

        distance = pos - previous

        if distance > WINDOW:
            pos += 1
            continue

        if data[previous:previous + 4] != data[pos:pos + 4]:
            pos += 1
            continue

        match_len = MIN_MATCH

        max_len = size - pos

        while (
            match_len < max_len
            and data[previous + match_len] == data[pos + match_len]
        ):
            match_len += 1

        if match_len < MIN_MATCH:
            pos += 1
            continue

        literals = data[anchor:pos]

        _emit_sequence(
            out,
            literals,
            distance,
            match_len,
        )

        end = pos + match_len

        # Add positions covered by the match to improve subsequent matches.
        p = pos + 1

        while p + MIN_MATCH <= end:
            table[hash4(p)] = p
            p += 1

        pos = end
        anchor = pos

    # Final literal-only sequence.
    literals = data[anchor:]

    if literals:
        _emit_sequence(
            out,
            literals,
            None,
            0,
        )

    return bytes(out)
