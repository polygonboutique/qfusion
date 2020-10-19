#include "bot.h"
#include "ai_aas_world.h"
#include <algorithm>

#ifndef _MSC_VER
// Allow getting an address of not initialized yet field movementState.entityPhysicsState.
// Saving this address for further use is legal, the field is not going to be used right now.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuninitialized"
#endif

Bot::Bot( edict_t *self_, float skillLevel_ )
	: Ai( self_, &botBrain, AiAasRouteCache::NewInstance(), &movementState.entityPhysicsState, PREFERRED_TRAVEL_FLAGS, ALLOWED_TRAVEL_FLAGS ),
	weightConfig( self_ ),
	dangersDetector( self_ ),
	botBrain( this, skillLevel_ ),
	skillLevel( skillLevel_ ),
	selectedEnemies( self_ ),
	weaponsSelector( self_, selectedEnemies, selectedWeapons, 600 - From0UpToMax( 300, skillLevel_ ) ),
	tacticalSpotsCache( self_ ),
	roamingManager( self_ ),
	builtinFireTargetCache( self_ ),
	scriptFireTargetCache( self_ ),
	grabItemGoal( this ),
	killEnemyGoal( this ),
	runAwayGoal( this ),
	reactToDangerGoal( this ),
	reactToThreatGoal( this ),
	reactToEnemyLostGoal( this ),
	attackOutOfDespairGoal( this ),
	roamGoal( this ),
	genericRunToItemAction( this ),
	pickupItemAction( this ),
	waitForItemAction( this ),
	killEnemyAction( this ),
	advanceToGoodPositionAction( this ),
	retreatToGoodPositionAction( this ),
	steadyCombatAction( this ),
	gotoAvailableGoodPositionAction( this ),
	attackFromCurrentPositionAction( this ),
	genericRunAvoidingCombatAction( this ),
	startGotoCoverAction( this ),
	takeCoverAction( this ),
	startGotoRunAwayTeleportAction( this ),
	doRunAwayViaTeleportAction( this ),
	startGotoRunAwayJumppadAction( this ),
	doRunAwayViaJumppadAction( this ),
	startGotoRunAwayElevatorAction( this ),
	doRunAwayViaElevatorAction( this ),
	stopRunningAwayAction( this ),
	dodgeToSpotAction( this ),
	turnToThreatOriginAction( this ),
	turnToLostEnemyAction( this ),
	startLostEnemyPursuitAction( this ),
	stopLostEnemyPursuitAction( this ),
	dummyMovementAction( this ),
	handleTriggeredJumppadMovementAction( this ),
	landOnSavedAreasSetMovementAction( this ),
	ridePlatformMovementAction( this ),
	swimMovementAction( this ),
	flyUntilLandingMovementAction( this ),
	campASpotMovementAction( this ),
	walkCarefullyMovementAction( this ),
	bunnyStraighteningReachChainMovementAction( this ),
	bunnyToBestShortcutAreaMovementAction( this ),
	bunnyInterpolatingReachChainMovementAction( this ),
	walkOrSlideInterpolatingReachChainMovementAction( this ),
	combatDodgeSemiRandomlyToTargetMovementAction( this ),
	movementPredictionContext( self_ ),
	vsayTimeout( level.time + 10000 ),
	isInSquad( false ),
	defenceSpotId( -1 ),
	offenseSpotId( -1 ),
	lastTouchedTeleportAt( 0 ),
	lastTouchedJumppadAt( 0 ),
	lastTouchedElevatorAt( 0 ),
	lastKnockbackAt( 0 ),
	similarWorldStateInstanceId( 0 ),
	lastItemSelectedAt( 0 ),
	noItemAvailableSince( 0 ),
	keptInFovPoint( self_ ),
	lastChosenLostOrHiddenEnemy( nullptr ),
	lastChosenLostOrHiddenEnemyInstanceId( 0 ) {
	self->r.client->movestyle = GS_CLASSICBUNNY;
	// Enable skimming for bots (since it is useful and should not be noticed from a 3rd person POV).
	self->r.client->ps.pmove.stats[PM_STAT_FEATURES] &= PMFEAT_CORNERSKIMMING;
	SetTag( self->r.client->netname );
}

