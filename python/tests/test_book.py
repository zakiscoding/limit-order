import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from lob.book import Book
from lob.generate import SplitMix64, generate
from lob.hashutil import FNV_OFFSET, FNV_PRIME, MASK, mix
from lob.io_util import read_binary, replay_lines, write_binary
from lob.spec import check_spec

SCENARIOS = ROOT.parent / "testdata" / "scenarios.txt"
EXPECTED = ROOT.parent / "testdata" / "expected.txt"


class HashTests(unittest.TestCase):
    def test_empty_offset(self):
        self.assertEqual(Book().book_hash(), FNV_OFFSET)
        self.assertEqual(Book().trade_hash, FNV_OFFSET)

    def test_mix_is_word_wise_fnv(self):
        h = FNV_OFFSET
        for value in (1, 2, 3):
            h = mix(h, value)
        expect = FNV_OFFSET
        for value in (1, 2, 3):
            expect ^= value
            expect = (expect * FNV_PRIME) & MASK
        self.assertEqual(h, expect)


class SpecTests(unittest.TestCase):
    def test_hand_written_scenarios(self):
        check_spec(SCENARIOS, EXPECTED)


class GeneratorTests(unittest.TestCase):
    def test_same_seed_same_events(self):
        a = generate(42, 1000)
        b = generate(42, 1000)
        self.assertEqual([e.fields() for e in a], [e.fields() for e in b])

    def test_different_seed_differs(self):
        a = generate(1, 100)
        b = generate(2, 100)
        self.assertNotEqual([e.fields() for e in a], [e.fields() for e in b])

    def test_splitmix_known_vector(self):
        rng = SplitMix64(0)
        first = rng.next()
        self.assertEqual(first, 0xE220A8397B1DCDAF)

    def test_binary_roundtrip(self):
        events = generate(7, 200)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "events.bin"
            write_binary(path, 7, events)
            seed, got = read_binary(path)
        self.assertEqual(seed, 7)
        self.assertEqual([e.fields() for e in got], [e.fields() for e in events])


class ReplayTests(unittest.TestCase):
    def test_fingerprint_is_deterministic(self):
        events = generate(99, 2000)
        self.assertEqual(replay_lines(events, 250), replay_lines(events, 250))

    def test_random_stream_stays_consistent(self):
        book = Book()
        for index, event in enumerate(generate(11, 4000), 1):
            book.apply(event)
            if index % 800 == 0:
                book.assert_consistent()
        book.assert_consistent()
        self.assertGreater(book.trade_count, 0)


if __name__ == "__main__":
    unittest.main()
