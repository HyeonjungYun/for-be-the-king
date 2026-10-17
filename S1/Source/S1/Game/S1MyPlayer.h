// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Game/S1Player.h"
#include "S1MyPlayer.generated.h"

class UInputMappingContext;
class UInputAction;
class US1ItemUIComponent;
struct FS1SkillDef;

/**
 * Binds one Input Action to one equipment slot.
 *
 * The slot is stored as a plain int rather than an enum because the authority for that
 * mapping is Protocol::EquipSlot, a protobuf enum that cannot be a UENUM. Duplicating it
 * as a UENUM just to expose it here would create a second definition to keep in sync.
 */
USTRUCT()
struct FSkillInputBinding
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Input")
	TObjectPtr<UInputAction> Action = nullptr;

	/** Protocol::EquipSlot value. 1=Shift 2=Q 3=E 4=R 5=Z 6=X — equipment-skill-binding.md B1. */
	UPROPERTY(EditAnywhere, Category = "Input", meta = (ClampMin = "1", ClampMax = "6"))
	int32 EquipSlot = 1;
};

/**
 *
 */
UCLASS()
class S1_API AS1MyPlayer : public AS1Player
{
	GENERATED_BODY()

public:

	/** Constructor */
	AS1MyPlayer();

protected:

	virtual void BeginPlay() override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

	/**
	 * A pawn spawned on re-entry (US1BaseCampSubsystem) has no controller when BeginPlay runs —
	 * it is possessed right after. The local setup runs again here once it has one.
	 */
	virtual void NotifyControllerChanged() override;

	virtual void Tick(float DeltaTime) override;

	/** Mapping contexts and the cursor-aiming input mode for a local controller. Safe to repeat. */
	void SetupLocalController();

protected:

	void Move(const FInputActionValue& Value);

	void Look(const FInputActionValue& Value);

	/** Space. Opens aiming for the basic attack — same flow as a Target-aimed skill. */
	void OnAimAttackKey();

	/**
	 * Asks the server to attack one object. The client never decides whether the hit lands
	 * — it only names a target.
	 * See design/gdd/combat-system.md Rule 1 (target lock) and Rule 10 (server authority).
	 */
	void SendAttack(uint64 TargetId);

	/**
	 * Sustained attack, driven by holding RMB. Re-picks a target every swing, so a dead
	 * one is replaced by whatever else is near the cursor — and nothing at all when the
	 * range is empty, because this never chases. combat-system.md Rule 1.
	 */
	void TickSustainedAttack();

	/** Living enemy inside attack range, nearest to the cursor. Null when there is none. */
	class AS1Player* FindEnemyNearCursorInRange() const;

	/**
	 * A basic attack that needs no target is active (일보일검 basic_dash · 난무 basic_spin, 2026-10-11) — the swing goes
	 * toward the cursor whether or not anything stands there. The server decides what it hits.
	 */
	bool HasTargetlessBasic() const;

	/** C_ATTACK { target 0, aim = cursor } on the same gate as SendAttack. */
	void SendTargetlessAttack();

	/** The armor skill's weapon condition (skills.csv requires_weapon) — false names the reason in OutWhy. */
	bool MeetsWeaponRequirement(const FS1SkillDef& Def, FText& OutWhy) const;

	/**
	 * 벽 차기 (1163, 2026-10-11) "벽에 붙어 있을 때만" — rays against the level's static collision in 16 directions,
	 * the same 110 cm the server's Room::IsNearWall uses. Refusing here keeps the local cooldown from starting on a cast
	 * the server would drop.
	 */
	bool IsNearWallLocal() const;

	/** 날개 (1184) — movement skills travel × (1 + passive move_dist of the bound skills). The server scales its window the same way. */
	float GetMoveDistScale() const;

	/** 전력 질주 (1169) — a hold that forbids attacking is running. */
	bool IsAttackBlockedByHold() const;