#ifndef _MSC_VER
#pragma GCC diagnostic pop
#endif

void Bot::ApplyPendingTurnToLookAtPoint( BotInput *botInput, BotMovementPredictionContext *context ) const {
	BotPendingLookAtPointState *pendingLookAtPointState;
	AiEntityPhysicsState *entityPhysicsState_;
	unsigned frameTime;
	if( context ) {
		pendingLookAtPointState = &context->movementState->pendingLookAtPointState;
		entityPhysicsState_ = &context->movementState->entityPhysicsState;
		frameTime = context->predictionStepMillis;
	} else {
		pendingLookAtPointState = &self->ai->botRef->movementState.pendingLookAtPointState;
		entityPhysicsState_ = &self->ai->botRef->movementState.entityPhysicsState;
		frameTime = game.frametime;
	}

	if( !pendingLookAtPointState->IsActive() ) {
		return;
	}

	const AiPendingLookAtPoint &pendingLookAtPoint = pendingLookAtPointState->pendingLookAtPoint;
	Vec3 toPointDir( pendingLookAtPoint.Origin() );
	toPointDir -= entityPhysicsState_->Origin();
	toPointDir.NormalizeFast();

	botInput->SetIntendedLookDir( toPointDir, true );
	botInput->isLookDirSet = true;

	float turnSpeedMultiplier = pendingLookAtPoint.TurnSpeedMultiplier();
	Vec3 newAngles = GetNewViewAngles( entityPhysicsState_->Angles().Data(), toPointDir, frameTime, turnSpeedMultiplier );
	botInput->SetAlreadyComputedAngles( newAngles );

	botInput->canOverrideLookVec = false;
	botInput->canOverridePitch = false;
}

void Bot::ApplyInput( BotInput *input, BotMovementPredictionContext *context ) {
	// It is legal (there are no enemies and no nav targets in some moments))
	if( !input->isLookDirSet ) {
		//const float *origin = entityPhysicsState ? entityPhysicsState->Origin() : self->s.origin;
		//AITools_DrawColorLine(origin, (Vec3(-32, +32, -32) + origin).Data(), COLOR_RGB(192, 0, 0), 0);
		return;
	}
	if( !input->isUcmdSet ) {
		//const float *origin = entityPhysicsState ? entityPhysicsState->Origin() : self->s.origin;
		//AITools_DrawColorLine(origin, (Vec3(+32, -32, +32) + origin).Data(), COLOR_RGB(192, 0, 192), 0);
		return;
	}

	if( context ) {
		auto *entityPhysicsState_ = &context->movementState->entityPhysicsState;
		if( !input->hasAlreadyComputedAngles ) {
			if( CheckInputInversion( input, context ) ) {
				InvertKeys( input, context );
				Vec3 newAngles( GetNewViewAngles( self->s.angles, -input->IntendedLookDir(),
												  context->predictionStepMillis, 5.0f * input->TurnSpeedMultiplier() ) );
				input->SetAlreadyComputedAngles( newAngles );
			} else {
				Vec3 newAngles( GetNewViewAngles( entityPhysicsState_->Angles(), input->IntendedLookDir(),
												  context->predictionStepMillis, input->TurnSpeedMultiplier() ) );
				input->SetAlreadyComputedAngles( newAngles );
			}
		}
		entityPhysicsState_->SetAngles( input->AlreadyComputedAngles() );
	} else {
		if( !input->hasAlreadyComputedAngles ) {
			if( CheckInputInversion( input, context ) ) {
				InvertKeys( input, context );
				Vec3 newAngles( GetNewViewAngles( self->s.angles, -input->IntendedLookDir(),
												  game.frametime, 5.0f * input->TurnSpeedMultiplier() ) );
				input->SetAlreadyComputedAngles( newAngles );
			} else {
				Vec3 newAngles( GetNewViewAngles( self->s.angles, input->IntendedLookDir(),
												  game.frametime, input->TurnSpeedMultiplier() ) );
				input->SetAlreadyComputedAngles( newAngles );
			}
		}
		input->AlreadyComputedAngles().CopyTo( self->s.angles );
	}
}

