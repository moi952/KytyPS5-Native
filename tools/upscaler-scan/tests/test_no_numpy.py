"""The byte-pattern indexes must give the same result with and without numpy.

The packaged Python has no numpy. Beta 4 shipped a scanner whose `rip_lea_refs` imported it unconditionally, so every
game on the FidelityFX FSR 3 route (Ghost of Yotei) failed to scan there. No game files are needed.
"""
import importlib.util
import random
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from kyty_upscaler_scan import selfelf  # noqa: E402

VA = 0x10000


def _code(seed: int, size: int = 1 << 16) -> bytes:
    """Random bytes seeded with the patterns of interest, including at the very end of the segment."""
    rng = random.Random(seed)
    buf = bytearray(rng.randbytes(size))
    for _ in range(400):
        at = rng.randrange(0, size - 8)
        kind = rng.randrange(4)
        rel = rng.randrange(-size, size)
        if kind == 0:  # lea r64, [rip+disp32]
            buf[at:at + 7] = bytes([rng.choice((0x48, 0x4C)), 0x8D, rng.choice(range(0x05, 0x40, 8))]) + struct.pack('<i', rel)
        elif kind == 1:  # call/jmp rel32
            buf[at:at + 5] = bytes([rng.choice((0xE8, 0xE9))]) + struct.pack('<i', rel)
        elif kind == 2:  # jcc rel32
            buf[at:at + 6] = bytes([0x0F, 0x80 | rng.randrange(16)]) + struct.pack('<i', rel)
        else:  # adjacent, overlapping-looking prefixes
            buf[at:at + 4] = bytes([0x48, 0x48, 0x8D, 0x05])
    buf[-7:] = bytes([0x48, 0x8D, 0x05]) + struct.pack('<i', -64)   # the last position that still fits
    buf[-3:] = bytes([0x48, 0x8D, 0x05])                            # ... and one that does not (overwrites the tail)
    return bytes(buf)


class Image(selfelf.SelfImage):
    def __init__(self, code: bytes):
        seg = selfelf.Segment(0, (selfelf.PT_LOAD, 5, 0, VA, 0, len(code), len(code), 0x10), code)
        self.segments = self.loaded = [seg]
        self._starts = [VA]
        self._relocated = {}
        self.relative = {}


def _reference_lea(code: bytes) -> dict:
    index = {}
    for at in range(len(code) - 6):
        if code[at] in (0x48, 0x4C) and code[at + 1] == 0x8D and code[at + 2] & 0xC7 == 0x05:
            index.setdefault(VA + at + 7 + struct.unpack_from('<i', code, at + 3)[0], []).append(VA + at)
    return index


def _reference_rel32(code: bytes) -> dict:
    index = {}
    for at in range(len(code) - 4):
        if code[at] in (0xE8, 0xE9):
            index.setdefault(VA + at + 5 + struct.unpack_from('<i', code, at + 1)[0], []).append((VA + at, code[at]))
    for at in range(len(code) - 5):
        if code[at] == 0x0F and code[at + 1] & 0xF0 == 0x80:
            index.setdefault(VA + at + 6 + struct.unpack_from('<i', code, at + 2)[0], []).append((VA + at, 0x0F))
    return index


class WithoutNumpy(unittest.TestCase):
    def setUp(self):
        self._saved = sys.modules.get('numpy', self)
        sys.modules['numpy'] = None  # `import numpy` now raises ImportError, as in the packaged Python

    def tearDown(self):
        if self._saved is self:
            del sys.modules['numpy']
        else:
            sys.modules['numpy'] = self._saved

    def test_rip_lea_refs(self):
        for seed in range(4):
            code = _code(seed)
            img, want = Image(code), _reference_lea(code)
            self.assertTrue(want)
            for target, sites in want.items():
                self.assertEqual(img.rip_lea_refs(target), sorted(sites))
            self.assertEqual({t: sorted(s) for t, s in img._lea_index.items()}, {t: sorted(s) for t, s in want.items()})

    def test_rel32_callers(self):
        for seed in range(4):
            code = _code(seed)
            img, want = Image(code), _reference_rel32(code)
            lo, hi = VA, VA + len(code)
            for target, sites in want.items():
                if lo <= target < hi:  # the numpy path keeps only targets inside the image; lookups never ask for others
                    self.assertEqual(img.rel32_callers(target), sorted(p for p, _ in sites))


@unittest.skipUnless(importlib.util.find_spec('numpy'), 'numpy not installed: nothing to compare with')
class NumpyAgrees(unittest.TestCase):
    def test_same_indexes(self):
        for seed in range(4):
            code = _code(seed)
            img = Image(code)
            want = _reference_lea(code)
            for target, sites in want.items():
                self.assertEqual(img.rip_lea_refs(target), sorted(sites))
            lo, hi = VA, VA + len(code)
            for target, sites in _reference_rel32(code).items():
                if lo <= target < hi:
                    self.assertEqual(img.rel32_callers(target), sorted(p for p, _ in sites))


if __name__ == '__main__':
    unittest.main()