	/**
	 * Basic attack reach as the server will judge it: the worn weapon's attack_range_cm (item_types.csv), then the
	 * buffs the server told us about — basic_melee replaces it (푸리아 · 은장도), attack_range adds to it (명경지수 ·
	 * 통아 · 육량전). ATTACK_RANGE_CM before the first inventory sync. Display and the RMB range gate only.
	 */
	float GetBasicAttackRangeCm() const;

	/** object_id under the cursor, or 0 when there is no valid target. */
	uint64 AcquireTargetUnderCursor();

	/** The hostile actor under the cursor, or null. Shares one trace with the id lookup. */
	class AS1Player* AcquireTargetActorUnderCursor() const;

	/**
	 * Draws the locally predicted windup as a closing arc at the character's feet.
	 * Rule 10 allows exactly one client-side prediction — our own windup animation —
	 * because waiting a round trip for *any* feedback makes the attack feel broken.
	 * The result (damage, crit, whether it landed at all) still waits for the server.
	 */
	void DrawWindupDebug();

	/**
	 * Six-slot skill bar readout.
	 *
	 * Not a UMG widget yet, on purpose. The skill content itself is still open (Q6 in
	 * design/gdd/skill-system.md), so a widget built now gets thrown away once we know
	 * what actually belongs on a slot. Promote this once input, cast and effects are
	 * wired and the contents have settled.
	 */
	void DrawSkillBarDebug();

	/**
	 * A skill key was pressed. Opens aiming, switches to another skill's aiming, or cancels
	 * it when the same key is pressed twice. Fires outright when the skill needs no aiming.
	 *
	 * Takes the slot as int because it arrives as a bound delegate payload; it is cast to
	 * Protocol::EquipSlot at the point of use.
	 */
	void OnSkillKey(int32 EquipSlotValue);

	/** LMB down. Fires whatever is being aimed; does nothing otherwise. */
	void OnConfirmPressed();

	/** LMB up. Ends a hold-cast — releasing the button IS the cancel. skill-system.md R4. */
	void OnConfirmReleased();

	/** RMB down. Cancels aiming when aiming; otherwise reserved for sustained attack (4-2). */
	void OnCancelPressed();

	/** RMB up. Clears the "this hold was spent on a cancel" latch. */
	void OnCancelReleased();

	/**
	 * Sends C_SKILL for the slot currently being aimed. The client only states intent —
	 * every check that decides whether the skill happens (cooldown, silence, range, whether
	 * the slot is even bound) is redone server-side. skill-system.md R4.
	 */
	void FireAimedSkill();

	/** Leaves aiming without firing. Sends nothing — the server never knew. */
	void CancelAiming();

	/**
	 * Drops an in-flight cast and everything it was going to do. Used by every cancel
	 * path, so a cancelled cast leaves no cooldown and no dash — R5's "어떤 흔적도
	 * 남기지 않는다" has to hold for the queued displacement too.
	 */
	void AbortCast();

	/** Slot data for a slot value, or null when the slot is empty or shadowed. */
	const Protocol::SkillInfo* FindUsableSlot(int32 EquipSlotValue) const;

	/** Seconds left on the predicted cooldown for a slot. 0 when ready. */
	float GetSlotCooldownRemaining(int32 EquipSlotValue) const;

	/** Starts the predicted cooldown for a slot. Called when the effect lands, not on cast start. */
	void StartSlotCooldown(int32 EquipSlotValue, uint32 CooldownMs);

	/** Ground indicator for the skill being aimed. Owner-only — nobody else may see it. */
	void DrawAimIndicator();

	/**
	 * Cast progress at the feet, plus the two things a caster has to know: that letting go
	 * of LMB cancels, and whether moving cancels too.
	 *
	 * Predicted locally rather than driven by S_SKILL_CAST. R4 allows exactly one client
	 * prediction — our own cast visual — because waiting a round trip for any feedback at
	 * all is what made this feel broken.
	 */
	void DrawCastGauge();

