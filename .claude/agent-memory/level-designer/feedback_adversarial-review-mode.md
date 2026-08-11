---
name: feedback-adversarial-review-mode
description: When explicitly asked for an "adversarial review" of a design doc, skip the collaborative question-first workflow and go straight to blunt, prioritized findings
metadata:
  type: feedback
---

When a task frames the work as an adversarial/red-team review of an existing document ("적대적 리뷰", "문제를 찾으세요", "이 설계를 검증하지 마세요") — as opposed to the normal collaborative design-partner mode — do not run the usual Question-First Workflow (clarifying questions, then options with AskUserQuestion) before responding. Read the target document(s) directly and return findings as prioritized, specific criticism in plain markdown text.

**Why:** This mode is typically invoked by an orchestrating skill (e.g. a `/design-review`-style command) spawning this agent as a subagent with no prior conversation context ("이 대화의 맥락이 없으니, 지금부터 직접 읽고 판단하세요"). The point of the invocation is a critique deliverable, not a negotiated design session — asking clarifying questions back would stall the pipeline the orchestrator is running.

**How to apply:**
- Treat explicit checklists in the task prompt as the required coverage, but feel free to add issues found by cross-referencing multiple docs (e.g. GDD vs art bible) even if not explicitly asked.
- Prioritize: internal contradictions within one doc, contradictions across docs, and circular/unresolved dependency chains rank above generic "this could be more specific" notes.
- Quote or cite the specific line/section making the claim so the user can verify without re-reading the whole doc.
- Do not soften findings into diplomatic hedging — the value of this mode is surfacing what a normal collaborative pass would gloss over.
- Still follow the "no report files" rule — return findings as the final chat message, not as a written file, unless the task explicitly asks for a file.
- This does not override standing rules like "ask before Write/Edit" — it only applies to the review/critique itself, not to any follow-up work that would modify project files.
