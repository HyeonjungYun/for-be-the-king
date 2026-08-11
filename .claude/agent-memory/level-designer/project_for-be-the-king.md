---
name: project-for-be-the-king
description: Core spatial/structural facts for the "For be the King" extraction dungeon crawler GDD relevant to level design work
metadata:
  type: project
---

Game concept doc: `design/gdd/game-concept.md` (Status: Draft, created 2026-08-10). Art bible: `design/art/art-bible.md` (9/9 sections locked). Topdown PvPvE extraction crawler, no character leveling (power = held gear only).

Key structural facts as of 2026-08-10 draft:
- 4 floors, target 100 concurrent players per floor (400 total). Each floor = 1 server Room (`층 1개 = Room 1개`) for bandwidth reasons.
- No floor-to-floor traversal inside the dungeon — every floor is entered directly from the base camp hub. This makes floors discrete hub-selected instances, not a continuous/seamless space, despite the Core Identity field labeling the genre "심리스 던전 크롤러" (seamless dungeon crawler).
- Session = personal 30-minute stay timer. House (loot room) and monster respawns are also 30 minutes, timer independent per house.
- Floor archetypes tied to sunken-kingdom lore and loot rarity: 1F 민가/일반, 2F 궁궐 외곽시설/희귀, 3F 낙하한 궁궐 파편/영웅 (art bible flags this as the visually most complex, least modular-reuse-friendly floor), 4F 궁궐 본체/전설.
- Extraction towers sit in open field, "여러 개," positions always visible, with an alert-on-approach mechanic meant to hide exact location/direction. Art bible Section 3③ separately states towers are "어디서든 보임" (landmark visible from anywhere) and the gauge is visualized as glow intensity climbing over the 60s charge — this glow-as-progress-indicator is also visible at range, which risks exposing exactly the location/progress the alert mechanic is designed to hide. Worth re-checking whenever tower placement or the alert mechanic is revisited.
- Open question explicitly owned by level-designer: "층당 집 개수" (houses per floor), deferred to "층 넓이 확정 후" (after floor area is settled) — but no formula/method exists yet to derive floor area from the stated density goal ("동시 가시 인원 15~20명"), and the Risks table separately already commits to a specific number ("집 3~5개") ahead of that open question being resolved — these two numbers are inconsistent and should be reconciled before area/layout work starts.
- MVP scope (Tier 1, due 2026-11-21) is one open-field + one house, tested with only 3 clients — this does not validate the 100-player-per-floor density/pacing premise. The only planned validation for "100명" is a server/network bot-scaling test (DummyClient, 3→8→16→30→100), which proves connection throughput, not level-design pacing/feel. No scope tier currently commits to testing the spatial pacing premise before Tier 3 (2027).

How to apply: before writing any layout diagram, encounter list, or pacing chart for this game, check whether these open questions (floor area/density formula, house count, which floor MVP targets) have since been resolved in the GDD — do not assume the 2026-08-10 draft's numbers are final. See [[feedback_adversarial-review-mode]] for the review approach used to surface these gaps.
