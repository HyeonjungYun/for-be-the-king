"""Build S1/Content/Data/anim_clips.csv from the Fab animation packs on disk (2026-10-12 animation task).

Each row: key,weapon,asset,rate,loop,hit_pct
  key     - what the client asks for: idle run walk hit death attack attack2 dodge_f dodge_b roll buff cast channel aim shoot ...
            and motion keys m_* (skill motions, see Server/Data/motions.csv)
  weapon  - sword hammer spear dagger bow knuckle staff | any (fallback) | monster
  asset   - /Game/... object path of the AnimSequence (no .uasset)
  rate    - play rate
  loop    - 1 = loops (idle / run / channel)
  hit_pct - where the strike lands, 0..100 % of the clip (the client stretches the clip so this point meets the server windup)

The packs reference their own skeleton copies; the client registers each clip's skeleton as compatible with the player's at runtime
(US1AnimCatalog). Paths are verified to exist here - a missing clip is an error, not a silent fallback.
Usage: python build_anim_clips.py [--content C:\\Server\\MMO\\S1\\Content] [--out S1/Content/Data/anim_clips.csv]
"""
import argparse
import csv
import io
import os
import re
import sys

ap = argparse.ArgumentParser()
ap.add_argument("--content", default=r"C:\Server\MMO\S1\Content")
ap.add_argument("--out", default=r"C:\Server\MMO\S1\Content\Data\anim_clips.csv")
ap.add_argument("--motions", default=r"C:\Server\MMO\Server\Data\motions.csv")
ap.add_argument("--skill-list", default=r"C:\Server\MMO\src\스킬_목록.csv")
ap.add_argument("--skills", default=r"C:\Server\MMO\Server\Data\skills.csv")
ap.add_argument("--skill-effects", default=r"C:\Server\MMO\Server\Data\skill_effects.csv")
ap.add_argument("--clip-lengths", default=r"C:\Server\MMO\Tools\anim\skill_clip_lengths.csv")
ap.add_argument("--recovery", default=r"C:\Server\MMO\Server\Data\skill_recovery.csv")
args = ap.parse_args()
C = args.content

# pack roots (UE5 Manny versions)
FIGHTER = "Fighter_Animations/Animation/Sequence2"
MSTAFF = "EssentialAnimation/MagicStaff/Animation/UE5/Sequence"	# staff (2026-10-15 - the Mage pack was removed)
SPEAR = "Spear/Animation/Sequence"
DAGGER = "Dagger/Animations/Sequence"
ARCHER = "Archer/Animation/Sequence"
CS2 = "BCD_Anims/Animations/CoolSwordV2/Manny_UE5"
CS3 = "BCD_Anims/Animations/CoolSwordV3/Manny_UE5"
CHTH = "BCD_Anims/Animations/CoolHandToHand/UE5_Manny"

index = {}
for root, _, files in os.walk(C):
    for f in files:
        if f.endswith(".uasset"):
            rel = os.path.relpath(os.path.join(root, f), C).replace("\\", "/")
            index.setdefault(f[:-7], []).append(rel[:-7])


def find(folder, name):
    """Asset object path for a clip name (or a list of candidate names) inside a pack folder - first that exists."""
    import fnmatch
    names = [name] if isinstance(name, str) else list(name)
    for n in names:
        if "*" in n:
            for key in sorted(index):
                if fnmatch.fnmatch(key, n):
                    for rel in index[key]:
                        if rel.startswith(folder + "/"):
                            return "/Game/" + rel
            continue
        for cand in (n, n + "_IP", n + "_Seq"):
            for rel in index.get(cand, []):
                if rel.startswith(folder + "/"):
                    return "/Game/" + rel
    raise SystemExit(f"missing clip {names} under {folder}")


rows = []


def add(key, weapon, folder, name, rate=1.0, loop=0, hit=40, speed=0):
    # speed = the clip's natural ground speed in cm/s (0 = unknown) - the driver plays loco clips at actual speed / speed
    rows.append((key, weapon, find(folder, name), rate, loop, hit, speed))


