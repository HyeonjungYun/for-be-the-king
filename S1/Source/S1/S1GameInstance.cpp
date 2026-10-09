// Fill out your copyright notice in the Description page of Project Settings.


#include "S1GameInstance.h"
#include "Sockets.h"
#include "Common/TcpSocketBuilder.h"
#include "Serialization/ArrayWriter.h"
#include "SocketSubsystem.h"
#include "PacketSession.h"
#include "Protocol.pb.h"
#include "ClientPacketHandler.h"
#include "Game/Item/S1ItemSubsystem.h"
#include "S1MyPlayer.h"


void US1GameInstance::Shutdown()
{
	DisconnectFromGameServer();

	Super::Shutdown();
}



void US1GameInstance::ConnectToGameServer()
{
	ConnectToGameServerWithToken(FString());
}

bool US1GameInstance::ConnectToGameServerWithToken(const FString& Token)
{
	Socket = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateSocket(TEXT("Stream"), TEXT("Client Socket"));

	// 서버 쪽 주소 받아오기
	FIPv4Address Ip;
	FIPv4Address::Parse(IpAddress, Ip);

	TSharedRef<FInternetAddr> InternetAddr = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
	InternetAddr->SetIp(Ip.Value);
	InternetAddr->SetPort(Port);

	GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Red, FString::Printf(TEXT("Connecting To Server")));

	bool Connected = Socket->Connect(*InternetAddr);

	if (Connected)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Red, FString::Printf(TEXT("Connection Success")));

		// Session
		GameServerSession = MakeShared<PacketSession>(Socket);
		GameServerSession->Run();

		// 로그인 화면의 토큰 (ADR-0011). 비어 있으면 명령줄 · ini 의 개발 토큰
		{
			Protocol::C_LOGIN Pkt;
			Pkt.set_token(TCHAR_TO_UTF8(*(Token.IsEmpty() ? ResolveLoginToken() : Token)));

			SendBufferRef SendBuffer = ClientPacketHandler::MakeSendBuffer(Pkt);
			SendPacket(SendBuffer);
		}
		return true;
	}

	GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Red, FString::Printf(TEXT("Connection Failed")));

	// 실패한 소켓은 닫는다 - 로그인 화면이 다시 시도하면 새로 만든다 (그대로 두면 다음 시도가 덮어써 새어 나간다)
	ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
	Socket = nullptr;
	return false;
}

void US1GameInstance::DisconnectFromGameServer()
{
	if (Socket == nullptr)
		return;

	if (GameServerSession != nullptr)
	{
		Protocol::C_LEAVE_GAME LeavePkt;
		SEND_PACKET(LeavePkt);

		GameServerSession->Disconnect();
		GameServerSession = nullptr;
	}

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get();
	SocketSubsystem->DestroySocket(Socket);
	Socket = nullptr;

	MyPlayer.Reset();
	Players.Empty();
}

void US1GameInstance::HandleRecvPackets()
{
	if (Socket == nullptr || GameServerSession == nullptr)
		return;

	GameServerSession->HandleRecvPackets();
}

void US1GameInstance::SendPacket(SendBufferRef SendBuffer)
{
	if (Socket == nullptr || GameServerSession == nullptr)
		return;

	GameServerSession->SendPacket(SendBuffer);
}

FString US1GameInstance::ResolveLoginToken() const
{
	FString Token;

	if (FParse::Value(FCommandLine::Get(), TEXT("token="), Token) && Token.Len() == 64)
	{
		UE_LOG(LogTemp, Warning, TEXT("[LOGIN] token from command line"));
		return Token;
	}

	if (GConfig->GetString(TEXT("/Script/S1.S1GameInstance"), TEXT("DevLoginToken"),
		Token, GGameIni) && Token.Len() == 64)
	{
		UE_LOG(LogTemp, Warning, TEXT("[LOGIN] token from DefaultGame.ini"));
		return Token;
	}

	UE_LOG(LogTemp, Error,
		TEXT("[LOGIN] no token configured. Set -token=<64> or DefaultGame.ini DevLoginToken"));

	return FString();
}

