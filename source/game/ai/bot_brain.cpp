#include "bot.h"
#include "ai_ground_trace_cache.h"
#include "ai_squad_based_team_brain.h"
#include "bot_brain.h"
#include "tactical_spots_registry.h"
#include <algorithm>
#include <limits>
#include <stdarg.h>

BotBaseGoal *BotBrain::GetGoalByName( const char *name ) {
	for( unsigned i = 0; i < scriptGoals.size(); ++i )
		if( !Q_stricmp( name, scriptGoals[i].Name() ) ) {
			return &scriptGoals[i];
		}

	return nullptr;
}

BotBaseAction *BotBrain::GetActionByName( const char *name ) {
	for( unsigned i = 0; i < scriptActions.size(); ++i )
		if( !Q_stricmp( name, scriptActions[i].Name() ) ) {
			return &scriptActions[i];
		}

	return nullptr;
}

void BotBrain::EnemyPool::OnEnemyRemoved( const Enemy *enemy ) {
	bot->ai->botRef->OnEnemyRemoved( enemy );
}

void BotBrain::EnemyPool::OnNewThreat( const edict_t *newThreat ) {
	bot->ai->botRef->botBrain.OnNewThreat( newThreat, this );
}

void BotBrain::OnAttachedToSquad( AiSquad *squad_ ) {
	this->squad = squad_;
	activeEnemyPool = squad_->EnemyPool();
	ClearGoalAndPlan();
}

void BotBrain::OnDetachedFromSquad( AiSquad *squad_ ) {
	if( this->squad != squad_ ) {
		FailWith( "was not attached to squad %s", squad_ ? squad_->Tag() : "???" );
	}
	this->squad = nullptr;
	activeEnemyPool = &botEnemyPool;
	ClearGoalAndPlan();
}

void BotBrain::OnAttitudeChanged( const edict_t *ent, int oldAttitude_, int newAttitude_ ) {
	if( oldAttitude_ < 0 && newAttitude_ >= 0 ) {
		botEnemyPool.Forget( ent );
		if( squad ) {
			squad->EnemyPool()->Forget( ent );
		}
	}
}

void BotBrain::OnNewThreat( const edict_t *newThreat, const AiFrameAwareUpdatable *threatDetector ) {
	// Reject threats detected by bot brain if there is active squad.
	// Otherwise there may be two calls for a single or different threats
	// detected by squad and the bot brain enemy pool itself.
	if( squad && threatDetector == &this->botEnemyPool ) {
		return;
	}

	bool hadValidThreat = activeThreat.IsValidFor( self );
	float totalInflictedDamage = activeEnemyPool->TotalDamageInflictedBy( self );
	if( hadValidThreat ) {
		// The active threat is more dangerous than a new one
		if( activeThreat.totalDamage > totalInflictedDamage ) {
			return;
		}
		// The active threat has the same inflictor
		if( activeThreat.inflictor == newThreat ) {
			return;
		}
	}

	vec3_t botLookDir;
	AngleVectors( self->s.angles, botLookDir, nullptr, nullptr );
	Vec3 toEnemyDir = Vec3( newThreat->s.origin ) - self->s.origin;
	float squareDistance = toEnemyDir.SquaredLength();
	if( squareDistance > 1 ) {
		float distance = 1.0f / Q_RSqrt( squareDistance );
		toEnemyDir *= 1.0f / distance;
		if( toEnemyDir.Dot( botLookDir ) < 0 ) {
			// Try to guess enemy origin
			toEnemyDir.X() += -0.25f + 0.50f * random();
			toEnemyDir.Y() += -0.10f + 0.20f * random();
			toEnemyDir.NormalizeFast();
			activeThreat.inflictor = newThreat;
			activeThreat.lastHitTimestamp = level.time;
			activeThreat.possibleOrigin = distance * toEnemyDir + self->s.origin;
			activeThreat.totalDamage = totalInflictedDamage;
			// Force replanning on new threat
			if( !hadValidThreat ) {
				nextActiveGoalUpdateAt = level.time;
			}
		}
	}
}

void BotBrain::OnEnemyRemoved( const Enemy *enemy ) {
	if( selectedEnemies.AreValid() ) {
		if( selectedEnemies.Contain( enemy ) ) {
			selectedEnemies.Invalidate();
			ClearGoalAndPlan();
		}
	}
}