# ── 8-direction locomotion (2026-10-15 user request: sword = BCD CoolKatana, hammer = BCD CoolSword V3, dagger = Dagger,
# bow = Archer, staff = MagicStaff, spear = Spear, knuckle = Fighter) ──────────────────────────────────────────────────────────────────────────────────────
# keys walk_<dir> / run_<dir>, dir = f fr r br b bl l fl relative to where the character faces; plain walk / run = the F clip.
# Speeds = root translation start->end / length, measured headless 2026-10-15 (%TEMP%\rg\loco_speed2.py): BCD from the
# RootMotion twins (the rows use InPlace), Dagger from 01_RM (rows use 02_IP), Archer / MagicStaff from the clips themselves
# (root-motion data with no root lock - the client locks the root of looping clips at playback).
DIRS = ("F", "FR", "R", "BR", "B", "BL", "L", "FL")
CK1_SPEED = {	# CoolKatana V1 and CoolSword V3 measure the same (one mocap base)
    "Walk": {"F": 170.0, "FR": 144.4, "R": 143.3, "BR": 147.1, "B": 146.7, "BL": 162.8, "L": 144.8, "FL": 174.9},
    "Run": {"F": 522.5, "FR": 371.1, "R": 371.7, "BR": 371.7, "B": 335.8, "BL": 372.1, "L": 371.2, "FL": 370.3},
}
DAGGER_SPEED = {"Walk": dict.fromkeys(DIRS, 164.8), "Run": dict.fromkeys(DIRS, 423.3)}
ARCHER_SPEED = {"Walk": {**dict.fromkeys(DIRS, 164.8), "BR": 163.8, "B": 163.8, "BL": 163.8}, "Run": dict.fromkeys(DIRS, 423.3)}
MSTAFF_SPEED = {"Walk": dict.fromkeys(DIRS, 91.5), "Run": dict.fromkeys(DIRS, 303.3)}	# Run = the pack's Jog_Loop
SPEAR_SPEED = ARCHER_SPEED	# same mocap base - measured identical (2026-10-15)
FIGHTER_SPEED = {	# from the 01_Run_RM / 03_Walk_Combat_RM twins (rows use IP)
    "Walk": {"F": 167.2, "FR": 158.2, "R": 162.9, "BR": 167.5, "B": 167.3, "BL": 167.4, "L": 168.1, "FL": 158.1},
    "Run": {"F": 439.8, "FR": 405.6, "R": 455.5, "BR": 459.1, "B": 459.1, "BL": 459.1, "L": 455.5, "FL": 405.5},
}
# "AS_" packs name directions by angle, in numbered folders
AS_DIR = {"F": "F_0", "FL": "F_L_45", "FR": "F_R_45", "L": "F_L_90", "R": "F_R_90", "B": "B_180", "BL": "B_L_45", "BR": "B_R_45"}
AS_DIR_NUM = {"F_0": "01", "F_L_45": "02", "F_R_45": "03", "F_L_90": "04", "F_R_90": "05", "B_180": "06", "B_L_45": "07", "B_R_45": "08"}


def directional(weapon, speeds, clip):
    """clip(gait, dir) -> (folder, name). Adds walk_<dir> / run_<dir> and the plain walk / run (= F)."""
    for gait in ("Walk", "Run"):
        for d in DIRS:
            folder, name = clip(gait, d)
            add(f"{gait.lower()}_{d.lower()}", weapon, folder, name, loop=1, speed=speeds[gait][d])
        folder, name = clip(gait, "F")
        add(gait.lower(), weapon, folder, name, loop=1, speed=speeds[gait]["F"])


def bcd_clip(folder, tag):
    return lambda gait, d: (folder, f"Anim_{tag}_{gait}_{d}_IP")


def dagger_clip(gait, d):
    a = AS_DIR[d]
    num = "03_Walk/02_Walk_Combat" if gait == "Walk" else "04_Run/02_Run_Combat"
    return f"{DAGGER}/{num}/{AS_DIR_NUM[a]}_{gait}_Combat_{a}/02_IP", f"AS_{gait}_Combat_Loop_{a}_Seq"


def as_angle_clip(folder):
    """Archer / Spear: one clip per angle, root-motion data (no IP twins)."""
    return lambda gait, d: (folder, f"AS_{gait}_Combat_{AS_DIR[d]}_Loop_Seq")


def fighter_clip(gait, d):
    return FIGHTER, f"{gait}_Combat_{AS_DIR[d]}_Loop_IP_Seq"


