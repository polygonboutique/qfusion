#include "bot_weapon_selector.h"
#include "bot.h"
#include "../../gameshared/q_collision.h"

bool SelectedEnemies::AreValid() const {
	for( const Enemy *enemy: activeEnemies )
		if( !enemy->IsValid() ) {
			return false;
		}

	return timeoutAt > level.time;
}

void SelectedEnemies::Set( const Enemy *primaryEnemy_,
						   unsigned timeoutPeriod,
						   const Enemy * const *activeEnemiesBegin,
						   const Enemy * const *activeEnemiesEnd ) {
	this->primaryEnemy = primaryEnemy_;
	this->timeoutAt = level.time + timeoutPeriod;

#ifndef _DEBUG
	if( !activeEnemies.empty() ) {
		AI_FailWith( "SelectedEnemies::Set()", "activeEnemies.size() %d > 0", activeEnemies.size() );
	}
#endif

	for( const Enemy *const *iter = activeEnemiesBegin; iter != activeEnemiesEnd; ++iter )
		this->activeEnemies.push_back( *iter );
}

Vec3 SelectedEnemies::ClosestEnemyOrigin( const vec3_t relativelyTo ) const {
	const Enemy *closestEnemy = nullptr;
	float minSquareDistance = std::numeric_limits<float>::max();
	for( const Enemy *enemy: activeEnemies ) {
		float squareDistance = enemy->LastSeenPosition().SquareDistanceTo( relativelyTo );
		if( minSquareDistance > squareDistance ) {
			minSquareDistance = squareDistance;
			closestEnemy = enemy;
		}
	}
	return closestEnemy->LastSeenPosition();
}

float SelectedEnemies::DamageToKill() const {
	CheckValid( __FUNCTION__ );
	float result = 0.0f;
	for( const Enemy *enemy: activeEnemies ) {
		float damageToKill = ::DamageToKill( enemy->ent, g_armor_protection->value, g_armor_degradation->value );
		if( enemy->HasShell() ) {
			damageToKill *= 4.0f;
		}
		result += damageToKill;
	}

	return result;
}

unsigned SelectedEnemies::FireDelay() const {
	unsigned minDelay = std::numeric_limits<unsigned>::max();

	for( const Enemy *enemy: activeEnemies ) {
		if( !enemy->IsValid() ) {
			return std::numeric_limits<unsigned>::max();
		}

		if( !enemy->ent->r.client ) {
			return 0;
		}

		unsigned delay = (unsigned)enemy->ent->r.client->ps.stats[STAT_WEAPON_TIME];
		if( delay < minDelay ) {
			minDelay = delay;
		}
	}

	return minDelay;
}

bool SelectedEnemies::HaveQuad() const {
	CheckValid( __FUNCTION__ );
	for( const Enemy *enemy: activeEnemies )
		if( enemy->HasQuad() ) {
			return true;
		}

	return false;
}

bool SelectedEnemies::HaveCarrier() const {
	CheckValid( __FUNCTION__ );
	for( const Enemy *enemy: activeEnemies )
		if( enemy->IsCarrier() ) {
			return true;
		}

	return false;
}

bool SelectedEnemies::Contain( const Enemy *enemy ) const {
	CheckValid( __FUNCTION__ );
	for( const Enemy *activeEnemy: activeEnemies )
		if( activeEnemy == enemy ) {
			return true;
		}

	return false;
}

bool SelectedEnemies::AreThreatening() const {
	CheckValid( __FUNCTION__ );
	const int64_t levelTime = level.time;
	for( const Enemy *activeEnemy: activeEnemies ) {
		if( levelTime - activeEnemy->LastAttackedByTime() < 1000 ) {
			return true;
		}

		// Try cut off quite expensive AngleVectors() call
		// TODO: Check environment trace too
		if( activeEnemy->ent->r.client ) {
			// If an enemy can shoot soon
			if( activeEnemy->ent->r.client->ps.stats[STAT_WEAPON_TIME] < 300 ) {
				Vec3 enemyToBotDir( self->s.origin );
				enemyToBotDir -= activeEnemy->ent->s.origin;
				enemyToBotDir.NormalizeFast();
				if( enemyToBotDir.Dot( activeEnemy->LookDir() ) > 0.9f ) {
					return true;
				}
			}
		} else {
			Vec3 enemyToBotDir( self->s.origin );
			enemyToBotDir -= activeEnemy->ent->s.origin;
			enemyToBotDir.NormalizeFast();
			if( enemyToBotDir.Dot( activeEnemy->LookDir() ) > 0.9f ) {
				return true;
			}
		}
	}

	return false;
}

float SelectedEnemies::TotalInflictedDamage() const {
	CheckValid( __FUNCTION__ );

	float damage = 0;
	for( const Enemy *activeEnemy: activeEnemies )
		damage += activeEnemy->TotalInflictedDamage();

	return damage;
}

float SelectedEnemies::MaxDotProductOfBotViewAndDirToEnemy() const {
	vec3_t botViewDir;

	float maxDot = -1.0f;
	for( const Enemy *enemy: activeEnemies ) {
		Vec3 toEnemyDir( enemy->LastSeenPosition() );
		toEnemyDir -= self->s.origin;
		toEnemyDir.NormalizeFast();
		float dot = toEnemyDir.Dot( botViewDir );
		if( dot > maxDot ) {
			maxDot = dot;
		}
	}
	return maxDot;
}

float SelectedEnemies::MaxDotProductOfEnemyViewAndDirToBot() const {
	float maxDot = -1.0f;
	for( const Enemy *enemy: activeEnemies ) {
		Vec3 toBotDir( self->s.origin );
		toBotDir -= enemy->LastSeenPosition();
		toBotDir.NormalizeFast();
		float dot = toBotDir.Dot( enemy->LookDir() );
		if( dot > maxDot ) {
			maxDot = dot;
		}
	}
	return maxDot;
}

bool SelectedEnemies::CanHit( const edict_t *ent ) const {
	CheckValid( __FUNCTION__ );

	edict_t *targetEnt = const_cast<edict_t *>( ent );
	trace_t trace;
	for( const Enemy *activeEnemy: activeEnemies ) {
		edict_t *enemyEnt = const_cast<edict_t *>( activeEnemy->ent );
		Vec3 traceStart( enemyEnt->s.origin );
		if( enemyEnt->r.client ) {
			// Put likely case (not crouching) first
			if( enemyEnt->r.client->ps.stats[PM_STAT_CROUCHTIME] == 0 ) {
				traceStart.Z() += playerbox_stand_viewheight;
			} else {
				traceStart.Z() += playerbox_crouch_viewheight;
			}
		}
		G_Trace( &trace, traceStart.Data(), nullptr, nullptr, targetEnt->s.origin, enemyEnt, MASK_AISOLID );
		if( trace.fraction != 1.0f && game.edicts + trace.ent == targetEnt ) {
			return true;
		}

		vec3_t bounds[2];
		VectorCopy( ent->r.absmin, bounds[0] );
		VectorCopy( ent->r.absmax, bounds[1] );
		for( int i = 0; i < 8; ++i ) {
			vec3_t traceEnd = { bounds[( i >> 2 ) & 1][0], bounds[( i >> 1 ) & 1][0], bounds[( i >> 0 ) & 1][2] };
			G_Trace( &trace, traceStart.Data(), nullptr, nullptr, traceEnd, enemyEnt, MASK_AISOLID );
			if( trace.fraction != 1.0f && game.edicts + trace.ent == targetEnt ) {
				return true;
			}
		}
	}
	return false;
}