BotBrain::BotBrain( Bot *bot, float skillLevel_ )
	: AiBaseBrain( bot->self ),
	bot( bot->self ),
	baseOffensiveness( 0.5f ),
	skillLevel( skillLevel_ ),
	reactionTime( 320 - From0UpToMax( 300, BotSkill() ) ),
	nextTargetChoiceAt( level.time ),
	targetChoicePeriod( 800 - From0UpToMax( 300, BotSkill() ) ),
	itemsSelector( bot->self ),
	selectedNavEntity( nullptr, std::numeric_limits<float>::max(), 0, 0 ),
	prevSelectedNavEntity( nullptr ),
	triggeredPlanningDanger( nullptr ),
	actualDanger( nullptr ),
	squad( nullptr ),
	botEnemyPool( bot->self, this, skillLevel_ ),
	selectedEnemies( bot->selectedEnemies ),
	lostEnemies( bot->self ),
	selectedWeapons( bot->selectedWeapons ),
	cachedWorldState( bot->self ) {
	squad = nullptr;
	activeEnemyPool = &botEnemyPool;
	SetTag( bot->self->r.client->netname );
}

void BotBrain::Frame() {
	// Call superclass method first
	AiBaseBrain::Frame();

	// Reset offensiveness to a default value
	if( G_ISGHOSTING( self ) ) {
		baseOffensiveness = 0.5f;
	}

	botEnemyPool.Update();

	// Prevent selected nav entity timeout when planning should be skipped.
	// Otherwise sometimes bot might be blocked in the middle of some movement action
	// having no actual goals.
	if( ShouldSkipPlanning() ) {
		selectedNavEntity.timeoutAt += game.frametime;
	}
}

void BotBrain::Think() {
	// It is important to do all these actions before AiBaseBrain::Think() to trigger a plan update if needed

	if( selectedEnemies.AreValid() ) {
		if( level.time - selectedEnemies.LastSeenAt() >= reactionTime ) {
			selectedEnemies.Invalidate();
			UpdateSelectedEnemies();
			UpdateBlockedAreasStatus();
		}
	} else {
		UpdateSelectedEnemies();
		UpdateBlockedAreasStatus();
	}

	self->ai->botRef->tacticalSpotsCache.Clear();

	CheckNewActiveDanger();

	// This call will try to find a plan if it is not present
	AiBaseBrain::Think();
}

void BotBrain::UpdateSelectedEnemies() {
	selectedEnemies.Invalidate();
	lostEnemies.Invalidate();
	float visibleEnemyWeight = 0.0f;
	if( const Enemy *visibleEnemy = activeEnemyPool->ChooseVisibleEnemy( self ) ) {
		const auto &activeEnemies = activeEnemyPool->ActiveEnemies();
		selectedEnemies.Set( visibleEnemy, targetChoicePeriod, activeEnemies.begin(), activeEnemies.end() );
		visibleEnemyWeight = 0.5f * ( visibleEnemy->AvgWeight() + visibleEnemy->MaxWeight() );
	}
	if( const Enemy *lostEnemy = activeEnemyPool->ChooseLostOrHiddenEnemy( self ) ) {
		float lostEnemyWeight = 0.5f * ( lostEnemy->AvgWeight() + lostEnemy->MaxWeight() );
		// If there is a lost or hidden enemy of higher weight, store it
		if( lostEnemyWeight > visibleEnemyWeight ) {
			// Provide a pair of iterators to the Set call:
			// lostEnemies.activeEnemies must contain the lostEnemy.
			const Enemy *enemies[] = { lostEnemy };
			lostEnemies.Set( lostEnemy, targetChoicePeriod, enemies, enemies + 1 );
		}
	}
}

void BotBrain::OnEnemyViewed( const edict_t *enemy ) {
	botEnemyPool.OnEnemyViewed( enemy );
	if( squad ) {
		squad->OnBotViewedEnemy( self, enemy );
	}
}

void BotBrain::OnPain( const edict_t *enemy, float kick, int damage ) {
	botEnemyPool.OnPain( self, enemy, kick, damage );
	if( squad ) {
		squad->OnBotPain( self, enemy, kick, damage );
	}
}

void BotBrain::OnEnemyDamaged( const edict_t *target, int damage ) {
	botEnemyPool.OnEnemyDamaged( self, target, damage );
	if( squad ) {
		squad->OnBotDamagedEnemy( self, target, damage );
	}
}

