"""Tests the rules of 10_comments over the fixtures in Fixtures/: the line of every site each pattern counts."""
import os
import sys
import unittest

sys.dont_write_bytecode = True
kGateFolder = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, kGateFolder)

import gate_common  # noqa: E402
import rule_comments  # noqa: E402


def site_lines(fixture, pattern):
    source = gate_common.load_source(os.path.join(kGateFolder, "Fixtures"), fixture)
    return [site.line for site in rule_comments.Rule([]).scan_file(source) if site.pattern == pattern]


class CommentRuleTests(unittest.TestCase):
    def test_history_comment_counts_comment_lines_with_a_history_word(self):
        self.assertEqual(site_lines("history_comment.cpp", "history_comment"), [5, 6, 7, 9, 11, 12, 13, 14, 15])

    def test_long_comment_counts_runs_over_five_lines_once(self):
        self.assertEqual(site_lines("long_comment.cpp", "long_comment"), [9, 22])


if __name__ == "__main__":
    unittest.main()
