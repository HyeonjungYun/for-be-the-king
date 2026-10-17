#include "pch.h"
#include "ServerPacketHandler.h"
#include "BufferReader.h"
#include "BufferWriter.h"
#include "ObjectUtils.h"
#include "Room.h"
#include "Player.h"
#include "AccountManager.h"
#include "ItemTable.h"
#include "EntryPointTable.h"
#include "EscapeLogic.h"
#include "GameSessionManager.h"
#include "InstanceManager.h"
#include "PartyManager.h"
#include "BaseCampTable.h"
#include <sstream>
#include <thread>

PacketHandlerFunc GPacketHandler[UINT16_MAX];

namespace
{
	// 지급 결과를 세션에 돌려준다. DB 스레드에서 호출
	void SendGrantResult(GameSessionRef session, const string& requestId, Protocol::GrantResult result, uint64 goldAfter)
	{
		Protocol::S_GRANT_REWARD pkt;
		pkt.set_request_id(requestId);
		pkt.set_result(result);
		pkt.set_gold_after(goldAfter);

		SEND_PACKET_DECLARATION(pkt);
		session->Send(sendBuffer);
	}

	// 캐릭터의 아이템을 전부 읽어 Player에 싣는다.
	// 이후 던전 내에서는 DB를 치지 않는다.
	// ownedRows = 이 캐릭터가 소유한 행 수 (착용 · 소지 · 창고 전부). 0 이면 기본 장비를 받는다 - 사망 가방의 행은
	// 주인이 NULL 이라 세지 않는다 (ObjectUtils CommitDeath)
	bool LoadPlayerItems(DBConnection* conn, PlayerRef player, OUT int32& ownedRows)
	{
		ownedRows = 0;

		const uint64 characterId = player->objectInfo->object_id();

		char query[512];

		if (conn->BeginTransaction() == false)
		{
			LOG_LINE << "[ENTER] begin failed: " << conn->GetError() << '\n';
			return false;
		}

		::snprintf(query, sizeof(query),
			"SELECT character_id FROM characters WHERE character_id = %llu FOR UPDATE", characterId);

		MYSQL_RES* lockRes = conn->Query(query);
		if (lockRes == nullptr)
		{
			LOG_LINE << "[ENTER] lock failed: " << conn->GetError() << '\n';
			conn->Rollback();
			return false;
		}
		conn->FreeResult(lockRes);

		::snprintf(query, sizeof(query),
			"SELECT instance_id, item_type_id, grade, level, state, slot, "
			"skill_id_primary, skill_id_secondary, lone_path FROM item_instances "
			"WHERE owner_character_id = %llu",
			characterId);

		MYSQL_RES* res = conn->Query(query);
		if (res == nullptr)
		{
			LOG_LINE << "[ENTER] item query failed: " << conn->GetError() << '\n';
			conn->Rollback();
			return false;
		}

		enum { COL_ID = 0, COL_TYPE, COL_GRADE, COL_LEVEL, COL_STATE, COL_SLOT, COL_SKILL1, COL_SKILL2, COL_LONE, COL_COUNT };

		if (::mysql_num_fields(res) < COL_COUNT)
		{
			LOG_LINE << "[ENTER] item column mismatch" << '\n';
			conn->FreeResult(res);
			conn->Rollback();
			return false;
		}

		while (MYSQL_ROW row = ::mysql_fetch_row(res))
		{
			ownedRows++;

			ItemEntry e;
			e.instanceId = ::strtoull(row[COL_ID], nullptr, 10);
			e.itemTypeId = static_cast<uint32>(::atoi(row[COL_TYPE]));
			e.grade = static_cast<Protocol::ItemGrade>(::atoi(row[COL_GRADE]));
			e.level = static_cast<uint32>(::atoi(row[COL_LEVEL]));
			e.skillIdPrimary = static_cast<uint32>(::atoi(row[COL_SKILL1]));
			e.skillIdSecondary = static_cast<uint32>(::atoi(row[COL_SKILL2]));
			e.lonePath = ::atoi(row[COL_LONE]) != 0;

			const int32 state = ::atoi(row[COL_STATE]);
			const int32 slot = ::atoi(row[COL_SLOT]);

			if (state == Protocol::ITEM_STATE_EQUIPPED && slot >= Protocol::SLOT_WEAPON_PRIMARY && slot <= Protocol::SLOT_TRINKET)
			{
				player->equipped[slot - 1] = e;
			}
			else if (state == Protocol::ITEM_STATE_CARRIED)
			{
				if (player->inventory.size() >= Player::INVENTORY_SLOT_COUNT)
					LOG_LINE << "[ENTER] inventory overflow - char=" << player->objectInfo->object_id() << '\n';

				player->inventory.push_back(e);
			}
		}

		conn->FreeResult(res);
		conn->Commit();
		return true;
	}

	enum class BaseCampOp : uint8 { ToVault, ToCarried, Destroy, Equip, Unequip, Enhance };

	const int32 VAULT_SLOT_COUNT = 25;

	const char* BaseCampOpName(BaseCampOp op)
	{
		switch (op)
		{
		case BaseCampOp::ToVault: return "vault";
		case BaseCampOp::ToCarried: return "unvault";
		case BaseCampOp::Equip: return "equip";
		case BaseCampOp::Unequip: return "unequip";
		case BaseCampOp::Enhance: return "enhance";
		default: return "destroy";
		}
	}

	// 아이템 한 행을 잠그며 읽는다. 조건에 맞는 행이 없으면 found = false. 쿼리가 실패하면 false
	bool LockItemRow(DBConnection* conn, const string& where, OUT ItemEntry& e, OUT int32& state, OUT int32& slot, OUT bool& found)
	{
		const string query =
			"SELECT instance_id, item_type_id, grade, level, state, slot, skill_id_primary, skill_id_secondary, lone_path "
			"FROM item_instances WHERE " + where + " FOR UPDATE";

		MYSQL_RES* res = conn->Query(query.c_str());
		if (res == nullptr)
			return false;

		found = false;
		if (MYSQL_ROW row = ::mysql_fetch_row(res))
		{
			e.instanceId = ::strtoull(row[0], nullptr, 10);
			e.itemTypeId = static_cast<uint32>(::atoi(row[1]));
			e.grade = static_cast<Protocol::ItemGrade>(::atoi(row[2]));
			e.level = static_cast<uint32>(::atoi(row[3]));
			state = ::atoi(row[4]);
			slot = ::atoi(row[5]);
			e.skillIdPrimary = static_cast<uint32>(::atoi(row[6]));
			e.skillIdSecondary = static_cast<uint32>(::atoi(row[7]));
			e.lonePath = ::atoi(row[8]) != 0;
			found = true;
		}
		conn->FreeResult(res);
		return true;
	}

	// 캐릭터가 가진 state 아이템 개수 - 용량 검사용. 쿼리가 실패하면 false
	bool CountItems(DBConnection* conn, uint64 ownerId, Protocol::ItemState state, OUT uint64& count)
	{
		char query[256];
		::snprintf(query, sizeof(query),
			"SELECT COUNT(*) FROM item_instances WHERE owner_character_id = %llu AND state = %d",
			ownerId, static_cast<int32>(state));

		MYSQL_RES* res = conn->Query(query);
		if (res == nullptr)
			return false;

		count = 0;
		if (MYSQL_ROW row = ::mysql_fetch_row(res))
			count = ::strtoull(row[0], nullptr, 10);
		conn->FreeResult(res);
		return true;
	}

