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
class UStaticMeshComponent;

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

/** One status line on a character (S_STATUS): a dot with its stacks, a buff with its magnitude, or a mark. Plain struct — not reflected. */
struct FS1StatusEntry
{
	Protocol::StatusKind Kind = Protocol::STATUS_NONE;
	FString Param;
	uint64 InstigatorId = 0;
	uint32 Stacks = 0;
	float Magnitude = 0.f;
	float Until = 0.f;
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

	/** Advances an in-flight dash. Stops dead on a blocking hit — movement-camera.md § Dashing. */
	void TickDash(float DeltaTime);

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

	/**
	 * Spawn snapshot beyond position — current/max HP and, for monsters, what to draw (monster-ai.md § H).
	 * Monsters reuse this remote-proxy class so every id-based handler (move, attack, damage, death) works on them
	 * unchanged; only the look differs: size from the server radius, a teal ground disc, a name label.
	 */
	void SetCreatureInfo(const Protocol::ObjectInfo& Info);

	bool IsMonster() const { return bIsMonster; }

	/** The monster's display name (String Table) — empty for players. Drawn by US1WorldLabelWidget, not here. */
	const FText& GetMonsterName() const { return MonsterName; }
	bool IsDown() const { return CurrentHp <= 0; }

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
	 * Root lets you swing, everything else does not — same set as CanTurn.
	 * Mirrors Creature::CanAttack server-side (combat-system.md Rule 5 table).
	 * Silence is not checked here: it blocks skills only, never the basic attack.
	 */
	bool CanAttack() const;

	/**
	 * Displacement API the skill system calls — movement-camera.md Formula 3.
	 *
	 * 🔴 The client moves the character; the server only opens a speed-validation window
	 * for the duration (movement-camera.md § Interactions: "이동기 판정=스킬, 위치변경
	 * 실행=이동"). That is why this is not a prediction to be corrected — within the
	 * window, this is where the position comes from.
	 *
	 * @param Direction  Normalised, XY only.
	 * @param DistCm     Total travel. Ends early on a wall.
	 * @param SpeedCms   Must satisfy DistCm / SpeedCms <= 0.4s (skill-system.md S2).
	 * @param bIgnoresWalls  skill_effects.csv ignores_walls (유령 신랑의 저주): moves unswept, and the end
	 *                       point is pushed out of any wall it lands in (forward first, then back the way it came).
	 * @param bThroughPawns  move param dash_through (그림자 베기, 2026-10-11): characters and monsters do not stop the
	 *                       dash - walls still do. The capsule ignores the Pawn channel for the dash only.
	 */
	void RequestDash(const FVector& Direction, float DistCm, float SpeedCms, bool bIgnoresWalls = false, bool bThroughPawns = false);

	/** True while a dash is travelling. Walk input is ignored throughout. */
	bool IsDashing() const { return DashRemainingCm > 0.f; }

	/**
	 * A server-driven push — knockback, pull, the lethal-save leap (S_CC.knockbacks, skill roster stage 1).
	 *
	 * 🔴 The server already moved its copy to the end point and ignores our C_MOVE while the knockback CC runs;
	 * this animates the same path locally so the two meet at the same spot. Remote proxies run it too, which is
	 * what keeps a 300 cm push from reading as a teleport when the next S_MOVE arrives 33 ms later.
	 */
	void ApplyKnockback(const FVector& Direction, float DistCm, float SpeedCms);

	/**
	 * S_STATUS — a damage-over-time, buff or mark on this character. Display only: the server owns every number
	 * and ticks every effect; the client just shows what it was told (skill roster stage 1, 2026-10-10).
	 */
	void ApplyStatus(const Protocol::StatusInfo& Info, bool bApplied);
	void ClearStatuses();

	/** Sum of active move_speed buffs — goes into MaxWalkSpeed the same way the server adds it to its ceiling. */
	float GetMoveSpeedBuff() const { return MoveSpeedBuff; }

	/** Blind (soft CC) — US1BlindOverlayWidget blacks out all but GetBlindViewScale() of the view around the local player. */
	bool IsBlinded() const;
	float GetBlindViewScale() const { return BlindViewScale; }

