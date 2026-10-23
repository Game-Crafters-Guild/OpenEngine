---
name: trace-the-chain
description: Method for reliably answering "is X enabled / what is the default / did we fix Y" questions — enumerate every layer of the chain before concluding, date it with git, and present the table before the verdict. Use for audits, investigations, and any question whose answer you might otherwise have to reverse.
---

# Trace the chain

"What is the default", "is X on", and "did we fix Y" are almost never one value — they are a chain. Each layer read alone yields a confident, defensible, wrong answer; two agents can reach opposite conclusions citing real evidence from different layers. The method:

1. **Enumerate the layers first**, before reading any of them. Typical shape: type/component initializer → is it present or attached by default → the baseline when it is absent → the conditions under which something writes it → downstream consumer defaults → environment/CLI overrides. Write the empty table.
2. **Fill every cell with file:line evidence.** A cell you can't fill is "unknown", not "probably".
3. **Date the chain.** `git log` the relevant files — a fix may have landed *the same day* as the census or report you're reading. Audits lag fixes. State your answer as "as of commit X".
4. **Ask what else would produce the same evidence** before committing to a cause, and rule those alternatives out explicitly.
5. **Present the table, then the verdict.** Never the verdict alone — the table is what lets the next reader (or agent) check you.
6. **A changed answer is a stop signal.** If you find yourself reversing a conclusion on new evidence, you never had the whole chain — go back to step 1 and complete the enumeration. Do not report the newest sample as the answer.
7. **Reports and comments are claims.** Reproduce what you build on; verify comments against the code they describe, and note where they differ.

## Worked example: "is this rule / skill / agent / setting in effect?"

Tooling and configuration are chains too, and this one is mistaken for a single value constantly. The layers: the file was written → it is in the directory the *consumer* reads (not merely the one you edited) → that path exists on the branch or worktree the consuming session has checked out → it is not excluded by ignore rules → the registry has rescanned since it appeared → it is invocable by name. "I committed it" covers at most the middle of that chain, and every layer read alone gives a confident wrong answer: the artifact is genuinely in the repo, genuinely on main, and still invisible to the session that needs it. Prove the last layer by exercising it — invoke the skill, spawn the agent type, read the file from the consumer's path. A lane that silently never loaded your contract is indistinguishable from one that ignored it.