	Protocol::ItemResult RunBaseCampItemOp(DBConnection* conn, GameSessionRef session, uint64 instanceId, BaseCampOp op, Protocol::EquipSlot equipSlot, OUT vector<Protocol::ItemInstance>& changed)
	{
		char query[512];

		// 주인 - 클라는 instance_id만 보낸다. 주인은 서버가 자기 데이터에서 찾는다.
		uint64 ownerId = 0;
		::snprintf(query, sizeof(query),
			"SELECT owner_character_id FROM item_instances WHERE instance_id = %llu", instanceId);

		MYSQL_RES* ownerRes = conn->Query(query);
		if (ownerRes == nullptr)
			return Protocol::ITEM_DB_ERROR;

		if (MYSQL_ROW row = ::mysql_fetch_row(ownerRes))
			ownerId = (row[0] != nullptr) ? ::strtoull(row[0], nullptr, 10) : 0;
		conn->FreeResult(ownerRes);

		// 남의 아이템, 가방 속 아이템은 없는 것과 같이 답한다 - 존재 여부를 흘리지 않는다.
		bool mine = false;
		for (const Protocol::CharacterInfo& c : session->characters)
			if (ownerId != 0 && c.object_info().object_id() == ownerId)
				mine = true;

		if (mine == false)
			return Protocol::ITEM_NOT_FOUND;

		if (conn->BeginTransaction() == false)
			return Protocol::ITEM_DB_ERROR;

		// 캐릭터 잠금
		::snprintf(query, sizeof(query),
			"SELECT character_id FROM characters WHERE character_id = %llu FOR UPDATE", ownerId);

		MYSQL_RES* lockRes = conn->Query(query);
		if (lockRes == nullptr)
		{
			conn->Rollback();
			return Protocol::ITEM_DB_ERROR;
		}
		conn->FreeResult(lockRes);

		// 잠근 뒤에 다시 본다 - 그 사이 입장을 시작했거나(Entering) 사망 커밋이 진행 중이면 손대지 않는다.
		if (session->place.load() != SessionPlace::BaseCamp || GDeathCommits.IsPending(ownerId) || GDetachedBodies.Contains(ownerId))
		{
			conn->Rollback();
			return Protocol::ITEM_INVALID_STATE;
		}

		// 검증 전용 - 잠금은 쥔 채 늦춰, 입장 적재가 이 커밋을 기다리는지 본다.
		const uint32 delayMs = GBaseCampDelayMs.load();
		if (delayMs > 0)
			this_thread::sleep_for(chrono::milliseconds(delayMs));

		// 아이템 현재 상태 (행 잠금)
		ItemEntry e;
		int32 state = -1;
		int32 slot = 0;
		bool found = false;

		if (LockItemRow(conn, "instance_id = " + to_string(instanceId) + " AND owner_character_id = " + to_string(ownerId), OUT e, OUT state, OUT slot, OUT found) == false)
		{
			conn->Rollback();
			return Protocol::ITEM_DB_ERROR;
		}

		// 주인 확인과 잠금 사이에 주인이 바뀌었다.
		if (found == false)
		{
			conn->Rollback();
			return Protocol::ITEM_NOT_FOUND;
		}

		// 규칙 - 이 아이템의 목표 상태, 슬롯을 정하고, 착용 교체면 밀려나는 아이템을 찾는다.
		Protocol::ItemResult result = Protocol::ITEM_OK;
		Protocol::ItemState target = Protocol::ITEM_STATE_NONE;
		int32 targetSlot = 0;
		ItemEntry outgoing;			// 착용 교체로 소지품이 되는 아이템. 없으면 instanceId 0
		
		switch (op)
		{
		case BaseCampOp::Destroy:
			// 착용품은 해제부터. Carried, Vaulted만 곧바로 파기
			if (state != Protocol::ITEM_STATE_CARRIED && state != Protocol::ITEM_STATE_VAULTED)
				result = Protocol::ITEM_INVALID_STATE;
			break;
		case BaseCampOp::ToVault:
		case BaseCampOp::ToCarried:
		{
			const Protocol::ItemState from = (op == BaseCampOp::ToVault) ? Protocol::ITEM_STATE_CARRIED : Protocol::ITEM_STATE_VAULTED;
			target = (op == BaseCampOp::ToVault) ? Protocol::ITEM_STATE_VAULTED : Protocol::ITEM_STATE_CARRIED;
			const uint64 capacity = (target == Protocol::ITEM_STATE_VAULTED) ? VAULT_SLOT_COUNT : Player::INVENTORY_SLOT_COUNT;

			uint64 count = 0;
			if (state == target)
				result = Protocol::ITEM_ALREADY;		// 재접속 뒤 재전송
			else if (state != from)
				result = Protocol::ITEM_INVALID_STATE;	// 착용품은 해제부터
			else if (CountItems(conn, ownerId, target, OUT count) == false)
				result = Protocol::ITEM_DB_ERROR;
			else if (count >= capacity)
				result = Protocol::ITEM_INVENTORY_FULL;		// 창고 25, 소지 25
			break;
		}

		case BaseCampOp::Equip:
		{
			target = Protocol::ITEM_STATE_EQUIPPED;
			targetSlot = equipSlot;
			
			if (state == Protocol::ITEM_STATE_EQUIPPED && slot == targetSlot)
			{
				result = Protocol::ITEM_ALREADY;
				break;
			}

			// 착용 대상은 소지품뿐이다. - 창고품은 먼저 반입, 다른 슬롯 착용품은 먼저 해제
			if (state != Protocol::ITEM_STATE_CARRIED)
			{
				result = Protocol::ITEM_INVALID_STATE;
				break;
			}

			// 종류의 부위와 요청 슬롯이 같아야 한다 - Room::HandleEquip 과 같은 검사 (quick-spec § 3)
			if (ItemTable::CanEquip(e.itemTypeId, equipSlot) == false)
			{
				result = Protocol::ITEM_BAD_REQUEST;
				break;
			}

			// 고독한 길 (2026-10-17) - 파티 중엔 낄 수 없다 (Room::HandleEquip 과 같다)
			if (e.lonePath && GParties.PartyOf(ownerId) != 0)
			{
				result = Protocol::ITEM_LONE_PATH_IN_PARTY;
				break;
			}

			// 슬롯에 이미 있으면 자동 교체 - 밀려난 것은 이 아이템이 비운 소지 칸으로 간다.
			// 소지 개수가 그대로라 25/25에서도 성공한다.
			int32 outState = 0;
			int32 outSlot = 0;
			bool occupied = false;
			if (LockItemRow(conn,
				"owner_character_id = " + to_string(ownerId)
				+ " AND state = " + to_string(static_cast<int32>(Protocol::ITEM_STATE_EQUIPPED))
				+ " AND slot = " + to_string(targetSlot),
				OUT outgoing, OUT outState, OUT outSlot, OUT occupied) == false)
			{
				result = Protocol::ITEM_DB_ERROR;
			}
			break;
		}

		case BaseCampOp::Enhance:
		{
			// 강화 (item-enhance-2026-10-12 R4): 베이스캠프의 내 것이면 상태 불문, 16 이면 거절. 재화 없음 - 사용자 결정. 상태 · 슬롯은 그대로
			target = static_cast<Protocol::ItemState>(state);
			targetSlot = slot;
			if (ItemLevel::CanEnhance(e.level) == false)
				result = Protocol::ITEM_INVALID_STATE;
			break;
		}

		case BaseCampOp::Unequip:
		{
			target = Protocol::ITEM_STATE_CARRIED;
			
			uint64 count = 0;
			if (state == Protocol::ITEM_STATE_CARRIED)
				result = Protocol::ITEM_ALREADY;
			else if (state != Protocol::ITEM_STATE_EQUIPPED)
				result = Protocol::ITEM_INVALID_STATE;
			else if (CountItems(conn, ownerId, Protocol::ITEM_STATE_CARRIED, OUT count) == false)
				result = Protocol::ITEM_DB_ERROR;
			else if (count >= static_cast<uint64>(Player::INVENTORY_SLOT_COUNT))
				result = Protocol::ITEM_INVENTORY_FULL;		
			break;
		}
		}

		if (result != Protocol::ITEM_OK)
		{
			conn->Rollback();
			return result;
		}

		bool ok = true;

		if (outgoing.instanceId != 0)
		{
			::snprintf(query, sizeof(query), "UPDATE item_instances SET state = %d, slot = 0 WHERE instance_id = %llu",
				static_cast<int32>(Protocol::ITEM_STATE_CARRIED), outgoing.instanceId);
			ok = conn->Excute(query) && conn->GetAffectedRows() == 1;
		}

		if (ok)
		{
			if (op == BaseCampOp::Destroy)
				::snprintf(query, sizeof(query), "DELETE FROM item_instances WHERE instance_id = %llu", instanceId);
			else if (op == BaseCampOp::Enhance)
			{
				e.level += 1;
				::snprintf(query, sizeof(query), "UPDATE item_instances SET level = %u WHERE instance_id = %llu", e.level, instanceId);
			}
			else
				::snprintf(query, sizeof(query), "UPDATE item_instances SET state = %d, slot = %d WHERE instance_id = %llu",
					static_cast<int32>(target), targetSlot, instanceId);
			
			ok = conn->Excute(query) && conn->GetAffectedRows() == 1;
		}

		if (ok == false || conn->Commit() == false)
		{
			conn->Rollback();
			return Protocol::ITEM_DB_ERROR;
		}

		changed.push_back(e.ToProto(target, static_cast<Protocol::EquipSlot>(targetSlot)));
		if (outgoing.instanceId != 0)
			changed.push_back(outgoing.ToProto(Protocol::ITEM_STATE_CARRIED));

		// 고독한 길 (2026-10-17) - 착용이 바뀌었으면 세션의 착용 수를 맞춘다 (파티 검사가 본다)
		if (op == BaseCampOp::Equip || op == BaseCampOp::Unequip)
		{
			::snprintf(query, sizeof(query),
				"SELECT COUNT(*) FROM item_instances WHERE owner_character_id = %llu AND state = %d AND lone_path = 1",
				ownerId, static_cast<int32>(Protocol::ITEM_STATE_EQUIPPED));
			if (MYSQL_RES* loneRes = conn->Query(query))
			{
				if (MYSQL_ROW row = ::mysql_fetch_row(loneRes))
					session->lonePieces.store(::atoi(row[0]));
				conn->FreeResult(loneRes);
			}
		}
		return Protocol::ITEM_OK;

	}

