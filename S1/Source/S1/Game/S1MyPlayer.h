// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Game/S1Player.h"
#include "S1MyPlayer.generated.h"

class UInputMappingContext;
class UInputAction;

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

	virtual void Tick(float DeltaTime) override;

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

public:

	FORCEINLINE class USpringArmComponent* GetCameraBoom() const { return CameraBoom; }

	FORCEINLINE class UCameraComponent* GetFollowCamera() const { return FollowCamera; }

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