	/**
	 * Where the cursor ray meets the plane the character stands on.
	 * Shared by facing and by skill aiming so the two can never disagree.
	 */
	bool GetCursorWorldLocation(FVector& OutLocation) const;

	/**
	 * Turns the character toward the cursor every frame, independently of where it is
	 * moving. This separation is the whole point of the control scheme — you retreat with
	 * WASD while keeping your aim on whoever is chasing you.
	 * Formula 2 in design/gdd/movement-camera.md.
	 */
	void UpdateCursorFacing(float DeltaTime);

protected:

	/** 0 snaps instantly. Higher converges faster. movement-camera.md Tuning Knobs (safe range 0-40). */
	UPROPERTY(EditAnywhere, Category = "Movement|Facing")
	float FaceInterpSpeed = 20.f;

	/** A cursor nearer than this leaves facing alone — there is no direction at the origin. */
	UPROPERTY(EditAnywhere, Category = "Movement|Facing")
	float CursorDeadRadius = 25.f;

public:

	UFUNCTION(BlueprintCallable, Category = "Input")
	virtual void DoMove(float Right, float Forward);

	UFUNCTION(BlueprintCallable, Category = "Input")
	virtual void DoLook(float Yaw, float Pitch);

	UFUNCTION(BlueprintCallable, Category = "Input")
	virtual void DoJumpStart();

	UFUNCTION(BlueprintCallable, Category = "Input")
	virtual void DoJumpEnd();

	/**
	 * DEBUG — M1 ⑤ (production/qa/playtest-m1-skill-binding.md). Console: DebugDashNoNotice
	 *
	 * Runs the 2001 displacement with no C_SKILL in front of it. The server never opens its
	 * speed window, so it must answer with [MOVE REJECT] and a snapback. A dash that gets
	 * through here means move validation has a hole.
	 */
	UFUNCTION(Exec)
	void DebugDashNoNotice();

	/**
	 * Logs what the fixed top-down camera actually shows, in metres from our character: where each screen
	 * edge meets the ground, and how far ahead a thing of a given height can still be on screen.
	 * The level design (tower sightlines, landmark heights, room sizes) is sized from this — design/gdd/level-map.md.
	 */
	UFUNCTION(Exec)
	void DebugCameraFootprint();

	/**
	 * radar.md AC-RD-25 — the enemy-hidden evidence. Logs two lines: what is *in range* (players, monsters,
	 * towers, chests — counted from the world) and what the radar *drew*. Players and monsters must never
	 * appear in the second line however many are in the first.
	 */
	UFUNCTION(Exec)
	void DebugRadar();

	/**
	 * Test mode from the console — "TestCmd spawn 1 2 1 0 0" sends "/t spawn 1 2 1 0 0" (design/quick-specs/test-mode-2026-10-11.md).
	 * The F7 panel sends the same commands; this is for automation and for anything the panel has no row for.
	 */
	UFUNCTION(Exec)
	void TestCmd(const FString& Args);

	/**
	 * Caps my own walk speed in cm/s (0 = off) — only ever lower than the real speed, so the server accepts every move.
	 * ToggleSec > 0 switches the cap on and off every ToggleSec seconds (walk ↔ run while a key stays held — opening the
	 * console mid-move would release it). For checking the walk clips and the walk ↔ run blend (2026-10-15).
	 */
	UFUNCTION(Exec)
	void DebugMoveSpeed(float Cms, float ToggleSec = 0.f);

	/**
	 * Shows a skill's projectile look (skill_vfx.csv) in front of me without casting - it drifts forward at SpeedCms for
	 * Seconds, then fades. Nothing is sent to the server. For tuning the look by eye (검의 찬양 1009, 2026-10-15).
	 */
	UFUNCTION(Exec)
	void DebugVfx(int32 SkillId, float Seconds = 3.f, float SpeedCms = 120.f);

public:

	FORCEINLINE class USpringArmComponent* GetCameraBoom() const { return CameraBoom; }