inline bool Bot::CheckInputInversion( BotInput *input, BotMovementPredictionContext *context ) {
	// We cannot just check whether the dot product is negative.
	// (a non-implemented side movement is required in case when fabs(the dot product) < 0.X).
	// Invert movement only if it can be represented as a negated forward movement without a substantial side part.
	constexpr const float INVERT_DOT_THRESHOLD = -0.3f;

	if( !keptInFovPoint.IsActive() ) {
		if( context ) {
			context->movementState->isDoingInputInversion = false;
		} else {
			self->ai->botRef->movementState.isDoingInputInversion = false;
		}

		return false;
	}

	static_assert( INVERT_DOT_THRESHOLD < -0.1f, "The dot threshold is assumed to be negative in all cases" );
	Vec3 selfToPoint( keptInFovPoint.Origin() );
	if( context ) {
		selfToPoint -= context->movementState->entityPhysicsState.Origin();
		selfToPoint.NormalizeFast();
		// Prevent choice jitter
		float dotThreshold = INVERT_DOT_THRESHOLD;
		if( context->movementState->isDoingInputInversion ) {
			dotThreshold += 0.1f;
		}

		bool result = selfToPoint.Dot( input->IntendedLookDir() ) < dotThreshold;
		context->movementState->isDoingInputInversion = result;
		return result;
	}

	selfToPoint -= self->s.origin;
	selfToPoint.NormalizeFast();

	float dotThreshold = INVERT_DOT_THRESHOLD;
	if( self->ai->botRef->movementState.isDoingInputInversion ) {
		dotThreshold += 0.1f;
	}

	bool result = selfToPoint.Dot( input->IntendedLookDir() ) < dotThreshold;
	self->ai->botRef->movementState.isDoingInputInversion = result;
	return result;
}

inline void Bot::InvertKeys( BotInput *input, BotMovementPredictionContext *context ) {
	input->SetForwardMovement( -input->ForwardMovement() );
	input->SetRightMovement( -input->RightMovement() );

	// If no keys are set, forward dash is preferred by default.
	// Set negative forward movement in this case manually.
	if( input->IsSpecialButtonSet() ) {
		return;
	}

	if( input->ForwardMovement() || input->RightMovement() ) {
		return;
	}

	if( context ) {
		if( context->movementState->entityPhysicsState.GroundEntity() ) {
			input->SetForwardMovement( -1 );
		}
	} else {
		if( self->groundentity ) {
			input->SetForwardMovement( -1 );
		}
	}
}

void Bot::UpdateKeptInFovPoint() {
	if( GetMiscTactics().shouldRushHeadless ) {
		keptInFovPoint.Deactivate();
		return;
	}

	if( selectedEnemies.AreValid() ) {
		Vec3 origin( selectedEnemies.ClosestEnemyOrigin( self->s.origin ) );
		if( !GetMiscTactics().shouldKeepXhairOnEnemy ) {
			if( !selectedEnemies.HaveQuad() && !selectedEnemies.HaveCarrier() ) {
				if( origin.SquareDistanceTo( self->s.origin ) > 1024 * 1024 ) {
					return;
				}
			}
		}

		keptInFovPoint.Update( origin, selectedEnemies.InstanceId() );
		return;
	}

	unsigned timeout = GetMiscTactics().shouldKeepXhairOnEnemy ? 2000 : 1000;
	if( GetMiscTactics().willRetreat ) {
		timeout = ( timeout * 3u ) / 2u;
	}

	if( const Enemy *lostOrHiddenEnemy = botBrain.activeEnemyPool->ChooseLostOrHiddenEnemy( self, timeout ) ) {
		if( !lastChosenLostOrHiddenEnemy ) {
			lastChosenLostOrHiddenEnemyInstanceId++;
		} else if( lastChosenLostOrHiddenEnemy->ent != lostOrHiddenEnemy->ent ) {
			lastChosenLostOrHiddenEnemyInstanceId++;
		}

		Vec3 origin( lostOrHiddenEnemy->LastSeenPosition() );
		if( !GetMiscTactics().shouldKeepXhairOnEnemy ) {
			float distanceThreshold = 768.0f;
			if( lostOrHiddenEnemy->ent && lostOrHiddenEnemy->ent->s.effects & ( EF_QUAD | EF_CARRIER ) ) {
				distanceThreshold = 2048.0f;
			}
			if( origin.SquareDistanceTo( self->s.origin ) > distanceThreshold * distanceThreshold ) {
				lastChosenLostOrHiddenEnemy = nullptr;
				return;
			}
		}

		lastChosenLostOrHiddenEnemy = lostOrHiddenEnemy;
		keptInFovPoint.Update( origin, lastChosenLostOrHiddenEnemyInstanceId );
		return;
	}

	lastChosenLostOrHiddenEnemy = nullptr;
	keptInFovPoint.Deactivate();
}

