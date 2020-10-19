#ifndef QFUSION_BOT_WEAPON_SELECTOR_H
#define QFUSION_BOT_WEAPON_SELECTOR_H

#include "ai_base_enemy_pool.h"

class WorldState;

class GenericFireDef
{
	// Allow SelectedWeapons to use the default constructor
	friend class SelectedWeapons;

	float projectileSpeed;
	float splashRadius;
	ai_weapon_aim_type aimType;
	short weaponNum;
	bool isBuiltin;

	GenericFireDef()
		: projectileSpeed( 0 ),
		splashRadius( 0 ),
		aimType( AI_WEAPON_AIM_TYPE_INSTANT_HIT ),
		weaponNum( -1 ),
		isBuiltin( false ) {}

public:
	GenericFireDef( int weaponNum_, const firedef_t *builtinFireDef ) {
		this->projectileSpeed = builtinFireDef->speed;
		this->splashRadius = builtinFireDef->splash_radius;
		this->aimType = BuiltinWeaponAimType( weaponNum_ );
		this->weaponNum = (short)weaponNum_;
		this->isBuiltin = true;
	}

	GenericFireDef( int weaponNum_, const AiScriptWeaponDef *scriptWeaponDef ) {
		this->projectileSpeed = scriptWeaponDef->projectileSpeed;
		this->splashRadius = scriptWeaponDef->splashRadius;
		this->aimType = scriptWeaponDef->aimType;
		this->weaponNum = (short)weaponNum_;
		this->isBuiltin = false;
	}

	inline int WeaponNum() const { return weaponNum; }
	inline bool IsBuiltin() const { return isBuiltin; }

	inline ai_weapon_aim_type AimType() const { return aimType; }
	inline float ProjectileSpeed() const { return projectileSpeed; }
	inline float SplashRadius() const { return splashRadius; }
	inline bool IsContinuousFire() const { return isBuiltin; }
};

class SelectedWeapons
{
	friend class BotWeaponSelector;
	friend class Bot;

	GenericFireDef builtinFireDef;
	GenericFireDef scriptFireDef;

	int64_t timeoutAt;
	unsigned instanceId;

	bool preferBuiltinWeapon;
	bool hasSelectedBuiltinWeapon;
	bool hasSelectedScriptWeapon;

	SelectedWeapons()
		: timeoutAt( 0 ),
		instanceId( 0 ),
		preferBuiltinWeapon( true ),
		hasSelectedBuiltinWeapon( false ),
		hasSelectedScriptWeapon( false ) {}

public:
	inline const GenericFireDef *BuiltinFireDef() const {
		return hasSelectedBuiltinWeapon ? &builtinFireDef : nullptr;
	}
	inline const GenericFireDef *ScriptFireDef() const {
		return hasSelectedScriptWeapon ? &scriptFireDef : nullptr;
	}
	inline int BuiltinWeaponNum() const {
		return hasSelectedBuiltinWeapon ? builtinFireDef.WeaponNum() : -1;
	}
	inline int ScriptWeaponNum() const {
		return hasSelectedScriptWeapon ? scriptFireDef.WeaponNum() : -1;
	}
	inline unsigned InstanceId() const { return instanceId; }
	inline bool AreValid() const { return timeoutAt > level.time; }
	inline void Invalidate() { timeoutAt = level.time; }
	inline int64_t TimeoutAt() const { return timeoutAt; }
	inline bool PreferBuiltinWeapon() const { return preferBuiltinWeapon; }
};

class SelectedEnemies
{
	friend class Bot;
	friend class BotBrain;

	const edict_t *self;

	const Enemy *primaryEnemy;
	StaticVector<const Enemy *, AiBaseEnemyPool::MAX_ACTIVE_ENEMIES> activeEnemies;

	int64_t timeoutAt;
	unsigned instanceId;

	inline void CheckValid( const char *function ) const {
#ifdef _DEBUG
		if( !AreValid() ) {
			AI_FailWith( "SelectedEnemies", "::%s(): Selected enemies are invalid\n", function );
		}
#endif
	}

	explicit SelectedEnemies( const edict_t *self_ )
		: self( self_ ),
        primaryEnemy( nullptr ),
        timeoutAt( 0 ),
        instanceId( 0 ) {}

public:
	bool AreValid() const;

	inline void Invalidate() {
		timeoutAt = 0;
		primaryEnemy = nullptr;
		activeEnemies.clear();
	}

	void Set( const Enemy *primaryEnemy_,
			  unsigned timeoutPeriod,
			  const Enemy *const *activeEnemiesBegin,
			  const Enemy *const *activeEnemiesEnd );

	inline unsigned InstanceId() const { return instanceId; }