def mstaff_clip(gait, d):
    # 6 directions only (F B L R BL BR) - the forward diagonals use F
    d = {"FL": "F", "FR": "F"}.get(d, d)
    return MSTAFF, f"UE5_BM_{'Walk' if gait == 'Walk' else 'Jog'}_Loop_{d}_Seq"


# ── the "AS_" packs (spear / dagger / bow / staff) share one naming scheme ──────────────────────
def as_pack(weapon, folder, attack_combo="AS_Combo_Attack_01_01_Seq", attack2="AS_Combo_Attack_01_02_Seq", skills=None, loco=True):
    add("idle", weapon, folder, "AS_Idle_Combat_Seq", loop=1)
    if loco:	# False = directional() adds walk / run
        add("walk", weapon, folder, ["AS_Walk_Combat_F_0_Loop_Seq", "AS_Walk_Combat_Loop_F_0_Seq"], loop=1)
        add("run", weapon, folder, ["AS_Run_Combat_F_0_Loop_Seq", "AS_Run_Combat_Loop_F_0_Seq", "AS_Run_Combat_Fast_Loop_Seq"], loop=1)
    add("hit", weapon, folder, "AS_Hit_Combat_F_Seq", hit=0)
    add("death", weapon, folder, "AS_Hit_Combat_Death_Seq", hit=0)
    add("attack", weapon, folder, attack_combo, hit=45)
    add("attack2", weapon, folder, attack2, hit=45)
    add("dodge_f", weapon, folder, "AS_Dodge_Combat_F_0_Seq", hit=0)
    add("dodge_b", weapon, folder, ["AS_Dodge_Combat_B_180_Seq", "AS_Dodge_Combat_B_Seq"], hit=0)
    add("roll", weapon, folder, "AS_Roll_Combat_F_0_Seq", hit=0)
    add("buff", weapon, folder, "AS_Buff_Seq", hit=50)
    add("execute", weapon, folder, "AS_Execution_01_Seq", hit=55)
    add("ultimate", weapon, folder, ["AS_Ultimate_Attack_All_Seq", "AS_Combo_Attack_05_All_Seq"], hit=60)
    add("counter", weapon, folder, "AS_Parry_Counter_Attack_R_Seq", hit=50)
    add("block", weapon, folder, "AS_Block_Loop_Seq", loop=1, hit=0)
    add("combo", weapon, folder, "AS_Combo_Attack_02_All_Seq", hit=35)
    add("run_attack", weapon, folder, "AS_Run_Attack_01_Seq", hit=45)
    add("jump_attack", weapon, folder, "AS_Attack_Up_01_Seq", hit=50)
    add("target", weapon, folder, "AS_Target_01_Seq", hit=50)
    for k, n in (skills or {}).items():
        add(k, weapon, folder, n, hit=50)


as_pack("spear", SPEAR, skills={"skill1": "AS_Skill_02_Seq", "skill2": "AS_Skill_03_Seq", "channel": "AS_Skill_01_Loop_Seq"}, loco=False)
rows[-1] = rows[-1][:4] + (1, 0, 0)  # channel loops
directional("spear", SPEAR_SPEED, as_angle_clip(SPEAR))
as_pack("dagger", DAGGER, skills={"skill1": "AS_Skill_02_Seq", "skill2": "AS_Combo_Attack_Wave_All_Seq", "channel": "AS_Skill_01_Loop_Seq"}, loco=False)
rows[-1] = rows[-1][:4] + (1, 0, 0)
directional("dagger", DAGGER_SPEED, dagger_clip)
as_pack("bow", ARCHER, attack_combo="AS_Combo_Attack_01_01_Seq", skills={"skill1": "AS_Skill_01_Seq", "skill2": "AS_Skill_02_Seq", "skill3": "AS_Skill_03_Seq",
                                                                            "shoot_charge": "AS_Skill_04_Charge_LV3_End_Seq", "aim": "AS_Aim_the_Target_Loop_Seq"}, loco=False)
rows[-1] = rows[-1][:4] + (1, 0, 0)  # aim loops
directional("bow", ARCHER_SPEED, as_angle_clip(ARCHER))
add("shoot", "bow", ARCHER, "AS_Combo_Attack_01_01_Seq", hit=45)
add("shoot_multi", "bow", ARCHER, "AS_Combo_Attack_03_All_Seq", hit=30)
add("channel", "bow", ARCHER, "AS_Skill_04_Charge_LV2_Loop_Seq", loop=1, hit=0)