void Bot::TouchedOtherEntity( const edict_t *entity ) {
	if( !entity->classname ) {
		return;
	}

	// Cut off string comparisons by doing these cheap tests first

	// Only triggers are interesting for following code
	if( entity->r.solid != SOLID_TRIGGER ) {
		return;
	}
	// Items should be handled by TouchedNavEntity() or skipped (if it is not a current nav entity)
	if( entity->item ) {
		return;
	}

	if( !Q_stricmp( entity->classname, "trigger_push" ) ) {
		lastTouchedJumppadAt = level.time;
		movementState.jumppadMovementState.Activate( entity );
		return;
	}

	if( !Q_stricmp( entity->classname, "trigger_teleport" ) ) {
		lastTouchedTeleportAt = level.time;
		return;
	}

	if( !Q_stricmp( entity->classname, "func_plat" ) ) {
		lastTouchedElevatorAt = level.time;
		return;
	}
}

void Bot::EnableAutoAlert( const AiAlertSpot &alertSpot, AlertCallback callback, void *receiver ) {
	// First check duplicate ids. Fail on error since callers of this method are internal.
	for( unsigned i = 0; i < alertSpots.size(); ++i ) {
		if( alertSpots[i].id == alertSpot.id ) {
			FailWith( "Duplicated alert spot (id=%d)\n", alertSpot.id );
		}
	}

	if( alertSpots.size() == alertSpots.capacity() ) {
		FailWith( "Can't add an alert spot (id=%d)\n: too many spots", alertSpot.id );
	}

	alertSpots.emplace_back( AlertSpot( alertSpot, callback, receiver ) );
}

void Bot::DisableAutoAlert( int id ) {
	for( unsigned i = 0; i < alertSpots.size(); ++i ) {
		if( alertSpots[i].id == id ) {
			alertSpots.erase( alertSpots.begin() + i );
			return;
		}
	}

	FailWith( "Can't find alert spot by id %d\n", id );
}