	/**
	 * Camera distance multiplier from S_VISION.camera_scale — 천리안 (helmet 1132, "기본 시야가 넓어진다") pulls the camera
	 * back with the wider sight (user decision 2026-10-11). Blind never moves the camera (US1BlindOverlayWidget draws it).
	 */
	void SetCameraScale(float Scale);

	/** Where the cursor meets the ground — the test panel's F6 places dummies there (GetCursorWorldLocation, read-only). */
	bool GetCursorGroundPoint(FVector& OutLocation) const { return GetCursorWorldLocation(OutLocation); }

	/** Drops every predicted cooldown — the test panel's "쿨다운 초기화" and new test gear (the server zeroes its side too). */
	void ResetAllSlotCooldowns();

	/**
	 * A passive fired server-side (염두 1090 · 맹수의 본능 1034, S_STATUS STATUS_COOLDOWN) — the slot holding that skill
	 * starts its cooldown readout. Passives never go through the key, so this is the only way the client learns (2026-10-11).
	 */
	void StartCooldownForSkill(uint32 SkillId, uint32 CooldownMs);

	/** cast_ms of the skill in the slot of my last C_SKILL (skills.csv via the equip sync), 0 if unknown — the cast clip is stretched to it (anim, 2026-10-12). */
	uint32 CastMsOfLastSkill() const;
	uint32 LastSentSkillSlot = 0;

	/** How long the dash of my last C_SKILL takes (distance / speed, 0 = no dash) — its clip strikes on arrival (2026-10-15). */
	virtual float LastSkillDashSec() const override { return LastSentDashSec; }
	float LastSentDashSec = 0.f;

	/**
	 * 후딜 (2026-10-15) - my skill just went off: no move / basic attack / other skill until DelaySec (its dash / leap / landing
	 * delay) + RecoveryMs from now - the same deadline the server keeps. RecoveryMs 0 (buff, pure movement) = no lock.
	 */
	void StartSkillLock(float DelaySec, uint32 RecoveryMs);
	uint32 LocalCastRecoveryMs = 0;		// the casting skill's recovery - the lock starts when its cast completes

	/**
	 * S_ATTACK for one of my own swings — the server's windup and attack interval (attack speed applied). The sustained
	 * attack sends on that beat instead of a fixed 1 s, which left fast weapons idling and slow ones refused (2026-10-11
	 * "우클릭을 꾹 누르고 있을 때는 본인 공격속도에 맞춰서").
	 */
	void OnMyAttackAccepted(uint32 WindupMs, uint32 IntervalMs);

	FORCEINLINE class UCameraComponent* GetFollowCamera() const { return FollowCamera; }

	/**
	 * The skill action bound to an equip slot, or null. The item UI asks Enhanced Input which
	 * key it is mapped to, so the key badges follow a rebind (design/ux/item-ui.md AC-UI-24).
	 */
	const UInputAction* GetSkillAction(int32 EquipSlot) const;

protected:

	/** Item UI — inventory panel and its keys (design/ux/item-ui.md). */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<US1ItemUIComponent> ItemUI;

	/** Test mode panel — F7 / F6 (design/quick-specs/test-mode-2026-10-11.md). The server must run FBTK_TEST_MODE=1. */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<class US1TestModeComponent> TestMode;

protected:

	/** Input mapping contexts applied to the local player on possession */
	UPROPERTY(EditAnywhere, Category = "Input")
	TArray<UInputMappingContext*> DefaultMappingContexts;

	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* JumpAction;

	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* MoveAction;

	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* LookAction;

	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* MouseLookAction;

	/** Space. Opens basic-attack aiming. Leave unset and only the sustained attack works. */
	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* AimAttackAction;

	/**
	 * Six entries expected, one per slot. Entries with no Action are skipped, so a
	 * partially filled list is a working list — useful while the IA assets are being made.
	 */
	UPROPERTY(EditAnywhere, Category = "Input")
	TArray<FSkillInputBinding> SkillActions;

