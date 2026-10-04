#include "ClientPacketHandler.h"
#include "BufferReader.h"
#include "S1.h"
#include "Game/Item/S1ItemSubsystem.h"
#include "Game/BaseCamp/S1BaseCampSubsystem.h"
#include "S1GameInstance.h"

PacketHandlerFunc GPacketHandler[UINT16_MAX];

bool Handle_INVALID(PacketSessionRef& session, BYTE* buffer, int32 len)
{
	return false;
}

bool Handle_S_LOGIN(PacketSessionRef& session, Protocol::S_LOGIN& pkt)
{
	if (pkt.success() == false || pkt.characters_size() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[LOGIN] failed - characters=%d"), pkt.characters_size());
		return true;
	}

	// H7 (UI 4단계) - 자동 입장하지 않는다. 베이스캠프 화면(S3)이 뜨고, [입장]이 C_ENTER_GAME 을 보낸다.
	// 이전: 여기서 C_ENTER_GAME { playerIndex 0, floor 0 } 을 바로 보냈다 - US1BaseCampSubsystem::RequestEnter 로 옮김
	if (US1BaseCampSubsystem* BaseCamp = US1BaseCampSubsystem::Get())
		BaseCamp->HandleLogin(pkt);

	return true;
}

bool Handle_S_ENTER_GAME(PacketSessionRef& session, Protocol::S_ENTER_GAME& pkt)
{
	if (pkt.success() == false)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ENTER] rejected by server"));

		// H8 - S3 로 돌아가 사유를 보이고 잠금을 푼다
		if (US1BaseCampSubsystem* BaseCamp = US1BaseCampSubsystem::Get())
			BaseCamp->HandleEnterRejected();

		return true;
	}

	// H8 - HandleSpawn 보다 먼저. 지난 런에서 내 폰이 지워졌으면 여기서 새로 만들어 빙의시킨다
	if (US1BaseCampSubsystem* BaseCamp = US1BaseCampSubsystem::Get())
		BaseCamp->HandleEnterAccepted(pkt.player());

	if (auto* GameInstance = Cast<US1GameInstance>(GWorld->GetGameInstance()))
	{
		GameInstance->HandleSpawn(pkt);
	}

	return true;
}

bool Handle_S_LEAVE_GAME(PacketSessionRef& session, Protocol::S_LEAVE_GAME& pkt)
{
	// H9 (UI 4단계) - 런이 끝났다(탈출 커밋 완료 · 사망 후 퇴장). 베이스캠프 화면(S3)으로.
	// 서버가 바로 뒤에 베이스캠프 인벤토리(S_INVENTORY_SYNC)를 보낸다.
	if (US1BaseCampSubsystem* BaseCamp = US1BaseCampSubsystem::Get())
		BaseCamp->HandleLeftGame();

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

		// H10 (UI 4단계) - 내가 죽었으면 잠시 뒤 베이스캠프로 돌아간다(C_LEAVE_GAME)
		if (US1BaseCampSubsystem* BaseCamp = US1BaseCampSubsystem::Get())
			BaseCamp->HandleDied(Info.victim_id());
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
	if (auto* GameInstance = Cast<US1GameInstance>(GWorld->GetGameInstance()))
	{
		GameInstance->HandleSkillCast(pkt);
	}

	return true;
}

bool Handle_S_SKILL_CANCEL(PacketSessionRef& session, Protocol::S_SKILL_CANCEL& pkt)
{
	if (auto* GameInstance = Cast<US1GameInstance>(GWorld->GetGameInstance()))
	{
		GameInstance->HandleSkillCancel(pkt);
	}

	return true;
}

bool Handle_S_SKILL_HIT(PacketSessionRef& session, Protocol::S_SKILL_HIT& pkt)
{
	if (auto* GameInstance = Cast<US1GameInstance>(GWorld->GetGameInstance()))
	{
		GameInstance->HandleSkillHit(pkt);

	}
	return true;
}

bool Handle_S_EQUIP_SYNC(PacketSessionRef& session, Protocol::S_EQUIP_SYNC& pkt)
{
	if (auto* GameInstance = Cast<US1GameInstance>(GWorld->GetGameInstance()))
	{
		GameInstance->HandleEquipSync(pkt);
	}

	if (US1ItemSubsystem* Items = US1ItemSubsystem::Get())
	{
		Items->OnEquipSync();
	}

	return true;
}

bool Handle_S_GRANT_REWARD(PacketSessionRef& session, Protocol::S_GRANT_REWARD& pkt)
{
	return true;
}

bool Handle_S_ITEM_RESULT(PacketSessionRef& session, Protocol::S_ITEM_RESULT& pkt)
{
	FString Line = FString::Printf(TEXT("[ITEM] result=%d changed=%d"),
		static_cast<int32>(pkt.result()), pkt.changed_size());

	for (const Protocol::ItemInstance& Item : pkt.changed())
	{
		Line += FString::Printf(TEXT(" | id=%llu slot=%d state=%d"),
			Item.instance_id(),
			static_cast<int32>(Item.slot()),
			static_cast<int32>(Item.state()));
	}

	if (GEngine != nullptr)
		GEngine->AddOnScreenDebugMessage(-1, 8.f, FColor::Yellow, Line);

	UE_LOG(LogTemp, Warning, TEXT("%s"), *Line);

	if (US1ItemSubsystem* Items = US1ItemSubsystem::Get())
		Items->OnItemResult(pkt);

	return true;
};

bool Handle_S_INVENTORY_SYNC(PacketSessionRef& session, Protocol::S_INVENTORY_SYNC& pkt)
{
	UE_LOG(LogTemp, Warning, TEXT("[ITEM] inventory sync - %d items"), pkt.items_size());

	for (const Protocol::ItemInstance& Item : pkt.items())
	{
		UE_LOG(LogTemp, Warning,
			TEXT(" id=%llu type=%u grade=%d state=%d slot=%d skills=%d"),
			Item.instance_id(), Item.item_type_id(),
			static_cast<int32>(Item.grade()),
			static_cast<int32>(Item.state()),
			static_cast<int32>(Item.slot()),
			Item.skills_size());
	}

	if (US1ItemSubsystem* Items = US1ItemSubsystem::Get())
	{
		Items->OnInventorySync(pkt);
	}

	return true;
}

bool Handle_S_BAG_CONTENTS(PacketSessionRef& session, Protocol::S_BAG_CONTENTS& pkt)
{
	if (auto* GameInstance = Cast<US1GameInstance>(GWorld->GetGameInstance()))
	{
		GameInstance->HandleBagContents(pkt);
	}

	if (US1ItemSubsystem* Items = US1ItemSubsystem::Get())
	{
		Items->OnBagContents(pkt);
	}

	return true;
}