void US1GameInstance::HandleSpawn(const Protocol::ObjectInfo& objectInfo, bool IsMine)
{
	if (Socket == nullptr || GameServerSession == nullptr)
		return;

	auto* World = GetWorld();
	if (World == nullptr)
		return;

	if (objectInfo.object_type() == Protocol::OBJECT_TYPE_BAG)
	{
		const Protocol::PosInfo& Pos = objectInfo.pos_info();
		UE_LOG(LogTemp, Log, TEXT("[BAG] spawn id=%llu pos=(%.0f, %.0f)"),
			objectInfo.object_id(), Pos.x(), Pos.y());

		Bags.Add(objectInfo.object_id());

		if (US1ItemSubsystem* Items = GetSubsystem<US1ItemSubsystem>())
		{
			Items->OnBagSpawned(objectInfo);
		}
		return;
	}

	// 중복 처리 체크
	const uint64 ObjectId = objectInfo.object_id();
	if (TWeakObjectPtr<AS1Player>* Found = Players.Find(ObjectId))
	{
		if (Found->IsValid())	// 액터가 살아있으면 중ㅂ고
		{
			// 탈출 뒤 재입장 - 내 폰은 지난 런 그대로 남아 있다(사망 때만 서버가 지운다). 예전엔 여기서 돌아가
			// 새 입장 위치 · 체력(서버는 가득 채운다)을 입히지 않았다 - 체력 바가 지난 런 끝 값으로 남았다 (2026-10-08 에이전트)
			if (IsMine)
			{
				AS1Player* Mine = Found->Get();
				Mine->SetPlayerInfo(objectInfo.pos_info());
				Mine->SetCreatureInfo(objectInfo);
				MyPlayer = Mine;
				UE_LOG(LogTemp, Log, TEXT("[ENTER] reused my pawn id=%llu hp=%d/%d at (%.0f, %.0f)"),
					ObjectId, objectInfo.hp(), objectInfo.max_hp(), objectInfo.pos_info().x(), objectInfo.pos_info().y());
			}
			return;
		}

		Players.Remove(ObjectId);
	}


	FVector SpawnLocation(objectInfo.pos_info().x(), objectInfo.pos_info().y(), objectInfo.pos_info().z());

	if (IsMine)
	{
		auto* PC = UGameplayStatics::GetPlayerController(this, 0);
		AS1Player* Player = Cast<AS1Player>(PC->GetPawn());
		if (Player == nullptr)
			return;

		Player->SetPlayerInfo(objectInfo.pos_info());
		Player->SetCreatureInfo(objectInfo);		// 장비가 더한 최대 체력 (서버 EquipStats, 2026-10-07 에이전트 - YOLO 세션 위임)

		MyPlayer = Player;
		Players.Add(objectInfo.object_id(), Player);
	}
	else
	{
		AS1Player* Player = Cast<AS1Player>(World->SpawnActor(OtherPlayerClass, &SpawnLocation));
		if (Player == nullptr)
			return;

		Player->SetPlayerInfo(objectInfo.pos_info());
		Player->SetCreatureInfo(objectInfo);		// 체력 · 몬스터 외형 (monster-ai.md § H, 2026-10-07 에이전트 - YOLO 세션 위임)
		Players.Add(objectInfo.object_id(), Player);
	}
}

void US1GameInstance::HandleSpawn(const Protocol::S_ENTER_GAME& EnterGamePkt)
{
	HandleSpawn(EnterGamePkt.player(), true);
}

void US1GameInstance::HandleSpawn(const Protocol::S_SPAWN& SpawnPkt)
{
	for (auto& Player : SpawnPkt.players())
	{
		HandleSpawn(Player, false);
	}
}

void US1GameInstance::HandleDespawn(uint64 ObjectId)
{
	if (Socket == nullptr || GameServerSession == nullptr)
		return;

	auto* World = GetWorld();
	if (World == nullptr)
		return;

	if (Bags.Remove(ObjectId) > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("[BAG] despawn id=%llu"), ObjectId);

		// 열어 둔 가방이 사라졌다.
		if (OpenBags.Remove(ObjectId) > 0)
			UE_LOG(LogTemp, Warning, TEXT("[BAG] closed id=%llu - bag gone"), ObjectId);

		if (US1ItemSubsystem* Items = GetSubsystem<US1ItemSubsystem>())
		{
			Items->OnBagDespawned(ObjectId);
		}

		return;
	}

	TWeakObjectPtr<AS1Player>* FindActor = Players.Find(ObjectId);
	if (FindActor == nullptr)
		return;

	if (AS1Player* Actor = FindActor->Get())
		World->DestroyActor(FindActor->Get());

	Players.Remove(ObjectId);
}

