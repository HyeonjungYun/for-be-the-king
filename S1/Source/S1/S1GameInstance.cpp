// Fill out your copyright notice in the Description page of Project Settings.


#include "S1GameInstance.h"
#include "Sockets.h"
#include "Common/TcpSocketBuilder.h"
#include "Serialization/ArrayWriter.h"
#include "SocketSubsystem.h"
#include "PacketSession.h"
#include "Protocol.pb.h"
#include "ClientPacketHandler.h"
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

void US1GameInstance::DebugChatCCStun()
{
	Protocol::C_CHAT pkt;
	pkt.set_msg("/stun 3 1250");

	SEND_PACKET(pkt);
}

void US1GameInstance::DebugChatCCSRoot()
{
	Protocol::C_CHAT pkt;
	pkt.set_msg("/root 3 1250");

	SEND_PACKET(pkt);
}

void US1GameInstance::DebugChatCCSlow4()
{
	Protocol::C_CHAT pkt;
	pkt.set_msg("/slow 3 5000 0.4");

	SEND_PACKET(pkt);
}

void US1GameInstance::DebugChatCCSlow3()
{
	Protocol::C_CHAT pkt;
	pkt.set_msg("/slow 3 5000 0.3");

	SEND_PACKET(pkt);
}

void US1GameInstance::DebugChatCCSlow2()
{
	Protocol::C_CHAT pkt;
	pkt.set_msg("/slow 3 5000 0.2");

	SEND_PACKET(pkt);
}

void US1GameInstance::DebugChatCCSlow15()
{
	Protocol::C_CHAT pkt;
	pkt.set_msg("/slow 3 5000 0.15");

	SEND_PACKET(pkt);
}