# ── staff = MagicStaff UE5, "BM" (battle mage) stance ─────────────────────────────────────────
add("idle", "staff", MSTAFF, "UE5_BM_Idle_Seq", loop=1)
directional("staff", MSTAFF_SPEED, mstaff_clip)
add("hit", "staff", MSTAFF, "UE5_BM_Hurt_Light_F_Seq", hit=0)
add("death", "staff", MSTAFF, "UE5_BM_Die_01_Seq", hit=0)
add("attack", "staff", MSTAFF, "UE5_BM_Attack_01_Seq", hit=45)
add("attack2", "staff", MSTAFF, "UE5_BM_Attack_02_Seq", hit=45)
add("dodge_f", "staff", MSTAFF, "UE5_BM_Dodge_02_F_Seq", hit=0)
add("dodge_b", "staff", MSTAFF, "UE5_BM_Dodge_02_B_Seq", hit=0)
add("roll", "staff", MSTAFF, "UE5_BM_Roll_F_Seq", hit=0)
add("buff", "staff", MSTAFF, "UE5_BM_Buff_01_Seq", hit=50)
add("execute", "staff", MSTAFF, "UE5_BM_Attack_05_Seq", hit=55)
add("ultimate", "staff", MSTAFF, "UE5_BM_Attack_18_Seq", hit=60)
add("counter", "staff", MSTAFF, "UE5_BM_Attack_04_Seq", hit=50)
add("block", "staff", MSTAFF, "UE5_Guard_Loop_Seq", loop=1, hit=0)
add("combo", "staff", MSTAFF, "UE5_BM_Attack_03_Seq", hit=35)
add("run_attack", "staff", MSTAFF, "UE5_BM_Attack_07_Seq", hit=45)
add("jump_attack", "staff", MSTAFF, "UE5_BM_Attack_06_Seq", hit=50)
add("target", "staff", MSTAFF, "UE5_BM_Attack_Aim_01_Seq", hit=50)
add("skill1", "staff", MSTAFF, "UE5_BM_Attack_08_Seq", hit=50)
add("skill2", "staff", MSTAFF, "UE5_BM_Attack_09_Seq", hit=50)
add("skill3", "staff", MSTAFF, "UE5_BM_Attack_10_Seq", hit=50)
add("cast", "staff", MSTAFF, "UE5_BM_Attack_01_Seq", hit=50)
add("aim", "staff", MSTAFF, "UE5_BM_Idle_Aim_Loop_Seq", loop=1, hit=0)
add("channel", "staff", MSTAFF, "UE5_BM_Telekinesis_A1_Loop_Seq", loop=1, hit=0)
add("shoot", "staff", MSTAFF, "UE5_BM_Attack_02_Seq", hit=45)

# ── knuckle = Fighter ──────────────────────────────────────────────────────────────────────────
add("idle", "knuckle", FIGHTER, "Idle_Combat_Seq", loop=1)
directional("knuckle", FIGHTER_SPEED, fighter_clip)
add("hit", "knuckle", FIGHTER, "Hit_F_Seq", hit=0)
add("death", "knuckle", FIGHTER, "Hit_Death_Seq", hit=0)
add("attack", "knuckle", FIGHTER, "Attack_01_Seq", hit=40)
add("attack2", "knuckle", FIGHTER, "Attack_02_Seq", hit=40)
add("dodge_f", "knuckle", FIGHTER, "Dodge_Front_Seq", hit=0)
add("dodge_b", "knuckle", FIGHTER, "Dodge_Back_Seq", hit=0)
add("roll", "knuckle", FIGHTER, "Roll_F_0_Seq", hit=0)
add("buff", "knuckle", FIGHTER, "Buff_Seq", hit=50)
add("execute", "knuckle", FIGHTER, "Execution_01_Seq", hit=55)
add("ultimate", "knuckle", FIGHTER, "combo_05-All_Seq", hit=60)
add("counter", "knuckle", FIGHTER, "Parry_R_Counter_Seq", hit=50)
add("block", "knuckle", FIGHTER, "Block_Idle_Seq", loop=1, hit=0)
add("combo", "knuckle", FIGHTER, "Combo_01_Seq", hit=35)
add("run_attack", "knuckle", FIGHTER, "Run_Attack_01_Seq", hit=45)
add("jump_attack", "knuckle", FIGHTER, ["Attack_Air_Up_Seq", "Attack_Air_Up*"], hit=50)
add("kick", "knuckle", CHTH, "AS_CHTH_Kick01_Low_IP", hit=45)
add("channel", "knuckle", FIGHTER, "Charge_Attack_Loop_Seq", loop=1, hit=0)
add("skill1", "knuckle", FIGHTER, "Charge_Attack_End_Seq", hit=40)
add("skill2", "knuckle", FIGHTER, "Attack_05_Seq", hit=45)