	void PushBaseCampItemOp(GameSessionRef session, string requestId, uint64 instanceId, BaseCampOp op, Protocol::EquipSlot slot = Protocol::SLOT_NONE)
	{
		GDBQueue.Push([session, requestId, instanceId, op, slot](DBConnection* conn)
			{
				vector<Protocol::ItemInstance> changed;
				const Protocol::ItemResult result = RunBaseCampItemOp(conn, session, instanceId, op, slot, OUT changed);

				LOG_LINE << "[BASECAMP] " << BaseCampOpName(op) << " item=" << instanceId << " slot=" << slot << " result=" << result << " changed=" << changed.size() <<'\n';

				session->SendItemResult(requestId, result, changed);

				// 베이스캠프 맵의 몸 (2026-10-17) - 착용 · 강화가 허수아비 앞의 스킬 · 스탯에 바로 보이게
				if (result == Protocol::ITEM_OK && changed.empty() == false)
				{
					if (PlayerRef body = session->player.load())
					{
						if (RoomRef room = body->room.load().lock(); room != nullptr && room->IsBaseCamp())
							room->DoAsync(&Room::BaseCampApplyItems, body, changed);
					}
				}
			});
	}
}

bool Handle_INVALID(PacketSessionRef& session, BYTE* buffer, int32 len)
{
	PacketHeader* header = reinterpret_cast<PacketHeader*>(buffer);
	// TODO : Log
	return false;
}

bool Handle_C_LOGIN(PacketSessionRef& session, Protocol::C_LOGIN& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	auto reject = [&gameSession]()
		{
			Protocol::S_LOGIN loginPkt;
			loginPkt.set_success(false);

			SEND_PACKET_DECLARATION(loginPkt);
			gameSession->Send(sendBuffer);
		};

	const string token = pkt.token();
	if (token.empty())
	{
		reject();
		return true;
	}

	// 🔴 연결 하나에 로그인은 하나 (ADR-0011 Migration 2 · AC-AL-07). 이미 로그인했거나 처리 중이면 거절한다.
	//    전에는 두 번째 C_LOGIN 이 다른 계정으로 accountId 를 덮어써서, 끊길 때 두 번째 계정만 풀리고 첫 계정은
	//    서버를 다시 켤 때까지 "already online" 으로 잠겼다. 실패한 로그인은 이 표시를 되돌려 다시 시도할 수 있다
	if (gameSession->loginStarted.exchange(true))
	{
		LOG_LINE << "[LOGIN] rejected - this connection already logged in or is logging in. account=" << gameSession->accountId.load() << '\n';
		reject();
		return true;
	}

	GDBQueue.Push([gameSession, token](DBConnection* conn)
		{
			// 검증 전용 (FBTK_LOGIN_DELAY_MS) - 이 사이에 접속을 끊어 아래 회수 경로를 시험한다
			if (const uint32 delayMs = GLoginDelayMs.load(); delayMs > 0)
				this_thread::sleep_for(chrono::milliseconds(delayMs));

			Protocol::S_LOGIN loginPkt;
			loginPkt.set_success(false);

			const string safeToken = conn->Escape(token);

			char query[256];
			::snprintf(query, sizeof(query),
				"SELECT account_id FROM login_sessions "
				"WHERE token = '%s' AND expires_at > NOW()",
				safeToken.c_str());

			uint64 accountId = 0;

			if (MYSQL_RES* result = conn->Query(query))
			{
				if (MYSQL_ROW row = ::mysql_fetch_row(result))
					accountId = ::strtoull(row[0], nullptr, 10);

				conn->FreeResult(result);
			}
			if (accountId != 0 && GAccountManager.TryLogin(accountId) == false)
			{
				LOG_LINE << "[LOGIN] already online - account=" << accountId << '\n';
				accountId = 0;
			}

			if (accountId != 0)
			{
				::snprintf(query, sizeof(query),
					"SELECT character_id, name, hp, max_hp, floor_id, "
					"pos_x, pos_y, pos_z, yaw FROM characters "
					"WHERE account_id = %llu ORDER BY character_id",
					accountId);

				// 캐릭터 목록을 다 읽었을 때만 성공이다. 전에는 조회 실패 · 열 수 불일치에도 success = true 로 답했고,
				// 열 수 불일치는 TryLogin 으로 잡은 계정을 놓지 않아 서버를 다시 켤 때까지 그 계정이 잠겼다
				bool loaded = false;

				MYSQL_RES* result = nullptr;
				if (GFakeLoginFail.load() > 0 && GFakeLoginFail.fetch_sub(1) > 0)
					LOG_LINE << "[LOGIN] injected character query failure - account=" << accountId << " (FBTK_FAKE_LOGIN_FAIL)\n";
				else
					result = conn->Query(query);

				if (result == nullptr)
				{
					LOG_LINE << "[LOGIN] character query failed - account=" << accountId << " err=" << conn->GetError() << '\n';
				}
				else
				{
					enum {COL_ID = 0, COL_NAME, COL_HP, COL_MAX_HP, COL_FLOOR, COL_X, COL_Y, COL_Z, COL_YAW, COL_COUNT };

					if (::mysql_num_fields(result) < COL_COUNT)
					{
						LOG_LINE << "[LOGIN] column count mismatch - expected " << COL_COUNT << " got " << ::mysql_num_fields(result) << '\n';
						conn->FreeResult(result);
					}
					else
					{
						while (MYSQL_ROW row = ::mysql_fetch_row(result))
						{
							Protocol::CharacterInfo* info = loginPkt.add_characters();
							info->set_name(row[1] ? row[1] : "");
							info->set_hp(::atoi(row[2]));
							info->set_max_hp(::atoi(row[3]));
							info->set_floor_id(static_cast<uint32>(::atoi(row[4])));

							Protocol::ObjectInfo* obj = info->mutable_object_info();
							obj->set_object_id(::strtoull(row[0], nullptr, 10));
							obj->set_object_type(Protocol::OBJECT_TYPE_CREATURE);

							Protocol::PosInfo* pos = obj->mutable_pos_info();
							pos->set_x(static_cast<float>(::atof(row[5])));
							pos->set_y(static_cast<float>(::atof(row[6])));
							pos->set_z(static_cast<float>(::atof(row[7])));
							pos->set_yaw(static_cast<float>(::atof(row[8])));
						}

						conn->FreeResult(result);
						loaded = true;
					}
				}

				if (loaded == false)
				{
					GAccountManager.LogOut(accountId);
					accountId = 0;
					loginPkt.clear_characters();
				}
				else
				{
					gameSession->characters.assign(loginPkt.characters().begin(), loginPkt.characters().end());
					gameSession->accountId.store(accountId);
					gameSession->place.store(SessionPlace::BaseCamp);

					// 🔴 이 작업이 도는 사이에 접속이 끊겼으면 OnDisconnected 는 이미 accountId 0 을 놓고 지나갔다 —
					//    여기서 회수하지 않으면 계정이 서버를 다시 켤 때까지 "already online" 이다.
					//    Disconnect 는 _connected 를 먼저 내리고 OnDisconnected 를 부르므로, 둘 중 exchange 로 먼저 가져간 쪽만 LogOut 한다
					if (gameSession->IsConnected() == false)
					{
						if (const uint64 abandoned = gameSession->accountId.exchange(0); abandoned != 0)
						{
							LOG_LINE << "[LOGIN] connection closed while logging in - released account=" << abandoned << '\n';
							GAccountManager.LogOut(abandoned);
						}
						return;
					}

					loginPkt.set_success(true);
				}
			}

			// 실패한 연결은 다시 로그인할 수 있다 (토큰 만료 · 이미 접속 중 · 조회 실패)
			if (loginPkt.success() == false)
				gameSession->loginStarted.store(false);

			SEND_PACKET_DECLARATION(loginPkt);
			gameSession->Send(sendBuffer);

			// 로그인하면 베이스캠프 화면(S3)이 뜬다 - 보관까지 담아 보낸다 (UI 4단계).
			// 캐릭터는 아직 계정당 하나이고, 입장도 0 번으로 한다.
			if (loginPkt.success() && loginPkt.characters_size() > 0)
			{
				SendBaseCampInventory(conn, gameSession, loginPkt.characters(0).object_info().object_id());

				// 런 중에 끊겼던 파티원이 돌아왔다 - 파티 상태를 다시 보낸다 (PVE 협동, ADR-0014)
				GParties.OnLogin(gameSession);
			}
		});

	return true;
}

