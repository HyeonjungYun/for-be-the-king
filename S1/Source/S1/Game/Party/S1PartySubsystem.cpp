// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Party/S1PartySubsystem.h"
#include "Game/UI/S1ItemUIStyle.h"
#include "Game/UI/S1PartyWidgets.h"
#include "Game/Escape/S1EscapeSubsystem.h"
#include "Game/S1Player.h"
#include "S1.h"
#include "S1GameInstance.h"
#include "Protocol.pb.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

namespace
{
	/** Above S3 (20) and S4 (30) — an invite must be seen whichever base-camp screen is up. */
	constexpr int32 InvitePopupZOrder = 40;
}

S1PartyLogic::EEnterBlock S1PartyLogic::EnterBlock(ES1RunMode Mode, int32 PartySize, bool bLeader, bool bPartyInRun, bool bEnteredThisRun)
{
	if (Mode != ES1RunMode::PveCoop)
		return PartySize >= 2 ? EEnterBlock::CoopOnly : EEnterBlock::None;
	if (PartySize >= 2 && bPartyInRun && bEnteredThisRun)
		return EEnterBlock::WaitForParty;
	if (PartySize >= 2 && bLeader == false && bPartyInRun == false)
		return EEnterBlock::WaitForLeader;
	return EEnterBlock::None;
}

const TCHAR* S1PartyLogic::EnterLabelKey(ES1RunMode Mode, int32 PartySize, bool bLeader, bool bPartyInRun, bool bEntering, bool bEnteredThisRun)
{
	if (bEntering)
		return TEXT("label.entering");
	if (Mode != ES1RunMode::PveCoop || PartySize < 2)
		return TEXT("label.enter");
	if (bPartyInRun)
		return bEnteredThisRun ? TEXT("label.enter_coop_back") : TEXT("label.enter_coop_join");
	return bLeader ? TEXT("label.enter_coop_leader") : TEXT("label.enter_coop_wait");
}

US1PartySubsystem* US1PartySubsystem::Get()
{
	if (GWorld == nullptr)
		return nullptr;

	UGameInstance* GameInstance = GWorld->GetGameInstance();
	return GameInstance != nullptr ? GameInstance->GetSubsystem<US1PartySubsystem>() : nullptr;
}

void US1PartySubsystem::Deinitialize()
{
	if (UGameInstance* GameInstance = GetGameInstance())
		GameInstance->GetTimerManager().ClearTimer(InviteTimer);

	InvitePopup = nullptr;
	Super::Deinitialize();
}

// ── Netcode hooks ────────────────────────────────────────────────────────

void US1PartySubsystem::HandleLogin(uint64 InMyCharacterId)
{
	MyCharacterId = InMyCharacterId;
}

void US1PartySubsystem::HandleState(const Protocol::S_PARTY_STATE& Pkt)
{
	// The party's run ended (or I am in another party now) - the next run is a new one I may enter
	if (Pkt.in_run() == false || Pkt.party_id() != PartyId)
		bEnteredThisRun = false;

	PartyId = Pkt.party_id();
	bInRun = Pkt.in_run();

	Members.Reset();
	for (const Protocol::PartyMember& M : Pkt.members())
	{
		FS1PartyMemberView& View = Members.AddDefaulted_GetRef();
		View.CharacterId = M.character_id();
		View.Name = UTF8_TO_TCHAR(M.name().c_str());
		View.bLeader = M.leader();
		View.State = static_cast<int32>(M.state());

		// Someone who joined the party during my co-op run is an ally there too
		if (RunMode == ES1RunMode::PveCoop && View.CharacterId != MyCharacterId)
			RunAllies.Add(View.CharacterId, View.Name);
	}

	if (Pkt.result() != Protocol::PARTY_OK)
	{
		LastResultText = ResultText(static_cast<int32>(Pkt.result()), UTF8_TO_TCHAR(Pkt.subject_name().c_str()), bInDungeon);
		LastResultAt = FPlatformTime::Seconds();
	}
	else if (Pkt.subject_name().empty() == false)
	{
		// OK about someone: my own name = I joined (accepted an invite), another name = my invite went out
		const FString Subject = UTF8_TO_TCHAR(Pkt.subject_name().c_str());
		const bool bMe = Subject == NameOf(MyCharacterId);
		LastResultText = FText::Format(S1ItemUIStyle::Text(bMe ? TEXT("party.result.joined") : TEXT("party.result.invited_sent")), FText::FromString(Subject));
		LastResultAt = FPlatformTime::Seconds();
	}

	UE_LOG(LogTemp, Log, TEXT("[PARTY] state party=%llu members=%d in_run=%d result=%d"), PartyId, Members.Num(), bInRun ? 1 : 0,
		static_cast<int32>(Pkt.result()));

	ApplyAllies();
	OnPartyChanged.Broadcast();
}