	/**
	 * Active CC as words over the head — "기절 · 둔화" (2026-10-11 "뇌섬 실명이 적용 안 됨" · "슬로우가 안 걸림": the effects
	 * landed but a fixed dummy shows none of them). Empty when nothing is active. US1WorldLabelWidget paints it (Hangul font).
	 */
	FText GetCcLabel() const;

	/** A short line over this character for Seconds — "대상을 클릭하세요" and other reasons a key did nothing. Owner only. */
	void ShowFloatingNotice(const FText& Text, float Seconds = 1.6f);
	const FText* GetFloatingNotice() const;

	/** The server ended a hold skill on this character (S_STATUS hold expired). The local player starts its cooldown here. */
	virtual void OnHoldEnded() {}

	/** 총명 (#91): this character's revealed loadout, shown above them for Seconds (TEMP text; the owner's own String Table names). */
	void ShowRevealedSkills(const FString& Line, float Seconds);

	/**
	 * A server-confirmed hit. The client never computes HP or decides whether a hit landed
	 * (Rule 10) — it only displays what arrived.
	 *
	 * @param NewMaxHp  DamageInfo.max_hp — 0 means unchanged. The server fills it on the damage-0 resync it
	 *                  sends after an equip change inside the dungeon, so every client's bar uses the new max.
	 * @param bMagic    Magic damage draws violet — a proc landing with the swing (혼원일체) reads as its own number (2026-10-11).
	 */
	void OnDamaged(int32 Damage, int32 RemainingHp, bool bIsCrit, int32 NewMaxHp = 0, bool bMagic = false);

	/**
	 * Someone started casting. Everyone sees this, including enemies.
	 *
	 * 🔴 All an observer gets is "that one is busy" and whether they are rooted to the spot.
	 * No skill_id, no cast_ms, no range — the server does not send them
	 * (skill-system.md B2a). Guessing what is coming is the fight.
	 */
	void OnSkillCastStarted(bool bStationary);

	/** The cast ended — completed, cancelled, or interrupted. Observers cannot tell which. */
	void OnSkillCastEnded();

	/**
	 * Plays the visual for a skill that landed.
	 *
	 * 🔴 Takes effect_id, never skill_id. That is the entire information-asymmetry contract
	 * (equipment-skill-binding.md B8): observers learn what it looked like, never what it
	 * was. Keeping skill_id out of the signature makes the leak structurally impossible
	 * rather than a rule someone has to remember.
	 *
	 * Static because the caster may be out of view, or dead, or never have been spawned on
	 * this client — the effect still has to play at the impact point.
	 *
	 * @param Aim     Where the caster aimed — lines, cones and projectiles are drawn toward it (2026-10-10).
	 * @param Target  The actor hit, or null for an area skill that hit nobody.
	 */
	static void PlaySkillEffect(UWorld* World, uint32 EffectId, const FVector& Impact, const FVector& Aim, AS1Player* Target);

	/**
	 * A hit drawn on one struck target (2026-10-11): a bolt from the sky for lightning (낙뢰), violet ghost rings for the
	 * ghost proc (혼원일체), an impact star for anything else (저 멀리 wall slam). Same effect_id rule as PlaySkillEffect.
	 */
	static void PlayTargetStrike(UWorld* World, uint32 EffectId, AS1Player* Target);

protected:
	class Protocol::PosInfo* PlayerInfo; // 현재 위치
	class Protocol::PosInfo* DestInfo; // 목적치

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

	/** Dash state. Only ever set on the locally controlled player — remotes just interpolate. */
	FVector DashDirection = FVector::ZeroVector;
	float DashRemainingCm = 0.f;
	float DashSpeedCms = 0.f;
	bool bDashIgnoresWalls = false;		// 유령 신랑의 저주 - unswept, end point resolved out of walls
	bool bDashThroughPawns = false;		// 그림자 베기 - Pawn channel ignored while dashing, restored at the end
	FVector DashStartLocation = FVector::ZeroVector;

	/** Ends a dash and gives the capsule its Pawn response back if the dash had dropped it. */
	void FinishDash();

	/** Wall-passing dash just ended: if the capsule sits inside blocking geometry, step forward (then back) to the first free spot. */
	void ResolveDashEndOutOfWalls(bool bForwardFirst = true);

	/** A wall-passing dash step that would cross the coast walls stops against them — true if it did. */
	bool StopAtCoast(float StepCm);