void US1GameInstance::ClearDungeonView()
{
	UWorld* World = GetWorld();
	const AS1Player* Mine = MyPlayer.Get();

	// 내 폰은 MyPlayer 약참조 말고 빙의 중인 폰으로도 확인한다 - 약참조가 비어 있어도 조종 중인 폰을 지우지 않게 (2026-10-08 리뷰 ⑥)
	const APlayerController* PC = GetFirstLocalPlayerController();
	const APawn* Possessed = (PC != nullptr) ? PC->GetPawn() : nullptr;

	int32 Actors = 0;
	for (auto It = Players.CreateIterator(); It; ++It)
	{
		AS1Player* Actor = It.Value().Get();
		if (Actor != nullptr && (Actor == Mine || Actor == Possessed))
			continue;

		if (Actor != nullptr && World != nullptr)
			World->DestroyActor(Actor);

		It.RemoveCurrent();
		Actors++;
	}

	// 가방은 HandleDespawn 경로로 - 열린 창 · 아이템 UI 도 같이 닫힌다
	const TArray<uint64> BagIds = Bags.Array();
	for (uint64 BagId : BagIds)
		HandleDespawn(BagId);

	UE_LOG(LogTemp, Log, TEXT("[LEAVE] cleared dungeon view - actors=%d bags=%d"), Actors, BagIds.Num());
}

void US1GameInstance::HandleDespawn(const Protocol::S_DESPAWN& DespawnPkt)
{
	for (auto& ObjectId : DespawnPkt.object_ids())
	{
		HandleDespawn(ObjectId);
	}
}

void US1GameInstance::HandleMove(const Protocol::S_MOVE& MovePkt)
{
	if (Socket == nullptr || GameServerSession == nullptr)
		return;

	auto* World = GetWorld();
	if (World == nullptr)
		return;

	for (const Protocol::PosInfo& Info : MovePkt.infos())
	{

		const uint64 ObjectId = Info.object_id();
		TWeakObjectPtr<AS1Player>* FindActor = Players.Find(ObjectId);

		if (FindActor == nullptr)
			continue;

		AS1Player* Player = (FindActor->Get());
		if (Player == nullptr)
		{
			Players.Remove(ObjectId);
			continue;
		}

		if (Player->IsMyPlayer())
		{
			if (MovePkt.correction())
				Player->SetPlayerInfo(Info);

			continue;
		}

		Player->SetDestInfo(Info);
	}
}

void US1GameInstance::HandleCc(const Protocol::S_CC& CcPkt)
{
	if (Socket == nullptr || GameServerSession == nullptr)
		return;

	auto ApplyTo = [this](const Protocol::CcEventInfo& Info, bool bApplied)
		{
			TWeakObjectPtr<AS1Player>* FindActor = Players.Find(Info.target_id());
			if (FindActor == nullptr)
				return;

			if (AS1Player* Player = FindActor->Get())
				Player->ApplyCcEvent(Info, bApplied);
		};

	for (const Protocol::CcEventInfo& Info : CcPkt.applied())
		ApplyTo(Info, true);

	for (const Protocol::CcEventInfo& Info : CcPkt.expired())
		ApplyTo(Info, false);

	// Knockback / pull - the server moved its copy to the end point; every client (owner and observers) animates
	// the same path so nobody sees a teleport (skill roster stage 1, 2026-10-10)
	for (const Protocol::KnockbackInfo& Info : CcPkt.knockbacks())
	{
		TWeakObjectPtr<AS1Player>* FindActor = Players.Find(Info.target_id());
		if (FindActor == nullptr)
			continue;

		if (AS1Player* Player = FindActor->Get())
			Player->ApplyKnockback(FVector(Info.dir_x(), Info.dir_y(), 0.f), Info.distance(), Info.speed());
	}
}

