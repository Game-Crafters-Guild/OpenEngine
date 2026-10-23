---
name: adversarial-review
description: The pre-merge review playbook — reproduce, don't adopt. Use for any branch review; defines the minimum attack set, the verdict format, and the honesty requirements for what was and wasn't reproduced.
---

# Adversarial review playbook

Your job is to try to make the change fail, not to confirm it works. The author's report is a list of claims; reproduce every claim you rely on.

## Minimum attack set

1. **Merge reality.** Fetch, then `merge-tree` against the LIVE tip (it moves during long reviews — re-check before verdict). Check file overlap with recent history for *semantic* conflicts, not just textual ones.
2. **Gates you can trust.** Reconcile the lane's gtest XML against the head SHA (declared = ran + disabled, every skip at its site). Run a suite yourself only when the XML is missing, predates the head, or one of your mutants contradicts it; when you do, delete the executable first, rebuild, and judge freshness by exit code and the binary's contents, never by mtime alone. The full mapped set is not a per-pull-request gate: it is the pre-release validation and the daily run on main (`AGENTS.md`, Test). Beware false greens: a test runner whose filter matches nothing can exit 0 — verify the suite ran by its test counts.
3. **Red-green.** Reproduce the red state for new tests with your own red-capable mutant scoped to the test (revert only the piece the test guards, keep the tests, observe the failure); a whole-change revert is the last resort. A test that was never red is a claim, not a proof. Check for tautologies — a control assertion that stays green in the red build.
4. **Hostile inputs.** Anything parsing external or untrusted bytes gets crafted-input attacks — overflowed length fields, truncation at every structural boundary, declared-size bombs, wrong-type fields — run under a watchdog. A hang is a finding, not a timeout.
5. **Threading and lifetime.** For concurrency changes: who owns destruction; what happens on the cancel, shutdown, and error paths; can a slot or lock leak or double-release; does any worker ever wait on work scheduled to its own pool; does anything publish state outside the lock its reader holds.
6. **Stale-context sweep.** Grep for comments and docs the diff falsifies. A correct fix sitting next to a contradicting comment is a future bug — require the comment fixed in the same change.
7. **Excuse verification.** When the author skipped something ("no test target exists", "not reproducible"), verify the excuse against the build system or the repro before accepting it.
8. **Deletions review.** Every removed line justified; no dead code left behind; no defense removed without an equivalent replacement.
9. **Evidence audit.** Claims must be tiered verified/inferred/assumed; measurements must exist as artifacts — spot-check that cited artifacts exist and say what the report says they say.

## Verdict

MERGE-READY or NOT-READY, with numbered findings tagged [BLOCKER] / [SHOULD-FIX] / [NIT], each with file:line and a minimal fix. State explicitly: what you reproduced vs reviewed statically, and what you deliberately skipped and why. If your verdict depends on something you did not reproduce, say so in the verdict line itself.

## The review owns the gap, the class, and the stack

- **Author-flagged gaps: execute, don't waive.** When the producing lane flags an unverified gap as disproportionate for its scope — a real packaged-build red/green, a real end-to-end pipeline run — the review with merge authority closes it live: run the thing, then verdict. Waiving the gap converts "unverified" into "merged unverified"; bouncing the branch back converts it into a queue delay for work only the reviewer can gate anyway. The author naming the gap honestly is the system working; the review executing it is what keeps that honesty cheap.
- **Escapes are first-class outputs.** A review may file an escape against an <em>earlier</em> merge it was not asked to review — recorded in the ledger's escapes column and routed to a fix lane. Finding one mid-review is the spot-audit working, not scope creep; suppressing it because "that branch already merged" is how escaped defects become permanent.
- **Two same-class defects mean the class is unswept.** When a second defect of the same class surfaces ad hoc, the review's brief expands to enumerating the full class mechanically — e.g. diff a binary's complete import table against the packaging manifest, rather than chasing the two libraries that happened to fail. Instances found ad hoc are a smell that no sweep ever ran; the enumeration is usually one command.
- **Stacked follow-ups are one review.** Same-file follow-up fixes stack on the pending branch rather than branching independently off the mainline (independent branches over the same hunks guarantee textual conflict). The review takes the whole stack, with a bounded timeout — past it, review the base alone and let the tail re-queue.

## The ledger row is part of the review

Before reporting, add your row as its own file under `GameEngine-Workbench/review-tier-log/` (the workbench checkout, found through `GE_WORKBENCH`; `<YYYY-MM-DD>-<slug>.md`, one row: date · branch · class · reviewer tier · verdict · catches B/SF/N · escapes · notes; format in its README). **A review that has not logged its row is not finished** — the log is the artifact other sessions trust; on 2026-07-27 it had zero rows for twelve reviewed-or-in-review branches and testified falsely in both directions (reviews looked absent that had happened; "awaiting review" piles hid branches already reviewed). If the review dies before logging, whoever salvages it logs the row with a `salvaged` note. Do not leave logging to the coordinator: coordinator memory is the mechanism that already failed.
