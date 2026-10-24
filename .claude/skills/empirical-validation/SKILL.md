---
name: empirical-validation
description: Method for answering an open quantitative question that gates a decision — pre-register the verdict rule, verify the instrument, sample the risky cases, keep a control group, report the tail. Use when a ship/hold decision depends on a number nobody has measured yet, rather than on a claim to check or a cause to find.
---

# Empirical validation

Distinct from the two neighbouring methods. `root-cause-debugging` finds a cause. `adversarial-review` checks whether someone's claim survives reproduction. This one answers **an open quantitative question that gates a decision** — is this margin sufficient, is this fast enough, does this change what it must not — where the honest answer today is "nobody has measured it". No amount of review substitutes: a reviewer of any capability meets the same missing number.

1. **Pre-register the verdict rule.** Before measuring, write down what result ships and what result blocks, and put it somewhere you cannot quietly edit afterwards (the brief, the report header, the tracking doc). A threshold chosen after seeing the data is rationalization wearing a lab coat. If you cannot state what would refute the change, you are not validating it — you are looking for reassurance.
2. **Verify the instrument before the reading.** Confirm you are measuring the path that actually runs in production, not a convenient proxy — the same transcode target, the same build config, the same pass, the same code path. Inject a known value and watch it appear. A number from the wrong path is worse than no number, because it is persuasive.
3. **Sample for risk, not for convenience.** Real content over synthetic. Deliberately include the cases *near the decision boundary* — a corpus of comfortable cases tells you nothing about a margin. Report how many cases were anywhere near the threshold at all; if none were, say so, because then the measurement did not test the thing you claimed it tested.
4. **Keep a control group.** Measure what should *not* change alongside what should. Without it, a clean result cannot be distinguished from having measured nothing — and false positives (the change firing where it must not) are usually the dangerous direction, not false negatives.
5. **Establish the noise floor first**, by measuring the same thing twice with nothing changed. Any effect smaller than that spread is not an effect. State the floor next to every result.
6. **Report the distribution, and the tail.** For a safety margin the worst case decides, so lead with max and the tail; a mean hides exactly the case that breaks you. For performance, median plus spread, never a single run.
7. **One variable at a time**, and re-verify the instrument if the harness changed between runs.
8. **A refuting result is a success.** Report it as plainly and as prominently as a confirming one. Do not re-scope the question, widen the population, or soften the threshold to rescue the change — if you find yourself adjusting the rule after seeing the data, stop and say that instead.
9. **Record it so it is comparable later.** Measurements that cannot be compared across changes decay into anecdotes; the record is what makes a later audit cheap.

## Record template

```
## Measurement: <the question, stated as a question>
Decision it gates:      <what ships or holds on this>
Pre-registered rule:    ships if <…>; blocks if <…>          (written BEFORE measuring)
Instrument:             <what measured it> — verified by <how you proved it sees the real path>
Population:             <what, how many, why representative; how many near the boundary>
Control:                <what should not change> — observed <did it>
Noise floor:            <method> = <value>
Result:                 median <…> / max <…> / tail <…>
Verdict:                <against the pre-registered rule, not against hope>
Residual:               <what remains unmeasured, and why it was acceptable to stop>
```
