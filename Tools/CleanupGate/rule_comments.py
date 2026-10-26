"""Class 10, comments: a comment that narrates a change (history_comment: a word of history_words.txt, which the change-scrutiny
skill's grep also reads) and a run of more than five full-line comment lines (long_comment: one site per run; a run with a
copyright or SPDX-License-Identifier line is a license notice and does not count)."""
import os
import re

from gate_common import Site, comment_lines

CLASS_ID = "10_comments"
PATTERNS = ["history_comment", "long_comment"]

kHistoryWordsFile = os.path.join(os.path.dirname(os.path.abspath(__file__)), "history_words.txt")
kMaxCommentLines = 5
kLicenseNotice = re.compile(r"\bcopyright\b|SPDX-License-Identifier", re.IGNORECASE)
kHistoryPiece = "a comment about the code as it is now, or no comment"
kLongPiece = "one to three lines: the invariant, the unit, the convention, the trap or the rule"


def history_pattern():
    """Returns one case-insensitive pattern for the words of history_words.txt; the words of a phrase match across any whitespace."""
    with open(kHistoryWordsFile, "r", encoding="utf-8") as file:
        phrases = [line.split() for line in file if line.strip()]
    alternatives = [r"\s+".join(re.escape(word) for word in phrase) for phrase in phrases]
    return re.compile(r"\b(?:" + "|".join(alternatives) + r")\b", re.IGNORECASE)


class Rule:
    def __init__(self, files):
        self.history = history_pattern()

    def scan_file(self, source):
        sites = []
        run = []
        for line_number, comment, code in comment_lines([raw for _, _, raw in source.code]):
            if comment is not None and self.history.search(comment):
                sites.append(Site(source.path, line_number, "history_comment", kHistoryPiece))
            if comment is not None and not code.strip():
                run.append((line_number, comment))
                continue
            sites.extend(self.long_comment_sites(source.path, run))
            run = []
        sites.extend(self.long_comment_sites(source.path, run))
        return sites

    def long_comment_sites(self, path, run):
        if len(run) <= kMaxCommentLines or any(kLicenseNotice.search(comment) for _, comment in run):
            return []
        return [Site(path, run[0][0], "long_comment", kLongPiece)]

    def finish(self):
        return []