bool SelectedEnemies::HaveGoodSniperRangeWeapons() const {
	CheckValid( __FUNCTION__ );
	for( const Enemy *activeEnemy: activeEnemies ) {
		if( activeEnemy->BoltsReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->BulletsReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->InstasReadyToFireCount() ) {
			return true;
		}
	}
	return false;
}

bool SelectedEnemies::HaveGoodFarRangeWeapons() const {
	CheckValid( __FUNCTION__ );
	for( const Enemy *activeEnemy: activeEnemies ) {
		if( activeEnemy->BoltsReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->BulletsReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->PlasmasReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->InstasReadyToFireCount() ) {
			return true;
		}
	}
	return false;
}

bool SelectedEnemies::HaveGoodMiddleRangeWeapons() const {
	CheckValid( __FUNCTION__ );
	for( const Enemy *activeEnemy: activeEnemies ) {
		if( activeEnemy->RocketsReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->LasersReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->PlasmasReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->BulletsReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->ShellsReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->InstasReadyToFireCount() ) {
			return true;
		}
	}
	return false;
}

bool SelectedEnemies::HaveGoodCloseRangeWeapons() const {
	CheckValid( __FUNCTION__ );
	for( const Enemy *activeEnemy: activeEnemies ) {
		if( activeEnemy->RocketsReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->PlasmasReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->ShellsReadyToFireCount() ) {
			return true;
		}
	}
	return false;
}

void BotWeaponSelector::Frame( const WorldState &cachedWorldState ) {
	if( nextFastWeaponSwitchActionCheckAt > level.time ) {
		return;
	}

	if( !self->ai->botRef->selectedEnemies.AreValid() ) {
		return;
	}

	// cachedWorldState is cached for Think() period and might be out of sync with selectedEnemies
	if( cachedWorldState.EnemyOriginVar().Ignore() ) {
		return;
	}

	if( CheckFastWeaponSwitchAction( cachedWorldState ) ) {
		nextFastWeaponSwitchActionCheckAt = level.time + 750;
	}
}

void BotWeaponSelector::Think( const WorldState &cachedWorldState ) {
	if( selectedWeapons.AreValid() ) {
		return;
	}

	if( !self->ai->botRef->selectedEnemies.AreValid() ) {
		return;
	}

	// cachedWorldState is cached for Think() period and might be out of sync with selectedEnemies
	if( cachedWorldState.EnemyOriginVar().Ignore() ) {
		return;
	}

	if( weaponChoiceRandomTimeoutAt <= level.time ) {
		weaponChoiceRandom = random();
		weaponChoiceRandomTimeoutAt = level.time + 2000;
	}

	SuggestAimWeapon( cachedWorldState );
}

bool BotWeaponSelector::CheckFastWeaponSwitchAction( const WorldState &worldState ) {
	if( self->r.client->ps.stats[STAT_WEAPON_TIME] >= 64 ) {
		return false;
	}

	// Easy bots do not perform fast weapon switch actions
	if( self->ai->botRef->Skill() < 0.33f ) {
		return false;
	}

	if( self->ai->botRef->Skill() < 0.66f ) {
		// Mid-skill bots do these actions in non-think frames occasionally
		if( self->ai->botRef->ShouldSkipThinkFrame() && random() > self->ai->botRef->Skill() ) {
			return false;
		}
	}

	bool botMovesFast, enemyMovesFast;

	int chosenWeapon = WEAP_NONE;
	if( worldState.DamageToKill() < 50 ) {
		chosenWeapon = SuggestFinishWeapon( worldState );
	}
	// Try to hit escaping enemies hard in a single shot
	else if( IsEnemyEscaping( worldState, &botMovesFast, &enemyMovesFast ) ) {
		chosenWeapon = SuggestHitEscapingEnemyWeapon( worldState, botMovesFast, enemyMovesFast );
	}
	// Try to hit enemy hard in a single shot before death
	else if( CheckForShotOfDespair( worldState ) ) {
		chosenWeapon = SuggestShotOfDespairWeapon( worldState );
	}

	if( chosenWeapon != WEAP_NONE ) {
		unsigned timeoutPeriod = 64 + 1;
		if( selectedWeapons.TimeoutAt() > level.time + 64 + 1 ) {
			timeoutPeriod += level.time - selectedWeapons.timeoutAt - 64 - 1;
		}
		SetSelectedWeapons( chosenWeapon, selectedWeapons.ScriptWeaponNum(), true, timeoutPeriod );
		return true;
	}

	return false;
}

static constexpr float CLOSE_RANGE = 150.0f;

inline float GetLaserRange() {
	const auto lgDef = GS_GetWeaponDef( WEAP_LASERGUN );
	return ( lgDef->firedef.timeout + lgDef->firedef.timeout ) / 2.0f;
}

void BotWeaponSelector::SuggestAimWeapon( const WorldState &worldState ) {
	Vec3 botOrigin( self->s.origin );
	TestTargetEnvironment( botOrigin, selectedEnemies.LastSeenOrigin(), selectedEnemies.Ent() );

	if( GS_Instagib() ) {
		// TODO: Select script weapon too
		SetSelectedWeapons( SuggestInstagibWeapon( worldState ), -1, true, weaponChoicePeriod );
		return;
	}

	if( BotHasPowerups() ) {
		// TODO: Select script weapon too
		SetSelectedWeapons( SuggestQuadBearerWeapon( worldState ), -1, true, weaponChoicePeriod );
		return;
	}

	if( worldState.EnemyIsOnSniperRange() ) {
		SuggestSniperRangeWeapon( worldState );
	} else if( worldState.EnemyIsOnFarRange() ) {
		SuggestFarRangeWeapon( worldState );
	} else if( worldState.EnemyIsOnMiddleRange() ) {
		SuggestMiddleRangeWeapon( worldState );
	} else {
		SuggestCloseRangeWeapon( worldState );
	}
}