	/** LMB. Confirms an aimed skill, and holding it is what keeps a cast alive. */
	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* ConfirmAction;

	/** RMB. Cancels aiming. Sustained basic attack moves here in 4-2. */
	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* CancelAction;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	USpringArmComponent* CameraBoom;

	/** Follow camera */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	UCameraComponent* FollowCamera;

protected:
	float MovePacketSendTimer = MOVE_PACKET_SEND_DELAY;

	/** World-seconds deadline for the predicted windup. 0 means we are not attacking. */
	float LocalWindupEndsAt = 0.f;

	/**
	 * Gated on the windup only, not the full interval. The server does not consume cooldown
	 * when an attack is cancelled, so blocking for the recovery too would make the client
	 * stricter than the server — you would be locked out after a miss that the server
	 * already forgave.
	 */
	float LocalReadyAt = 0.f;

	/**
	 * The server's attack interval for us (S_ATTACK interval_ms) — attack speed from gear and buffs applied. Starts at
	 * ATTACK_INTERVAL_SECONDS until the first swing is acknowledged.
	 */
	float AttackIntervalSec = 1.0f;

	/** When the last C_ATTACK left, 0 once the server acknowledged it. No ack in ATTACK_ACK_TIMEOUT = refused, try again. */
	float AttackSentAt = 0.f;
	static constexpr float ATTACK_ACK_TIMEOUT = 0.3f;

	/** Arrival slack so the next swing does not land a hair before the server's ready time and get dropped. */
	static constexpr float ATTACK_SEND_SLACK = 0.04f;

	/** The windup arc's length — the server's real windup once a swing is acknowledged. */
	float LocalWindupSeconds = 0.3f;

	/**
	 * Mirrors the server's Formula C3 (windup_ratio 0.3 x attack_interval 1000ms).
	 * 🔴 Duplicated constant. When the server value changes this must change with it —
	 * P2 should send attack speed to the client instead of restating the formula here.
	 */
	static constexpr float PREDICTED_WINDUP_SECONDS = 0.3f;
	static constexpr float WINDUP_ARC_RADIUS = 70.f;
	static constexpr int32 WINDUP_ARC_SEGMENTS = 32;

	/**
	 * 🔴 Two more duplicated server constants, same problem as PREDICTED_WINDUP_SECONDS:
	 * attack_interval at attack_speed 1.0, and the naked attack_range from
	 * combat-system.md Rule 1. Both come from equipment once items exist, and the server
	 * should be sending them by then.
	 */
	static constexpr float ATTACK_INTERVAL_SECONDS = 1.0f;
	static constexpr float ATTACK_RANGE_CM = 150.f;

	/** Base key for the skill bar lines. Six consecutive keys, one per slot. */
	static constexpr int32 SKILL_BAR_DEBUG_KEY = 7100;

	/** Six equipment slots — equipment-skill-binding.md B1. Matches SKILL_SLOT_COUNT server-side. */
	static constexpr int32 SKILL_SLOT_MAX = 6;

protected:

	/**
	 * Slot currently being aimed, as a Protocol::EquipSlot value. 0 means not aiming.
	 *
	 * 🔴 Purely local. The server has no idea this state exists and must not — knowing it
	 * would mean synchronising a state that feeds no judgement (skill-system.md R4), and it
	 * would leak "that player is about to do something" to everyone else.
	 */
	int32 AimingSlot = 0;

	/**
	 * AimingSlot value that means "aiming the basic attack".
	 *
	 * The basic attack shares the aiming state rather than owning a second flag, because
	 * only one thing can be aimed at a time (skill-system.md § States D-1b) and two
	 * booleans would let that invariant break silently.
	 */
	static constexpr int32 AIMING_BASIC_ATTACK = -1;

	/** True while RMB is held outside of aiming — the sustained attack. */
	bool bSustainedAttack = false;