	bool IsPrimaryEnemy( const edict_t *ent ) const {
		return primaryEnemy && primaryEnemy->ent == ent;
	}

	bool IsPrimaryEnemy( const Enemy *enemy ) const {
		return primaryEnemy && primaryEnemy == enemy;
	}

	Vec3 LastSeenOrigin() const {
		CheckValid( __FUNCTION__ );
		return primaryEnemy->LastSeenPosition();
	}

	Vec3 ActualOrigin() const {
		CheckValid( __FUNCTION__ );
		return Vec3( primaryEnemy->ent->s.origin );
	}

	Vec3 LastSeenVelocity() const {
		CheckValid( __FUNCTION__ );
		return primaryEnemy->LastSeenVelocity();
	}

	unsigned LastSeenAt() const {
		CheckValid( __FUNCTION__ );
		return primaryEnemy->LastSeenAt();
	}

	Vec3 ClosestEnemyOrigin( const Vec3 &relativelyTo ) const {
		return ClosestEnemyOrigin( relativelyTo.Data() );
	}

	Vec3 ClosestEnemyOrigin( const vec3_t relativelyTo ) const;

	typedef Enemy::SnapshotsQueue SnapshotsQueue;
	const SnapshotsQueue &LastSeenSnapshots() const {
		CheckValid( __FUNCTION__ );
		return primaryEnemy->lastSeenSnapshots;
	}

	Vec3 ActualVelocity() const {
		CheckValid( __FUNCTION__ );
		return Vec3( primaryEnemy->ent->velocity );
	}

	Vec3 Mins() const {
		CheckValid( __FUNCTION__ );
		return Vec3( primaryEnemy->ent->r.mins );
	}

	Vec3 Maxs() const {
		CheckValid( __FUNCTION__ );
		return Vec3( primaryEnemy->ent->r.maxs );
	}

	Vec3 LookDir() const {
		CheckValid( __FUNCTION__ );
		vec3_t lookDir;
		AngleVectors( primaryEnemy->ent->s.angles, lookDir, nullptr, nullptr );
		return Vec3( lookDir );
	}

	Vec3 EnemyAngles() const {
		CheckValid( __FUNCTION__ );
		return Vec3( primaryEnemy->ent->s.angles );
	}

	float DamageToKill() const;

	int PendingWeapon() const {
		if( primaryEnemy && primaryEnemy->ent && primaryEnemy->ent->r.client ) {
			return primaryEnemy->ent->r.client->ps.stats[STAT_PENDING_WEAPON];
		}
		return -1;
	}

	unsigned FireDelay() const;

	inline bool IsStaticSpot() const {
		return Ent()->r.client == nullptr;
	}

	inline const edict_t *Ent() const {
		CheckValid( __FUNCTION__ );
		return primaryEnemy->ent;
	}

	inline const edict_t *TraceKey() const {
		CheckValid( __FUNCTION__ );
		return primaryEnemy->ent;
	}

	inline const bool OnGround() const {
		CheckValid( __FUNCTION__ );
		return primaryEnemy->ent->groundentity != nullptr;
	}

	bool HaveQuad() const;
	bool HaveCarrier() const;
	bool Contain( const Enemy *enemy ) const;
	bool AreThreatening() const;
	float TotalInflictedDamage() const;

	float MaxDotProductOfBotViewAndDirToEnemy() const;
	float MaxDotProductOfEnemyViewAndDirToBot() const;

	typedef const Enemy **EnemiesIterator;
	inline EnemiesIterator begin() const { return (EnemiesIterator)activeEnemies.cbegin(); }
	inline EnemiesIterator end() const { return (EnemiesIterator)activeEnemies.cend(); }

	bool CanHit( const edict_t *ent ) const;

	bool HaveGoodSniperRangeWeapons() const;
	bool HaveGoodFarRangeWeapons() const;
	bool HaveGoodMiddleRangeWeapons() const;
	bool HaveGoodCloseRangeWeapons() const;
};

class BotWeaponSelector
{
	edict_t *self;

	SelectedWeapons &selectedWeapons;
	const SelectedEnemies &selectedEnemies;

	float weaponChoiceRandom;
	int64_t weaponChoiceRandomTimeoutAt;

	int64_t nextFastWeaponSwitchActionCheckAt;
	const unsigned weaponChoicePeriod;

public:
	BotWeaponSelector( edict_t *self_,
					   const SelectedEnemies &selectedEnemies_,
					   SelectedWeapons &selectedWeapons_,
					   unsigned weaponChoicePeriod_ )
		: self( self_ ),
		selectedWeapons( selectedWeapons_ ),
		selectedEnemies( selectedEnemies_ ),
		weaponChoiceRandom( 0.5f ),
		weaponChoiceRandomTimeoutAt( 0 ),
		nextFastWeaponSwitchActionCheckAt( 0 ),
		weaponChoicePeriod( weaponChoicePeriod_ ) {
		// Shut an analyzer up
		memset( &targetEnvironment, 0, sizeof( TargetEnvironment ) );
	}