void BotWeaponSelector::SuggestSniperRangeWeapon( const WorldState &worldState ) {
	int chosenWeapon = WEAP_NONE;

	// Spam plasma from long range to blind enemy
	if( selectedEnemies.PendingWeapon() == WEAP_ELECTROBOLT ) {
		if( PlasmasReadyToFireCount() && weaponChoiceRandom > 0.5f ) {
			chosenWeapon = WEAP_PLASMAGUN;
		}
	} else if( !self->ai->botRef->ShouldKeepXhairOnEnemy() ) {
		// In this case try preferring weapons that does not require precise aiming.
		// Otherwise the bot is unlikely to start firing at all due to view angles mismatch.
		if( PlasmasReadyToFireCount() && weaponChoiceRandom > 0.7f ) {
			chosenWeapon = WEAP_PLASMAGUN;
		} else if( ShellsReadyToFireCount() ) {
			chosenWeapon = WEAP_RIOTGUN;
		}
	}

	if( chosenWeapon == WEAP_NONE ) {
		// Add a hack for draining health with MG if there is no health to pick on the level
		if( worldState.DamageToKill() > 50.0f && ( level.gametype.spawnableItemsMask & IT_HEALTH ) ) {
			if( BoltsReadyToFireCount() ) {
				chosenWeapon = WEAP_ELECTROBOLT;
			} else if( BulletsReadyToFireCount() ) {
				chosenWeapon = WEAP_MACHINEGUN;
			}
		} else {
			if( BulletsReadyToFireCount() ) {
				chosenWeapon = WEAP_MACHINEGUN;
			} else if( BoltsReadyToFireCount() ) {
				chosenWeapon = WEAP_ELECTROBOLT;
			}
		}
	}
	// Still not chosen
	if( chosenWeapon == WEAP_NONE ) {
		if( worldState.DamageToKill() < 25.0f && ShellsReadyToFireCount() ) {
			chosenWeapon = WEAP_RIOTGUN;
		} else {
			chosenWeapon = WEAP_GUNBLADE;
		}
	}

	Debug( "(sniper range)... : chose %s \n", GS_GetWeaponDef( chosenWeapon )->name );

	int scriptWeaponTier;
	if( const auto *scriptWeaponDef = SuggestScriptWeapon( worldState, &scriptWeaponTier ) ) {
		bool preferBuiltinWeapon = true;
		if( scriptWeaponTier > 3 ) {
			preferBuiltinWeapon = false;
		} else if( scriptWeaponTier > 1 ) {
			preferBuiltinWeapon = ( chosenWeapon == WEAP_ELECTROBOLT );
		}
		SetSelectedWeapons( chosenWeapon, scriptWeaponDef->weaponNum, preferBuiltinWeapon, weaponChoicePeriod );
	} else {
		SetSelectedWeapons( chosenWeapon, -1, true, weaponChoicePeriod );
	}
}
struct WeaponAndScore {
	int weapon;
	float score;
	WeaponAndScore( int weapon = WEAP_NONE, float score = 0.0f ) {
		this->weapon = weapon;
		this->score = score;
	}
};

int BotWeaponSelector::ChooseWeaponByScores( struct WeaponAndScore *begin, struct WeaponAndScore *end ) {
	int weapon = WEAP_NONE;
	const int pendingWeapon = self->r.client->ps.stats[STAT_PENDING_WEAPON];
	float pendingWeaponScore = std::numeric_limits<float>::infinity();
	for( WeaponAndScore *it = begin; it != end; ++it ) {
		if( pendingWeapon == it->weapon ) {
			pendingWeaponScore = it->score;
			break;
		}
	}
	float maxScore = 0.0f;
	if( pendingWeaponScore != std::numeric_limits<float>::infinity() ) {
		float weightDiffThreshold = 0.3f;
		// Do not switch too often continuous fire weapons
		if( pendingWeapon == WEAP_PLASMAGUN || pendingWeapon == WEAP_MACHINEGUN ) {
			weightDiffThreshold += 0.15f;
		} else if( pendingWeapon == WEAP_LASERGUN ) {
			weightDiffThreshold += 0.15f;
		}
		for( WeaponAndScore *it = begin; it != end; ++it ) {
			float currScore = it->score;
			if( maxScore < currScore ) {
				// Do not change weapon if its score is almost equal to current one to avoid weapon choice "jitter"
				// when a bot tries to change weapon infinitely when weapon scores are close to each other
				if( pendingWeapon == it->weapon || fabsf( currScore - pendingWeaponScore ) > weightDiffThreshold ) {
					maxScore = currScore;
					weapon = it->weapon;
				}
			}
		}
	} else {
		for( WeaponAndScore *it = begin; it != end; ++it ) {
			float currScore = it->score;
			if( maxScore < currScore ) {
				maxScore = currScore;
				weapon = it->weapon;
			}
		}
	}

	return weapon;
}

void BotWeaponSelector::SuggestFarRangeWeapon( const WorldState &worldState ) {
	// First, try to choose a long range weapon of EB, MG, PG and RG
	enum { EB, MG, PG, RG };
	WeaponAndScore weaponScores[4] =
	{
		WeaponAndScore( WEAP_ELECTROBOLT, 1.0f * BoundedFraction( BoltsReadyToFireCount(), 2.0f ) ),
		WeaponAndScore( WEAP_MACHINEGUN, 1.0f * BoundedFraction( BulletsReadyToFireCount(), 10.0f ) ),
		WeaponAndScore( WEAP_PLASMAGUN, 0.8f * BoundedFraction( PlasmasReadyToFireCount(), 15.0f ) ),
		WeaponAndScore( WEAP_RIOTGUN, 0.6f * BoundedFraction( ShellsReadyToFireCount(), 2 ) )
	};

	weaponScores[EB].score += self->ai->botRef->Skill() / 3;

	// Counteract EB with PG or MG
	if( selectedEnemies.PendingWeapon() == WEAP_ELECTROBOLT ) {
		weaponScores[PG].score *= 1.3f;
		weaponScores[MG].score *= 1.2f;
	}
	// Counteract PG with MG or EB
	if( selectedEnemies.PendingWeapon() == WEAP_PLASMAGUN ) {
		weaponScores[EB].score *= 1.4f;
		weaponScores[MG].score *= 1.2f;
	}
	// Counteract MG with PG or EB
	if( selectedEnemies.PendingWeapon() == WEAP_MACHINEGUN ) {
		weaponScores[PG].score *= 1.2f;
		weaponScores[EB].score *= 1.3f;
	}

	// Do not use plasma on fast-moving side-to-side enemies
	Vec3 targetMoveDir( selectedEnemies.LastSeenVelocity() );
	float enemySpeed = targetMoveDir.SquaredLength();
	if( enemySpeed > 0.1f ) {
		enemySpeed = 1.0f / Q_RSqrt( enemySpeed );
	}
	if( enemySpeed > DEFAULT_DASHSPEED ) {
		targetMoveDir *= 1.0f / enemySpeed;
		Vec3 botToTargetDir = selectedEnemies.LastSeenOrigin() - self->s.origin;
		botToTargetDir.NormalizeFast();

		float speedFactor = BoundedFraction( enemySpeed - DEFAULT_DASHSPEED, 1000.0f - DEFAULT_DASHSPEED );
		float dirFactor = fabsf( botToTargetDir.Dot( targetMoveDir ) );
		// If enemy moves fast but on botToTargetDir line, pg score is unaffected
		weaponScores[PG].score *= 1.0f - ( 1.0f - dirFactor ) * speedFactor;
	}

	if( self->ai->botRef->IsInSquad() ) {
		// In squad prefer MG to gun down an enemy together
		weaponScores[MG].score *= 1.75f;
	}

	// Add extra scores for weapons that do not require precise aiming in this case
	if( !self->ai->botRef->ShouldKeepXhairOnEnemy() ) {
		weaponScores[PG].score *= 3.0f;
		weaponScores[RG].score *= 2.0f;
	}

	// Add a hack for draining health with MG if there is no health to pick on the level
	if( !( level.gametype.spawnableItemsMask & IT_HEALTH ) ) {
		weaponScores[MG].score *= 2.25f;
		weaponScores[RG].score *= 1.25f;
	}

	int chosenWeapon = ChooseWeaponByScores( weaponScores, weaponScores + 4 );

	if( chosenWeapon == WEAP_NONE ) {
		// Bot needs to have lots of rocket since most of rockets will not hit at this distance
		float rocketScore = targetEnvironment.factor * std::min( 6, RocketsReadyToFireCount() ) / 6.0f;
		if( rocketScore > 0.4f ) {
			chosenWeapon = WEAP_ROCKETLAUNCHER;
		} else {
			chosenWeapon = WEAP_GUNBLADE;
		}
	}

	Debug( "(far range)... chose %s\n", GS_GetWeaponDef( chosenWeapon )->name );

	int scriptWeaponTier;
	if( const auto *scriptWeaponDef = SuggestScriptWeapon( worldState, &scriptWeaponTier ) ) {
		bool preferBuiltinWeapon = true;
		if( scriptWeaponTier > 3 ) {
			preferBuiltinWeapon = false;
		} else if( scriptWeaponTier > 1 ) {
			preferBuiltinWeapon = ( chosenWeapon == WEAP_ELECTROBOLT );
		}
		SetSelectedWeapons( chosenWeapon, scriptWeaponDef->weaponNum, preferBuiltinWeapon, weaponChoicePeriod );
	} else {
		SetSelectedWeapons( chosenWeapon, -1, true, weaponChoicePeriod );
	}
}