namespace
{
	// 입장 거절 - 베이스캠프로 되돌리고 S_ENTER_GAME false. notice 가 있으면 파티 안내도 (협동 · 모드 규칙)
	void RejectEnter(const GameSessionRef& session, const char* why, uint64 characterId, Protocol::PartyResult notice = Protocol::PARTY_OK)
	{
		SessionPlace expected = SessionPlace::Entering;
		session->place.compare_exchange_strong(expected, SessionPlace::BaseCamp);

		LOG_LINE << "[ENTER] rejected - " << why << " char=" << characterId << '\n';

		Protocol::S_ENTER_GAME rejectPkt;
		rejectPkt.set_success(false);
		SEND_PACKET_DECLARATION(rejectPkt);
		session->Send(sendBuffer);

		if (notice != Protocol::PARTY_OK)
			GParties.SendNotice(characterId, notice);
	}

	// 베이스캠프 맵의 몸을 조용히 뺀다 (2026-10-17) - 던전으로 들어갈 때 · 베이스캠프를 다시 요청할 때. 없으면 아무것도 안 한다
	void DropCampBody(const GameSessionRef& session)
	{
		PlayerRef old = session->player.load();
		if (old == nullptr)
			return;
		if (RoomRef room = old->room.load().lock(); room != nullptr && room->IsBaseCamp())
			room->DoAsync(&Room::DropBaseCampBody, old);
	}

	// 베이스캠프 맵 입장 (2026-10-17, design/quick-specs/basecamp-map-2026-10-17.md) - C_ENTER_GAME{mode=BASECAMP}.
	// 런이 아니다: place 는 BaseCamp 그대로, 개인 방(RoomKind::BaseCamp)에 몸 하나. 봇 · 예전 클라는 요청하지 않으니 그대로다
	void EnterBaseCamp(const GameSessionRef& session, const Protocol::CharacterInfo& character)
	{
		const uint64 characterId = character.object_info().object_id();

		auto reject = [&](const char* why)
			{
				LOG_LINE << "[CAMP] enter rejected - " << why << " char=" << characterId << '\n';
				Protocol::S_ENTER_GAME rejectPkt;
				rejectPkt.set_success(false);
				rejectPkt.set_mode(Protocol::RUN_MODE_BASECAMP);
				SEND_PACKET_DECLARATION(rejectPkt);
				session->Send(sendBuffer);
			};

		if (BaseCampTable::Loaded() == false)
			return reject("no basecamp map");
		if (session->place.load() != SessionPlace::BaseCamp)
			return reject("not in base camp");

		// 이미 몸이 있으면 (클라가 다시 요청 - 재로딩 · 거절 뒤 재시도) 빼고 새로 만든다
		DropCampBody(session);

		RoomRef room = GInstances.Create(RoomKind::BaseCamp, 0, 0, 0);
		PlayerRef player = ObjectUtils::CreatePlayer(session, characterId);
		player->nakedMaxHp = (character.max_hp() > 0) ? character.max_hp() : BaseStats::MAX_HP;
		player->maxHp = player->nakedMaxHp;
		player->hp = player->maxHp;

		const BaseCampPoint& spawn = BaseCampTable::Spawn();
		player->posInfo->set_x(spawn.x);
		player->posInfo->set_y(spawn.y);
		player->posInfo->set_z(100.f);
		player->posInfo->set_yaw(spawn.yaw);

		LOG_LINE << "[CAMP] enter char=" << characterId << " room=" << room->Tag() << '\n';

		GDBQueue.Push([session, player, room](DBConnection* conn)
			{
				// 기본 장비는 주지 않는다 - 던전 입장이 준다 (여기서 주면 베이스캠프 동기화가 그 행을 모른다)
				int32 ownedRows = 0;
				if (LoadPlayerItems(conn, player, OUT ownedRows) == false)
				{
					LOG_LINE << "[CAMP] item load failed - char=" << player->objectInfo->object_id() << '\n';
					GInstances.ReleaseJoin(room->_instanceId);
					return;
				}
				room->DoAsync(&Room::HandleEnterPlayer, player);
			});
	}