	/**
	 * Casting marker shown to everyone. Server-driven, not predicted — this is what other
	 * players see, so it has to match what the server actually broadcast.
	 */
	bool bCastMarkerVisible = false;
	bool bCastMarkerStationary = false;

	/**
	 * Server-authoritative CC mirrored as world-time deadlines. These drift a little from
	 * the server clock, which is fine — the server still rejects anything it disagrees
	 * with, and S_CC_STATE resyncs them periodically. Never extend these locally.
	 */
	float StunUntil = 0.f;
	float RootUntil = 0.f;
	float KnockbackUntil = 0.f;
	float LaunchUntil = 0.f;

	/** Taunt (2026-10-10): the server walks us toward the taunter and sends corrections; we only stop input, attacks and facing. */
	float TauntUntil = 0.f;

	/** Silence - read for the CC label only (the server refuses the skill itself). */
	float SilenceUntil = 0.f;

	/**
	 * Airborne (launch) - the mesh rises and falls on a parabola for the CC's length (2026-10-11 "에어본이 되었다는 효과 -
	 * 캐릭터를 실제로 위로 띄우기"). The capsule stays on the ground: the server's position is 2D and nothing else moves.
	 */
	float LaunchStart = 0.f;
	float LaunchDuration = 0.f;
	FVector MeshBaseOffset = FVector::ZeroVector;
	bool bMeshBaseCaptured = false;
	void TickAirborne();

	FText FloatingNotice;
	float FloatingNoticeUntil = 0.f;

	/** [0, soft_cc_slow_cap]. 0 means unslowed. */
	float ActiveSlow = 0.f;

	float BlindUntil = 0.f;
	float BlindViewScale = 1.f;

	/** Status readout lines. Keyed by kind + param + instigator — a refresh overwrites, a stack change updates. */
	TArray<FS1StatusEntry> Statuses;
	float MoveSpeedBuff = 0.f;

	FString RevealedSkills;
	float RevealedUntil = 0.f;

	/** TEMP — ASCII text above the health bar (the debug font has no Hangul). Replaced by the HUD later. */
	void DrawStatusDebug();

	/** Base walk speed captured at construction so slow is applied against a fixed value. */
	float BaseWalkSpeed = 340.f;

	/** Turn off in BP when the debug bars get in the way of a screenshot. */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowCombatDebug = true;

	/**
	 * Naked baseline (entities.yaml: base_stats_naked) until the server's snapshot arrives —
	 * ObjectInfo.hp / max_hp on S_ENTER_GAME and S_SPAWN, with equipment already added (EquipStats).
	 * An equip change inside the dungeon arrives as S_DAMAGE { damage 0, remaining_hp, max_hp } (2026-10-08) —
	 * before that field existed the bar was off by the item's max_hp share until the next run.
	 */
	int32 CurrentHp = 500;
	int32 MaxHp = 500;
	int32 DamagePopupCount = 0;

	/** Set by SetCreatureInfo from ObjectInfo.creature_type. */
	bool bIsMonster = false;
	uint32 MonsterTypeId = 0;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> MonsterDisc = nullptr;

	/**
	 * Added to the server Z for a scaled monster. Scaling grows the capsule around its centre, so at the server's
	 * standing height (z 100) a 1.31x pack leader sank 26 cm into the floor and its disc vanished under it — the boss
	 * at 2.62x sank more than a metre (2026-10-11 screenshot). Lifting by HalfHeight x (scale - 1) puts every size's
	 * feet where a player's are.
	 */
	float GroundLiftCm = 0.f;

	/**
	 * Not a UTextRenderComponent: its default font (RobotoDistanceField) has no Hangul and every name
	 * rendered as boxes (2026-10-07 screenshot). The label is painted in screen space with the UI font.
	 */
	FText MonsterName;

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

	/** Cast marker ring at the feet. Larger than the windup arc (70) so the two never merge. */
	static constexpr float CAST_MARKER_RADIUS = 120.f;

	/**
	 * How long a skill effect stays on screen. Long enough to notice and place, short
	 * enough that a busy fight does not turn into a pile of overlapping circles.
	 */
	static constexpr float EFFECT_LIFETIME_SEC = 0.4f;

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