void US1GameInstance::HandleStatus(const Protocol::S_STATUS& StatusPkt)
{
	if (Socket == nullptr || GameServerSession == nullptr)
		return;

	auto ApplyTo = [this](const Protocol::StatusInfo& Info, bool bApplied)
		{
			TWeakObjectPtr<AS1Player>* FindActor = Players.Find(Info.target_id());
			if (FindActor == nullptr)
				return;

			if (AS1Player* Player = FindActor->Get())
				Player->ApplyStatus(Info, bApplied);
		};

	for (const Protocol::StatusInfo& Info : StatusPkt.applied())
		ApplyTo(Info, true);

	for (const Protocol::StatusInfo& Info : StatusPkt.expired())
		ApplyTo(Info, false);
}

void US1GameInstance::HandleCcState(const Protocol::S_CC_STATE& StatePkt)
{
	if (Socket == nullptr || GameServerSession == nullptr)
		return;

	for (const Protocol::CcStateInfo& State : StatePkt.states())
	{
		TWeakObjectPtr<AS1Player>* FindActor = Players.Find(State.target_id());
		if (FindActor == nullptr)
			continue;

		if (AS1Player* Player = FindActor->Get())
			Player->ApplyCcState(State);
	}
}

void US1GameInstance::HandleDamage(const Protocol::S_DAMAGE& DamagePkt)
{
	if (Socket == nullptr || GameServerSession == nullptr)
		return;

	for (const Protocol::DamageInfo& Info : DamagePkt.damages())
	{
		TWeakObjectPtr<AS1Player>* FindActor = Players.Find(Info.target_id());
		if (FindActor == nullptr)
			continue;

		if (AS1Player* Player = FindActor->Get())
			Player->OnDamaged(Info.damage(), Info.remaining_hp(), Info.is_crit(), Info.max_hp());
	}
}

void US1GameInstance::HandleEquipSync(const Protocol::S_EQUIP_SYNC& EquipPkt)
{
	SkillSlots.Reset(EquipPkt.skills_size());

	for (const Protocol::SkillInfo& Info : EquipPkt.skills())
		SkillSlots.Add(Info);
}

void US1GameInstance::HandleSkillCast(const Protocol::S_SKILL_CAST& CastPkt)
{
	for (const Protocol::SkillCastInfo& Info : CastPkt.casts())
	{
		if (TWeakObjectPtr<AS1Player>* Found = Players.Find(Info.caster_id()))
		{
			if (AS1Player* Caster = Found->Get())
				Caster->OnSkillCastStarted(Info.is_stationary());
		}
	}
}

void US1GameInstance::HandleSkillCancel(const Protocol::S_SKILL_CANCEL& CancelPkt)
{
	for (const uint64 CasterId : CancelPkt.caster_ids())
	{
		if (TWeakObjectPtr<AS1Player>* Found = Players.Find(CasterId))
		{
			if (AS1Player* Caster = Found->Get())
				Caster->OnSkillCastEnded();
		}
	}
}

void US1GameInstance::HandleSkillHit(const Protocol::S_SKILL_HIT& HitPkt)
{
	for (const Protocol::SkillHitInfo& Info : HitPkt.hits())
	{
		if (TWeakObjectPtr<AS1Player>* Found = Players.Find(Info.caster_id()))
		{
			if (AS1Player* Caster = Found->Get())
				Caster->OnSkillCastEnded();
		}

		const FVector Impact(Info.impact_x(), Info.impact_y(), 0.f);
		const FVector Aim(Info.aim_x(), Info.aim_y(), 0.f);

		AS1Player* Target = nullptr;
		if (Info.target_id() != 0)
		{
			if (TWeakObjectPtr<AS1Player>* FoundTarget = Players.Find(Info.target_id()))
				Target = FoundTarget->Get();
		}

		AS1Player::PlaySkillEffect(GetWorld(), Info.effect_id(), Impact, Aim, Target);
	}
}

