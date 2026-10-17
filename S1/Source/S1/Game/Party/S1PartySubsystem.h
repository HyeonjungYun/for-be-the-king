// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "TimerManager.h"
#include "S1PartySubsystem.generated.h"

namespace Protocol
{
	class S_PARTY_STATE;
	class S_PARTY_INVITED;
	class S_PARTY_BETRAYED;
}

class US1PartyInviteWidget;

/** One party member as the server said (S_PARTY_STATE). */
struct FS1PartyMemberView
{
	uint64	CharacterId = 0;
	FString	Name;
	bool	bLeader = false;
	int32	State = 0;		// Protocol::PartyMemberState - 0 base camp · 1 in a run · 2 offline
};

/** The run modes S3 offers - the same values as Protocol::RunMode. */
enum class ES1RunMode : uint8
{
	Pvp = 0,
	PveSolo = 1,
	PveCoop = 2,
};

DECLARE_MULTICAST_DELEGATE(FOnS1PartyChanged);

/** S3's [입장] for the chosen mode and party — pure, so the table is tested (S1.Party.Logic). The server refuses the same. */
namespace S1PartyLogic
{
	enum class EEnterBlock : uint8 { None, CoopOnly, WaitForLeader, WaitForParty };

	/**
	 * A party of 2+ enters PVE co-op only (D7). In co-op a member waits for the leader to open the run, then joins it;
	 * whoever already entered this run waits for the party to come back (one entry per run, D3).
	 */
	S1_API EEnterBlock EnterBlock(ES1RunMode Mode, int32 PartySize, bool bLeader, bool bPartyInRun, bool bEnteredThisRun = false);

	/** 입장 · 입장 중 · 함께 입장 (co-op leader) · 합류 (party run alive) · 파티장 대기 (member) · 파티 귀환 대기 — String Table keys. */
	S1_API const TCHAR* EnterLabelKey(ES1RunMode Mode, int32 PartySize, bool bLeader, bool bPartyInRun, bool bEntering, bool bEnteredThisRun = false);
}

/**
 * PVE co-op party on the client (ADR-0014 · design/quick-specs/pve-modes-2026-10-11.md).
 *
 * Holds what the server last said about my party (S_PARTY_STATE) and the invite I was sent (S_PARTY_INVITED), and
 * sends the requests. The server decides everything - this only mirrors it. The invite popup is shown from here so it
 * appears wherever base camp is (S3 or S4 on top of it).
 *
 * Allies in a run: the co-op members at S_ENTER_GAME plus anyone in the party since - the server's team is fixed at
 * entry, so someone who left the party mid-run is still an ally there; the client keeps them too until the run ends.
 */
UCLASS()
class S1_API US1PartySubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:

	static US1PartySubsystem* Get();
	virtual void Deinitialize() override;

	// ── Netcode hooks ────────────────────────────────────────────────────
	void HandleLogin(uint64 InMyCharacterId);
	void HandleState(const Protocol::S_PARTY_STATE& Pkt);
	void HandleInvited(const Protocol::S_PARTY_INVITED& Pkt);
	/** 던전 파티 배신 방송 (party-betrayal 2026-10-12): 토스트 + 배신자 표식. 파티원 목록은 따로 오는 S_PARTY_STATE 가 바꾼다 */
	void HandleBetrayed(const Protocol::S_PARTY_BETRAYED& Pkt);
	void HandleEnterGame(ES1RunMode Mode);
	void HandleLeftGame();

	// ── UI ───────────────────────────────────────────────────────────────
	void Invite(const FString& Name);
	void RespondToInvite(bool bAccept);
	void Leave();
	void Kick(uint64 CharacterId);

	bool	IsInParty() const { return PartyId != 0 && Members.Num() > 0; }
	/** PvP 층 런 안의 파티 = 던전 파티 (베이스캠프 파티는 PvP 에 못 들어온다) - 나가기 = 배신 */
	bool	IsDungeonParty() const { return RunMode == ES1RunMode::Pvp && bInDungeon && IsInParty(); }
	bool	IsInDungeon() const { return bInDungeon; }
	int32	Size() const { return Members.Num(); }
	bool	IsLeader() const;
	bool	IsInRun() const { return bInRun; }					// the party's instance is alive - members may join
	bool	HasEnteredThisRun() const { return bEnteredThisRun; }	// I already went into the party's live run (one entry per run)
	uint64	GetMyCharacterId() const { return MyCharacterId; }
	const TArray<FS1PartyMemberView>& GetMembers() const { return Members; }

	/** An ally in my current run (PVE co-op only). Not me. */
	bool	IsAlly(uint64 ObjectId) const;

	/**
	 * The betrayer mark (party-betrayal R6, 2026-10-13 user decision · art-bible 배신자 표식 규칙): only someone the betrayer
	 * left behind sees it, for BetrayerMarkSec. Kept here by object id, not on the actor, so it survives the betrayer leaving
	 * my sight and coming back (a new actor). Cleared when my run ends.
	 */
	bool	GetBetrayerMark(uint64 ObjectId, FString& OutName) const;
	static constexpr double BetrayerMarkSec = 60.0;
	FString	NameOf(uint64 CharacterId) const;

	bool			HasInvite() const { return InvitePartyId != 0; }
	const FString&	GetInviterName() const { return InviterName; }
	double			GetInviteExpiresAt() const { return InviteExpiresAt; }

	/** The outcome of my last request, as text ("" when it was OK) and when it came. */
	const FText&	GetLastResultText() const { return LastResultText; }
	double			GetLastResultAt() const { return LastResultAt; }
	/** bInRun: inside a PvP run the "not found" / "not visible" wording is about this floor, not the base camp (party-betrayal R2). */
	static FText	ResultText(int32 Result, const FString& Subject, bool bInRun);

	FOnS1PartyChanged OnPartyChanged;

private:
	void ShowInvitePopup(bool bShow);
	void ExpireInvite();
	void ApplyAllies();

	uint64						MyCharacterId = 0;
	uint64						PartyId = 0;
	bool						bInRun = false;
	bool						bEnteredThisRun = false;
	TArray<FS1PartyMemberView>	Members;

	uint64						InvitePartyId = 0;
	FString						InviterName;
	double						InviteExpiresAt = 0.0;
	FTimerHandle				InviteTimer;

	FText						LastResultText;
	double						LastResultAt = 0.0;

	ES1RunMode					RunMode = ES1RunMode::Pvp;		// of the run I am in (S_ENTER_GAME)
	bool						bInDungeon = false;				// between S_ENTER_GAME and S_LEAVE_GAME
	TMap<uint64, FString>		RunAllies;

	struct FBetrayerMark { FString Name; double ExpiresAt = 0.0; };
	TMap<uint64, FBetrayerMark>	BetrayerMarks;					// object id -> mark I see (I was betrayed)

	UPROPERTY() TObjectPtr<US1PartyInviteWidget>	InvitePopup;
};