	// 몸을 만들고 DB 에서 장비를 실어 방에 넣는다 (예전 Handle_C_ENTER_GAME 본문). 세션은 이미 Entering 이다.
	// 인스턴스면 입장 예약을 쥔 채 들어온다 - 방이 HandleEnterPlayer 에서, 적재가 실패하면 여기서 놓는다
	void QueueEnter(const GameSessionRef& gameSession, const Protocol::CharacterInfo& character, RoomRef room, uint32 floorId, uint32 entryPointId)
	{
		// 플레이어 생성
		PlayerRef player = ObjectUtils::CreatePlayer(gameSession, character.object_info().object_id());

		// 맨몸 바닥. 장비 합산은 Room::EnterRoom 이 착용을 다 실은 뒤 더한다 (EquipStats)
		player->nakedMaxHp = (character.max_hp() > 0) ? character.max_hp() : BaseStats::MAX_HP;
		player->maxHp = player->nakedMaxHp;

		// 입장 체력은 항상 최대치 - 런마다 새로 시작한다. 베이스캠프에 회복 수단이 없다 (entry-point.md · 2026-10-05).
		// 세션의 characters 는 로그인 때 캐시라 런 사이에 낡는다 - 위치 · 체력을 거기서 읽지 않는다.
		player->hp = player->maxHp;

		if (GEntryFromDb.load())
		{
			// 부하 측정 · 테스트 픽스처 전용(FBTK_ENTRY_FROM_DB=1) - 예전처럼 저장 좌표에서 시작한다.
			// 봇이 진입 지점 몇 곳에 몰리면 이전 측정과 비교가 안 된다. 원점이면 Room 이 층 기본 위치로 둔다.
			const Protocol::PosInfo& saved = character.object_info().pos_info();
			player->posInfo->set_x(saved.x());
			player->posInfo->set_y(saved.y());
			player->posInfo->set_z(saved.z());
			player->posInfo->set_yaw(saved.yaw());

			// 죽은 채로 저장된 캐릭터는 흩뿌려 되살린다 - 이전 측정과 같은 규칙
			if (character.hp() <= 0)
			{
				player->posInfo->set_x(Utils::GetRandom(-2000.f, 2000.f));
				player->posInfo->set_y(Utils::GetRandom(-2000.f, 2000.f));
				player->posInfo->set_z(100.f);
			}
		}
		else
		{
			// 플레이어가 S4 에서 고른 진입 지점. 0 · 없는 id · 다른 층이면 그 층 지점 중 무작위 (AC-EP-03 · 08)
			thread_local mt19937 rng{ random_device{}() };
			bool wasRandom = false;

			if (const EntryPoint* point = EntryPointTable::Resolve(floorId, entryPointId, rng, OUT wasRandom))
			{
				player->posInfo->set_x(point->x);
				player->posInfo->set_y(point->y);
				player->posInfo->set_z(point->z);
				player->posInfo->set_yaw(point->yaw);
				player->spreadOnEnter = true;		// 같은 지점을 고른 사람끼리 겹치지 않게 Room 이 둘레에 펼친다 (AC-EP-04)
				player->entryPointId = point->entryId;	// 텔레메트리 - 어디서 시작한 런인가

				LOG_LINE << "[ENTER] point=" << (wasRandom ? "random" : "chosen") << " entry=" << point->entryId
					<< " requested=" << entryPointId << " char=" << character.object_info().object_id() << " room=" << room->Tag() << '\n';
			}
			else
			{
				LOG_LINE << "[ENTER] no entry point on floor " << floorId << " - floor default. char="
					<< character.object_info().object_id() << '\n';
			}
		}

		GDBQueue.Push([gameSession, player, room, name = character.name()](DBConnection* conn)
			{
				int32 ownedRows = 0;
				if (LoadPlayerItems(conn, player, OUT ownedRows) == false)
				{
					LOG_LINE << "[ENTER] item load failed - char=" << player->objectInfo->object_id() << '\n';

					if (room->IsInstance())
						GInstances.ReleaseJoin(room->_instanceId);
					gameSession->Disconnect(L"item load failed");
					return;
				}

				// 부하 측정 전용 - 맨몸 봇에 기본 3점 (FBTK_BOT_REGEAR).
				// 기본 장비보다 먼저 - 봇이 채워지면 아래는 채울 부위가 없어 [REGEAR] 집계가 예전 측정과 같다
				if (GBotRegear.load())
				{
					if (const int32 filled = RegearBot(*player, name); filled > 0)
						LOG_LINE << "[REGEAR] char=" << player->objectInfo->object_id() << " filled=" << filled << '\n';
				}

				// 기본 장비 - 착용 · 소지 · 창고가 모두 비었다 (account-login-mvp-2026-10-09.md Core Rule 8 · ADR-0011 Decision 4).
				// 창고에 하나라도 있으면 주지 않는다 - 맡겨 두고 공짜 장비만 들고 들어가는 것을 막는다
				if (ownedRows == 0)
				{
					if (const int32 filled = EquipStarterKit(*player); filled > 0)
						LOG_LINE << "[STARTER] char=" << player->objectInfo->object_id() << " filled=" << filled << '\n';
				}

				// 방에 입장
				room->DoAsync(&Room::HandleEnterPlayer, player);
			});
	}
}

bool Handle_C_ENTER_GAME(PacketSessionRef& session, Protocol::C_ENTER_GAME& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	if (gameSession->accountId.load() == 0)
		return false;

	const uint64 index = pkt.playerindex();
	if (index >= gameSession->characters.size())
		return false;

	const Protocol::CharacterInfo& character = gameSession->characters[index];

	// 사망 커밋이 끝나기 전에는 재입장 시키지 않는다. (복제 방지)
	if (GDeathCommits.IsPending(character.object_info().object_id()))
	{
		LOG_LINE << "[ENTER] death commit pending - char=" << character.object_info().object_id() << '\n';

		Protocol::S_ENTER_GAME rejectPkt;
		rejectPkt.set_success(false);
		rejectPkt.set_mode(pkt.mode());		// 클라가 어느 요청의 거절인지 안다 (베이스캠프 맵 요청은 다시 묻는다, 2026-10-17)
		SEND_PACKET_DECLARATION(rejectPkt);
		gameSession->Send(sendBuffer);
		
		return true;
	}

	if (PlayerRef body = GDetachedBodies.TryTake(character.object_info().object_id()))
	{
		if (RoomRef bodyRoom = body->room.load().lock())
		{
			gameSession->place.store(SessionPlace::Entering);
			bodyRoom->DoAsync(&Room::HandleReattach, gameSession, body);
			return true;
		}
	}

	const uint64 characterId = character.object_info().object_id();
	const uint32 floorId = pkt.floor_id();

	// 베이스캠프 맵 (2026-10-17) - 런이 아니라 개인 방. 끊긴 몸 · 사망 커밋은 위에서 먼저 걸렀다
	if (pkt.mode() == Protocol::RUN_MODE_BASECAMP)
	{
		EnterBaseCamp(gameSession, character);
		return true;
	}

	// 베이스캠프에서만 입장한다 - 예전엔 결과를 버려 두 번째 C_ENTER_GAME 이 몸을 하나 더 만들었다.
	// 인스턴스가 생기며 "두 방에 두 몸" 이 되어 캐릭터 단위 전역(GDeathCommits · GDetachedBodies)이 깨진다 (ADR-0014)
	SessionPlace expected = SessionPlace::BaseCamp;
	if (gameSession->place.compare_exchange_strong(expected, SessionPlace::Entering) == false)
	{
		LOG_LINE << "[ENTER] rejected - not in base camp (place=" << static_cast<int32>(expected) << ") char=" << characterId << '\n';

		Protocol::S_ENTER_GAME rejectPkt;
		rejectPkt.set_success(false);
		SEND_PACKET_DECLARATION(rejectPkt);
		gameSession->Send(sendBuffer);
		return true;
	}

	// 베이스캠프 맵의 몸은 여기서 뺀다 - 입장이 거절되면 클라가 베이스캠프를 다시 요청한다
	DropCampBody(gameSession);

	if (floorId >= FLOOR_COUNT)
	{
		RejectEnter(gameSession, "no such floor", characterId);
		return true;
	}

	// 방 고르기 (ADR-0014) - PvP = 층 방, PVE 솔로 = 새 인스턴스, PVE 협동 = 파티 인스턴스 (파티장이 열고 멤버를 데려간다).
	// 2명 이상 파티는 협동으로만 들어간다 (D7)
	RoomRef room;
	vector<GameSessionRef> pulls;
	switch (pkt.mode())
	{
	case Protocol::RUN_MODE_PVE_SOLO:
		if (GParties.SizeOf(characterId) >= 2)
		{
			RejectEnter(gameSession, "party of 2+ asked for solo", characterId, Protocol::PARTY_COOP_ONLY);
			return true;
		}
		room = GInstances.Create(RoomKind::PveSolo, floorId, 0, 0);
		break;

	case Protocol::RUN_MODE_PVE_COOP:
	{
		PartyManager::CoopEnter coop = GParties.BeginCoopEnter(gameSession, floorId);
		if (coop.room == nullptr)
		{
			RejectEnter(gameSession, "co-op refused", characterId, coop.result);
			return true;
		}
		room = coop.room;
		pulls = move(coop.pulls);
		break;
	}

	default:
		if (GParties.SizeOf(characterId) >= 2)
		{
			RejectEnter(gameSession, "party of 2+ asked for PvP", characterId, Protocol::PARTY_COOP_ONLY);
			return true;
		}
		room = GetRoomForFloor(floorId);
		break;
	}

	if (room->IsInstance())
		LOG_LINE << "[INST] enter char=" << characterId << " room=" << room->Tag() << " pulls=" << pulls.size() << '\n';

	// 데려가는 멤버는 파티장과 같은 지점에 - [무작위] 면 여기서 한 번 뽑아 모두에게 준다 (둘레 고리로 펼쳐진다, AC-EP-04)
	uint32 entryPointId = pkt.entry_point_id();
	if (pulls.empty() == false && entryPointId == 0)
	{
		thread_local mt19937 rng{ random_device{}() };
		bool wasRandom = false;
		if (const EntryPoint* point = EntryPointTable::Resolve(floorId, 0, rng, OUT wasRandom))
			entryPointId = point->entryId;
	}

	QueueEnter(gameSession, character, room, floorId, entryPointId);

	// 파티장이 데려가는 멤버 - 각자 베이스캠프에서 Entering 으로 넘어가야 한다. 그 사이 다른 데로 갔으면 건너뛴다 (예약을 놓는다)
	for (const GameSessionRef& member : pulls)
	{
		SessionPlace memberPlace = SessionPlace::BaseCamp;
		if (member->characters.empty() || member->place.compare_exchange_strong(memberPlace, SessionPlace::Entering) == false)
		{
			GInstances.ReleaseJoin(room->_instanceId);
			continue;
		}

		DropCampBody(member);
		QueueEnter(member, member->characters[0], room, floorId, entryPointId);
	}

	return true;
}