void Bot::RegisterVisibleEnemies() {
	if( G_ISGHOSTING( self ) || GS_MatchState() == MATCH_STATE_COUNTDOWN || GS_ShootingDisabled() ) {
		return;
	}

	CheckIsInThinkFrame( __FUNCTION__ );

	// Compute look dir before loop
	vec3_t lookDir;
	AngleVectors( self->s.angles, lookDir, nullptr, nullptr );

	const float dotFactor = FovDotFactor();

	struct EntAndDistance {
		int entNum;
		float distance;

		EntAndDistance( int entNum_, float distance_ ) : entNum( entNum_ ), distance( distance_ ) {}
		bool operator<( const EntAndDistance &that ) const { return distance < that.distance; }
	};

	// Do not call inPVS() and G_Visible() for potential targets inside a loop for all clients.
	// In worst case when all bots may see each other we get N^2 traces and PVS tests
	// First, select all candidate targets along with distance to a bot.
	// Then choose not more than BotBrain::maxTrackedEnemies nearest enemies for calling OnEnemyViewed()
	// It may cause data loss (far enemies may have higher logical priority),
	// but in a common good case (when there are few visible enemies) it preserves data,
	// and in the worst case mentioned above it does not act weird from player POV and prevents server hang up.
	// Note: non-client entities also may be candidate targets.
	StaticVector<EntAndDistance, MAX_EDICTS> candidateTargets;

	for( int i = 1; i < game.numentities; ++i ) {
		edict_t *ent = game.edicts + i;
		if( botBrain.MayNotBeFeasibleEnemy( ent ) ) {
			continue;
		}

		// Reject targets quickly by fov
		Vec3 toTarget( ent->s.origin );
		toTarget -= self->s.origin;
		float squareDistance = toTarget.SquaredLength();
		if( squareDistance < 1 ) {
			continue;
		}
		if( squareDistance > ent->aiVisibilityDistance * ent->aiVisibilityDistance ) {
			continue;
		}

		float invDistance = Q_RSqrt( squareDistance );
		toTarget *= invDistance;
		if( toTarget.Dot( lookDir ) < dotFactor ) {
			continue;
		}

		// It seams to be more instruction cache-friendly to just add an entity to a plain array
		// and sort it once after the loop instead of pushing an entity in a heap on each iteration
		candidateTargets.emplace_back( EntAndDistance( ENTNUM( ent ), 1.0f / invDistance ) );
	}

	std::sort( candidateTargets.begin(), candidateTargets.end() );

	// Select inPVS/visible targets first to aid instruction cache, do not call callbacks in loop
	StaticVector<edict_t *, MAX_CLIENTS> targetsInPVS;
	StaticVector<edict_t *, MAX_CLIENTS> visibleTargets;

	static_assert( AiBaseEnemyPool::MAX_TRACKED_ENEMIES <= MAX_CLIENTS, "targetsInPVS capacity may be exceeded" );

	for( int i = 0, end = std::min( candidateTargets.size(), botBrain.MaxTrackedEnemies() ); i < end; ++i ) {
		edict_t *ent = game.edicts + candidateTargets[i].entNum;
		if( trap_inPVS( self->s.origin, ent->s.origin ) ) {
			targetsInPVS.push_back( ent );
		}
	}

	for( auto ent: targetsInPVS )
		if( G_Visible( self, ent ) ) {
			visibleTargets.push_back( ent );
		}

	// Call bot brain callbacks on visible targets
	for( auto ent: visibleTargets )
		botBrain.OnEnemyViewed( ent );

	botBrain.AfterAllEnemiesViewed();

	CheckAlertSpots( visibleTargets );
}

void Bot::CheckAlertSpots( const StaticVector<edict_t *, MAX_CLIENTS> &visibleTargets ) {
	float scores[MAX_ALERT_SPOTS];

	// First compute scores (good for instruction cache)
	for( unsigned i = 0; i < alertSpots.size(); ++i ) {
		float score = 0.0f;
		const auto &alertSpot = alertSpots[i];
		const float squareRadius = alertSpot.radius * alertSpot.radius;
		const float invRadius = 1.0f / alertSpot.radius;
		for( const edict_t *ent: visibleTargets ) {
			float squareDistance = DistanceSquared( ent->s.origin, alertSpot.origin.Data() );
			if( squareDistance > squareRadius ) {
				continue;
			}
			float distance = Q_RSqrt( squareDistance + 0.001f );
			score += 1.0f - distance * invRadius;
			// Put likely case first
			if( !( ent->s.effects & EF_CARRIER ) ) {
				score *= alertSpot.regularEnemyInfluenceScale;
			} else {
				score *= alertSpot.carrierEnemyInfluenceScale;
			}
		}
		// Clamp score by a max value
		clamp_high( score, 3.0f );
		// Convert score to [0, 1] range
		score /= 3.0f;
		// Get a square root of score (values closer to 0 gets scaled more than ones closer to 1)
		score = 1.0f / Q_RSqrt( score + 0.001f );
		// Sanitize
		clamp( score, 0.0f, 1.0f );
		scores[i] = score;
	}

	// Then call callbacks
	const int64_t levelTime = level.time;
	for( unsigned i = 0; i < alertSpots.size(); ++i ) {
		auto &alertSpot = alertSpots[i];
		uint64_t nonReportedFor = (uint64_t)( levelTime - alertSpot.lastReportedAt );
		if( nonReportedFor >= 1000 ) {
			alertSpot.lastReportedScore = 0.0f;
		}

		// Since scores are sanitized, they are in range [0.0f, 1.0f], and abs(scoreDelta) is in range [-1.0f, 1.0f];
		float scoreDelta = scores[i] - alertSpot.lastReportedScore;
		if( scoreDelta >= 0 ) {
			if( nonReportedFor >= 1000 - scoreDelta * 500 ) {
				alertSpot.Alert( this, scores[i] );
			}
		} else {
			if( nonReportedFor >= 500 - scoreDelta * 500 ) {
				alertSpot.Alert( this, scores[i] );
			}
		}
	}
}