void BotWeaponSelector::SuggestMiddleRangeWeapon( const WorldState &worldState ) {
	const float lgRange = GetLaserRange();
	// Should be equal to max mid range distance - min mid range distance
	const float midRangeLen = lgRange - CLOSE_RANGE;
	// Relative distance from min mid range distance
	const float midRangeDistance = worldState.DistanceToEnemy() - CLOSE_RANGE;

	int chosenWeapon = WEAP_NONE;

	enum { RL, LG, PG, MG, RG, GL };
	WeaponAndScore weaponScores[6];
	weaponScores[RL].weapon = WEAP_ROCKETLAUNCHER;
	weaponScores[LG].weapon = WEAP_LASERGUN;
	weaponScores[PG].weapon = WEAP_PLASMAGUN;
	weaponScores[MG].weapon = WEAP_MACHINEGUN;
	weaponScores[RG].weapon = WEAP_RIOTGUN;
	weaponScores[GL].weapon = WEAP_GRENADELAUNCHER;

	weaponScores[RL].score = 1.5f * BoundedFraction( RocketsReadyToFireCount(), 3.0f );
	weaponScores[LG].score = 1.5f * BoundedFraction( LasersReadyToFireCount(), 15.0f );
	weaponScores[PG].score = 0.7f * BoundedFraction( PlasmasReadyToFireCount(), 15.0f );
	weaponScores[MG].score = 1.0f * BoundedFraction( BulletsReadyToFireCount(), 15.0f );
	weaponScores[RG].score = 0.7f * BoundedFraction( ShellsReadyToFireCount(), 3.0f );
	weaponScores[GL].score = 0.5f * BoundedFraction( GrenadesReadyToFireCount(), 5.0f );

	if( self->ai->botRef->IsInSquad() ) {
		// In squad prefer continuous fire weapons to burn an enemy quick together
		float boost = 1.5f;
		weaponScores[LG].score *= boost;
		weaponScores[PG].score *= boost;
		weaponScores[MG].score *= boost;
	}

	if( self->ai->botRef->WillAdvance() ) {
		weaponScores[RL].score *= 1.3f;
		weaponScores[GL].score *= 0.7f;
		weaponScores[RG].score *= 1.1f;
	}
	if( self->ai->botRef->WillRetreat() ) {
		// Plasma is a great defensive weapon
		weaponScores[PG].score *= 1.5f;
		weaponScores[LG].score *= 1.1f;
		weaponScores[MG].score *= 1.1f;
		weaponScores[GL].score *= 1.1f;
	}

	// 1 on mid range bound, 0 on close range bound
	float distanceFactor = ( worldState.DistanceToEnemy() - CLOSE_RANGE ) / ( lgRange - CLOSE_RANGE );

	weaponScores[RL].score *= 1.0f - distanceFactor;
	weaponScores[LG].score *= 0.7f + 0.3f * distanceFactor;
	weaponScores[PG].score *= 1.0f - 0.4f * distanceFactor;
	weaponScores[MG].score *= 0.3f + 0.7f * distanceFactor;
	weaponScores[RG].score *= 1.0f - 0.7f * distanceFactor;
	// GL score is maximal in the middle on mid-range zone and is zero on the zone bounds
	weaponScores[GL].score *= 1.0f - fabsf( midRangeDistance - midRangeLen / 2.0f ) / midRangeDistance;

	weaponScores[RL].score *= targetEnvironment.factor;
	weaponScores[LG].score *= 1.0f - 0.4f * targetEnvironment.factor;
	weaponScores[PG].score *= 0.5f + 0.5f * targetEnvironment.factor;
	weaponScores[MG].score *= 1.0f - 0.4f * targetEnvironment.factor;
	weaponScores[RG].score *= 1.0f - 0.5f * targetEnvironment.factor;
	weaponScores[GL].score *= targetEnvironment.factor;

	// Add extra scores for weapons that do not require precise aiming in this case
	if( !self->ai->botRef->ShouldKeepXhairOnEnemy() ) {
		weaponScores[PG].score *= 1.5f;
		weaponScores[RG].score *= 2.0f;
		weaponScores[GL].score *= 1.5f;
		if( self->ai->botRef->WillRetreat() ) {
			weaponScores[PG].score *= 1.5f;
			weaponScores[GL].score *= 1.5f;
		}
	}

	chosenWeapon = ChooseWeaponByScores( weaponScores, weaponScores + 6 );
	if( chosenWeapon == WEAP_NONE ) {
		chosenWeapon = WEAP_GUNBLADE;
	}

	int scriptWeaponTier = 0;
	if( const auto *scriptWeaponDef = SuggestScriptWeapon( worldState, &scriptWeaponTier ) ) {
		bool preferBuiltinWeapon = scriptWeaponTier < BuiltinWeaponTier( chosenWeapon );
		SetSelectedWeapons( chosenWeapon, scriptWeaponDef->weaponNum, preferBuiltinWeapon, weaponChoicePeriod );
	} else {
		SetSelectedWeapons( chosenWeapon, -1, true, weaponChoicePeriod );
	}
}