bool Handle_C_PARTY_INVITE(PacketSessionRef& session, Protocol::C_PARTY_INVITE& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);
	if (gameSession->accountId.load() == 0)
		return false;

	GParties.Invite(gameSession, pkt.target_name());
	return true;
}

bool Handle_C_PARTY_RESPONSE(PacketSessionRef& session, Protocol::C_PARTY_RESPONSE& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);
	if (gameSession->accountId.load() == 0)
		return false;

	GParties.Respond(gameSession, pkt.party_id(), pkt.accept());
	return true;
}

bool Handle_C_PARTY_LEAVE(PacketSessionRef& session, Protocol::C_PARTY_LEAVE& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);
	if (gameSession->accountId.load() == 0)
		return false;

	GParties.Leave(gameSession, pkt.character_id());
	return true;
}

bool Handle_C_LEAVE_GAME(PacketSessionRef& session, Protocol::C_LEAVE_GAME& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	PlayerRef player = gameSession->player.load();
	if (player == nullptr)
		return false;

	RoomRef room = player->room.load().lock();
	if (room == nullptr)
		return false;

	room->DoAsync(&Room::HandleLeavePlayer, player);

	return true;
}

bool Handle_C_MOVE(PacketSessionRef& session, Protocol::C_MOVE& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	PlayerRef player = gameSession->player.load();
	if (player == nullptr)
		return false;

	RoomRef room = player->room.load().lock();
	if (room == nullptr)
		return false;

	const uint64 realId = player->objectInfo->object_id();
	if (pkt.info().object_id() != realId)
	{
		// 클라가 S_ENTER_GAME 을 처리했다면 0 이 아닌 제 id 를 보낸다.
		LOG_LINE << "[ID MISMATCH] claimed=" << pkt.info().object_id()
			<< " actual=" << realId << '\n';
	}
	pkt.mutable_info()->set_object_id(realId);

	room->DoAsync(&Room::HandleMove, pkt);

	return true;
}

bool Handle_C_ATTACK(PacketSessionRef& session, Protocol::C_ATTACK& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	PlayerRef player = gameSession->player.load();
	if (player == nullptr)
		return false;

	RoomRef room = player->room.load().lock();
	if (room == nullptr)
		return false;

	// 공격자 id는 패킷에서 받지 않는다. 세션에서 가져온다.
	const uint64 attackerId = player->objectInfo->object_id();

	// 조준점 - 대상 없는 평타 (일보일검 · 난무, 2026-10-11). 보통 평타는 쓰지 않는다
	room->DoAsync(&Room::HandleAttack, attackerId, pkt.target_id(), pkt.aim_x(), pkt.aim_y());

	return true;
}

// /t <명령> ... - 테스트 모드 (RoomTest.cpp). 숫자를 못 읽으면 아무 일도 없다 (로그만)
//   /t gear <무기> <Shift> <Q> <모자> <모자스킬> <갑옷> <갑옷스킬> <신발> <신발스킬> <장신구> <장신구스킬> <등급> <레벨>
//   /t spawn <몬스터 종류> <수> <ai 0|1> <x> <y>    /t mode <ai 0|1>    /t clear    /t heal    /t cd    /t god <0|1>    /t fow <0|1>
static void HandleTestCommand(const RoomRef& room, uint64 playerId, const string& msg)
{
	istringstream in(msg.substr(2));
	string command;
	in >> command;

	if (command == "gear")
	{
		TestGearRequest req;
		in >> req.weaponType >> req.weaponSkill1 >> req.weaponSkill2;
		for (int32 k = 0; k < 4; k++)
			in >> req.types[k] >> req.skills[k];
		in >> req.grade >> req.level;
		if (in.fail())
		{
			LOG_LINE << "[TEST] /t gear needs 13 numbers. char=" << playerId << '\n';
			return;
		}
		room->DoAsync(&Room::TestGear, playerId, req);
		return;
	}

	if (command == "spawn")
	{
		uint32 typeId = 0;
		int32 count = 1;
		int32 ai = 0;
		float x = 0.f;
		float y = 0.f;
		in >> typeId >> count >> ai >> x >> y;
		if (in.fail())
		{
			LOG_LINE << "[TEST] /t spawn needs <type> <count> <ai> <x> <y>. char=" << playerId << '\n';
			return;
		}
		room->DoAsync(&Room::TestSpawnDummies, playerId, typeId, count, ai != 0, x, y);
		return;
	}

	int32 flag = 0;
	if (command == "mode" && (in >> flag))
		room->DoAsync(&Room::TestSetDummyMode, playerId, flag != 0);
	else if (command == "god" && (in >> flag))
		room->DoAsync(&Room::TestGod, playerId, flag != 0);
	else if (command == "fow" && (in >> flag))
		room->DoAsync(&Room::TestFow, playerId, flag != 0);
	else if (command == "pack" && (in >> flag))
		room->DoAsync(&Room::TestSpawnPack, playerId, flag != 0);		// 0 = 탐지형 · 1 = 반응형 (2026-10-11 몬스터 개선)
	else if (command == "clear")
		room->DoAsync(&Room::TestClearDummies, playerId);
	else if (command == "heal")
		room->DoAsync(&Room::TestHeal, playerId);
	else if (command == "cd")
		room->DoAsync(&Room::TestResetCooldowns, playerId);
	else
		LOG_LINE << "[TEST] unknown /t command '" << command << "' char=" << playerId << '\n';
}