void US1PartySubsystem::HandleInvited(const Protocol::S_PARTY_INVITED& Pkt)
{
	InvitePartyId = Pkt.party_id();
	InviterName = UTF8_TO_TCHAR(Pkt.from_name().c_str());
	InviteExpiresAt = FPlatformTime::Seconds() + Pkt.expires_ms() / 1000.0;

	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().SetTimer(InviteTimer, FTimerDelegate::CreateUObject(this, &US1PartySubsystem::ExpireInvite),
			FMath::Max(Pkt.expires_ms() / 1000.f, 0.1f), false);
	}

	UE_LOG(LogTemp, Log, TEXT("[PARTY] invited by %s party=%llu"), *InviterName, InvitePartyId);
	ShowInvitePopup(true);
	OnPartyChanged.Broadcast();
}

void US1PartySubsystem::HandleEnterGame(ES1RunMode Mode)
{
	RunMode = Mode;
	bInDungeon = true;
	RunAllies.Reset();
	if (Mode == ES1RunMode::PveCoop)
	{
		bEnteredThisRun = true;
		for (const FS1PartyMemberView& M : Members)
		{
			if (M.CharacterId != MyCharacterId)
				RunAllies.Add(M.CharacterId, M.Name);
		}
	}
	ApplyAllies();
}

void US1PartySubsystem::HandleLeftGame()
{
	bInDungeon = false;
	RunMode = ES1RunMode::Pvp;
	RunAllies.Reset();
	BetrayerMarks.Reset();
}

bool US1PartySubsystem::GetBetrayerMark(uint64 ObjectId, FString& OutName) const
{
	const FBetrayerMark* Mark = BetrayerMarks.Find(ObjectId);
	if (Mark == nullptr || FPlatformTime::Seconds() >= Mark->ExpiresAt)
		return false;
	OutName = Mark->Name;
	return true;
}

// ── UI ───────────────────────────────────────────────────────────────────

void US1PartySubsystem::Invite(const FString& Name)
{
	const FString Trimmed = Name.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
		return;

	Protocol::C_PARTY_INVITE Pkt;
	Pkt.set_target_name(TCHAR_TO_UTF8(*Trimmed));
	SEND_PACKET(Pkt);
	UE_LOG(LogTemp, Log, TEXT("[PARTY] invite %s"), *Trimmed);
}

void US1PartySubsystem::RespondToInvite(bool bAccept)
{
	if (InvitePartyId == 0)
		return;

	Protocol::C_PARTY_RESPONSE Pkt;
	Pkt.set_party_id(InvitePartyId);
	Pkt.set_accept(bAccept);
	SEND_PACKET(Pkt);
	UE_LOG(LogTemp, Log, TEXT("[PARTY] %s invite party=%llu"), bAccept ? TEXT("accept") : TEXT("decline"), InvitePartyId);

	InvitePartyId = 0;
	if (UGameInstance* GameInstance = GetGameInstance())
		GameInstance->GetTimerManager().ClearTimer(InviteTimer);
	ShowInvitePopup(false);
	OnPartyChanged.Broadcast();
}

void US1PartySubsystem::Leave()
{
	Protocol::C_PARTY_LEAVE Pkt;
	Pkt.set_character_id(0);
	SEND_PACKET(Pkt);
}

void US1PartySubsystem::Kick(uint64 CharacterId)
{
	Protocol::C_PARTY_LEAVE Pkt;
	Pkt.set_character_id(CharacterId);
	SEND_PACKET(Pkt);
}

bool US1PartySubsystem::IsLeader() const
{
	for (const FS1PartyMemberView& M : Members)
	{
		if (M.CharacterId == MyCharacterId)
			return M.bLeader;
	}
	return false;
}

bool US1PartySubsystem::IsAlly(uint64 ObjectId) const
{
	if (ObjectId == MyCharacterId)
		return false;
	if (RunMode == ES1RunMode::PveCoop)
		return RunAllies.Contains(ObjectId);
	// PvP 층 던전 파티 (party-betrayal R3): 지금 파티원이 아군 - 나가면(배신) 그 순간 S_PARTY_STATE 로 빠진다
	if (RunMode == ES1RunMode::Pvp && bInDungeon)
		return Members.ContainsByPredicate([ObjectId](const FS1PartyMemberView& M) { return M.CharacterId == ObjectId; });
	return false;
}

FString US1PartySubsystem::NameOf(uint64 CharacterId) const
{
	if (const FString* Found = RunAllies.Find(CharacterId))
		return *Found;
	for (const FS1PartyMemberView& M : Members)
	{
		if (M.CharacterId == CharacterId)
			return M.Name;
	}
	return FString();
}

