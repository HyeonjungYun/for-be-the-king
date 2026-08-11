---
name: project-dungeonking-concept
description: Core economy-relevant facts and open questions from the CURRENT concept doc ("For be the King", 2026-08-10) — supersedes the old DungeonKing v2 level/zone model
metadata:
  type: project
---

**Superseded note (2026-08-11):** The concept doc was fully rewritten. Working
title is now **"For be the King"** (가제), dated 2026-08-10, at
`design/gdd/game-concept.md`. It **supersedes** the old DungeonKing v2 doc
archived at `design/archive/dungeonking-2026-07/game-concept.md`. The old
model below no longer applies and should not be reused:
- ~~Character level 1-18, 4 level-banded zones~~ → replaced by **no character
  level at all**; power comes only from equipped gear.
- ~~"Traitor" flag / Paladin counter~~ → replaced by **no penalty for
  betrayal at all** (Pillar 3), enforcement is purely player-vs-player.

## Current model (top-down PvPvE extraction, 100 players/floor x 4 floors)

- **No character level.** Power = equipped gear only. Two gear stats:
  **Item Level** (grinds up via an in-game currency, resets to 1 on
  forced transfer) and **Item Grade** (fixed rarity tier, survives
  drop/theft, never resets).
- **Tier** = sum of equipped Item Levels. Tier gates which of the 4 floors
  you may enter (own tier or below = free; above = blocked, no lower
  bound). Party size is capped by tier gap (a tier-4 character gets 0
  party slots on floor 1 — must solo).
- **Death**: drop everything (equipped + carried) as a lootable bag for
  10 minutes, then it despawns (destroyed) if unlooted. Character itself
  is "rescued" by the Guild (not permadeath) for a **gold rescue fee**
  (named gold sink #1).
- **Item Level resets to 1 on "드랍" (drop/steal)** — i.e., on forced
  ownership transfer (death-loot or theft). The doc's own wording pairs
  "드랍" with "강탈" (theft), not with voluntary storage. **It does NOT
  explicitly say whether extracting to or withdrawing from the player's
  own basecamp warehouse also triggers this reset.**
- **Warehouse (창고)** is the stated long-term progression axis (explicitly
  named as the replacement for character leveling) — items extracted
  there are permanently safe. **No capacity cap or decay is specified
  anywhere in the doc.**
- Floor loot pools are tiered: floor 1 = consumables + "enhancement
  currency" (재화), floor 2 = basic gear, floor 3 = rare gear, floor 4 =
  epic/legendary-equivalent gear only.
- MVP (13 weeks, target 2026-11-21) explicitly excludes ALL economy
  content: no floors 2-4, no tier gating, no enhancement, no party, no
  economy system at all. Single floor + single house only.

## Open economy questions raised in 2026-08-11 adversarial review

These are the load-bearing unknowns for any future balance pass — check
whether a newer doc version has resolved them before doing numeric work:

1. **Highest priority / most economy-breaking**: does the Item Level
   reset apply to voluntary warehouse extraction/withdrawal, or only to
   forced death/theft transfer? If only the latter (current text implies
   this), a player can stockpile pre-leveled, top-grade gear in an
   uncapped personal warehouse and swap in a fresh maxed loadout after
   every death — silently defeating both the stated "스노우볼 억제" purpose
   of the level-reset AND the "인구 자동 순환" claim that death demotes a
   player's tier. Veterans with banked spares would never actually get
   demoted in practice.
2. Warehouse has no stated capacity cap or decay — unbounded accumulator
   (violates basic sink/faucet health regardless of question 1).
3. Two named currencies/sinks — gold (구출 수수료) and "재화" (강화) — have
   **zero explicit faucet**. The only hinted source is one lore-prose
   line ("길드가... 전리품을 매입한다"), never connected to the system-rules
   section. Unclear if gold and 재화 are even the same currency.
4. Unclear if currency is a safe wallet balance or a lootable/carried
   item (Tarkov-style roubles-in-inventory, which this game explicitly
   cites as a reference). If the latter, a player who just died may have
   zero gold on hand to pay their own rescue fee — a possible deadlock
   in the stated mechanic.
5. "인구 자동 순환" (population self-balances via death-driven tier
   demotion) has no numeric target anywhere: no target death rate, no
   expected time-in-tier, no population distribution target across the 4
   floors. Cannot verify the "100/floor x 4" concurrency goal is
   achievable; risk of dead middle floors (2-3) if progression outpaces
   demotion.
6. Floor-1-monopolization-by-geared-solo-veteran is already an Open
   Question in the doc itself (owned by economy-designer, timing "Tier
   3"), but the doc's own proposed self-correction (death → demotion,
   peer-gank counter-play) is structurally weakest exactly in this
   scenario, because an overgeared player farming the weakest floor has
   an abnormally low death rate. This needs a structural answer (e.g.
   gear power normalization by floor, a floor-access ceiling not just a
   floor-access floor), not just drop-rate tuning. Correctly out of MVP
   scope since MVP has no floors/tiers at all yet.
7. The 10-minute unlooted-bag-decay is the only sink that destroys the
   underlying item object (grade-carrying template), as opposed to just
   resetting its level. It has to simultaneously serve two goals that
   pull in opposite directions: (a) the "잃은 것 되찾기" retention hook
   wants high recirculation / low decay, (b) long-term grade-item supply
   control (floor 4 keeps minting new grade-S drops every ~30 min
   respawn, with no other destruction sink) wants meaningful decay. The
   doc never states which goal wins if they conflict.
8. Scope: Tier 3 / Full Vision includes a player-facing "길드 마켓" and an
   uncapped "베이스캠프 창고," which is ambitious for a solo dev with a
   ~1,990,000 KRW **art-only** budget who is already carrying unproven
   100-concurrent-per-floor netcode risk. Recommended fallback if scope
   pressure hits: NPC fixed/dynamic-price vendor buy-sell loop instead of
   a true player-to-player market.

**Why:** This is the primary reference for any future economy-design pass
(loot tables, floor loot pools, tier/warehouse balance, gold/재화
faucet-sink modeling) on this project. The concept doc is still
pre-numeric (no drop rates, no prices, no XP/itemLevel curve, no death
rate targets), so early econ modeling stays structural/directional until
these gaps are filled.

**How to apply:** Before doing further economy work, re-read
`design/gdd/game-concept.md` in full (it changes fast — this is the
second full rewrite in a month) and check whether questions 1-8 above
have been answered in a newer version before trusting this snapshot. See
also [[feedback-critique-not-validate]].
