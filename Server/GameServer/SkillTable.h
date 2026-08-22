#pragma once
#include "Enum.pb.h"

/*-----------------
	SkillTable
------------------*/

enum class SkillShape : uint8
{
	Single = 0,
	Line,
	CircleSelf,
	CirclePoint
};

enum class SkillAimType : uint8
{
	None = 0,
	SelfArea,
	Target,
	Direction,
	Point
};

enum class SkillEffectType : uint8
{
	Damage = 0,
	HardCc,
	Knockback,
	SoftCc,
	Movement,
	SelfBuff
};

enum class SkillBuffType : uint8
{
	None = 0,
	MoveSpeed,
	WallPierce
};

struct SkillEffect
{
	SkillEffectType			type = SkillEffectType::Damage;

	// Damage
	float					adRatio = 0.f;
	float					apRatio = 0.f;
	Protocol::DamageType	damageType = Protocol::DAMAGE_TYPE_PHYSICAL;

	// HardCc, SoftCc
	Protocol::CcType		ccType = Protocol::CC_TYPE_NONE;
	uint32					ccDurationMs = 0;
	float					ccMagnitude = 0.f;

	// Knockback, Movement 
	float					distCm = 0.f;
	float					speedCms = 0.f;
	bool					ignoresWalls = false;

	// SelfBuff
	SkillBuffType			buffType = SkillBuffType::None;
	float					buffMagnitude = 0.f;
	uint32					buffDurationMs = 0;
};

struct SkillDef
{
	uint32					skillId = 0;
	uint32					effectId = 0;

	uint32					castMs = 0;
	uint32					cooldownMs = 0;
	bool					canMoveWhileCasting = false;

	SkillShape				shape = SkillShape::Single;
	SkillAimType			aimType = SkillAimType::None;
	float					rangeCm = 0.f;
	float					widthCm = 0.f;
	float					radiusCm = 0.f;

	vector<SkillEffect>		effects;

	bool HasHardCc() const;
	bool HasMovement() const;
};

class SkillTable
{
public:
	static void				Init();
	static const SkillDef*	Find(uint32 skillId);

private:
	static unordered_map<uint32, SkillDef> s_defs;
};

