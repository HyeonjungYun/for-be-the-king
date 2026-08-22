#include "ClientPacketHandler.h"
#include "BufferReader.h"
#include "S1.h"
#include "S1GameInstance.h"

PacketHandlerFunc GPacketHandler[UINT16_MAX];

bool Handle_INVALID(PacketSessionRef& session, BYTE* buffer, int32 len)
{
	return false;
}

bool Handle_S_LOGIN(PacketSessionRef& session, Protocol::S_LOGIN& pkt)
{
	for (auto& Player : pkt.players())
	{
	}

	for (int32 i = 0; i < pkt.players_size(); i++)
	{
		const Protocol::ObjectInfo& Player = pkt.players(i);
	}

	// 로비에서 캐릭터 선택해서 인덱스 전송
	Protocol::C_ENTER_GAME EnterGamePkt;
	EnterGamePkt.set_playerindex(0);
	SEND_PACKET(EnterGamePkt);

	return true;
}

bool Handle_S_ENTER_GAME(PacketSessionRef& session, Protocol::S_ENTER_GAME& pkt)
{
	if (auto* GameInstance = Cast<US1GameInstance>(GWorld->GetGameInstance()))
	{
		GameInstance->HandleSpawn(pkt);
	}

	return true;
}

bool Handle_S_LEAVE_GAME(PacketSessionRef& session, Protocol::S_LEAVE_GAME& pkt)
{
	if (auto* GameInstance = Cast<US1GameInstance>(GWorld->GetGameInstance()))
	{
		// TODO : 게임 종료? 로비로?
	}

	return true;
}

bool Handle_S_SPAWN(PacketSessionRef& session, Protocol::S_SPAWN& pkt)
{
	if (auto* GameInstance = Cast<US1GameInstance>(GWorld->GetGameInstance()))
	{
		GameInstance->HandleSpawn(pkt);
	}
	return true;
}

bool Handle_S_DESPAWN(PacketSessionRef& session, Protocol::S_DESPAWN& pkt)
{
	if (auto* GameInstance = Cast<US1GameInstance>(GWorld->GetGameInstance()))
	{
		GameInstance->HandleDespawn(pkt);
	}

	return true;
}

bool Handle_S_MOVE(PacketSessionRef& session, Protocol::S_MOVE& pkt)
{
	if (auto* GameInstance = Cast<US1GameInstance>(GWorld->GetGameInstance()))
	{
		GameInstance->HandleMove(pkt);
	}
	return true;
}

bool Handle_S_ATTACK(PacketSessionRef& session, Protocol::S_ATTACK& pkt)
{
	for (const Protocol::AttackInfo& Info : pkt.attacks())
	{
		UE_LOG(LogTemp, Warning, TEXT("[ATTACK] %llu -> %llu windup=%ums"),
			Info.attacker_id(), Info.target_id(), Info.windup_ms());
	}
	return true;
}

bool Handle_S_ATTACK_CANCEL(PacketSessionRef& session, Protocol::S_ATTACK_CANCEL& pkt)
{
	return true;
}

bool Handle_S_DAMAGE(PacketSessionRef& session, Protocol::S_DAMAGE& pkt)
{
	if (auto* GameInstance = Cast<US1GameInstance>(GWorld->GetGameInstance()))
	{
		GameInstance->HandleDamage(pkt);
	}

	for (const Protocol::DamageInfo& Info : pkt.damages())
	{
		UE_LOG(LogTemp, Warning, TEXT("[DAMAGE] %llu -> %llu dmg-%d hp=%d crit=%d"),
			Info.attacker_id(), Info.target_id(), Info.damage(), Info.remaining_hp(), Info.is_crit() ? 1 : 0);
	}
	return true;
}

bool Handle_S_CC(PacketSessionRef& session, Protocol::S_CC& pkt)
{
	if (auto* GameInstance = Cast<US1GameInstance>(GWorld->GetGameInstance()))
	{
		GameInstance->HandleCc(pkt);
	}

	for (const Protocol::CcEventInfo& Info : pkt.applied())
	{
		UE_LOG(LogTemp, Warning, TEXT("[CC+] target=%llu type=%d dur=%ums mag=%.2f"),
			Info.target_id(), static_cast<int32>(Info.cc_type()),
			Info.duration_ms(), Info.magnitude());
	}

	for (const Protocol::CcEventInfo& Info : pkt.expired())
	{
		UE_LOG(LogTemp, Warning, TEXT("[CC-] target=%llu type=%d"),
			Info.target_id(), static_cast<int32>(Info.cc_type()));
	}

	return true;
}

namespace
{
	// 로그를 눈으로 읽으려고 두는 것. 숫자만 찍으면 매번 Enum.proto 를 열어봐야 한다.
	const TCHAR* CcTypeName(Protocol::CcType Type)
	{
		switch (Type)
		{
		case Protocol::CC_TYPE_STUN:			return TEXT("STUN");
		case Protocol::CC_TYPE_ROOT:			return TEXT("ROOT");
		case Protocol::CC_TYPE_KNOCKBACK:		return TEXT("KNOCKBACK");
		case Protocol::CC_TYPE_LAUNCH:			return TEXT("LAUNCH");
		case Protocol::CC_TYPE_SLOW:			return TEXT("SLOW");
		case Protocol::CC_TYPE_HEAL_REDUCTION:	return TEXT("HEAL_REDUC");
		case Protocol::CC_TYPE_SILENCE:			return TEXT("SILENCE");
		default:								return TEXT("NONE");
		}
	}
}

bool Handle_S_CC_STATE(PacketSessionRef& session, Protocol::S_CC_STATE& pkt)
{
	if (auto* GameInstance = Cast<US1GameInstance>(GWorld->GetGameInstance()))
	{
		GameInstance->HandleCcState(pkt);		
	}

	for (const Protocol::CcStateInfo& State : pkt.states())
	{
		FString Slots;

		for (const Protocol::CcSlot& Slot : State.slots())
		{
			Slots += FString::Printf(TEXT("%s(%ums"), CcTypeName(Slot.cc_type()), Slot.remaining_ms());
			Slots += (Slot.magnitude() > 0.f)
				? FString::Printf(TEXT(",%.2f) "), Slot.magnitude())
				: TEXT(") ");
		}

		UE_LOG(LogTemp, Verbose, TEXT("[CC STATE] target=%llu slow=%.2f  %s"),
			State.target_id(), State.active_slow(), *Slots);
	}
	return true;
}

bool Handle_S_DIED(PacketSessionRef& session, Protocol::S_DIED& pkt)
{
	for (const Protocol::DiedInfo& Info : pkt.deaths())
	{
		UE_LOG(LogTemp, Error, TEXT("[DIED] victim=%llu by=%llu"),
			Info.victim_id(), Info.instigator_id());
	}
	return true;
}

bool Handle_S_CHAT(PacketSessionRef& session, Protocol::S_CHAT& pkt)
{
	auto Msg = pkt.msg();

	return true;
}

bool Handle_S_SKILL_CAST(PacketSessionRef& session, Protocol::S_SKILL_CAST& pkt)
{
	return true;
}

bool Handle_S_SKILL_CANCEL(PacketSessionRef& session, Protocol::S_SKILL_CANCEL& pkt)
{
	return true;
}

bool Handle_S_SKILL_HIT(PacketSessionRef& session, Protocol::S_SKILL_HIT& pkt)
{
	return true;
}

bool Handle_S_EQUIP_SYNC(PacketSessionRef& session, Protocol::S_EQUIP_SYNC& pkt)
{
	return true;
}