# ── sword = BCD Cool Sword V2 attacks + Cool Katana V1 stance and 8-direction locomotion (2026-10-15), hammer = Cool Sword V3 ──
CK1 = "BCD_Anims/Animations/CoolKatanaV1/Manny_UE5/InPlace"
add("idle", "sword", CK1, "Anim_CK_Idle_S1_IP", loop=1)
directional("sword", CK1_SPEED, bcd_clip(CK1, "CK"))
add("idle", "hammer", CS3, "Anim_CS3_Idle_IP", loop=1)
directional("hammer", CK1_SPEED, bcd_clip(CS3, "CS3"))
for weapon, folder, tag in (("sword", CS2, "CS2"), ("hammer", CS3, "CS3")):
    add("jump_attack", weapon, folder, f"Anim_{tag}_JumpAttack_IP" if tag == "CS3" else "Anim_CS2_ComboF_IP", hit=55)
add("hit", "sword", CS2, "Anim_CS2_Hit_Middle_F_IP", hit=0)
add("death", "sword", CS2, "Anim_CS2_Hit_Big_F_IP", hit=0)
add("attack", "sword", CS2, "Anim_CS2_Slash_IP", hit=40)
add("attack2", "sword", CS2, "Anim_CS2_Stab01_IP", hit=45)
add("combo", "sword", CS2, "Anim_CS2_ComboA_IP", hit=35)
add("skill1", "sword", CS2, "Anim_CS2_ComboB_IP", hit=40)
add("skill2", "sword", CS2, "Anim_CS2_ComboC_IP", hit=40)
add("ultimate", "sword", CS2, "Anim_CS2_ComboD_IP", hit=55)
add("execute", "sword", CS2, "Anim_CS2_ComboE_IP", hit=55)
add("hit", "hammer", CS2, "Anim_CS2_Hit_Big_F_IP", hit=0)
add("death", "hammer", CS2, "Anim_CS2_Hit_Big_B_IP", hit=0)
add("attack", "hammer", CS3, "Anim_CS3_ComboA1_IP", hit=45)
add("attack2", "hammer", CS3, "Anim_CS3_ComboA2_IP", hit=45)
add("combo", "hammer", CS3, "Anim_CS3_ComboA_IP", hit=35)
add("skill1", "hammer", CS3, "Anim_CS3_ComboB_IP", hit=45)
add("skill2", "hammer", CS3, "Anim_CS3_ComboC_IP", hit=45)
add("ultimate", "hammer", CS3, "Anim_CS3_ComboD_IP", hit=55)
add("execute", "hammer", CS3, "Anim_CS3_ComboF_IP", hit=55)

# ── unarmed / monster (Fighter non-combat) ─────────────────────────────────────────────────────
for w in ("monster", "any"):
    add("idle", w, FIGHTER, "Idle_Seq", loop=1)
    add("walk", w, FIGHTER, "Walk_F_0_Loop_IP_Seq", loop=1)
    add("run", w, FIGHTER, "Run_F_0_Loop_IP_Seq", loop=1)
    add("hit", w, FIGHTER, "Hit_F_Seq", hit=0)
    add("death", w, FIGHTER, "Knock_Down_Death_Seq", hit=0)
    add("attack", w, FIGHTER, "Attack_01_Seq", hit=40)
    add("attack2", w, FIGHTER, "Attack_02_Seq", hit=40)
    add("dodge_f", w, FIGHTER, "Dodge_Front_Seq", hit=0)
    add("dodge_b", w, FIGHTER, "Dodge_Back_Seq", hit=0)
    add("roll", w, FIGHTER, "Roll_F_0_Seq", hit=0)
    add("buff", w, FIGHTER, "Buff_Seq", hit=50)
    add("channel", w, FIGHTER, "Charge_Attack_Loop_Seq", loop=1, hit=0)
    add("execute", w, FIGHTER, "Execution_01_Seq", hit=55)
    add("ultimate", w, FIGHTER, "combo_05-All_Seq", hit=60)
    add("counter", w, FIGHTER, "Parry_R_Counter_Seq", hit=50)
    add("block", w, FIGHTER, "Block_Idle_Seq", loop=1, hit=0)
    add("combo", w, FIGHTER, "Combo_01_Seq", hit=35)
    add("kick", w, CHTH, "AS_CHTH_Kick01_Low_IP", hit=45)
    add("jump_attack", w, FIGHTER, ["Attack_Air_Up_Seq", "Attack_Air_Up*"], hit=50)