void US1GameInstance::HandleBagContents(const Protocol::S_BAG_CONTENTS BagPkt)
{
	TArray<uint64>& Items = OpenBags.FindOrAdd(BagPkt.bag_id());
	Items.Reset();

	FString Line = FString::Printf(TEXT("[BAG] contents id=%llu items=%d remain=%llums"),
		BagPkt.bag_id(), BagPkt.items_size(), BagPkt.remaining_ms());

	for (const Protocol::ItemInstance& Item : BagPkt.items())
	{
		Items.Add(Item.instance_id());
		Line += FString::Printf(TEXT(" | id=%llu type=%u g=%d lv=%u skills=%d"),
			Item.instance_id(), Item.item_type_id(),
			static_cast<int32>(Item.grade()), Item.level(), Item.skills_size());
	}

	UE_LOG(LogTemp, Warning, TEXT("%s"), *Line);
}

void US1GameInstance::DebugEquipWeapon()
{
	Protocol::C_EQUIP Pkt;
	Pkt.set_request_id(TCHAR_TO_UTF8(*FGuid::NewGuid().ToString()));
	Pkt.set_instance_id(4);
	Pkt.set_slot(Protocol::SLOT_WEAPON_PRIMARY);

	SEND_PACKET(Pkt);
}

void US1GameInstance::DebugUnequipWeapon()
{
	Protocol::C_UNEQUIP Pkt;
	Pkt.set_request_id(TCHAR_TO_UTF8(*FGuid::NewGuid().ToString()));
	Pkt.set_instance_id(4);

	SEND_PACKET(Pkt);
}

void US1GameInstance::DebugEquipIdempotent()
{
	Protocol::C_EQUIP Pkt;
	Pkt.set_request_id("debug-fixed-request-id");
	Pkt.set_instance_id(5);
	Pkt.set_slot(Protocol::SLOT_BOOTS);

	{ SEND_PACKET(Pkt); }
	{ SEND_PACKET(Pkt); }
}

void US1GameInstance::DebugEquipBadSlot()
{
	Protocol::C_EQUIP Pkt;
	Pkt.set_request_id(TCHAR_TO_UTF8(*FGuid::NewGuid().ToString()));
	Pkt.set_instance_id(4);
	Pkt.set_slot(Protocol::SLOT_WEAPON_SECONDARY);

	SEND_PACKET(Pkt);
}

void US1GameInstance::DebugEquip(int64 InstanceId, int32 Slot)
{
	Protocol::C_EQUIP Pkt;
	Pkt.set_request_id(TCHAR_TO_UTF8(*FGuid::NewGuid().ToString()));
	Pkt.set_instance_id(static_cast<uint64>(InstanceId));
	Pkt.set_slot(static_cast<Protocol::EquipSlot>(Slot));

	UE_LOG(LogTemp, Warning, TEXT("[DEBUG] C_EQUIP id=%lld slot=%d"), InstanceId, Slot);
	SEND_PACKET(Pkt);
}

void US1GameInstance::DebugUnequip(int64 InstanceId)
{
	Protocol::C_UNEQUIP Pkt;
	Pkt.set_request_id(TCHAR_TO_UTF8(*FGuid::NewGuid().ToString()));
	Pkt.set_instance_id(static_cast<uint64>(InstanceId));

	UE_LOG(LogTemp, Warning, TEXT("[DEBUG] C_UNEQUIP id=%lld"), InstanceId);
	SEND_PACKET(Pkt);
}

void US1GameInstance::DebugBag()
{
	Protocol::C_CHAT Pkt;
	Pkt.set_msg("/bag");

	UE_LOG(LogTemp, Warning, TEXT("[DEBUG] C_CHAT /bag"));
	SEND_PACKET(Pkt);
}

void US1GameInstance::DebugUnbag(int64 BagId)
{
	Protocol::C_CHAT Pkt;
	Pkt.set_msg(TCHAR_TO_UTF8(*FString::Printf(TEXT("/unbag %lld"), BagId)));

	UE_LOG(LogTemp, Warning, TEXT("[DEBUG] C_CHAT /unbag %lld"), BagId);
	SEND_PACKET(Pkt);
}