bool Bot::CanChangeWeapons() const {
	if( !movementState.weaponJumpMovementState.IsActive() ) {
		return true;
	}

	if( movementState.weaponJumpMovementState.hasTriggeredRocketJump ) {
		return true;
	}

	return false;
}

void Bot::ChangeWeapons( const SelectedWeapons &selectedWeapons_ ) {
	if( selectedWeapons_.BuiltinFireDef() != nullptr ) {
		self->r.client->ps.stats[STAT_PENDING_WEAPON] = selectedWeapons_.BuiltinWeaponNum();
	}
	if( selectedWeapons_.ScriptFireDef() != nullptr ) {
		GT_asSelectScriptWeapon( self->r.client, selectedWeapons_.ScriptWeaponNum() );
	}
}

void Bot::ChangeWeapon( int weapon ) {
	self->r.client->ps.stats[STAT_PENDING_WEAPON] = weapon;
}

//==========================================
// BOT_DMclass_VSAYmessages
//==========================================
void Bot::SayVoiceMessages() {
	if( GS_MatchState() != MATCH_STATE_PLAYTIME ) {
		return;
	}

	if( self->snap.damageteam_given > 25 ) {
		if( rand() & 1 ) {
			if( rand() & 1 ) {
				G_BOTvsay_f( self, "oops", true );
			} else {
				G_BOTvsay_f( self, "sorry", true );
			}
		}
		return;
	}

	if( vsayTimeout > level.time ) {
		return;
	}

	if( GS_MatchDuration() && game.serverTime + 4000 > GS_MatchEndTime() ) {
		vsayTimeout = game.serverTime + ( 1000 + ( GS_MatchEndTime() - game.serverTime ) );
		if( rand() & 1 ) {
			G_BOTvsay_f( self, "goodgame", false );
		}
		return;
	}

	vsayTimeout = (int64_t)( level.time + ( ( 8 + random() * 12 ) * 1000 ) );

	// the more bots, the less vsays to play
	if( random() > 0.1 + 1.0f / game.numBots ) {
		return;
	}

	if( GS_TeamBasedGametype() && !GS_InvidualGameType() ) {
		if( self->health < 20 && random() > 0.3 ) {
			G_BOTvsay_f( self, "needhealth", true );
			return;
		}

		if( ( self->s.weapon == 0 || self->s.weapon == 1 ) && random() > 0.7 ) {
			G_BOTvsay_f( self, "needweapon", true );
			return;
		}

		if( self->r.client->resp.armor < 10 && random() > 0.8 ) {
			G_BOTvsay_f( self, "needarmor", true );
			return;
		}
	}

	// NOT team based here

	if( random() > 0.2 ) {
		return;
	}

	switch( (int)brandom( 1, 8 ) ) {
		default:
			break;
		case 1:
			G_BOTvsay_f( self, "roger", false );
			break;
		case 2:
			G_BOTvsay_f( self, "noproblem", false );
			break;
		case 3:
			G_BOTvsay_f( self, "yeehaa", false );
			break;
		case 4:
			G_BOTvsay_f( self, "yes", false );
			break;
		case 5:
			G_BOTvsay_f( self, "no", false );
			break;
		case 6:
			G_BOTvsay_f( self, "booo", false );
			break;
		case 7:
			G_BOTvsay_f( self, "attack", false );
			break;
		case 8:
			G_BOTvsay_f( self, "ok", false );
			break;
	}
}


//==========================================
// BOT_DMClass_BlockedTimeout
// the bot has been blocked for too long
//==========================================
void Bot::OnBlockedTimeout() {
	self->health = 0;
	blockedTimeout = level.time + BLOCKED_TIMEOUT;
	self->die( self, self, self, 100000, vec3_origin );
	G_Killed( self, self, self, 999, vec3_origin, MOD_SUICIDE );
	self->nextThink = level.time + 1;
}