add("slam", "monster", CS3, "Anim_CS3_ComboD_IP", hit=55)		# house boss 9001 - overhead smash

# ── motion keys → clip keys per weapon (the client resolves m_* through this table, then the fallbacks) ──
MOTION_TO_KEY = {
    "m_none": None, "m_buff": "buff", "m_shout": "buff", "m_attack": "attack", "m_slash": "attack", "m_thrust": "attack2",
    "m_slam": "jump_attack", "m_spin": "combo", "m_combo": "combo", "m_dash": "dodge_f", "m_dash_back": "dodge_b", "m_roll": "roll",
    "m_leap": "jump_attack", "m_blink": "dodge_f", "m_shoot": "shoot", "m_shoot_multi": "shoot_multi", "m_shoot_charge": "shoot_charge",
    "m_throw": "attack2", "m_kick": "kick", "m_cast": "cast", "m_channel": "channel", "m_aim": "aim", "m_counter": "counter",
    "m_block": "block", "m_execute": "execute", "m_ultimate": "ultimate", "m_ground": "jump_attack", "m_summon": "buff",
}
# weapon-specific overrides where a better clip exists
OVERRIDES = {
    ("m_slam", "hammer"): "ultimate", ("m_slam", "monster"): "slam", ("m_spin", "sword"): "ultimate", ("m_spin", "spear"): "ultimate",
    ("m_spin", "hammer"): "skill2", ("m_shoot", "sword"): "attack2", ("m_cast", "staff"): "cast", ("m_cast", "bow"): "skill2",
    ("m_summon", "staff"): "skill1", ("m_ground", "staff"): "skill3", ("m_ground", "spear"): "skill1", ("m_throw", "dagger"): "skill1",
    ("m_throw", "spear"): "skill2", ("m_throw", "hammer"): "skill1", ("m_thrust", "spear"): "attack",
}
# ── per-skill clips (2026-10-15): the user picks a clip per skill in src/스킬_목록.csv, column "스킬 애니메이션" ─────────────
# Applied one batch at a time - SKILL_ANIM_HIT lists the game IDs done so far, with where the strike lands in the clip
# (% of length, the right hand's peak speed - %TEMP%\rg\clip_probe.py run headless). Each gets its own motion key
# m_sk_<id> (motion ids after the shared vocabulary); Server/Data/skill_motions.csv maps the skill to it by hand.
# rate = length * hit / cast_ms (what observers play - they do not know cast_ms), clamped like the client's own cast.
SKILL_ANIM_HIT = {1004: 19, 1007: 36, 1008: 37, 1009: 24,	# 1009 검의 찬양: overhead 0-16 %, the down cut 22-28 %
                  # 2026-10-15 batch 4 (%TEMP%\rg\clip_phases.py): 1014 원혼들의 몸부림 the flat sweep 16-24 % (an overhead slam
                  # follows at 52 %) · 1005 머리 부수기 jump, raise, slam 52 % · 1006 흉갑 부수기 lunge-slash 20 % (root motion,
                  # played in place) · 1030 맹수의 어금니 held high 24-44 %, the cut 52 % · 1031 맹수의 앞발 low 0-36 %, the rising cut 40 %
                  1014: 20, 1005: 52, 1006: 20, 1030: 88, 1031: 40}
