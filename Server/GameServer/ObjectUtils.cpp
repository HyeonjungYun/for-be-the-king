#include "pch.h"
#include "ObjectUtils.h"
#include "Player.h"
#include "GameSession.h"

atomic<int64> ObjectUtils::s_idGenerator = 1;

PlayerRef ObjectUtils::CreatPlayer(GameSessionRef session)
{
	// ID »ý¼º±â
	const int64 newId = s_idGenerator.fetch_add(1);

	PlayerRef player = make_shared<Player>();
	player->objectInfo->set_object_id(newId);
	player->posInfo->set_object_id(newId);

	player->session = session;
	session->player.store(player);

	return player;
}