void BotWeaponSelector::SuggestCloseRangeWeapon( const WorldState &worldState ) {
	int chosenWeapon = WEAP_NONE;

	int lasersCount = LasersReadyToFireCount();
	int rocketsCount = RocketsReadyToFireCount();
	int plasmasCount = PlasmasReadyToFireCount();

	float distanceFactor = BoundedFraction( worldState.DistanceToEnemy(), CLOSE_RANGE );

	if( g_allow_selfdamage->integer ) {
		if( ShellsReadyToFireCount() && ( worldState.DamageToKill() < 90 || weaponChoiceRandom < 0.4 ) ) {
			chosenWeapon = WEAP_RIOTGUN;
		} else if( rocketsCount > 0 && worldState.DamageToBeKilled() > 100.0f - 75.0f * distanceFactor ) {
			chosenWeapon = WEAP_ROCKETLAUNCHER;
		} else if( plasmasCount > 10 && worldState.DamageToBeKilled() > 75.0f - 50.0f * distanceFactor ) {
			chosenWeapon = WEAP_PLASMAGUN;
		} else if( lasersCount > 10 ) {
			chosenWeapon = WEAP_LASERGUN;
		}
	} else {
		if( rocketsCount ) {
			chosenWeapon = WEAP_ROCKETLAUNCHER;
		} else if( lasersCount > 10 ) {
			chosenWeapon = WEAP_LASERGUN;
		}
	}
	// Still not chosen
	if( chosenWeapon == WEAP_NONE ) {
		int shellsCount = ShellsReadyToFireCount();
		if( shellsCount > 0 ) {
			chosenWeapon = WEAP_RIOTGUN;
		}
		if( plasmasCount > 0 ) {
			chosenWeapon = WEAP_PLASMAGUN;
		} else if( lasersCount > 0 ) {
			chosenWeapon = WEAP_LASERGUN;
		} else if( BulletsReadyToFireCount() ) {
			chosenWeapon = WEAP_MACHINEGUN;
		} else if( BoltsReadyToFireCount() ) {
			chosenWeapon = WEAP_ELECTROBOLT;
		} else {
			chosenWeapon = WEAP_GUNBLADE;
		}
	}

	Debug( "(close range) : chose %s \n", GS_GetWeaponDef( chosenWeapon )->name );

	int scriptWeaponTier;
	if( const auto *scriptWeaponDef = SuggestScriptWeapon( worldState, &scriptWeaponTier ) ) {
		bool preferBuiltinWeapon = scriptWeaponTier < BuiltinWeaponTier( chosenWeapon );
		SetSelectedWeapons( chosenWeapon, scriptWeaponDef->weaponNum, preferBuiltinWeapon, weaponChoicePeriod );
	} else {
		SetSelectedWeapons( chosenWeapon, -1, true, weaponChoicePeriod );
	}
}

const AiScriptWeaponDef *BotWeaponSelector::SuggestScriptWeapon( const WorldState &worldState, int *effectiveTier ) {
	const auto &scriptWeaponDefs = self->ai->botRef->scriptWeaponDefs;
	const auto &scriptWeaponCooldown = self->ai->botRef->scriptWeaponCooldown;

	const AiScriptWeaponDef *bestWeapon = nullptr;
	float bestScore = 0.000001f;

	const float distanceToEnemy = worldState.DistanceToEnemy();

	for( unsigned i = 0; i < scriptWeaponDefs.size(); ++i ) {
		const auto &weaponDef = scriptWeaponDefs[i];
		int cooldown = scriptWeaponCooldown[i];
		if( cooldown >= 1000 ) {
			continue;
		}

		if( distanceToEnemy > weaponDef.maxRange ) {
			continue;
		}
		if( distanceToEnemy < weaponDef.minRange ) {
			continue;
		}

		float score = 1.0f;

		score *= 1.0f - BoundedFraction( cooldown, 1000.0f );

		if( GS_SelfDamage() ) {
			float estimatedSelfDamage = 0.0f;
			estimatedSelfDamage = weaponDef.maxSelfDamage;
			estimatedSelfDamage *= ( 1.0f - BoundedFraction( worldState.DistanceToEnemy(), weaponDef.splashRadius ) );
			if( estimatedSelfDamage > 100.0f ) {
				continue;
			}
			if( worldState.DistanceToEnemy() < estimatedSelfDamage ) {
				continue;
			}
			score *= 1.0f - BoundedFraction( estimatedSelfDamage, 100.0f );
		}

		// We assume that maximum ordinary tier is 3
		score *= weaponDef.tier / 3.0f;

		// Treat points in +/- 192 units of best range as in best range too
		float bestRangeLowerBounds = weaponDef.bestRange - std::min( 192.0f, weaponDef.bestRange - weaponDef.minRange );
		float bestRangeUpperBounds = weaponDef.bestRange + std::min( 192.0f, weaponDef.maxRange - weaponDef.bestRange );

		if( distanceToEnemy < bestRangeLowerBounds ) {
			score *= distanceToEnemy / bestRangeLowerBounds;
		} else if( distanceToEnemy > bestRangeUpperBounds ) {
			score *= ( distanceToEnemy - bestRangeUpperBounds ) / ( weaponDef.maxRange - bestRangeLowerBounds );
		}

		if( score > bestScore ) {
			bestScore = score;
			bestWeapon = &scriptWeaponDefs[i];
			*effectiveTier = (int)( score * 3.0f + 0.99f );
		}
	}

	return bestWeapon;
}

