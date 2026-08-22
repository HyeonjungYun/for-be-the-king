// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "InputActionValue.h"
#include "Protocol.pb.h"
#include "S1Player.generated.h"

class USpringArmComponent;
class UCameraComponent;
class UInputAction;
struct FInputActionValue;

class UTextRenderComponent;

/**
 * One floating damage number, backed by its own text component.
 *
 * DrawDebugString was the obvious choice and it does not work here: every string goes into
 * a single engine-managed list keyed by actor, and what actually renders is decided inside
 * AHUD. Giving each number its own component removes that shared state entirely — each one
 * moves, ages and dies on its own, which is the whole requirement.
 */
USTRUCT()
struct FDamagePopup
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TObjectPtr<UTextRenderComponent> Text = nullptr;

	/** Sideways offset in screen space, so simultaneous hits do not land on each other. */
	float ScreenOffsetX = 0.f;

	float SpawnedAt = 0.f;
	bool bActive = false;
};

UCLASS()
class S1_API AS1Player : public ACharacter
{
	GENERATED_BODY()

public:

	AS1Player();
	virtual ~AS1Player();

	Protocol::MoveState GetMoveState() { return PlayerInfo->state(); }
	void SetMoveState(Protocol::MoveState State);

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

	void TickRemotePlayer(float DeltaTime);
	void ResetInterpolation(Protocol::MoveState State);

	/** Pushes the mirrored CC state into the movement component. Called every frame. */
	void TickCc();

	/**
	 * Debug-only combat readout drawn with DrawDebug*. This exists to verify the P1 loop
	 * ("3 players kill each other for 10 minutes"), not to ship — the real HUD waits on
	 * design/ux/hud.md. It deliberately ignores the art bible's UI rules because none of
	 * this survives to production.
	 */
	void DrawCombatDebug();

	/** Advances every live popup, redraws it, and drops the expired ones. */
	void TickDamagePopups();

public:
	bool IsMyPlayer();

public:
	void SetPlayerInfo(const Protocol::PosInfo& Info);
	void SetDestInfo(const Protocol::PosInfo& Info);
	Protocol::PosInfo* GetPlayerInfo() { return PlayerInfo; };

public:
	/**
	 * Mirrors a server CC event locally. The client never decides duration or whether the
	 * CC lands — it only reflects what the server already decided, so that movement and
	 * facing stop immediately instead of waiting for a rejection round trip.
	 * See design/gdd/combat-system.md Rule 5 (CC table) and Rule 10 (server authority).
	 */
	void ApplyCcEvent(const Protocol::CcEventInfo& Info, bool bApplied);

	/**
	 * Overwrites every CC deadline from a server snapshot. Events alone cannot express a
	 * partial expiry — when one of several slow sources runs out the aggregate drops but no
	 * event fires — so the snapshot is what keeps the client from drifting.
	 * This overwrites rather than merges on purpose; see the comment in the body.
	 */
	void ApplyCcState(const Protocol::CcStateInfo& State);

	/** Aggregated slow from the server (Formula C5). Drives MaxWalkSpeed, never the reverse. */
	void SetActiveSlow(float InActiveSlow);

	/** Hard CC — all four types stop movement. */
	bool CanMove() const;

	/** Root is the exception: it pins you in place but lets you keep aiming. */
	bool CanTurn() const;

	/**
	 * A server-confirmed hit. The client never computes HP or decides whether a hit landed
	 * (Rule 10) — it only displays what arrived.
	 */
	void OnDamaged(int32 Damage, int32 RemainingHp, bool bIsCrit);

protected:
	class Protocol::PosInfo* PlayerInfo; // ���� ��ġ
	class Protocol::PosInfo* DestInfo; // ����ġ

	// 30Hz. movement-camera.md Core Rule 10, entities.yaml move_packet_send_rate.
	// The server's validation margin (1.15) and min_delta_t (16.6ms = half of this)
	// are both derived from a 33ms interval — they mean nothing at the old 5Hz.
	// InterpDuration for remote proxies also reads this value, so both move together.
	const float MOVE_PACKET_SEND_DELAY = 1.f / 30.f;
	const float IDLE_SNAP_DURATION = 0.1f;
	const float TELEPORT_THERESHOLD = 250.0f;

	FVector InterpStartLocation = FVector::ZeroVector;
	float InterpStartYaw = 0.f;
	float InterpElapsed = 0.f;
	float InterpDuration = 0.f;

	/**
	 * Server-authoritative CC mirrored as world-time deadlines. These drift a little from
	 * the server clock, which is fine — the server still rejects anything it disagrees
	 * with, and S_CC_STATE resyncs them periodically. Never extend these locally.
	 */
	float StunUntil = 0.f;
	float RootUntil = 0.f;
	float KnockbackUntil = 0.f;
	float LaunchUntil = 0.f;

	/** [0, soft_cc_slow_cap]. 0 means unslowed. */
	float ActiveSlow = 0.f;

	/** Base walk speed captured at construction so slow is applied against a fixed value. */
	float BaseWalkSpeed = 340.f;

	/** Turn off in BP when the debug bars get in the way of a screenshot. */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowCombatDebug = true;

	/**
	 * MaxHp is hardcoded to the naked baseline (entities.yaml: base_stats_naked). The server
	 * never sends it because nothing changes it yet — equipment does, so this becomes a
	 * protocol field in P2. Adding it now would only mean changing it twice.
	 */
	int32 CurrentHp = 500;
	int32 MaxHp = 500;
	int32 DamagePopupCount = 0;

	/** Pooled — components are reused rather than created and destroyed per hit. */
	UPROPERTY(Transient)
	TArray<FDamagePopup> DamagePopups;

	/**
	 * Must exceed the attack interval or numbers never coexist. At attack speed 1.0 the
	 * interval is 1.0s, so a 1.2s lifetime left only a 0.2s window where two were on screen
	 * — which reads as "only one ever appears" even though every hit spawns its own.
	 * 2.5s keeps two or three up at once, which is what makes the per-hit feedback legible.
	 */
	static constexpr float DAMAGE_POPUP_SECONDS = 2.5f;

	/**
	 * Both offsets are in *screen* space, derived from the camera each frame. Using world
	 * axes was the earlier mistake: at pitch -60 both world +X and world +Z project onto
	 * screen-up, so the sideways spread and the rise fought each other and numbers crossed
	 * over one another instead of separating.
	 */
	static constexpr float DAMAGE_POPUP_RISE = 160.f;
	static constexpr float DAMAGE_POPUP_SPREAD = 45.f;
	static constexpr float DAMAGE_POPUP_BASE_HEIGHT = 120.f;
	static constexpr float DAMAGE_POPUP_TEXT_SIZE = 48.f;

	/** Hard cap so a burst of hits cannot turn the screen into a wall of numbers. */
	static constexpr int32 DAMAGE_POPUP_MAX = 8;

	static constexpr float HEALTH_BAR_WIDTH = 80.f;

	/** Flip the sign if the bar lands below the character instead of above it. */
	static constexpr float DEBUG_BAR_OFFSET_X = -70.f;

	// Cache
	FVector2D DesiredInput;
	FVector DesiredMoveDirection;
	float DesiredYaw;

	// Dirty Flag Test
	FVector2D LastDesiredInput;
};

inline FVector ToVector(const Protocol::PosInfo& Info)
{
	return FVector(Info.x(), Info.y(), Info.z());
}