inline const SelectedNavEntity &BotBrain::GetOrUpdateSelectedNavEntity() {
	if( selectedNavEntity.IsValid() ) {
		return selectedNavEntity;
	}

	// Use direct access to the field to skip assertion
	prevSelectedNavEntity = selectedNavEntity.navEntity;
	selectedNavEntity = itemsSelector.SuggestGoalNavEntity( selectedNavEntity );

	if( !selectedNavEntity.IsEmpty() ) {
		self->ai->botRef->lastItemSelectedAt = level.time;
	} else if( self->ai->botRef->lastItemSelectedAt >= self->ai->botRef->noItemAvailableSince ) {
		self->ai->botRef->noItemAvailableSince = level.time;
	}

	return selectedNavEntity;
}

void BotBrain::UpdateBlockedAreasStatus() {
	if( activeEnemyPool->ActiveEnemies().empty() || ShouldRushHeadless() ) {
		// Reset all possibly blocked areas
		RouteCache()->SetDisabledRegions( nullptr, nullptr, 0, DroppedToFloorAasAreaNum() );
		return;
	}

	// Wait for enemies selection, do not perform cache flushing
	// which is extremely expensive and are likely to be overridden next frame
	if( !selectedEnemies.AreValid() ) {
		return;
	}

	StaticVector<Vec3, EnemyPool::MAX_TRACKED_ENEMIES> mins;
	StaticVector<Vec3, EnemyPool::MAX_TRACKED_ENEMIES> maxs;

	AiGroundTraceCache *groundTraceCache = AiGroundTraceCache::Instance();
	for( const Enemy *enemy: selectedEnemies ) {
		if( selectedEnemies.IsPrimaryEnemy( enemy ) && WillAttackMelee() ) {
			continue;
		}

		// TODO: This may act as cheating since actual enemy origin is used.
		// This is kept for conformance to following ground trace check.
		float squareDistance = DistanceSquared( self->s.origin, enemy->ent->s.origin );
		// (Otherwise all nearby paths are treated as blocked by enemy)
		if( squareDistance < 72.0f ) {
			continue;
		}
		float distance = 1.0f / Q_RSqrt( squareDistance );
		float side = 24.0f + 96.0f * BoundedFraction( distance - 72.0f, 4 * 384.0f );

		// Try to drop an enemy origin to floor
		// TODO: AiGroundTraceCache interface forces using an actual enemy origin
		// and not last seen one, so this may act as cheating.
		vec3_t origin;
		// If an enemy is close to ground (an origin may and has been dropped to floor)
		if( groundTraceCache->TryDropToFloor( enemy->ent, 128.0f, origin, level.time - enemy->LastSeenAt() ) ) {
			// Do not use bounds lower than origin[2] (except some delta)
			mins.push_back( Vec3( -side, -side, -8.0f ) + origin );
			maxs.push_back( Vec3( +side, +side, 128.0f ) + origin );
		} else {
			// Use a bit lower bounds (an enemy is likely to fall down)
			mins.push_back( Vec3( -side, -side, -192.0f ) + origin );
			maxs.push_back( Vec3( +side, +side, +108.0f ) + origin );
		}
	}

	// getting mins[0] address for an empty vector in debug will trigger an assertion
	if( !mins.empty() ) {
		RouteCache()->SetDisabledRegions( &mins[0], &maxs[0], mins.size(), DroppedToFloorAasAreaNum() );
	} else {
		RouteCache()->SetDisabledRegions( nullptr, nullptr, 0, DroppedToFloorAasAreaNum() );
	}
}

bool BotBrain::FindDodgeDangerSpot( const Danger &danger, vec3_t spotOrigin ) {
	TacticalSpotsRegistry::OriginParams originParams( self, 128.0f + 192.0f * BotSkill(), RouteCache() );
	TacticalSpotsRegistry::DodgeDangerProblemParams problemParams( danger.hitPoint, danger.direction, danger.splash );
	problemParams.SetCheckToAndBackReachability( false );
	problemParams.SetMinHeightAdvantageOverOrigin( -64.0f );
	// Influence values are quite low because evade direction factor must be primary
	problemParams.SetHeightOverOriginInfluence( 0.2f );
	problemParams.SetMaxFeasibleTravelTimeMillis( 2500 );
	problemParams.SetOriginDistanceInfluence( 0.4f );
	problemParams.SetOriginWeightFalloffDistanceRatio( 0.9f );
	problemParams.SetTravelTimeInfluence( 0.2f );
	return TacticalSpotsRegistry::Instance()->FindEvadeDangerSpots( originParams, problemParams, (vec3_t *)spotOrigin, 1 ) > 0;
}

