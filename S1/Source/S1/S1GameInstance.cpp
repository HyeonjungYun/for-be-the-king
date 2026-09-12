// Fill out your copyright notice in the Description page of Project Settings.


#include "S1GameInstance.h"
#include "Sockets.h"
#include "Common/TcpSocketBuilder.h"
#include "Serialization/ArrayWriter.h"
#include "SocketSubsystem.h"
#include "PacketSession.h"
#include "Protocol.pb.h"
#include "ClientPacketHandler.h"
#include "HAL/PlatformMisc.h"
#include "S1MyPlayer.h"


void US1GameInstance::Shutdown()
{
	DisconnectFromGameServer();

	Super::Shutdown();
}



void US1GameInstance::ConnectToGameServer()
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

		// TEMP: Lobby에서 캐릭터 선택창 등
		{
			Protocol::C_LOGIN Pkt;
			Pkt.set_token(TCHAR_TO_UTF8(*ResolveLoginToken()));

			SendBufferRef SendBuffer = ClientPacketHandler::MakeSendBuffer(Pkt);
			SendPacket(SendBuffer);
		}
	}
	else
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Red, FString::Printf(TEXT("Connection Failed")));
	}
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

	// 중복 처리 체크
	const uint64 ObjectId = objectInfo.object_id();
	if (TWeakObjectPtr<AS1Player>* Found = Players.Find(ObjectId))
	{
		if (Found->IsValid())	// 액터가 살아있으면 중ㅂ고
			return;

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

		MyPlayer = Player;
		Players.Add(objectInfo.object_id(), Player);
	}
	else
	{
		AS1Player* Player = Cast<AS1Player>(World->SpawnActor(OtherPlayerClass, &SpawnLocation));
		if (Player == nullptr)
			return;

		Player->SetPlayerInfo(objectInfo.pos_info());
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

	// TODO : Despawn

	TWeakObjectPtr<AS1Player>* FindActor = Players.Find(ObjectId);
	if (FindActor == nullptr)
		return;

	if (AS1Player* Actor = FindActor->Get())
		World->DestroyActor(FindActor->Get());

	Players.Remove(ObjectId);
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
			Player->OnDamaged(Info.damage(), Info.remaining_hp(), Info.is_crit());
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

		AS1Player* Target = nullptr;
		if (Info.target_id() != 0)
		{
			if (TWeakObjectPtr<AS1Player>* FoundTarget = Players.Find(Info.target_id()))
				Target = FoundTarget->Get();
		}

		AS1Player::PlaySkillEffect(GetWorld(), Info.effect_id(), Impact, Target);
	}
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