bool Handle_C_CHAT(PacketSessionRef& session, Protocol::C_CHAT& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	PlayerRef player = gameSession->player.load();
	if (player == nullptr)
		return false;

	RoomRef room = player->room.load().lock();
	if (room == nullptr)
		return false;

	// 채팅은 아직 명령만 받는다 - 일반 채팅 시스템이 없다
	const string& msg = pkt.msg();
	if (msg.empty() || msg[0] != '/')
		return true;

	// /escape · /gold 는 각자 플래그가 있다(아래). 나머지 디버그 명령은 FBTK_DEBUG_COMMANDS=1 일 때만 -
	// 꺼져 있으면 남에게 CC 를 걸거나 가방을 지우는 길이 막힌다
	const bool ownFlag = (msg.rfind("/escape", 0) == 0) || (msg.rfind("/gold", 0) == 0);
	if (ownFlag == false && GDebugCommands.load() == false)
	{
		LOG_LINE << "[CHAT] debug command ignored - FBTK_DEBUG_COMMANDS is off. char=" << player->objectInfo->object_id()
			<< " cmd=" << msg.substr(0, min<size_t>(msg.find(' '), 32)) << '\n';
		return true;
	}

	// 테스트 모드 (FBTK_TEST_MODE=1, design/quick-specs/test-mode-2026-10-11.md) - 장비를 바꾸고 몬스터를 세우니 디버그 명령보다 한 단계 더 막는다
	if (msg == "/t" || msg.rfind("/t ", 0) == 0)
	{
		if (GTestMode.load() == false)
		{
			LOG_LINE << "[TEST] /t ignored - FBTK_TEST_MODE is off. char=" << player->objectInfo->object_id() << '\n';
			return true;
		}
		HandleTestCommand(room, player->objectInfo->object_id(), msg);
		return true;
	}

	// 가방 디버그 전용
	if (msg.rfind("/bag", 0) == 0)
	{
		room->DoAsync(&Room::DebugDropBag, player->objectInfo->object_id());
		return true;
	}

	if (msg.rfind("/unbag", 0) == 0)
	{
		uint64 bagId = 0;
		if (::sscanf_s(msg.c_str(), "/unbag %llu", &bagId) == 1)
			room->DoAsync(&Room::RemoveBag, bagId);
		return true;
	}

	if (msg.rfind("/die", 0) == 0)
	{
		room->DoAsync(&Room::DebugKill, player->objectInfo->object_id());
		return true;
	}

	// 즉시 탈출은 FBTK_DEBUG_ESCAPE=1 일 때만 - 정식 탈출(타워 게이지)이 생겼으니 공짜 탈출은 끈다 (escape-timer.md Core Rule 19)
	if (msg.rfind("/escape", 0) == 0)
	{
		if (EscapeConfig::Get().debugEscapeCommand)
			room->DoAsync(&Room::BeginEscapeCommit, player->objectInfo->object_id());
		else
			LOG_LINE << "[ESCAPE] /escape ignored - FBTK_DEBUG_ESCAPE is off. char=" << player->objectInfo->object_id() << '\n';
		return true;
	}

	// 런 골드 - MVP 에는 던전 골드 획득 경로가 없어 디버그로만 넣는다 (Core Rule 18 · AC-ET-20). FBTK_DEBUG_GOLD=1
	if (msg.rfind("/gold", 0) == 0)
	{
		uint64 amount = 0;
		if (GDebugGold.load() && ::sscanf_s(msg.c_str(), "/gold %llu", &amount) == 1)
			room->DoAsync(&Room::DebugAddRunGold, player->objectInfo->object_id(), amount);
		return true;
	}

	char command[32] = {};
	uint64 targetId = 0;
	uint32 durationMs = 0;
	float magnitude = 0.f;

	const int32 parsed = ::sscanf_s(msg.c_str(), "/%31s %llu %u %f",
		command, static_cast<uint32>(sizeof(command)), &targetId, &durationMs, &magnitude);

	if (parsed < 3)
		return true;

	Protocol::CcType type = Protocol::CC_TYPE_NONE;

	if (::strcmp(command, "stun") == 0)
		type = Protocol::CC_TYPE_STUN;
	else if (::strcmp(command, "root") == 0)
		type = Protocol::CC_TYPE_ROOT;
	else if (::strcmp(command, "silence") == 0)
		type = Protocol::CC_TYPE_SILENCE;
	else if (::strcmp(command, "slow") == 0)
		type = Protocol::CC_TYPE_SLOW;
	else return true;

	const uint64 instigatorId = player->objectInfo->object_id();

	LOG_LINE << "[DEBUG CC] " << command << " target=" << targetId
		<< " ms=" << durationMs << " mag=" << magnitude << '\n';

	room->DoAsync(&Room::ApplyCc, targetId, instigatorId, type, durationMs, magnitude);

	return true;
}

bool Handle_C_SKILL(PacketSessionRef& session, Protocol::C_SKILL& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	PlayerRef player = gameSession->player.load();
	if (player == nullptr)
		return false;

	RoomRef room = player->room.load().lock();
	if (room == nullptr)
		return false;

	const uint64 casterId = player->objectInfo->object_id();

	room->DoAsync(&Room::HandleSkill, casterId, pkt);

	return true;
}

bool Handle_C_SKILL_CANCEL(PacketSessionRef& session, Protocol::C_SKILL_CANCEL& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	PlayerRef player = gameSession->player.load();
	if (player == nullptr)
		return false;

	RoomRef room = player->room.load().lock();
	if (room == nullptr)
		return false;

	room->DoAsync(&Room::HandleSkillCancel, player->objectInfo->object_id());

	return true;
}

bool Handle_C_GRANT_REWARD(PacketSessionRef& session, Protocol::C_GRANT_REWARD& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	const string requestId = pkt.request_id();
	const uint64 characterId = pkt.character_id();
	const uint64 gold = pkt.gold();
	const string reason = pkt.reason();

	if (requestId.empty() || requestId.size() > 36 || characterId == 0 || gold == 0)
	{
		SendGrantResult(gameSession, requestId, Protocol::GRANT_BAD_REQUEST, 0);
		return true;
	}

	GDBQueue.Push([gameSession, requestId, characterId, gold, reason](DBConnection* conn)
		{
			const string safeRequestId = conn->Escape(requestId);
			const string safeReason = conn->Escape(reason);

			Protocol::GrantResult result = Protocol::GRANT_DB_ERROR;
			uint64 goldAfter = 0;

			char query[512];

			if (conn->BeginTransaction() == false)
			{
				SendGrantResult(gameSession, requestId, Protocol::GRANT_DB_ERROR, 0);
				return;
			}

			::snprintf(query, sizeof(query),
				"SELECT gold FROM characters WHERE character_id = %llu FOR UPDATE",
				characterId);

			bool targetExists = false;

			if (MYSQL_RES* res = conn->Query(query))
			{
				targetExists = (::mysql_fetch_row(res) != nullptr);
				conn->FreeResult(res);
			}
			else
			{
				LOG_LINE << "[GRANT] lock failed err=" << conn->GetLastErrorNo()
					<< " " << conn->GetError() << '\n';

				conn->Rollback();
				SendGrantResult(gameSession, requestId, Protocol::GRANT_DB_ERROR, 0);
				return;
			}

			// 잠금 단계에서 대상 유무가 판명된다. FK 나 affected_rows 보다 앞선다
			if (targetExists == false)
			{
				conn->Rollback();
				SendGrantResult(gameSession, requestId, Protocol::GRANT_NO_TARGET, 0);
				return;
			}

			::snprintf(query, sizeof(query),
				"INSERT INTO reward_grants (request_id, character_id, gold, reason) "
				"VALUES ('%s', %llu, %llu, '%s')",
				safeRequestId.c_str(), characterId, gold, safeReason.c_str());

			if (conn->Excute(query) == false)
			{
				const uint32 errorNo = conn->GetLastErrorNo();
				conn->Rollback();

				// 1062 = ER_DUP_ENTRY. 이미 처리된 요청.
				if (errorNo == 1062)
				{
					// 이미 지급됐으므로 현재 잔액을 읽어 돌려준다.
					::snprintf(query, sizeof(query),
						"SELECT gold FROM characters WHERE character_id = %llu", characterId);

					if (MYSQL_RES* res = conn->Query(query))
					{
						if (MYSQL_ROW row = ::mysql_fetch_row(res))
							goldAfter = ::strtoull(row[0], nullptr, 10);

						conn->FreeResult(res);
					}

					result = Protocol::GRANT_ALREADY;
				}
				else
				{
					LOG_LINE << "[GRANT] insert failed err=" << errorNo << " " << conn->GetError() << '\n';
					result = Protocol::GRANT_DB_ERROR;
				}

				SendGrantResult(gameSession, requestId, result, goldAfter);
				return;
			}

			// 잔액을 올린다.
			::snprintf(query, sizeof(query), "UPDATE characters SET gold = gold + %llu WHERE character_id = %llu", gold, characterId);

			if (conn->Excute(query) == false)
			{
				LOG_LINE << "[GRANT] update failed err=" << conn->GetLastErrorNo() << " " << conn->GetError() << '\n';
				conn->Rollback();
				SendGrantResult(gameSession, requestId, Protocol::GRANT_DB_ERROR, 0);
				return;
			}

			// 0행이면 캐릭터가 없다는 뜻, FK가 있어 Insert단계에서 걸러지지만, FK가 없더라도 한 번 더 걸러냄
			if (conn->GetAffectedRows() == 0)
			{
				conn->Rollback();
				SendGrantResult(gameSession, requestId, Protocol::GRANT_NO_TARGET, 0);
				return;
			}

			::snprintf(query, sizeof(query), "SELECT gold FROM characters WHERE character_Id = %llu", characterId);

			if (MYSQL_RES* res = conn->Query(query))
			{
				if (MYSQL_ROW row = ::mysql_fetch_row(res))
					goldAfter = ::strtoull(row[0], nullptr, 10);

				conn->FreeResult(res);
			}

			if (conn->Commit() == false)
			{
				LOG_LINE << "[GRANT] commit failed " << conn->GetError() << '\n';
				conn->Rollback();
				SendGrantResult(gameSession, requestId, Protocol::GRANT_DB_ERROR, 0);
				return;
			}

			SendGrantResult(gameSession, requestId, Protocol::GRANT_OK, goldAfter);
		});

	return true;
}