# 1030 맹수의 어금니 (user, 2026-10-15): the hit lands 0.3 s before the clip ends (88 % of 2.45 s), not on landing - the server
# waits skills.csv telegraph_ms (850) after the landing, sized so the clip plays near its natural speed after a full leap
# start_pct - where a skill clip starts (skips a windup the skill does not have). 1007 기습 · 1008 일도양단 dash to the target
# (move to_target, cast 60 ms) and strike on arrival (user, 2026-10-15): the GhostSamurai clips crouch 0-26 %, carry the body
# forward 28-34 % and strike 36-38 % (%TEMP%\rg\clip_phases.py) - start in the held crouch so the dash pose covers the travel
SKILL_ANIM_START = {1007: 24, 1008: 24,
                    1030: 40, 1031: 12}	# dash skills (move dash 400 cm): the held pose rides the dash, the cut lands on arrival
INSTANT_STRIKE_MS = 150	# a skill with no cast and no dash lands as it is pressed - show its strike this soon (1014)
TO_TARGET_STOP_CM = 90	# S1MyPlayer StopShortCm - a to_target dash ends this far before the target
start_pct = {}	# (key, weapon) -> start_pct, written as the start_pct column
SKILL_RATE_MIN, SKILL_RATE_MAX = 0.5, 3.0	# S1Player.cpp ANIM_ATTACK_RATE_MIN / ANIM_SKILL_RATE_MAX
SKILL_WEAPON = {"검": "sword", "망치": "hammer", "창": "spear", "단검": "dagger", "활": "bow", "너클": "knuckle", "스태프": "staff"}


def read_text(path):
    raw = open(path, "rb").read()
    try:
        return raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        return raw.decode("cp949")		# 한셀 saves CP949


skill_list = {int(r["게임 ID"]): r for r in csv.DictReader(io.StringIO(read_text(args.skill_list))) if r.get("게임 ID", "").isdigit()}
skill_rows = list(csv.DictReader(io.StringIO(read_text(args.skills))))
cast_ms = {int(r["skill_id"]): int(r["cast_ms"]) for r in skill_rows}
telegraph_ms = {int(r["skill_id"]): int(r["telegraph_ms"]) for r in skill_rows}
dash_ms = {}	# skill -> a full dash's travel time (observers' clip rate - the caster's client uses the real distance)
for r in csv.DictReader(io.StringIO(read_text(args.skill_effects))):
    if r["effect"] == "move" and float(r["speed_cms"] or 0) > 0 and r["requires"] == "none":
        sid = int(r["skill_id"])
        dist = float(r["dist_cm"]) - (TO_TARGET_STOP_CM if r["param"] in ("to_target", "leap_target") else 0)
        dash_ms[sid] = max(dist, 0) / float(r["speed_cms"]) * 1000
        if r["param"] == "leap_target":
            dash_ms[sid] += telegraph_ms[sid]		# the server hits telegraph_ms after the landing
clip_len = {r["asset"]: float(r["length_s"]) for r in csv.DictReader(io.StringIO(read_text(args.clip_lengths)))}
skill_motion_keys = []
after_hit_ms = {}	# skill -> ms from its strike to the end of its clip
for sid, hit in SKILL_ANIM_HIT.items():	# insertion order = motion id order - append new skills at the end so ids never shift
    row = skill_list[sid]
    m = re.search(r"AnimSequence'(/Game/[^'.]+)\.", row["스킬 애니메이션"])
    if m is None:
        raise SystemExit(f"skill {sid}: no AnimSequence in 스킬 애니메이션")
    rel = m.group(1)[len("/Game/"):]
    part = re.search(r"\((.+)\)", row["착용 부위"])
    weapon = SKILL_WEAPON.get(part.group(1), "any") if part else "any"
    asset = find(rel.rsplit("/", 1)[0], rel.rsplit("/", 1)[1])
    if asset not in clip_len:
        raise SystemExit(f"skill {sid}: {asset} has no length in {args.clip_lengths} (run clip_probe.py)")
    start = SKILL_ANIM_START.get(sid, 0)
    strike_ms = (cast_ms[sid] + dash_ms.get(sid, 0)) or INSTANT_STRIKE_MS
    rate = max(SKILL_RATE_MIN, min(SKILL_RATE_MAX, clip_len[asset] * (hit - start) / 100 / (strike_ms / 1000)))
    key = f"m_sk_{sid}"
    rows.append((key, weapon, asset, round(rate, 2), 0, hit, 0))
    if start:
        start_pct[(key, weapon)] = start
    skill_motion_keys.append(key)
    # what is left of the clip after its strike, at the table's rate - the post-skill lock (below). An instant skill's strike is
    # shown INSTANT_STRIKE_MS after the server already fired it, so that much is added
    after_hit_ms[sid] = clip_len[asset] * (100 - hit) / 100 / rate * 1000 + (INSTANT_STRIKE_MS if (cast_ms[sid] + dash_ms.get(sid, 0)) == 0 else 0)