//==========================================
// BOT_DMclass_DeadFrame
// ent is dead = run this think func
//==========================================
void Bot::GhostingFrame() {
	selectedEnemies.Invalidate();
	selectedWeapons.Invalidate();

	lastChosenLostOrHiddenEnemy = nullptr;

	botBrain.ClearGoalAndPlan();

	movementState.Reset();

	blockedTimeout = level.time + BLOCKED_TIMEOUT;
	self->nextThink = level.time + 100;

	// wait 4 seconds after entering the level
	if( self->r.client->level.timeStamp + 4000 > level.time || !level.canSpawnEntities ) {
		return;
	}

	if( self->r.client->team == TEAM_SPECTATOR ) {
		// try to join a team
		// note that G_Teams_JoinAnyTeam is quite slow so only call it per frame
		if( !self->r.client->queueTimeStamp && self == level.think_client_entity ) {
			G_Teams_JoinAnyTeam( self, false );
		}

		if( self->r.client->team == TEAM_SPECTATOR ) { // couldn't join, delay the next think
			self->nextThink = level.time + 2000 + (int)( 4000 * random() );
		} else {
			self->nextThink = level.time + 1;
		}
		return;
	}

	BotInput botInput;
	botInput.isUcmdSet = true;
	// ask for respawn if the minimum bot respawning time passed
	if( level.time > self->deathTimeStamp + 3000 ) {
		botInput.SetAttackButton( true );
	}

	CallGhostingClientThink( botInput );
}

void Bot::CallGhostingClientThink( const BotInput &input ) {
	usercmd_t ucmd;
	input.CopyToUcmd( &ucmd );
	// set approximate ping and show values
	ucmd.serverTimeStamp = game.serverTime;
	ucmd.msec = (uint8_t)game.frametime;
	self->r.client->r.ping = 0;

	ClientThink( self, &ucmd, 0 );
}

void Bot::OnRespawn() {
	ResetNavigation();
}

void Bot::Think() {
	// Call superclass method first
	Ai::Think();

	if( IsGhosting() ) {
		return;
	}

	RegisterVisibleEnemies();

	UpdateKeptInFovPoint();

	if( CanChangeWeapons() ) {
		weaponsSelector.Think( botBrain.cachedWorldState );
		ChangeWeapons( selectedWeapons );
	}
}

//==========================================
// BOT_DMclass_RunFrame
// States Machine & call client movement
//==========================================
void Bot::Frame() {
	// Call superclass method first
	Ai::Frame();

	if( IsGhosting() ) {
		GhostingFrame();
	} else {
		ActiveFrame();
	}
}

void Bot::ActiveFrame() {
	//get ready if in the game
	if( GS_MatchState() <= MATCH_STATE_WARMUP && !IsReady() && self->r.client->teamstate.timeStamp + 4000 < level.time ) {
		G_Match_Ready( self );
	}

	weaponsSelector.Frame( botBrain.cachedWorldState );

	BotInput botInput;
	// Might modify botInput
	ApplyPendingTurnToLookAtPoint( &botInput );
	// Might modify botInput
	MovementFrame( &botInput );
	// Might modify botInput
	if( ShouldAttack() ) {
		FireWeapon( &botInput );
	}

	// Apply modified botInput
	ApplyInput( &botInput );
	CallActiveClientThink( botInput );

	SayVoiceMessages();
}

void Bot::CallActiveClientThink( const BotInput &input ) {
	usercmd_t ucmd;
	input.CopyToUcmd( &ucmd );

	//set up for pmove
	for( int i = 0; i < 3; i++ )
		ucmd.angles[i] = (short)ANGLE2SHORT( self->s.angles[i] ) - self->r.client->ps.pmove.delta_angles[i];

	VectorSet( self->r.client->ps.pmove.delta_angles, 0, 0, 0 );

	// set approximate ping and show values
	ucmd.msec = (uint8_t)game.frametime;
	ucmd.serverTimeStamp = game.serverTime;

	ClientThink( self, &ucmd, 0 );
	self->nextThink = level.time + 1;
}