void US1GameInstance::DebugDie()
{
	Protocol::C_CHAT Pkt;
	Pkt.set_msg("/die");

	UE_LOG(LogTemp, Warning, TEXT("[DEBUG] C_CHAT /die"));
	SEND_PACKET(Pkt);
}

void US1GameInstance::DebugOpenBag(int64 BagId)
{
	Protocol::C_BAG_OPEN Pkt;
	Pkt.set_bag_id(static_cast<uint64>(BagId));

	UE_LOG(LogTemp, Warning, TEXT("[DEBUG] C_BAG_OPEN bag=%lld"), BagId);
	SEND_PACKET(Pkt);
}

void US1GameInstance::DebugLoot(int64 BagId, int64 InstanceId)
{
	Protocol::C_LOOT Pkt;
	Pkt.set_request_id(TCHAR_TO_UTF8(*FGuid::NewGuid().ToString()));
	Pkt.set_bag_id(static_cast<uint64>(BagId));
	Pkt.set_instance_id(static_cast<uint64>(InstanceId));

	UE_LOG(LogTemp, Warning, TEXT("[DEBUG] C_LOOT bag=%lld item=%lld"), BagId, InstanceId);
	SEND_PACKET(Pkt);
}

void US1GameInstance::DebugLootAll(int64 BagId)
{
	const TArray<uint64>* Items = OpenBags.Find(static_cast<uint64>(BagId));
	if (Items == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DEBUG] LootAll bag=%lld - open it first"), BagId);
		return;
	}

	// "전체 획득" = 개별 C_LOOT N개. 부분 루팅 결과는 S_ITEM_RESULT 의 INVENTORY_FULL 개수로 안다 (AC-IE-32)
	const TArray<uint64> Snapshot = *Items;
	for (uint64 InstanceId : Snapshot)
		DebugLoot(BagId, static_cast<int64>(InstanceId));
}

void US1GameInstance::DebugDrop(int64 InstanceId)
{
	Protocol::C_DROP Pkt;
	Pkt.set_request_id(TCHAR_TO_UTF8(*FGuid::NewGuid().ToString()));
	Pkt.set_instance_id(static_cast<uint64>(InstanceId));

	UE_LOG(LogTemp, Warning, TEXT("[DEBUG] C_DROP id=%lld"), InstanceId);
	SEND_PACKET(Pkt);
}

void US1GameInstance::DebugLeave()
{
	Protocol::C_LEAVE_GAME Pkt;

	UE_LOG(LogTemp, Warning, TEXT("[DEBUG] C_LEAVE_GAME"));
	SEND_PACKET(Pkt);
}

void US1GameInstance::DebugEnter()
{
	Protocol::C_ENTER_GAME Pkt;
	Pkt.set_playerindex(0);
	Pkt.set_floor_id(0);

	UE_LOG(LogTemp, Warning, TEXT("[DEBUG] C_ENTER_GAME"));
	SEND_PACKET(Pkt);
}

void US1GameInstance::DebugVault(int64 InstanceId)
{
	Protocol::C_STASH Pkt;
	Pkt.set_request_id(TCHAR_TO_UTF8(*FGuid::NewGuid().ToString()));
	Pkt.set_instance_id(static_cast<uint64>(InstanceId));
	Pkt.set_to_vault(true);

	UE_LOG(LogTemp, Warning, TEXT("[DEBUG] C_STASH vault id=%lld"), InstanceId);
	SEND_PACKET(Pkt);
}

void US1GameInstance::DebugUnvault(int64 InstanceId)
{
	Protocol::C_STASH Pkt;
	Pkt.set_request_id(TCHAR_TO_UTF8(*FGuid::NewGuid().ToString()));
	Pkt.set_instance_id(static_cast<uint64>(InstanceId));
	Pkt.set_to_vault(false);

	UE_LOG(LogTemp, Warning, TEXT("[DEBUG] C_STASH unvault id=%lld"), InstanceId);
	SEND_PACKET(Pkt);
}

void US1GameInstance::DebugEscape()
{
	Protocol::C_CHAT Pkt;
	Pkt.set_msg("/escape");

	UE_LOG(LogTemp, Warning, TEXT("[DEBUG] C_CHAT /escape"));
	SEND_PACKET(Pkt);
}
