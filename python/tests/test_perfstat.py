"""Tests for lob.perfstat.

Every number below is artificial (round values chosen so the arithmetic can be
checked by hand). They test parsing and subtraction, not performance.
"""

import unittest

from lob import perfstat


class ParseTests(unittest.TestCase):
    def test_plain_and_modifier_names(self):
        text = "1000,,cycles:u,500,100.00,,\n2000,,instructions:u,500,100.00,,\n"
        self.assertEqual(perfstat.parse_stat_csv(text), {"cycles": 1000.0, "instructions": 2000.0})

    def test_hybrid_names_are_summed_and_not_counted_ignored(self):
        text = (
            "700,,cpu_core/cycles/u,500,100.00,,\n"
            "<not counted>,,cpu_atom/cycles/u,0,0.00,,\n"
        )
        self.assertEqual(perfstat.parse_stat_csv(text), {"cycles": 700.0})

    def test_unsupported_is_none(self):
        text = "<not supported>,,cache-misses:u,0,100.00,,\n"
        self.assertEqual(perfstat.parse_stat_csv(text), {"cache-misses": None})

    def test_comments_and_blank_lines_skipped(self):
        self.assertEqual(perfstat.parse_stat_csv("# started\n\n5,,page-faults,1,100.00,,\n"), {"page-faults": 5.0})

    def test_hyphenated_names_keep_their_hyphens(self):
        self.assertEqual(perfstat.normalize_event("L1-dcache-load-misses:u"), "L1-dcache-load-misses")
        self.assertEqual(perfstat.normalize_event("cpu_core/branch-misses/u"), "branch-misses")
        self.assertEqual(perfstat.normalize_event("cpu_core/cycles:u/"), "cycles")


class AggregateTests(unittest.TestCase):
    def runs(self, **events):
        return [dict(events)]

    def test_setup_is_subtracted_and_ratios_follow(self):
        base = {"core": [{"cycles": 100.0, "instructions": 100.0, "branches": 10.0, "branch-misses": 1.0}] * 3}
        loop = {
            "core": [
                {"cycles": 1100.0, "instructions": 2100.0, "branches": 410.0, "branch-misses": 21.0},
                {"cycles": 1100.0, "instructions": 2100.0, "branches": 410.0, "branch-misses": 21.0},
                {"cycles": 1100.0, "instructions": 2100.0, "branches": 410.0, "branch-misses": 21.0},
            ]
        }
        totals, ratios = perfstat.aggregate(base, loop)
        self.assertEqual(totals["cycles"][0], 1000.0)
        self.assertEqual(totals["instructions"][0], 2000.0)
        self.assertEqual(ratios["instructions per cycle"][0], 2.0)
        self.assertEqual(ratios["branch miss rate"][0], 20.0 / 400.0)

    def test_event_missing_in_one_run_is_dropped(self):
        base = {"cache": [{"cache-misses": 1.0, "cache-references": 2.0}]}
        loop = {"cache": [{"cache-misses": None, "cache-references": 12.0}]}
        totals, ratios = perfstat.aggregate(base, loop)
        self.assertNotIn("cache-misses", totals)
        self.assertIn("cache-references", totals)
        self.assertEqual(ratios, {})

    def test_spread_reports_min_and_max(self):
        base = {"software": [{"page-faults": 10.0}]}
        loop = {"software": [{"page-faults": 110.0}, {"page-faults": 130.0}, {"page-faults": 120.0}]}
        totals, _ = perfstat.aggregate(base, loop)
        self.assertEqual(totals["page-faults"], (110.0, 100.0, 120.0))


class RenderTests(unittest.TestCase):
    def test_per_event_division(self):
        meta = {"events": "1000", "passes": "2", "repeats": "3", "seed": "42"}
        totals = {"cycles": (6000.0, 5000.0, 7000.0)}
        text = perfstat.render(meta, totals, {}, ["cache-misses"], None)
        self.assertIn("| cycles | 3 [2.5..3.5] | 6,000 |", text)
        self.assertIn("cache-misses", text)  # listed as not measured

    def test_alloc_line_parse(self):
        line = "APPLY_LOOP seed=42 events=10 passes=1 new_calls=7 delete_calls=3\n"
        self.assertEqual(perfstat.parse_apply_loop_line(line)["new_calls"], "7")


if __name__ == "__main__":
    unittest.main()