bool Handle_C_EQUIP(PacketSessionRef& session, Protocol::C_EQUIP& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	if (gameSession->accountId.load() == 0)
		return false;

	if (gameSession->TryBeginRequest(pkt.request_id()) == false)
	{
		gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_ALREADY);
		return true;
	}

	// 베이스캠프 - DB 직행. 슬롯 규칙은 Room::HandleEquip과 같다.
	if (gameSession->place.load() == SessionPlace::BaseCamp)
	{
		if (pkt.slot() < Protocol::SLOT_WEAPON_PRIMARY || pkt.slot() > Protocol::SLOT_TRINKET)
			gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_BAD_REQUEST);
		else if (pkt.slot() == Protocol::SLOT_WEAPON_SECONDARY)
			gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_INVALID_STATE);
		else
			PushBaseCampItemOp(gameSession, pkt.request_id(), pkt.instance_id(), BaseCampOp::Equip, pkt.slot());
		return true;
	}

	// 던전 - Room 메모리 우선
	PlayerRef player = gameSession->player.load();
	RoomRef room = (player != nullptr) ? player->room.load().lock() : nullptr;
	if (room == nullptr)
	{
		gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_INVALID_STATE);
		return true;
	}

	room->DoAsync(&Room::HandleEquip, gameSession, pkt.request_id(), pkt.instance_id(), pkt.slot());

	return true;
}

bool Handle_C_UNEQUIP(PacketSessionRef& session, Protocol::C_UNEQUIP& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	if (gameSession->accountId.load() == 0)
		return false;

	if (gameSession->TryBeginRequest(pkt.request_id()) == false)
	{
		gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_ALREADY);
		return true;
	}

	if (gameSession->place.load() == SessionPlace::BaseCamp)
	{
		PushBaseCampItemOp(gameSession, pkt.request_id(), pkt.instance_id(), BaseCampOp::Unequip);
		return true;
	}

	PlayerRef player = gameSession->player.load();
	RoomRef room = (player != nullptr) ? player->room.load().lock() : nullptr;
	if (room == nullptr)
	{
		gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_INVALID_STATE);
		return true;
	}

	room->DoAsync(&Room::HandleUnequip, gameSession, pkt.request_id(), pkt.instance_id());

	return true;
}

bool Handle_C_LOOT(PacketSessionRef& session, Protocol::C_LOOT& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);
	
	PlayerRef player = gameSession->player.load();
	if (player == nullptr)
		return false;

	RoomRef room = player->room.load().lock();
	if (room == nullptr)
		return false;

	if (gameSession->TryBeginRequest(pkt.request_id()) == false)
	{
		gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_ALREADY);
		return true;
	}

	room->DoAsync(&Room::HandleLoot, gameSession, pkt.request_id(), pkt.bag_id(), pkt.instance_id());
	
	return true;
}

bool Handle_C_DROP(PacketSessionRef& session, Protocol::C_DROP& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	if (gameSession->accountId.load() == 0)
		return false;

	if (gameSession->TryBeginRequest(pkt.request_id()) == false)
	{
		gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_ALREADY);
		return true;
	}

	if (gameSession->place.load() == SessionPlace::BaseCamp)
	{
		PushBaseCampItemOp(gameSession, pkt.request_id(), pkt.instance_id(), BaseCampOp::Destroy);
		return true;
	}

	PlayerRef player = gameSession->player.load();
	RoomRef room = (player != nullptr) ? player->room.load().lock() : nullptr;
	if (room == nullptr)
	{
		gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_INVALID_STATE);
		return true;
	}
	room->DoAsync(&Room::HandleDrop, gameSession, pkt.request_id(), pkt.instance_id());

	return true;
}

// 베이스캠프 강화 - 버튼 한 번 = 레벨 +1 (item-enhance-2026-10-12 R4 · R5). 던전에서는 거절, 응답은 S_ITEM_RESULT{changed=[레벨 오른 인스턴스]}
bool Handle_C_ENHANCE(PacketSessionRef& session, Protocol::C_ENHANCE& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);
	if (gameSession->accountId.load() == 0 || pkt.instance_id() == 0)
	{
		gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_BAD_REQUEST);
		return true;
	}
	// 같은 request_id 재전송은 한 번만 - 증가형 op 라 상태 지정 op 처럼 저절로 멱등이 아니다 (ADR-0003 · TR-item-027, 형제 op 5 종과 같은 가드)
	if (gameSession->TryBeginRequest(pkt.request_id()) == false)
	{
		gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_ALREADY);
		return true;
	}
	if (gameSession->place.load() != SessionPlace::BaseCamp)
	{
		LOG_LINE << "[BASECAMP] enhance rejected - not in base camp. item=" << pkt.instance_id() << '\n';
		gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_BAD_REQUEST);
		return true;
	}
	PushBaseCampItemOp(gameSession, pkt.request_id(), pkt.instance_id(), BaseCampOp::Enhance);
	return true;
}

bool Handle_C_STASH(PacketSessionRef& session, Protocol::C_STASH& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	if (gameSession->accountId.load() == 0)
		return false;

	if (gameSession->TryBeginRequest(pkt.request_id()) == false)
	{
		gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_ALREADY);
		return true;
	}

	// 던전 안에서는 창고에 접근할 수 없다. 입장 중(Entering)도 막는다.
	if (gameSession->place.load() != SessionPlace::BaseCamp)
	{
		gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_INVALID_STATE);
		return true;
	}

	PushBaseCampItemOp(gameSession, pkt.request_id(), pkt.instance_id(),
		pkt.to_vault() ? BaseCampOp::ToVault : BaseCampOp::ToCarried);

	return true;
}

bool Handle_C_BAG_OPEN(PacketSessionRef& session, Protocol::C_BAG_OPEN& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	PlayerRef player = gameSession->player.load();
	if (player == nullptr)
		return false;

	RoomRef room = player->room.load().lock();
	if (room == nullptr)
		return false;

	room->DoAsync(&Room::HandleBagOpen, gameSession, pkt.bag_id());
	return true;
}