void BotBrain::CheckNewActiveDanger() {
	if( BotSkill() <= 0.33f ) {
		return;
	}

	const Danger *danger = self->ai->botRef->perceptionManager.PrimaryDanger();
	if( !danger ) {
		return;
	}

	actualDanger = *danger;
	bool needsUrgentReplanning = false;
	// The old danger has timed out
	if( !triggeredPlanningDanger.IsValid() ) {
		needsUrgentReplanning = true;
	}
	// The old danger is about to time out
	else if( level.time - triggeredPlanningDanger.timeoutAt < Danger::TIMEOUT / 3 ) {
		needsUrgentReplanning = true;
	} else if( actualDanger.splash != triggeredPlanningDanger.splash ) {
		needsUrgentReplanning = true;
	} else if( actualDanger.attacker != triggeredPlanningDanger.attacker ) {
		needsUrgentReplanning = true;
	} else if( actualDanger.damage - triggeredPlanningDanger.damage > 10 ) {
		needsUrgentReplanning = true;
	} else if( ( actualDanger.hitPoint - triggeredPlanningDanger.hitPoint ).SquaredLength() > 32 * 32 ) {
		needsUrgentReplanning = true;
	} else if( actualDanger.direction.Dot( triggeredPlanningDanger.direction ) < 0.9 ) {
		needsUrgentReplanning = true;
	}

	if( needsUrgentReplanning ) {
		triggeredPlanningDanger = actualDanger;
		nextActiveGoalUpdateAt = level.time;
	}
}

bool BotBrain::Threat::IsValidFor( const edict_t *self_ ) const {
	if( level.time - lastHitTimestamp > 350 ) {
		return false;
	}

	// Check whether the inflictor entity is no longer valid

	if( !inflictor->r.inuse ) {
		return false;
	}

	if( !inflictor->r.client && inflictor->aiIntrinsicEnemyWeight <= 0 ) {
		return false;
	}

	if( G_ISGHOSTING( inflictor ) ) {
		return false;
	}

	// It is not cheap to call so do it after all other tests have passed
	vec3_t lookDir;
	AngleVectors( self_->s.angles, lookDir, nullptr, nullptr );
	Vec3 toThreat( inflictor->s.origin );
	toThreat -= self_->s.origin;
	toThreat.NormalizeFast();
	return toThreat.Dot( lookDir ) < self_->ai->botRef->FovDotFactor();
}