int BotWeaponSelector::SuggestFinishWeapon( const WorldState &worldState ) {
	const float distance = worldState.DistanceToEnemy();
	const float damageToBeKilled = worldState.DamageToBeKilled();
	const float damageToKill = worldState.DamageToKill();

	if( worldState.DistanceToEnemy() < CLOSE_RANGE ) {
		if( g_allow_selfdamage->integer && damageToBeKilled < 75 ) {
			if( LasersReadyToFireCount() > damageToKill * 0.3f * 14 ) {
				return WEAP_LASERGUN;
			}
			if( ShellsReadyToFireCount() > 0 ) {
				return WEAP_RIOTGUN;
			}
			if( BulletsReadyToFireCount() > damageToKill * 0.3f * 10 ) {
				return WEAP_MACHINEGUN;
			}
			if( PlasmasReadyToFireCount() > damageToKill * 0.3f * 14 ) {
				return WEAP_PLASMAGUN;
			}
			// Hard bots do not do this high risk action
			if( self->ai->botRef->Skill() < 0.66f && RocketsReadyToFireCount() ) {
				return WEAP_ROCKETLAUNCHER;
			}
		} else {
			if( RocketsReadyToFireCount() && targetEnvironment.factor > 0 ) {
				return WEAP_ROCKETLAUNCHER;
			}
			if( ShellsReadyToFireCount() ) {
				return WEAP_RIOTGUN;
			}
			if( PlasmasReadyToFireCount() > worldState.DamageToKill() * 0.3f * 14 ) {
				return WEAP_PLASMAGUN;
			}
			if( LasersReadyToFireCount() > worldState.DamageToKill() * 0.3f * 14 ) {
				return WEAP_LASERGUN;
			}
			if( BulletsReadyToFireCount() > worldState.DamageToKill() * 0.3f * 10 ) {
				return WEAP_MACHINEGUN;
			}
		}
		return WEAP_GUNBLADE;
	}

	const float lgRange = GetLaserRange();
	if( distance < lgRange ) {
		if( distance < lgRange / 2 && targetEnvironment.factor > 0.6f && RocketsReadyToFireCount() ) {
			return WEAP_ROCKETLAUNCHER;
		}
		if( BoltsReadyToFireCount() && damageToKill > 30 && selectedEnemies.PendingWeapon() == WEAP_LASERGUN ) {
			return WEAP_ELECTROBOLT;
		}
		if( LasersReadyToFireCount() > damageToKill * 0.3f * 14 ) {
			return WEAP_LASERGUN;
		}
		if( ShellsReadyToFireCount() ) {
			return WEAP_RIOTGUN;
		}
		if( BulletsReadyToFireCount() > damageToKill * 0.3f * 10 ) {
			return WEAP_MACHINEGUN;
		}
		if( PlasmasReadyToFireCount() > damageToKill * 0.3f * 14 ) {
			return WEAP_PLASMAGUN;
		}

		// Surprise...
		if( weaponChoiceRandom < 0.15f && self->ai->botRef->Skill() > 0.5f ) {
			if( GrenadesReadyToFireCount() && targetEnvironment.factor > 0.5f ) {
				return WEAP_GRENADELAUNCHER;
			}
		}

		return WEAP_GUNBLADE;
	}

	if( BulletsReadyToFireCount() > damageToKill * 0.3f * 10 ) {
		return WEAP_MACHINEGUN;
	}
	if( BoltsReadyToFireCount() ) {
		return WEAP_ELECTROBOLT;
	}
	if( ShellsReadyToFireCount() ) {
		return WEAP_RIOTGUN;
	}

	return WEAP_GUNBLADE;
}

static bool IsEscapingFromStandingEntity( const edict_t *escaping, const edict_t *standing, float escapingVelocitySqLen ) {
	// Too low relative speed with almost standing enemy
	float speedThreshold = DEFAULT_PLAYERSPEED * 1.35f;
	if( escapingVelocitySqLen < speedThreshold * speedThreshold ) {
		return false;
	}

	Vec3 escapingVelocityDir( escaping->velocity );
	escapingVelocityDir *= Q_RSqrt( escapingVelocitySqLen );

	Vec3 escapingToStandingDir( standing->s.origin );
	escapingToStandingDir -= escaping->s.origin;

	float len = escapingToStandingDir.SquaredLength();
	if( len < 1 ) {
		return false;
	}

	escapingToStandingDir *= Q_RSqrt( len );
	return escapingToStandingDir.Dot( escapingVelocityDir ) < -0.5f;
}

bool BotWeaponSelector::IsEnemyEscaping( const WorldState &worldState, bool *botMovesFast, bool *enemyMovesFast ) {
	// Very basic. Todo: Check env. behind an enemy or the bot, is it really tries to escape or just pushed on a wall

	float botVelocitySqLen = VectorLengthSquared( self->velocity );
	float enemyVelocitySqLen = selectedEnemies.LastSeenVelocity().SquaredLength();

	// Enemy is moving fast
	if( enemyVelocitySqLen >= DEFAULT_DASHSPEED * DEFAULT_DASHSPEED ) {
		// Both entities are moving fast
		if( botVelocitySqLen >= DEFAULT_DASHSPEED * DEFAULT_DASHSPEED ) {
			Vec3 botVelocityDir( self->velocity );
			Vec3 enemyVelocityDir( selectedEnemies.LastSeenVelocity() );
			enemyVelocityDir *= Q_RSqrt( enemyVelocitySqLen );
			botVelocityDir *= Q_RSqrt( botVelocitySqLen );
			if( botVelocityDir.Dot( enemyVelocityDir ) < -0.5f ) {
				*botMovesFast = true;
				*enemyMovesFast = true;
				return true;
			}
			return false;
		}
		// Bot is standing or walking, direction of its speed does not matter
		if( IsEscapingFromStandingEntity( selectedEnemies.Ent(), self, enemyVelocitySqLen ) ) {
			*botMovesFast = false;
			*enemyMovesFast = true;
			return true;
		}
		return false;
	}

	// Enemy is standing or walking, direction of its speed does not matter
	if( IsEscapingFromStandingEntity( self, selectedEnemies.Ent(), botVelocitySqLen ) ) {
		*botMovesFast = true;
		*enemyMovesFast = false;
		return true;
	}
	return false;
}

int BotWeaponSelector::SuggestHitEscapingEnemyWeapon( const WorldState &worldState, bool botMovesFast, bool enemyMovesFast ) {
	if( worldState.DistanceToEnemy() < CLOSE_RANGE ) {
		constexpr const char *format = "(hit escaping) too small distance %.1f to change weapon, too risky\n";
		Debug( format, worldState.DistanceToEnemy() );
		return WEAP_NONE;
	}

	// If target will be lost out of sight, its worth to do a fast weapon switching

	constexpr float predictionSeconds = 0.8f;

	// Extrapolate bot origin
	Vec3 extrapolatedBotOrigin( self->velocity );
	extrapolatedBotOrigin *= predictionSeconds;
	extrapolatedBotOrigin += self->s.origin;
	Vec3 predictedBotOrigin( extrapolatedBotOrigin );

	// Extrapolate enemy origin
	float *initialEnemyOrigin = selectedEnemies.LastSeenOrigin().Data();
	Vec3 extrapolatedEnemyOrigin( selectedEnemies.LastSeenVelocity() );
	extrapolatedEnemyOrigin *= predictionSeconds;
	extrapolatedEnemyOrigin += initialEnemyOrigin;
	Vec3 predictedEnemyOrigin( extrapolatedEnemyOrigin );

	float *const mins = playerbox_stand_mins;
	float *const maxs = playerbox_stand_maxs;

	trace_t trace;
	// Get a coarse predicted bot origin
	G_Trace( &trace, self->s.origin, mins, playerbox_stand_maxs, extrapolatedBotOrigin.Data(), self, MASK_AISOLID );
	if( trace.fraction != 1.0f ) {
		predictedBotOrigin.Set( trace.endpos );
		// Compensate Z for ground trace hit point
		if( trace.endpos[2] > extrapolatedBotOrigin.Z() ) {
			predictedBotOrigin.Z() += -playerbox_stand_mins[2];
		}
	}

	// Get a coarse predicted enemy origin
	auto *skip = const_cast<edict_t *>( selectedEnemies.Ent() );
	G_Trace( &trace, initialEnemyOrigin, mins, maxs, extrapolatedEnemyOrigin.Data(), skip, MASK_AISOLID );
	if( trace.fraction != 1.0f ) {
		predictedEnemyOrigin.Set( trace.endpos );
		if( trace.endpos[2] > extrapolatedEnemyOrigin.Z() ) {
			predictedEnemyOrigin.Z() += -playerbox_stand_mins[2];
		}
	}

	// Check whether the bot is likely to be able to hit the enemy
	if( trap_inPVS( predictedBotOrigin.Data(), predictedBotOrigin.Data() ) ) {
		G_Trace( &trace, predictedBotOrigin.Data(), nullptr, nullptr, predictedEnemyOrigin.Data(), self, MASK_AISOLID );
		// Still may hit, keep using current weapon
		if( trace.fraction == 1.0f || selectedEnemies.Ent() == game.edicts + trace.ent ) {
			return WEAP_NONE;
		}
	}

	// Only switch to EB on far range. Keep using the current weapon.
	if( worldState.DistanceToEnemy() > GetLaserRange() ) {
		if( BoltsReadyToFireCount() ) {
			return WEAP_ELECTROBOLT;
		}
		return WEAP_NONE;
	}

	// Hit fast-moving enemy using EB
	if( BoltsReadyToFireCount() && ( enemyMovesFast && !botMovesFast ) ) {
		return WEAP_ELECTROBOLT;
	}

	if( botMovesFast && !enemyMovesFast ) {
		TestTargetEnvironment( Vec3( self->s.origin ), selectedEnemies.LastSeenOrigin(), selectedEnemies.Ent() );
		if( targetEnvironment.factor > 0.5f ) {
			if( RocketsReadyToFireCount() ) {
				return WEAP_ROCKETLAUNCHER;
			}
			if( GrenadesReadyToFireCount() ) {
				return WEAP_GRENADELAUNCHER;
			}
		}
	}

	if( BlastsReadyToFireCount() && worldState.DamageToKill() < 100 ) {
		return WEAP_GUNBLADE;
	}

	return WEAP_NONE;
}

