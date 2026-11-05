#!/usr/bin/env python3
"""Generate and verify the community/funding QR codes shipped in the toolbox.

SPDX-License-Identifier: GPL-3.0-or-later

The app draws raw RGBA plates (see ui/src/main.cpp load_rgba), so every code
ships as three files beside the logo: <name>.png (the human-viewable plate),
<name>.rgba (straight RGBA, top-down, exactly what the renderer uploads) and
<name>.txt ("<width> <height>").

Nothing here is traced from a picture: the encoder below implements QR Model 2
byte mode (versions 1-10, error correction level M) and the decoder reads the
finished matrix back the way a scanner would -- format information, unmask,
codeword walk, block de-interleave, Reed-Solomon syndrome check and payload
parse. `--self-test` proves the decoder against a code produced by an
independent third-party generator before it is trusted to judge ours.

usage:
  tools/make-qr-assets.py                # regenerate the three shipped codes
  tools/make-qr-assets.py --verify-only  # decode the shipped assets in place
  tools/make-qr-assets.py --decode FILE  # decode any PNG, print the payload
  tools/make-qr-assets.py --self-test REFERENCE.png
"""

from __future__ import annotations

import argparse
import os
import sys

# ---------------------------------------------------------------- GF(256) ----


def _gf_tables():
    exp = [0] * 512
    log = [0] * 256
    x = 1
    for i in range(255):
        exp[i] = x
        log[x] = i
        x <<= 1
        if x & 0x100:
            x ^= 0x11D
    for i in range(255, 512):
        exp[i] = exp[i - 255]
    return exp, log


GF_EXP, GF_LOG = _gf_tables()


def gf_mul(a: int, b: int) -> int:
    if a == 0 or b == 0:
        return 0
    return GF_EXP[GF_LOG[a] + GF_LOG[b]]


def rs_divisor(degree: int) -> list[int]:
    """Generator polynomial coefficients, highest order term dropped."""
    result = [0] * (degree - 1) + [1]
    root = 1
    for _ in range(degree):
        for j in range(degree):
            result[j] = gf_mul(result[j], root)
            if j + 1 < degree:
                result[j] ^= result[j + 1]
        root = gf_mul(root, 0x02)
    return result


def rs_remainder(data: list[int], degree: int) -> list[int]:
    divisor = rs_divisor(degree)
    result = [0] * degree
    for byte in data:
        factor = byte ^ result.pop(0)
        result.append(0)
        for i, coefficient in enumerate(divisor):
            result[i] ^= gf_mul(coefficient, factor)
    return result


def rs_syndromes(codeword: list[int], ec_len: int) -> list[int]:
    """All-zero syndromes mean the block is a valid RS codeword.

    QR's generator has roots a^0 .. a^(ec_len-1), so the syndromes are
    evaluated at those same powers."""
    out = []
    for root in range(ec_len):
        value = 0
        for byte in codeword:
            value = gf_mul(value, GF_EXP[root]) ^ byte
        out.append(value)
    return out


# ------------------------------------------------------------- constants ----

# level M: (total codewords, ec codewords per block, [(block count, data each)])
BLOCKS_M = {
    1: (26, 10, [(1, 16)]),
    2: (44, 16, [(1, 28)]),
    3: (70, 26, [(1, 44)]),
    4: (100, 18, [(2, 32)]),
    5: (134, 24, [(2, 43)]),
    6: (172, 16, [(4, 27)]),
    7: (196, 18, [(4, 31)]),
    8: (242, 22, [(2, 38), (2, 39)]),
    9: (292, 22, [(3, 36), (2, 37)]),
    10: (346, 26, [(4, 43), (1, 44)]),
}

ALIGNMENT = {
    1: [],
    2: [6, 18],
    3: [6, 22],
    4: [6, 26],
    5: [6, 30],
    6: [6, 34],
    7: [6, 22, 38],
    8: [6, 24, 42],
    9: [6, 26, 46],
    10: [6, 28, 50],
}

# Format information: level bits (L=1, M=0, Q=3, H=2) then the 3-bit mask.
EC_BITS_M = 0


# --------------------------------------------------------------- encoder ----