FText US1PartySubsystem::ResultText(int32 Result, const FString& Subject, bool bInRun)
{
	const TCHAR* Key = nullptr;
	switch (static_cast<Protocol::PartyResult>(Result))
	{
	case Protocol::PARTY_NOT_FOUND:			Key = bInRun ? TEXT("party.result.not_found_run") : TEXT("party.result.not_found"); break;
	case Protocol::PARTY_FULL:				Key = TEXT("party.result.full"); break;
	case Protocol::PARTY_ALREADY_IN_PARTY:	Key = TEXT("party.result.already"); break;
	case Protocol::PARTY_DECLINED:			Key = TEXT("party.result.declined"); break;
	case Protocol::PARTY_EXPIRED:			Key = TEXT("party.result.expired"); break;
	case Protocol::PARTY_NOT_LEADER:		Key = TEXT("party.result.not_leader"); break;
	case Protocol::PARTY_BUSY:				Key = TEXT("party.result.busy"); break;
	case Protocol::PARTY_SELF:				Key = TEXT("party.result.self"); break;
	case Protocol::PARTY_NO_INSTANCE:		Key = TEXT("party.result.no_instance"); break;
	case Protocol::PARTY_ALREADY_ENTERED:	Key = TEXT("party.result.already_entered"); break;
	case Protocol::PARTY_COOP_ONLY:			Key = TEXT("party.result.coop_only"); break;
	case Protocol::PARTY_NOT_VISIBLE:		Key = TEXT("party.result.not_visible"); break;		// party-betrayal R2: 시야 안의 플레이어만
	case Protocol::PARTY_NO_KICK_IN_RUN:	Key = TEXT("party.result.no_kick_in_run"); break;	// party-betrayal R4: 던전 안 추방 없음
	case Protocol::PARTY_LONE_PATH:			Key = TEXT("party.result.lone_path"); break;		// 고독한 길 (2026-10-17): 착용 중엔 파티 불가
	default:								return FText::GetEmpty();
	}
	return FText::Format(S1ItemUIStyle::Text(Key), FText::FromString(Subject));
}

// ── Internals ────────────────────────────────────────────────────────────

void US1PartySubsystem::ShowInvitePopup(bool bShow)
{
	UGameInstance* GameInstance = GetGameInstance();
	APlayerController* PC = GameInstance != nullptr ? GameInstance->GetFirstLocalPlayerController() : nullptr;

	if (bShow)
	{
		if (PC == nullptr)
			return;
		if (InvitePopup == nullptr)
			InvitePopup = CreateWidget<US1PartyInviteWidget>(PC, US1PartyInviteWidget::StaticClass());
		if (InvitePopup != nullptr && InvitePopup->IsInViewport() == false)
			InvitePopup->AddToViewport(InvitePopupZOrder);
		return;
	}

	if (InvitePopup != nullptr && InvitePopup->IsInViewport())
		InvitePopup->RemoveFromParent();
}

void US1PartySubsystem::ExpireInvite()
{
	if (InvitePartyId == 0)
		return;

	InvitePartyId = 0;
	ShowInvitePopup(false);
	OnPartyChanged.Broadcast();
}

void US1PartySubsystem::HandleBetrayed(const Protocol::S_PARTY_BETRAYED& Pkt)
{
	const FString Name = UTF8_TO_TCHAR(Pkt.betrayer_name().c_str());
	const FString MyName = NameOf(MyCharacterId);
	bool bVictim = false;
	for (const std::string& V : Pkt.victim_names())
		bVictim |= (UTF8_TO_TCHAR(V.c_str()) == MyName);

	// 표식 "배신자 이름" - 배신당한 파티원(나)에게만, 1 분 (2026-10-13 사용자 결정 · art-bible 배신자 표식 규칙 1 · 2). 제3자는 토스트만 본다
	if (bVictim)
		BetrayerMarks.Add(Pkt.betrayer_id(), FBetrayerMark{ Name, FPlatformTime::Seconds() + BetrayerMarkSec });

	// 토스트 - 파티원이었으면 "X 가 배신했다!", 아니면 "X 가 파티를 배신했다" (party-betrayal § 7 토스트 5 s)
	constexpr float BetrayToastSec = 5.f;
	if (US1EscapeSubsystem* Escape = US1EscapeSubsystem::Get())
		Escape->ShowNotice(FText::Format(S1ItemUIStyle::Text(bVictim ? TEXT("party.betrayed_me") : TEXT("party.betrayed_other")), FText::FromString(Name)), BetrayToastSec, bVictim);

	UE_LOG(LogTemp, Log, TEXT("[PARTY] betrayed by %s (%llu) party=%llu victims=%d me=%d"), *Name, Pkt.betrayer_id(), Pkt.party_id(), Pkt.victim_names_size(), bVictim ? 1 : 0);
	OnPartyChanged.Broadcast();
}

void US1PartySubsystem::ApplyAllies()
{
	US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance());
	if (GameInstance == nullptr)
		return;

	for (const TPair<uint64, TWeakObjectPtr<AS1Player>>& Pair : GameInstance->Players)
	{
		AS1Player* Actor = Pair.Value.Get();
		if (Actor == nullptr || Actor->IsMonster())
			continue;

		const bool bAlly = IsAlly(Pair.Key);
		if (bAlly != Actor->IsAlly())
			Actor->SetAlly(bAlly, NameOf(Pair.Key));
	}
}