bool BotWeaponSelector::CheckForShotOfDespair( const WorldState &worldState ) {
	if( BotHasPowerups() ) {
		return false;
	}

	// Restrict weapon switch time even more compared to generic fast switch action
	if( self->r.client->ps.stats[STAT_WEAPON_TIME] > 16 ) {
		return false;
	}

	float adjustedDamageToBeKilled = worldState.DamageToBeKilled() * ( selectedEnemies.HaveQuad() ? 0.25f : 1.0f );
	if( adjustedDamageToBeKilled > 25 ) {
		return false;
	}

	if( worldState.DamageToKill() < 35 ) {
		return false;
	}

	const float lgRange = GetLaserRange();

	if( worldState.DistanceToEnemy() > lgRange ) {
		return false;
	}

	switch( selectedEnemies.PendingWeapon() ) {
		case WEAP_LASERGUN:
			return true;
		case WEAP_PLASMAGUN:
			return random() > worldState.DistanceToEnemy() / lgRange;
		case WEAP_ROCKETLAUNCHER:
			return random() > worldState.DistanceToEnemy() / lgRange;
		case WEAP_MACHINEGUN:
			return true;
		default:
			return false;
	}
}

int BotWeaponSelector::SuggestShotOfDespairWeapon( const WorldState &worldState ) {
	// Prevent negative scores from self-damage suicide.
	int score = self->r.client->ps.stats[STAT_SCORE];
	if( level.gametype.inverseScore ) {
		score *= -1;
	}

	if( score <= 0 ) {
		if( BoltsReadyToFireCount() > 0 ) {
			return WEAP_ELECTROBOLT;
		}
		if( ShellsReadyToFireCount() > 0 ) {
			return WEAP_RIOTGUN;
		}
		return WEAP_NONE;
	}

	const float lgRange = GetLaserRange();

	enum { EB, RG, RL, GB, GL, WEIGHTS_COUNT };

	WeaponAndScore scores[WEIGHTS_COUNT] =
	{
		WeaponAndScore( WEAP_ELECTROBOLT, BoltsReadyToFireCount() > 0 ),
		WeaponAndScore( WEAP_RIOTGUN, ShellsReadyToFireCount() > 0 ),
		WeaponAndScore( WEAP_ROCKETLAUNCHER, RocketsReadyToFireCount() > 0 ),
		WeaponAndScore( WEAP_GUNBLADE, 0.8f ),
		WeaponAndScore( WEAP_GRENADELAUNCHER, 0.7f )
	};

	TestTargetEnvironment( Vec3( self->s.origin ), Vec3( selectedEnemies.LastSeenOrigin() ), selectedEnemies.Ent() );

	// Do not touch hitscan weapons scores, we are not going to do a continuous fight
	scores[RL].score *= targetEnvironment.factor;
	scores[GL].score *= targetEnvironment.factor;
	scores[GB].score *= 0.5f + 0.5f * targetEnvironment.factor;

	// Since shots of despair are done in LG range, do not touch GB
	scores[RL].score *= 1.0f - 0.750f * worldState.DistanceToEnemy() / lgRange;
	scores[GL].score *= 1.0f - 0.999f * worldState.DistanceToEnemy() / lgRange;

	// Add extra scores for very close shots (we are not going to prevent bot suicide)
	if( worldState.DistanceToEnemy() < 150 ) {
		scores[RL].score *= 2.0f;
		scores[GL].score *= 2.0f;
	}

	// Prioritize EB for relatively far shots
	if( worldState.DistanceToEnemy() > lgRange * 0.66 ) {
		scores[EB].score *= 1.5f;
	}

	// Counteract some weapons with their antipodes
	switch( selectedEnemies.PendingWeapon() ) {
		case WEAP_LASERGUN:
		case WEAP_PLASMAGUN:
			scores[RL].score *= 1.75f;
			scores[GL].score *= 1.35f;
			break;
		case WEAP_ROCKETLAUNCHER:
			scores[RG].score *= 2.0f;
			break;
		default: // Shut up inspections
			break;
	}

	// Do not call ChoseWeaponByScores() which gives current weapon a slight priority, weapon switch is intended
	float bestScore = 0;
	int bestWeapon = WEAP_NONE;
	for( const auto &weaponAndScore: scores ) {
		if( bestScore < weaponAndScore.score ) {
			bestScore = weaponAndScore.score;
			bestWeapon = weaponAndScore.weapon;
		}
	}

	return bestWeapon;
}