class Code:
    def __init__(self, version: int):
        self.version = version
        self.size = 17 + 4 * version
        self.modules = [[False] * self.size for _ in range(self.size)]
        self.function = [[False] * self.size for _ in range(self.size)]

    # -- function patterns -------------------------------------------------
    def _set(self, x: int, y: int, dark: bool) -> None:
        self.modules[y][x] = dark
        self.function[y][x] = True

    def _finder(self, cx: int, cy: int) -> None:
        # 7x7 finder plus its one-module separator ring.
        for dy in range(-4, 5):
            for dx in range(-4, 5):
                x, y = cx + dx, cy + dy
                if not (0 <= x < self.size and 0 <= y < self.size):
                    continue
                dist = max(abs(dx), abs(dy))
                self._set(x, y, dist not in (2, 4))

    def _alignment(self, cx: int, cy: int) -> None:
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                self._set(cx + dx, cy + dy, max(abs(dx), abs(dy)) != 1)

    def draw_function_patterns(self) -> None:
        n = self.size
        for i in range(n):
            self._set(6, i, i % 2 == 0)
            self._set(i, 6, i % 2 == 0)
        self._finder(3, 3)
        self._finder(n - 4, 3)
        self._finder(3, n - 4)
        centers = ALIGNMENT[self.version]
        for cy in centers:
            for cx in centers:
                if (cx == 6 and cy == 6) or (cx == 6 and cy == n - 7) or \
                        (cx == n - 7 and cy == 6):
                    continue
                self._alignment(cx, cy)
        # Reserve the format areas; the real bits are written last.
        for i in range(9):
            if i != 6:
                self._set(8, i, False)
                self._set(i, 8, False)
        for i in range(8):
            self._set(8, n - 1 - i, False)
            self._set(n - 1 - i, 8, False)
        self._set(8, n - 8, True)  # dark module

    # -- codewords ---------------------------------------------------------
    def draw_codewords(self, data: list[int]) -> None:
        n = self.size
        index = 0
        total_bits = len(data) * 8
        right = n - 1
        while right >= 1:
            if right == 6:
                right = 5
            for vertical in range(n):
                for j in range(2):
                    x = right - j
                    upward = ((right + 1) & 2) == 0
                    y = n - 1 - vertical if upward else vertical
                    if not self.function[y][x] and index < total_bits:
                        self.modules[y][x] = (
                            (data[index >> 3] >> (7 - (index & 7))) & 1) == 1
                        index += 1
            right -= 2
        if index != total_bits:
            raise ValueError("codeword placement left bits over")

    def apply_mask(self, mask: int) -> None:
        for y in range(self.size):
            for x in range(self.size):
                if self.function[y][x]:
                    continue
                if mask == 0:
                    invert = (x + y) % 2 == 0
                elif mask == 1:
                    invert = y % 2 == 0
                elif mask == 2:
                    invert = x % 3 == 0
                elif mask == 3:
                    invert = (x + y) % 3 == 0
                elif mask == 4:
                    invert = (x // 3 + y // 2) % 2 == 0
                elif mask == 5:
                    invert = (x * y) % 2 + (x * y) % 3 == 0
                elif mask == 6:
                    invert = ((x * y) % 2 + (x * y) % 3) % 2 == 0
                else:
                    invert = ((x + y) % 2 + (x * y) % 3) % 2 == 0
                if invert:
                    self.modules[y][x] = not self.modules[y][x]

    def draw_format(self, mask: int) -> None:
        n = self.size
        data = (EC_BITS_M << 3) | mask
        rem = data
        for _ in range(10):
            rem = (rem << 1) ^ ((rem >> 9) * 0x537)
        bits = ((data << 10) | rem) ^ 0x5412

        def bit(i: int) -> bool:
            return ((bits >> i) & 1) != 0

        for i in range(6):
            self._set(8, i, bit(i))
        self._set(8, 7, bit(6))
        self._set(8, 8, bit(7))
        self._set(7, 8, bit(8))
        for i in range(9, 15):
            self._set(14 - i, 8, bit(i))
        for i in range(8):
            self._set(n - 1 - i, 8, bit(i))
        for i in range(8, 15):
            self._set(8, n - 15 + i, bit(i))
        self._set(8, n - 8, True)

    # -- penalty -----------------------------------------------------------
    def penalty(self) -> int:
        n = self.size
        score = 0
        lines = []
        for y in range(n):
            lines.append([self.modules[y][x] for x in range(n)])
        for x in range(n):
            lines.append([self.modules[y][x] for y in range(n)])
        for line in lines:
            run = 1
            for i in range(1, n):
                if line[i] == line[i - 1]:
                    run += 1
                else:
                    if run >= 5:
                        score += 3 + (run - 5)
                    run = 1
            if run >= 5:
                score += 3 + (run - 5)
            # A finder-like 1:1:3:1:1 run framed by four light modules.
            pattern = [True, False, True, True, True, False, True]
            for i in range(n - 6):
                if line[i:i + 7] == pattern:
                    before = line[max(0, i - 4):i]
                    after = line[i + 7:i + 11]
                    if (len(before) == 4 and not any(before)) or \
                            (len(after) == 4 and not any(after)):
                        score += 40
        dark = 0
        for y in range(n):
            for x in range(n):
                dark += 1 if self.modules[y][x] else 0
        for y in range(n - 1):
            for x in range(n - 1):
                c = self.modules[y][x]
                if c == self.modules[y][x + 1] == self.modules[y + 1][x] == \
                        self.modules[y + 1][x + 1]:
                    score += 3
        total = n * n
        percent = dark * 100 // total
        score += 10 * (abs(percent - 50) // 5)
        return score


def capacity_bytes(version: int) -> int:
    total = BLOCKS_M[version][0]
    data_cw = sum(count * size for count, size in BLOCKS_M[version][2])
    assert data_cw + version_ec_total(version) == total, version
    count_bits = 8 if version <= 9 else 16
    return (data_cw * 8 - 4 - count_bits) // 8


def version_ec_total(version: int) -> int:
    _, ec, groups = BLOCKS_M[version]
    return ec * sum(count for count, _ in groups)


def encode_payload(payload: bytes, version: int | None = None,
                   mask: int | None = None) -> Code:
    if version is None:
        for candidate in sorted(BLOCKS_M):
            if len(payload) <= capacity_bytes(candidate):
                version = candidate
                break
        else:
            raise ValueError("payload does not fit versions 1-10")
    if len(payload) > capacity_bytes(version):
        raise ValueError(f"{len(payload)} bytes exceed version {version}")

    data_cw_total = sum(count * size for count, size in BLOCKS_M[version][2])
    bits: list[int] = []

    def push(value: int, width: int) -> None:
        for i in range(width - 1, -1, -1):
            bits.append((value >> i) & 1)

    push(0b0100, 4)                      # byte mode
    push(len(payload), 8 if version <= 9 else 16)
    for byte in payload:
        push(byte, 8)
    capacity_bits = data_cw_total * 8
    terminator = min(4, capacity_bits - len(bits))
    push(0, terminator)
    while len(bits) % 8:
        bits.append(0)
    codewords = [int("".join(str(b) for b in bits[i:i + 8]), 2)
                 for i in range(0, len(bits), 8)]
    pads = (0xEC, 0x11)
    index = 0
    while len(codewords) < data_cw_total:
        codewords.append(pads[index % 2])
        index += 1

    # Split, add ECC, then interleave exactly as a decoder expects.
    _, ec_len, groups = BLOCKS_M[version]
    blocks: list[list[int]] = []
    ec_blocks: list[list[int]] = []
    offset = 0
    for count, size in groups:
        for _ in range(count):
            block = codewords[offset:offset + size]
            offset += size
            blocks.append(block)
            ec_blocks.append(rs_remainder(block, ec_len))
    interleaved: list[int] = []
    for i in range(max(len(b) for b in blocks)):
        for block in blocks:
            if i < len(block):
                interleaved.append(block[i])
    for i in range(ec_len):
        for block in ec_blocks:
            interleaved.append(block[i])

    best = None
    for candidate_mask in ([mask] if mask is not None else range(8)):
        code = Code(version)
        code.draw_function_patterns()
        code.draw_codewords(interleaved)
        code.apply_mask(candidate_mask)
        code.draw_format(candidate_mask)
        score = code.penalty()
        if best is None or score < best[0]:
            best = (score, candidate_mask, code)
    return best[2]


# --------------------------------------------------------------- decoder ----


def _format_remainder(value: int) -> int:
    """Remainder of a 15-bit format codeword modulo the BCH generator 0x537."""
    for i in range(14, 9, -1):
        if value & (1 << i):
            value ^= 0x537 << (i - 10)
    return value & 0x3FF


def _format_bits(matrix: list[list[bool]], n: int) -> int | None:
    bits = 0
    positions = [(8, i) for i in range(6)] + [(8, 7), (8, 8), (7, 8)] + \
        [(14 - i, 8) for i in range(9, 15)]
    for i, (x, y) in enumerate(positions):
        if matrix[y][x]:
            bits |= 1 << i
    # A second copy sits along the other two finders; accept either.
    mirror = 0
    for i in range(8):
        if matrix[8][n - 1 - i]:
            mirror |= 1 << i
    for i in range(8, 15):
        if matrix[n - 15 + i][8]:
            mirror |= 1 << i
    for candidate in (bits, mirror):
        value = candidate ^ 0x5412
        # 15-bit format codeword: 5 data bits, 10 BCH check bits.
        if _format_remainder(value) == 0:
            return value
    return None


def decode_matrix(matrix: list[list[bool]]) -> tuple[str, dict]:
    n = len(matrix)
    if n != len(matrix[0]) or (n - 17) % 4:
        raise ValueError("not a QR matrix")
    version = (n - 17) // 4
    if version not in BLOCKS_M:
        raise ValueError(f"unsupported version {version}")
    info = _format_bits(matrix, n)
    if info is None:
        raise ValueError("format information does not check out")
    ec_bits = (info >> 13) & 0x3
    mask = (info >> 10) & 0x7
    if ec_bits != EC_BITS_M:
        raise ValueError(f"unexpected error correction level bits {ec_bits}")

    function = Code(version)
    function.draw_function_patterns()

    # Read the zigzag walk, undoing the mask on the way out.
    bits: list[int] = []
    right = n - 1
    while right >= 1:
        if right == 6:
            right = 5
        for vertical in range(n):
            for j in range(2):
                x = right - j
                upward = ((right + 1) & 2) == 0
                y = n - 1 - vertical if upward else vertical
                if function.function[y][x]:
                    continue
                if mask == 0:
                    invert = (x + y) % 2 == 0
                elif mask == 1:
                    invert = y % 2 == 0
                elif mask == 2:
                    invert = x % 3 == 0
                elif mask == 3:
                    invert = (x + y) % 3 == 0
                elif mask == 4:
                    invert = (x // 3 + y // 2) % 2 == 0
                elif mask == 5:
                    invert = (x * y) % 2 + (x * y) % 3 == 0
                elif mask == 6:
                    invert = ((x * y) % 2 + (x * y) % 3) % 2 == 0
                else:
                    invert = ((x + y) % 2 + (x * y) % 3) % 2 == 0
                bit = matrix[y][x] != invert
                bits.append(1 if bit else 0)
        right -= 2

    total = BLOCKS_M[version][0]
    codewords = [int("".join(str(b) for b in bits[i:i + 8]), 2)
                 for i in range(0, total * 8, 8)]

    # De-interleave into the standard block layout.
    _, ec_len, groups = BLOCKS_M[version]
    sizes: list[int] = []
    for count, size in groups:
        sizes.extend([size] * count)
    blocks: list[list[int]] = [[] for _ in sizes]
    position = 0
    for i in range(max(sizes)):
        for index, size in enumerate(sizes):
            if i < size:
                blocks[index].append(codewords[position])
                position += 1
    ec_blocks: list[list[int]] = [[] for _ in sizes]
    for i in range(ec_len):
        for index in range(len(sizes)):
            ec_blocks[index].append(codewords[position])
            position += 1

    for index, block in enumerate(blocks):
        codeword = block + ec_blocks[index]
        # Syndromes over the received block: a valid RS codeword has none.
        if any(rs_syndromes(codeword, ec_len)):
            raise ValueError(f"block {index} fails the Reed-Solomon check")

    stream: list[int] = []
    for block in blocks:
        stream.extend(block)
    text_bits = []
    for byte in stream:
        for i in range(7, -1, -1):
            text_bits.append((byte >> i) & 1)

    def take(width: int) -> int:
        value = 0
        for _ in range(width):
            value = (value << 1) | text_bits.pop(0)
        return value

    mode = take(4)
    if mode != 0b0100:
        raise ValueError(f"expected byte mode, got {mode:04b}")
    length = take(8 if version <= 9 else 16)
    payload = bytes(take(8) for _ in range(length))
    return payload.decode("utf-8"), {"version": version, "mask": mask,
                                     "ec_bits": ec_bits, "size": n}


def matrix_from_image(path: str) -> list[list[bool]]:
    from PIL import Image
    import numpy as np

    image = Image.open(path).convert("L")
    pixels = np.asarray(image)
    dark = pixels < 128
    if not dark.any():
        raise ValueError("image is blank")
    rows = np.where(dark.any(axis=1))[0]
    cols = np.where(dark.any(axis=0))[0]
    y0, y1, x0, x1 = rows[0], rows[-1], cols[0], cols[-1]
    span_x = x1 - x0 + 1
    span_y = y1 - y0 + 1
    for version in sorted(BLOCKS_M):
        n = 17 + 4 * version
        if span_x % n or span_y % n:
            continue
        scale_x = span_x // n
        scale_y = span_y // n
        if scale_x != scale_y or scale_x < 1:
            continue
        matrix = []
        for row in range(n):
            line = []
            for col in range(n):
                px = x0 + col * scale_x
                py = y0 + row * scale_y
                line.append(bool(dark[py, px]))
            matrix.append(line)
        yield version, matrix


def decode_png(path: str) -> tuple[str, dict]:
    last = None
    for version, matrix in matrix_from_image(path):
        try:
            payload, info = decode_matrix(matrix)
            return payload, info
        except ValueError as error:  # try the next plausible module count
            last = error
    raise ValueError(f"could not decode {path}: {last}")


def decode_png_exact(path: str, version: int) -> tuple[str, dict]:
    """Decode assuming one version, so the caller can insist on a module count."""
    for candidate, matrix in matrix_from_image(path):
        if candidate == version:
            return decode_matrix(matrix)
    raise ValueError(f"{path} is not a version {version} code")


# ------------------------------------------------------------------ output ---

QUIET = 4
SCALE = 10  # version 3 (29 modules) + 8 quiet = 37 modules -> 370 px


def write_assets(payload: str, name: str, out_dir: str,
                 version: int = 3) -> dict:
    from PIL import Image
    import numpy as np

    code = encode_payload(payload.encode("utf-8"), version=version)
    n = code.size
    total = n + 2 * QUIET
    canvas = np.full((total * SCALE, total * SCALE), 255, dtype=np.uint8)
    for y in range(n):
        for x in range(n):
            if code.modules[y][x]:
                top = (y + QUIET) * SCALE
                left = (x + QUIET) * SCALE
                canvas[top:top + SCALE, left:left + SCALE] = 0
    rgba = np.dstack([
        canvas, canvas, canvas, np.full_like(canvas, 255)]).astype(np.uint8)
    png_path = os.path.join(out_dir, name + ".png")
    Image.fromarray(canvas, mode="L").convert("RGB").save(png_path, optimize=True)
    with open(os.path.join(out_dir, name + ".rgba"), "wb") as handle:
        handle.write(rgba.tobytes())
    with open(os.path.join(out_dir, name + ".txt"), "w", encoding="ascii") as handle:
        handle.write(f"{rgba.shape[1]} {rgba.shape[0]}\n")

    # Read the files back the way a scanner would, from the shipped pixels.
    decoded, info = decode_png(png_path)
    if decoded != payload:
        raise SystemExit(f"{name}: decoded {decoded!r} != {payload!r}")
    shipped = np.fromfile(os.path.join(out_dir, name + ".rgba"), dtype=np.uint8)
    if shipped.size != rgba.size or not np.array_equal(shipped, rgba.ravel()):
        raise SystemExit(f"{name}: .rgba does not match the verified matrix")
    return {"name": name, "payload": payload, "png": png_path,
            "size": rgba.shape[1], **info}


CARDS = [
    ("qr-discord", "https://discord.com/invite/kcgQPxhde"),
    ("qr-btc", "bc1qa9zfgnccajsw8vg7k287qz5a7apf8pefj5jjx5"),
    ("qr-patreon", "patreon.com/c/u4772250"),
]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "ui", "assets", "ghostcontrol"))
    parser.add_argument("--verify-only", action="store_true",
                        help="decode the shipped PNGs without rewriting them")
    parser.add_argument("--decode", metavar="PNG",
                        help="decode any PNG and print its payload")
    parser.add_argument("--self-test", metavar="PNG",
                        help="decode a third-party code to prove the decoder")
    args = parser.parse_args(argv)

    if args.decode:
        payload, info = decode_png(args.decode)
        print(f"{args.decode}: {payload!r} {info}")
        return 0

    if args.self_test:
        payload, info = decode_png(args.self_test)
        print(f"decoder self-test on {args.self_test}: {payload!r} {info}")
        return 0

    if args.verify_only:
        failed = 0
        for name, payload in CARDS:
            path = os.path.join(args.out_dir, name + ".png")
            try:
                decoded, info = decode_png(path)
            except (ValueError, OSError) as error:
                print(f"FAIL {name}: {error}")
                failed += 1
                continue
            expected_size = (17 + 4 * info["version"] + 2 * QUIET) * SCALE
            meta = open(os.path.join(args.out_dir, name + ".txt"),
                        encoding="ascii").read().split()
            ok = decoded == payload and meta == [str(expected_size)] * 2
            print(f"{'PASS' if ok else 'FAIL'} {name}: {decoded!r} "
                  f"v{info['version']} mask {info['mask']} {expected_size}px")
            failed += 0 if ok else 1
        return 1 if failed else 0

    results = []
    for name, payload in CARDS:
        results.append(write_assets(payload, name, args.out_dir))
    for result in results:
        print(f"wrote {result['name']}: {result['size']}x{result['size']} "
              f"v{result['version']} mask {result['mask']} -> "
              f"{result['payload']} (decoded back and compared)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