have = {(k, w) for k, w, *_ in rows}
WEAPONS =("sword", "hammer", "spear", "dagger", "bow", "knuckle", "staff", "monster", "any")


def resolve(key, weapon):
    for cand in ((key, weapon), (key, "any"), ("attack", weapon), ("attack", "any")):
        if cand in have:
            return cand
    return None


motion_rows = []
for i, (m, base) in enumerate(MOTION_TO_KEY.items(), start=1):
    motion_rows.append((i, m))
    if base is None:
        continue
    for w in WEAPONS:
        key = OVERRIDES.get((m, w), base)
        src = resolve(key, w)
        if src is None:
            raise SystemExit(f"motion {m} weapon {w}: nothing to resolve")
        if (m, w) not in have:
            r = next(r for r in rows if (r[0], r[1]) == src)
            rows.append((m, w, r[2], r[3], r[4], r[5], r[6]))
            have.add((m, w))
for m in skill_motion_keys:		# other weapons fall back to their attack in the client - no copied rows
    motion_rows.append((len(motion_rows) + 1, m))

rows.sort(key=lambda r: (r[0].startswith("m_"), r[0], WEAPONS.index(r[1]) if r[1] in WEAPONS else 99))
with open(args.out, "w", encoding="utf-8", newline="") as f:
    f.write("key,weapon,asset,rate,loop,hit_pct,speed_cms,start_pct\r\n")
    for k, w, a, rate, loop, hit, speed in rows:
        f.write(f"{k},{w},{a},{rate},{loop},{hit},{speed:g},{start_pct.get((k, w), 0)}\r\n")
with open(args.motions, "w", encoding="utf-8", newline="") as f:
    f.write("motion_id,key\r\n")
    for i, m in motion_rows:
        f.write(f"{i},{m}\r\n")
print(f"wrote {args.out}: {len(rows)} rows; {args.motions}: {len(motion_rows)} motions")

# ── post-skill lock (user, 2026-10-15): after a skill goes off, no move / basic attack / other skill until its animation
# ends. recovery_ms = how long after the strike - the server adds the cast, dash / leap and telegraph itself. A skill with
# no clip gets DEFAULT_RECOVERY_MS. 0 = no lock: buffs (스킬 종류 "버프" in the skill list) and pure movement (only move /
# anchor effects) - the user's two exceptions. Passive never casts; hold is locked while it is held.
DEFAULT_RECOVERY_MS = 300
PURE_MOVE_EFFECTS = {"move", "anchor"}
effects_of = {}
for r in csv.DictReader(io.StringIO(read_text(args.skill_effects))):
    effects_of.setdefault(int(r["skill_id"]), set()).add(r["effect"])
with open(args.recovery, "w", encoding="utf-8", newline="") as f:
    f.write("skill_id,recovery_ms,why\r\n")
    counts = {}
    for r in skill_rows:
        sid = int(r["skill_id"])
        listed = skill_list.get(sid, {})
        if r["kind"] in ("passive", "hold"):
            ms, why = 0, r["kind"]
        elif listed.get("스킬 종류", "") == "버프":
            ms, why = 0, "buff"
        elif effects_of.get(sid, set()) and effects_of[sid] <= PURE_MOVE_EFFECTS:
            ms, why = 0, "movement"
        elif sid in after_hit_ms:
            ms, why = round(after_hit_ms[sid]), "clip"
        else:
            ms, why = DEFAULT_RECOVERY_MS, "default"
        counts[why] = counts.get(why, 0) + 1
        f.write(f"{sid},{ms},{why}\r\n")
print(f"wrote {args.recovery}: " + " ".join(f"{k}={v}" for k, v in sorted(counts.items())))