	void Frame( const WorldState &cachedWorldState );
	void Think( const WorldState &cachedWorldState );

private:
#ifndef _MSC_VER
	inline void Debug( const char *format, ... ) const __attribute__( ( format( printf, 2, 3 ) ) )
#else
	inline void Debug( _Printf_format_string_ const char *format, ... ) const
#endif
	{
		va_list va;
		va_start( va, format );
		AI_Debugv( self->r.client->netname, format, va );
		va_end( va );
	}

	inline bool BotHasQuad() const { return ::HasQuad( self ); }
	inline bool BotHasShell() const { return ::HasShell( self ); }
	inline bool BotHasPowerups() const { return ::HasPowerups( self ); }
	inline bool BotIsCarrier() const { return ::IsCarrier( self ); }

	inline float DamageToKill( const edict_t *client ) const {
		return ::DamageToKill( client, g_armor_protection->value, g_armor_degradation->value );
	}

	inline const int *Inventory() const { return self->r.client->ps.inventory; }

	template <int Weapon>
	inline int AmmoReadyToFireCount() const {
		if( !Inventory()[Weapon] ) {
			return 0;
		}
		return Inventory()[WeaponAmmo < Weapon > ::strongAmmoTag] + Inventory()[WeaponAmmo < Weapon > ::weakAmmoTag];
	}

	inline int BlastsReadyToFireCount() const {
		// Check only strong ammo, the weak ammo enables blade attack
		return Inventory()[AMMO_GUNBLADE];
	}

	inline int ShellsReadyToFireCount() const { return AmmoReadyToFireCount<WEAP_RIOTGUN>(); }
	inline int GrenadesReadyToFireCount() const { return AmmoReadyToFireCount<WEAP_GRENADELAUNCHER>(); }
	inline int RocketsReadyToFireCount() const { return AmmoReadyToFireCount<WEAP_ROCKETLAUNCHER>(); }
	inline int PlasmasReadyToFireCount() const { return AmmoReadyToFireCount<WEAP_PLASMAGUN>(); }
	inline int BulletsReadyToFireCount() const { return AmmoReadyToFireCount<WEAP_MACHINEGUN>(); }
	inline int LasersReadyToFireCount() const { return AmmoReadyToFireCount<WEAP_LASERGUN>(); }
	inline int BoltsReadyToFireCount() const { return AmmoReadyToFireCount<WEAP_ELECTROBOLT>(); }

	bool CheckFastWeaponSwitchAction( const WorldState &worldState );

	void SuggestAimWeapon( const WorldState &worldState );
	void SuggestSniperRangeWeapon( const WorldState &worldState );
	void SuggestFarRangeWeapon( const WorldState &worldState );
	void SuggestMiddleRangeWeapon( const WorldState &worldState );
	void SuggestCloseRangeWeapon( const WorldState &worldState );

	int SuggestInstagibWeapon( const WorldState &worldState );
	int SuggestFinishWeapon( const WorldState &worldState );

	const AiScriptWeaponDef *SuggestScriptWeapon( const WorldState &worldState, int *effectiveTier );
	bool IsEnemyEscaping( const WorldState &worldState, bool *botMovesFast, bool *enemyMovesFast );

	int SuggestHitEscapingEnemyWeapon( const WorldState &worldState, bool botMovesFast, bool enemyMovesFast );

	bool CheckForShotOfDespair( const WorldState &worldState );
	int SuggestShotOfDespairWeapon( const WorldState &worldState );
	int SuggestQuadBearerWeapon( const WorldState &worldState );

	int ChooseWeaponByScores( struct WeaponAndScore *begin, struct WeaponAndScore *end );

	struct TargetEnvironment {
		// Sides are relative to direction from bot origin to target origin
		// Order: top, bottom, front, back, left, right
		trace_t sideTraces[6];

		enum Side { TOP, BOTTOM, FRONT, BACK, LEFT, RIGHT };

		float factor;
		static const float TRACE_DEPTH;
	};

	TargetEnvironment targetEnvironment;
	void TestTargetEnvironment( const Vec3 &botOrigin, const Vec3 &targetOrigin, const edict_t *traceKey );

	void SetSelectedWeapons( int builtinWeapon, int scriptWeapon, bool preferBuiltinWeapon, unsigned timeoutPeriod );
};

#endif
