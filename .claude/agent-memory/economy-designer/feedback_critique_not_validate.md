---
name: feedback-critique-not-validate
description: User wants adversarial, problem-finding critique of a design as delivered — not validation or a balanced pros/cons writeup
metadata:
  type: feedback
---

When asked to review a design doc (e.g., DungeonKing's game-concept.md),
the user explicitly frames the task as "find problems," not "validate this
design." Deliverables should be a ranked, critical findings list — most
economy-breaking issue first — each a short title + a specific, concrete
problem statement (name the exact mechanic, the exact exploit/failure
mode, and why it breaks under rational player behavior). Avoid hedging
into "this could be fine if tuned well" language as the primary framing;
state the structural risk plainly, and only append tuning caveats at the
end.

**Why:** The task instructions were explicit ("Your job is NOT to
validate this design") and the request asked for specific, critical
findings tied to named mechanics (full-loss-on-death, zone caps, Guild
market, cooperation multiplier, progression reset) rather than generic
economy theory.

**How to apply:** Default to this adversarial framing whenever the user
hands over a concept/GDD for economy review, unless they explicitly ask
for a balanced pros/cons or validation pass instead. See
[[project-dungeonking-concept]] for the specific project this applies to.