	/**
	 * True while an RMB hold has already been spent cancelling an aim. Keeps that same hold
	 * from rolling straight into a sustained attack — cancelling and attacking are opposite
	 * intentions and they must not share one press. combat-system.md Rule 1.
	 */
	bool bCancelHoldConsumed = false;

	/** Slot whose cast is in flight, so releasing LMB knows what to cancel. 0 when idle. */
	int32 CastingSlot = 0;

	/** World-seconds deadline for the predicted cast. 0 means we are not casting. */
	float LocalCastEndsAt = 0.f;

	/** Full cast length in seconds, kept so the gauge can show progress rather than remainder. */
	float LocalCastDuration = 0.f;

	/** Mirrors the casting skill's can_move_while_casting, for the "don't move" hint. */
	bool bLocalCastLocksMovement = false;

	/** Cooldown of the skill in flight, applied when the cast completes rather than starts. */
	uint32 LocalCastCooldownMs = 0;

	/**
	 * Dash queued behind an in-flight cast. Direction is frozen at cast time, not read
	 * again on completion — the server stored the same aim when the packet arrived, so
	 * swinging the cursor mid-cast must not change where the dash goes.
	 */
	FVector PendingDashDirection = FVector::ZeroVector;
	float PendingDashDistCm = 0.f;
	float PendingDashSpeedCms = 0.f;
	bool bPendingDashIgnoresWalls = false;		// skill_effects.csv ignores_walls (유령 신랑의 저주)
	bool bPendingDashThroughPawns = false;		// move param dash_through (그림자 베기 - characters do not stop it)

	/**
	 * Recast (kind recast — 피의 추적 · 올가미): after the first press lands, the same key within
	 * skill_rules recast_window_ms fires the second part at the target the first press locked.
	 * Mirrors Creature::recastSkillId server-side; the server re-checks everything (skill roster stage 1).
	 */
	bool bPendingRecast = false;
	uint64 PendingRecastTargetId = 0;
	int32 RecastSlot = 0;
	float RecastUntil = 0.f;
	uint64 RecastTargetId = 0;

	/** Camera distance (the boom never zooms). Blind is drawn by US1BlindOverlayWidget, not by moving the camera (2026-10-11). */
	float DefaultArmLength = 2600.f;

	/** Console: DebugBlind <seconds> - pretend the server blinded us, to look at the blind darkness without a bot (TEMP). */
	UFUNCTION(Exec)
	void DebugBlind(float Seconds = 2.f);

	/** Opens the recast window for the slot whose first press just landed. */
	void OpenRecastWindow(int32 EquipSlotValue);

	/** Second press inside the window: sends C_SKILL at the locked target and runs the recast dash toward it. */
	void FireRecast();

	/**
	 * Hold (kind hold — 회오리 · 빈쿨룸 · 정좌): the first press starts it, the same key stops it, no cooldown until it stops.
	 * Mirrors Creature::holdSkillId server-side; the server also ends it on its max duration or a hard CC (stage 3).
	 */
	int32 HoldingSlot = 0;
	virtual void OnHoldEnded() override;

	/**
	 * Predicted cooldown deadline per slot, in world seconds. Index is EquipSlot - 1.
	 *
	 * 🔴 A prediction, not authority. The server owns cooldowns and never sends them
	 * (skill-system.md § States D-4). It can therefore drift — a skill the server refused
	 * for being out of range still looks like it went on cooldown here. GDD R4 accepts
	 * that: it only happens in play the rules already call abnormal, and the missing
	 * S_SKILL_CAST is what reveals it.
	 */
	float SlotCooldownEndsAt[SKILL_SLOT_MAX] = {};

	static constexpr int32 AIM_CIRCLE_SEGMENTS = 48;

	/** Outside the windup arc (70) so a cast and an auto-attack never read as the same ring. */
	static constexpr float CAST_ARC_RADIUS = 100.f;
	static constexpr int32 CAST_ARC_SEGMENTS = 48;

	static constexpr int32 CAST_HINT_DEBUG_KEY = 7120;
};