int BotWeaponSelector::SuggestQuadBearerWeapon( const WorldState &worldState ) {
	float distance = ( selectedEnemies.LastSeenOrigin() - self->s.origin ).LengthFast();
	auto lgDef = GS_GetWeaponDef( WEAP_LASERGUN );
	auto lgRange = ( lgDef->firedef.timeout + lgDef->firedef_weak.timeout ) / 2.0f;
	int lasersCount = 0;
	if( Inventory()[WEAP_LASERGUN] && distance < lgRange ) {
		lasersCount = Inventory()[AMMO_LASERS] + Inventory()[AMMO_WEAK_LASERS];
		if( lasersCount > 7 ) {
			return WEAP_LASERGUN;
		}
	}
	int bulletsCount = BulletsReadyToFireCount();
	if( bulletsCount > 10 ) {
		return WEAP_MACHINEGUN;
	}
	int plasmasCount = PlasmasReadyToFireCount();
	if( plasmasCount > 10 ) {
		return WEAP_PLASMAGUN;
	}
	if( ShellsReadyToFireCount() ) {
		return WEAP_RIOTGUN;
	}
	if( Inventory()[WEAP_ROCKETLAUNCHER] ) {
		if( RocketsReadyToFireCount() > 0 && distance > CLOSE_RANGE && distance < lgRange * 1.25f ) {
			return WEAP_ROCKETLAUNCHER;
		}
	}
	if( lasersCount > 0 && distance < lgRange ) {
		return WEAP_LASERGUN;
	}
	if( bulletsCount > 0 ) {
		return WEAP_MACHINEGUN;
	}
	if( plasmasCount > 0 ) {
		return WEAP_PLASMAGUN;
	}
	if( Inventory()[WEAP_GRENADELAUNCHER] ) {
		if( GrenadesReadyToFireCount() > 0 && distance > CLOSE_RANGE && distance < lgRange ) {
			float deltaZ = self->s.origin[2] - selectedEnemies.LastSeenOrigin().Z();
			if( deltaZ < -250.0f && random() > 0.5f ) {
				return WEAP_GRENADELAUNCHER;
			}
		}
	}
	return WEAP_GUNBLADE;
}

int BotWeaponSelector::SuggestInstagibWeapon( const WorldState &worldState ) {
	// Prefer hitscan weapons
	if( BulletsReadyToFireCount() ) {
		return WEAP_MACHINEGUN;
	}
	if( Inventory()[WEAP_LASERGUN] ) {
		auto lgDef = GS_GetWeaponDef( WEAP_LASERGUN );
		float squaredDistance = ( selectedEnemies.LastSeenOrigin() - self->s.origin ).SquaredLength();
		if( Inventory()[AMMO_LASERS] && squaredDistance < lgDef->firedef.timeout * lgDef->firedef.timeout ) {
			return WEAP_LASERGUN;
		}
		if( Inventory()[AMMO_WEAK_LASERS] && squaredDistance < lgDef->firedef_weak.timeout * lgDef->firedef_weak.timeout ) {
			return WEAP_LASERGUN;
		}
	}
	if( ShellsReadyToFireCount() ) {
		return WEAP_RIOTGUN;
	}

	if( Inventory()[WEAP_PLASMAGUN] ) {
		float squaredDistance = ( selectedEnemies.LastSeenOrigin() - self->s.origin ).SquaredLength();
		if( squaredDistance < 1000 && PlasmasReadyToFireCount() ) {
			return WEAP_PLASMAGUN;
		}
	}
	if( Inventory()[WEAP_INSTAGUN] ) {
		return WEAP_INSTAGUN;
	}
	if( BoltsReadyToFireCount() ) {
		return WEAP_ELECTROBOLT;
	}
	return WEAP_GUNBLADE;
}

const float BotWeaponSelector::TargetEnvironment::TRACE_DEPTH = 250.0f;

void BotWeaponSelector::TestTargetEnvironment( const Vec3 &botOrigin, const Vec3 &targetOrigin, const edict_t *traceKey ) {
	Vec3 forward = targetOrigin - botOrigin;
	forward.Z() = 0;
	float frontSquareLen = forward.SquaredLength();
	if( frontSquareLen > 1 ) {
		// Normalize
		forward *= Q_RSqrt( frontSquareLen );
	} else {
		// Pick dummy horizontal direction
		forward = Vec3( &axis_identity[AXIS_FORWARD] );
	}
	Vec3 right = Vec3( &axis_identity[AXIS_UP] ).Cross( forward );

	// Now botToTarget is a normalized horizontal part of botToTarget - botOrigin

	edict_t *passedict = const_cast<edict_t*>( traceKey );
	float *start = const_cast<float*>( botOrigin.Data() );
	trace_t *traces = targetEnvironment.sideTraces;

	const float TRACE_DEPTH = TargetEnvironment::TRACE_DEPTH;

	vec_t offsets[6 * 3];
	VectorSet( offsets + 3 * TargetEnvironment::TOP, 0, 0, +TRACE_DEPTH );
	VectorSet( offsets + 3 * TargetEnvironment::BOTTOM, 0, 0, -TRACE_DEPTH );
	VectorSet( offsets + 3 * TargetEnvironment::FRONT, -TRACE_DEPTH, 0, 0 );
	VectorSet( offsets + 3 * TargetEnvironment::BACK, +TRACE_DEPTH, 0, 0 );
	VectorSet( offsets + 3 * TargetEnvironment::LEFT, 0, -TRACE_DEPTH, 0 );
	VectorSet( offsets + 3 * TargetEnvironment::RIGHT, 0, +TRACE_DEPTH, 0 );

	vec3_t mins = { -32, -32, -32 };
	vec3_t maxs = { +32, +32, +32 };

	float factor = 0.0f;
	for( int i = 0; i < 6; ++i ) {
		vec3_t end;
		trace_t *trace = traces + i;
		G_ProjectSource( start, offsets + 3 * i, forward.Data(), right.Data(), end );
		G_Trace( trace, start, mins, maxs, end, passedict, MASK_AISOLID );
		// Give some non-zero score by the fact that trace is detected a hit itself
		if( trace->fraction < 1.0f ) {
			factor += 1.0f / 6.0f;
		}
		// Compute a dot product between a bot-to-target direction and trace-point-to-target direction
		// If the dot product is close to 1, a bot may shoot almost perpendicular to the traced point.
		// If trace point is itself close to a target, bot may inflict enemy lots of damage by rockets.
		Vec3 hitPointToTarget = targetOrigin - trace->endpos;
		if( trace->fraction > 0.01f ) {
			// Normalize
			hitPointToTarget *= 1.0f / ( TRACE_DEPTH * trace->fraction );
			factor += 1.5f * ( 1.0f - trace->fraction ) * fabsf( hitPointToTarget.Dot( forward ) );
		} else {
			// The target is very close to a traced solid surface anyway, use 1 instead of a dot product.
			factor += 1.5f * ( 1.0f - trace->fraction );
		}
	}

	targetEnvironment.factor = std::min( 1.0f, factor / 6.0f );
}

void BotWeaponSelector::SetSelectedWeapons( int builtinWeapon, int scriptWeapon, bool preferBuiltinWeapon, unsigned timeoutPeriod ) {
	selectedWeapons.hasSelectedBuiltinWeapon = false;
	selectedWeapons.hasSelectedScriptWeapon = false;
	if( builtinWeapon >= 0 ) {
		selectedWeapons.builtinFireDef = GenericFireDef( builtinWeapon, &GS_GetWeaponDef( builtinWeapon )->firedef );
		selectedWeapons.hasSelectedBuiltinWeapon = true;
	}
	if( scriptWeapon >= 0 ) {
		selectedWeapons.scriptFireDef = GenericFireDef( scriptWeapon, &self->ai->botRef->scriptWeaponDefs[scriptWeapon] );
		selectedWeapons.hasSelectedScriptWeapon = true;
	}
	selectedWeapons.instanceId++;
	selectedWeapons.preferBuiltinWeapon = preferBuiltinWeapon;
	selectedWeapons.timeoutAt = level.time + timeoutPeriod;
}