void BotBrain::PrepareCurrWorldState( WorldState *worldState ) {
	worldState->SetIgnoreAll( false );

	worldState->BotOriginVar().SetValue( self->s.origin );
	worldState->PendingOriginVar().SetIgnore( true );

	if( selectedEnemies.AreValid() ) {
		worldState->EnemyOriginVar().SetValue( selectedEnemies.LastSeenOrigin() );
		worldState->HasThreateningEnemyVar().SetValue( selectedEnemies.AreThreatening() );
		worldState->RawDamageToKillVar().SetValue( (short)selectedEnemies.DamageToKill() );
		worldState->EnemyHasQuadVar().SetValue( selectedEnemies.HaveQuad() );
		worldState->EnemyHasGoodSniperRangeWeaponsVar().SetValue( selectedEnemies.HaveGoodSniperRangeWeapons() );
		worldState->EnemyHasGoodFarRangeWeaponsVar().SetValue( selectedEnemies.HaveGoodFarRangeWeapons() );
		worldState->EnemyHasGoodMiddleRangeWeaponsVar().SetValue( selectedEnemies.HaveGoodMiddleRangeWeapons() );
		worldState->EnemyHasGoodCloseRangeWeaponsVar().SetValue( selectedEnemies.HaveGoodCloseRangeWeapons() );
		worldState->EnemyCanHitVar().SetValue( selectedEnemies.CanHit( self ) );
	} else {
		worldState->EnemyOriginVar().SetIgnore( true );
		worldState->HasThreateningEnemyVar().SetIgnore( true );
		worldState->RawDamageToKillVar().SetIgnore( true );
		worldState->EnemyHasQuadVar().SetIgnore( true );
		worldState->EnemyHasGoodSniperRangeWeaponsVar().SetIgnore( true );
		worldState->EnemyHasGoodFarRangeWeaponsVar().SetIgnore( true );
		worldState->EnemyHasGoodMiddleRangeWeaponsVar().SetIgnore( true );
		worldState->EnemyHasGoodCloseRangeWeaponsVar().SetIgnore( true );
		worldState->EnemyCanHitVar().SetIgnore( true );
	}

	if( lostEnemies.AreValid() ) {
		worldState->IsReactingToEnemyLostVar().SetValue( false );
		worldState->HasReactedToEnemyLostVar().SetValue( false );
		worldState->LostEnemyLastSeenOriginVar().SetValue( lostEnemies.LastSeenOrigin() );
		trace_t trace;
		G_Trace( &trace, self->s.origin, nullptr, nullptr, lostEnemies.ActualOrigin().Data(), self, MASK_AISOLID );
		if( trace.fraction == 1.0f ) {
			worldState->MightSeeLostEnemyAfterTurnVar().SetValue( true );
		} else if( game.edicts + trace.ent == lostEnemies.TraceKey() ) {
			worldState->MightSeeLostEnemyAfterTurnVar().SetValue( true );
		} else {
			worldState->MightSeeLostEnemyAfterTurnVar().SetValue( false );
		}
	} else {
		worldState->IsReactingToEnemyLostVar().SetIgnore( true );
		worldState->HasReactedToEnemyLostVar().SetIgnore( true );
		worldState->LostEnemyLastSeenOriginVar().SetIgnore( true );
		worldState->MightSeeLostEnemyAfterTurnVar().SetIgnore( true );
	}

	worldState->HealthVar().SetValue( (short)HEALTH_TO_INT( self->health ) );
	worldState->ArmorVar().SetValue( self->r.client->ps.stats[STAT_ARMOR] );

	worldState->HasQuadVar().SetValue( ::HasQuad( self ) );
	worldState->HasShellVar().SetValue( ::HasShell( self ) );

	bool hasGoodSniperRangeWeapons = false;
	bool hasGoodFarRangeWeapons = false;
	bool hasGoodMiddleRangeWeapons = false;
	bool hasGoodCloseRangeWeapons = false;

	if( BoltsReadyToFireCount() || BulletsReadyToFireCount() || InstasReadyToFireCount() ) {
		hasGoodSniperRangeWeapons = true;
	}
	if( BoltsReadyToFireCount() || BulletsReadyToFireCount() || PlasmasReadyToFireCount() || InstasReadyToFireCount() ) {
		hasGoodFarRangeWeapons = true;
	}
	if( RocketsReadyToFireCount() || LasersReadyToFireCount() || PlasmasReadyToFireCount() ||
		BulletsReadyToFireCount() || ShellsReadyToFireCount() || InstasReadyToFireCount() ) {
		hasGoodMiddleRangeWeapons = true;
	}
	if( RocketsReadyToFireCount() || PlasmasReadyToFireCount() || ShellsReadyToFireCount() ) {
		hasGoodCloseRangeWeapons = true;
	}

	worldState->HasGoodSniperRangeWeaponsVar().SetValue( hasGoodSniperRangeWeapons );
	worldState->HasGoodFarRangeWeaponsVar().SetValue( hasGoodFarRangeWeapons );
	worldState->HasGoodMiddleRangeWeaponsVar().SetValue( hasGoodMiddleRangeWeapons );
	worldState->HasGoodCloseRangeWeaponsVar().SetValue( hasGoodCloseRangeWeapons );

	worldState->HasQuadVar().SetValue( ::HasQuad( self ) );
	worldState->HasShellVar().SetValue( ::HasShell( self ) );

	const SelectedNavEntity &currSelectedNavEntity = GetOrUpdateSelectedNavEntity();
	if( currSelectedNavEntity.IsEmpty() ) {
		// HACK! If there is no selected nav entity, set the value to the roaming spot origin.
		if( self->ai->botRef->ShouldUseRoamSpotAsNavTarget() ) {
			Vec3 spot( self->ai->botRef->roamingManager.GetCachedRoamingSpot() );
			Debug( "Using a roaming spot @ %.1f %.1f %.1f as a world state nav target var\n", spot.X(), spot.Y(), spot.Z() );
			worldState->NavTargetOriginVar().SetValue( spot );
		} else {
			worldState->NavTargetOriginVar().SetIgnore( true );
		}
		worldState->GoalItemWaitTimeVar().SetIgnore( true );
	} else {
		const NavEntity *navEntity = currSelectedNavEntity.GetNavEntity();
		worldState->NavTargetOriginVar().SetValue( navEntity->Origin() );
		// Find a travel time to the goal itme nav entity in milliseconds
		// We hope this router call gets cached by AAS subsystem
		unsigned travelTime = 10U * FindTravelTimeToGoalArea( navEntity->AasAreaNum() );
		// AAS returns 1 seconds^-2 as a lowest feasible value
		if( travelTime <= 10 ) {
			travelTime = 0;
		}
		int64_t spawnTime = navEntity->SpawnTime();
		// If the goal item spawns before the moment when it gets reached
		if( level.time + travelTime >= spawnTime ) {
			worldState->GoalItemWaitTimeVar().SetValue( 0 );
		} else {
			worldState->GoalItemWaitTimeVar().SetValue( (unsigned)( spawnTime - level.time - travelTime ) );
		}
	}

	worldState->HasJustPickedGoalItemVar().SetValue( HasJustPickedGoalItem() );

	worldState->HasPositionalAdvantageVar().SetValue( false );
	worldState->CanHitEnemyVar().SetValue( true );

	worldState->HasJustKilledEnemyVar().SetValue( false );

	// If methods corresponding to these comparisons are extracted, their names will be confusing
	// (they are useful for filling world state only as not always corresponding to what a human caller expect).
	worldState->HasJustTeleportedVar().SetValue( level.time - self->ai->botRef->lastTouchedTeleportAt < 64 + 1 );
	worldState->HasJustTouchedJumppadVar().SetValue( level.time - self->ai->botRef->lastTouchedJumppadAt < 64 + 1 );
	worldState->HasJustEnteredElevatorVar().SetValue( level.time - self->ai->botRef->lastTouchedElevatorAt < 64 + 1 );

	worldState->HasPendingCoverSpotVar().SetIgnore( true );
	worldState->HasPendingRunAwayTeleportVar().SetIgnore( true );
	worldState->HasPendingRunAwayJumppadVar().SetIgnore( true );
	worldState->HasPendingRunAwayElevatorVar().SetIgnore( true );

	worldState->IsRunningAwayVar().SetIgnore( true );
	worldState->HasRunAwayVar().SetIgnore( true );

	worldState->HasReactedToDangerVar().SetValue( false );
	if( BotSkill() > 0.33f && actualDanger.IsValid() ) {
		worldState->PotentialDangerDamageVar().SetValue( (short)actualDanger.damage );
		worldState->DangerHitPointVar().SetValue( actualDanger.hitPoint );
		worldState->DangerDirectionVar().SetValue( actualDanger.direction );
		vec3_t dodgeDangerSpot;
		if( FindDodgeDangerSpot( actualDanger, dodgeDangerSpot ) ) {
			worldState->DodgeDangerSpotVar().SetValue( dodgeDangerSpot );
		} else {
			worldState->DodgeDangerSpotVar().SetIgnore( true );
		}
	} else {
		worldState->PotentialDangerDamageVar().SetIgnore( true );
		worldState->DangerHitPointVar().SetIgnore( true );
		worldState->DangerDirectionVar().SetIgnore( true );
		worldState->DodgeDangerSpotVar().SetIgnore( true );
	}

	worldState->HasReactedToThreatVar().SetValue( false );
	if( activeThreat.IsValidFor( self ) ) {
		worldState->ThreatInflictedDamageVar().SetValue( (short)activeThreat.totalDamage );
		worldState->ThreatPossibleOriginVar().SetValue( activeThreat.possibleOrigin );
	} else {
		worldState->ThreatInflictedDamageVar().SetIgnore( true );
		worldState->ThreatPossibleOriginVar().SetIgnore( true );
	}

	worldState->ResetTacticalSpots();

	worldState->SimilarWorldStateInstanceIdVar().SetIgnore( true );

	worldState->PrepareAttachment();

	cachedWorldState = *worldState;
}

bool BotBrain::ShouldSkipPlanning() const {
	// Skip planning moving on a jumppad
	if( self->ai->botRef->movementState.jumppadMovementState.IsActive() ) {
		return true;
	}

	// Skip planning while preparing for a weaponjump / landing after it
	if( self->ai->botRef->movementState.weaponJumpMovementState.IsActive() ) {
		return true;
	}

	if( self->ai->botRef->movementState.flyUntilLandingMovementState.IsActive() ) {
		return true;
	}

	// Skip planning moving on an elevator
	if( self->groundentity && self->groundentity->use == Use_Plat && self->groundentity->moveinfo.state != STATE_TOP ) {
		return true;
	}

	return false;
}

float BotBrain::GetEffectiveOffensiveness() const {
	if( !squad ) {
		if( selectedEnemies.AreValid() && selectedEnemies.HaveCarrier() ) {
			return 0.65f + 0.35f * decisionRandom;
		}
		return baseOffensiveness;
	}
	return squad->IsSupporter( self ) ? 1.0f : 0.0f;
}
