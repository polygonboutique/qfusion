#include "bot.h"
#include "ai_shutdown_hooks_holder.h"

inline bool operator!=( const AiScriptWeaponDef &first, const AiScriptWeaponDef &second ) {
	return memcmp( &first, &second, sizeof( AiScriptWeaponDef ) ) != 0;
}

void Bot::UpdateScriptWeaponsStatus() {
	int scriptWeaponsNum = GT_asGetScriptWeaponsNum( self->r.client );

	if( (int)scriptWeaponDefs.size() != scriptWeaponsNum ) {
		scriptWeaponDefs.clear();
		scriptWeaponCooldown.clear();

		for( int weaponNum = 0; weaponNum < scriptWeaponsNum; ++weaponNum ) {
			AiScriptWeaponDef weaponDef;
			if( GT_asGetScriptWeaponDef( self->r.client, weaponNum, &weaponDef ) ) {
				scriptWeaponDefs.emplace_back( std::move( weaponDef ) );
				scriptWeaponCooldown.push_back( GT_asGetScriptWeaponCooldown( self->r.client, weaponNum ) );
			}
		}

		selectedWeapons.Invalidate();
		botBrain.ClearGoalAndPlan();
		return;
	}

	bool hasStatusChanged = false;
	for( int weaponNum = 0; weaponNum < scriptWeaponsNum; ++weaponNum ) {
		AiScriptWeaponDef actualWeaponDef;
		// Try to retrieve the weapon def
		if( !GT_asGetScriptWeaponDef( self->r.client, weaponNum, &actualWeaponDef ) ) {
			// If weapon def retrieval failed, treat the weapon as unavailable by setting a huge cooldown
			scriptWeaponCooldown[weaponNum] = std::numeric_limits<int>::max();
			hasStatusChanged = true;
			continue;
		}

		if( actualWeaponDef != scriptWeaponDefs[weaponNum] ) {
			scriptWeaponDefs[weaponNum] = actualWeaponDef;
			hasStatusChanged = true;
		}

		int cooldown = GT_asGetScriptWeaponCooldown( self->r.client, weaponNum );
		// A weapon became unavailable
		if( cooldown > scriptWeaponCooldown[weaponNum] ) {
			hasStatusChanged = true;
		} else {
			for( int thresholdMillis = 1000; thresholdMillis >= 0; thresholdMillis -= 500 ) {
				if( scriptWeaponCooldown[weaponNum] > thresholdMillis && cooldown <= thresholdMillis ) {
					hasStatusChanged = true;
				}
			}
		}
		scriptWeaponCooldown[weaponNum] = cooldown;
	}

	if( hasStatusChanged ) {
		selectedWeapons.Invalidate();
		botBrain.ClearGoalAndPlan();
	}
}

void Bot::FireWeapon( BotInput *input ) {
	if( !selectedEnemies.AreValid() ) {
		return;
	}

	if( !selectedWeapons.AreValid() ) {
		return;
	}

	const GenericFireDef *builtinFireDef = selectedWeapons.BuiltinFireDef();
	const GenericFireDef *scriptFireDef = selectedWeapons.ScriptFireDef();

	// Should not really happen but an analyzer is unaware of this.
	if( !builtinFireDef && !scriptFireDef ) {
		AI_FailWith( "Bot::FireWeapon()", "Both builtin and script fire defs are null\n" );
	}

	AimParams builtinWeaponAimParams;
	AimParams scriptWeaponAimParams;

	if( builtinFireDef ) {
		builtinFireTargetCache.AdjustAimParams( selectedEnemies, selectedWeapons, *builtinFireDef, &builtinWeaponAimParams );
	}

	if( scriptFireDef ) {
		scriptFireTargetCache.AdjustAimParams( selectedEnemies, selectedWeapons, *scriptFireDef, &scriptWeaponAimParams );
	}

	// Select a weapon that has a priority in adjusting view angles for it
	const GenericFireDef *primaryFireDef = nullptr;
	const GenericFireDef *secondaryFireDef = nullptr;
	AimParams *aimParams;
	if( selectedWeapons.PreferBuiltinWeapon() ) {
		aimParams = &builtinWeaponAimParams;
		primaryFireDef = builtinFireDef;
		if( scriptFireDef ) {
			secondaryFireDef = scriptFireDef;
		}
	} else {
		aimParams = &scriptWeaponAimParams;
		primaryFireDef = scriptFireDef;
		if( builtinFireDef ) {
			secondaryFireDef = builtinFireDef;
		}
	}

	// Always track enemy with a "crosshair" like a human does in each frame
	LookAtEnemy( aimParams->EffectiveCoordError( Skill() ), aimParams->fireOrigin, aimParams->fireTarget, input );

	// Attack only in Think() frames unless a continuousFire is required or the bot has hard skill
	if( ShouldSkipThinkFrame() && Skill() < 0.66f ) {
		if( !primaryFireDef || !primaryFireDef->IsContinuousFire() ) {
			if( !secondaryFireDef || !secondaryFireDef->IsContinuousFire() ) {
				return;
			}
		}
	}

	// Shut an analyzer up by this condition
	if( primaryFireDef ) {
		if( CheckShot( *aimParams, input, selectedEnemies, *primaryFireDef ) ) {
			PressAttack( primaryFireDef, builtinFireDef, scriptFireDef, input );
		}
	}

	if( secondaryFireDef ) {
		// Check whether view angles adjusted for the primary weapon are suitable for firing secondary weapon too
		if( CheckShot( *aimParams, input, selectedEnemies, *secondaryFireDef ) ) {
			PressAttack( secondaryFireDef, builtinFireDef, scriptFireDef, input );
		}
	}

	// Shut an analyzer up by testing scriptFireDef too
	if( input->fireScriptWeapon && scriptFireDef ) {
		GT_asFireScriptWeapon( self->r.client, scriptFireDef->WeaponNum() );
	}
}

void Bot::LookAtEnemy( float coordError, const vec_t *fire_origin, vec_t *target, BotInput *input ) {
	if( !input->canOverrideLookVec && !input->canOverridePitch && input->isLookDirSet ) {
		return;
	}

	Vec3 toTargetDir( target );
	toTargetDir -= fire_origin;
	toTargetDir.NormalizeFast();

	for( int i = 0; i < 3; ++i )
		target[i] += ( aimingRandomHolder.GetCoordRandom( i ) - 0.5f ) * coordError;

	// If there is no look vec set or it can be completely overridden
	if( !input->isLookDirSet || input->canOverrideLookVec ) {
		input->SetIntendedLookDir( toTargetDir );
		Vec3 newAngles = GetNewViewAngles( self->s.angles, toTargetDir, game.frametime, 1.0f );
		input->SetAlreadyComputedAngles( newAngles );
		return;
	}

	// (in case when XY view movement is exactly specified and Z view movement can vary)
	assert( input->canOverridePitch );
	// These angles can be intended by the already set look vec (can be = not always ideal due to limited view speed).
	Vec3 intendedAngles = GetNewViewAngles( self->s.angles, input->IntendedLookDir(), game.frametime, 1.0f );
	// These angles can be required to hit the target
	Vec3 targetAimAngles = GetNewViewAngles( self->s.angles, toTargetDir, game.frametime, 1.0f );
	// Override pitch in hope this will be sufficient for hitting a target
	intendedAngles.Data()[PITCH] = targetAimAngles.Data()[PITCH];
	input->SetAlreadyComputedAngles( intendedAngles );
}

void Bot::PressAttack( const GenericFireDef *fireDef,
					   const GenericFireDef *builtinFireDef,
					   const GenericFireDef *scriptFireDef,
					   BotInput *input ) {
	if( fireDef == scriptFireDef ) {
		input->fireScriptWeapon = true;
		return;
	}

	auto weapState = self->r.client->ps.weaponState;
	if( weapState == WEAPON_STATE_READY || weapState == WEAPON_STATE_REFIRE || weapState == WEAPON_STATE_REFIRESTRONG ) {
		input->SetAttackButton( true );
	}
}

bool Bot::TryTraceShot( trace_t *tr, const Vec3 &newLookDir, const AimParams &aimParams, const GenericFireDef &fireDef ) {
	if( fireDef.AimType() != AI_WEAPON_AIM_TYPE_DROP ) {
		Vec3 traceEnd( newLookDir );
		traceEnd *= 999999.0f;
		traceEnd += aimParams.fireOrigin;
		G_Trace( tr, const_cast<float*>( aimParams.fireOrigin ), nullptr, nullptr, traceEnd.Data(), self, MASK_AISOLID );
		return true;
	}

	// For drop aim type weapons (a gravity is applied to a projectile) split projectile trajectory in segments
	vec3_t segmentStart;
	vec3_t segmentEnd;
	VectorCopy( aimParams.fireOrigin, segmentEnd );

	Vec3 projectileVelocity( newLookDir );
	projectileVelocity *= fireDef.ProjectileSpeed();

	const int numSegments = (int)( 2 + 4 * Skill() );
	// Predict for 1 second
	const float timeStep = 1.0f / numSegments;
	const float halfGravity = 0.5f * level.gravity;
	const float *fireOrigin = aimParams.fireOrigin;

	float currTime = timeStep;
	for( int i = 0; i < numSegments; ++i ) {
		VectorCopy( segmentEnd, segmentStart );
		segmentEnd[0] = fireOrigin[0] + projectileVelocity.X() * currTime;
		segmentEnd[1] = fireOrigin[1] + projectileVelocity.Y() * currTime;
		segmentEnd[2] = fireOrigin[2] + projectileVelocity.Z() * currTime - halfGravity * currTime * currTime;

		G_Trace( tr, segmentStart, nullptr, nullptr, segmentEnd, self, MASK_AISOLID );
		if( tr->fraction != 1.0f ) {
			break;
		}

		currTime += timeStep;
	}

	if( tr->fraction != 1.0f ) {
		return true;
	}

	// If hit point has not been found for predicted for 1 second trajectory
	// Check a trace from the last segment end to an infinite point
	VectorCopy( segmentEnd, segmentStart );
	currTime = 999.0f;
	segmentEnd[0] = fireOrigin[0] + projectileVelocity.X() * currTime;
	segmentEnd[1] = fireOrigin[1] + projectileVelocity.Y() * currTime;
	segmentEnd[2] = fireOrigin[2] + projectileVelocity.Z() * currTime - halfGravity * currTime * currTime;
	G_Trace( tr, segmentStart, nullptr, nullptr, segmentEnd, self, MASK_AISOLID );
	if( tr->fraction == 1.0f ) {
		return false;
	}

	return true;
}

bool Bot::CheckSplashTeamDamage( const vec3_t hitOrigin, const AimParams &aimParams, const GenericFireDef &fireDef ) {
	// TODO: Predict actual teammates origins at the moment of explosion
	// (it requires a coarse physics simulation and collision tests)

	trace_t trace;
	// Make sure the tested explosion origin is not on a solid plane
	Vec3 traceStart( aimParams.fireOrigin );
	traceStart -= hitOrigin;
	traceStart.NormalizeFast();
	traceStart += hitOrigin;

	int entNums[32];
	// This call might return an actual entities count exceeding the buffer capacity
	int numEnts = GClip_FindInRadius( const_cast<float *>( hitOrigin ), fireDef.SplashRadius(), entNums, 32 );
	const edict_t *gameEdicts = game.edicts;
	for( int i = 0, end = std::min( 32, numEnts ); i < end; ++i ) {
		const edict_t *ent = gameEdicts + entNums[i];
		if( ent->s.team != self->s.team ) {
			continue;
		}
		if( ent->s.solid == SOLID_NOT ) {
			continue;
		}
		// Also prevent damaging non-client team entities
		if( !ent->r.client && ent->takedamage == DAMAGE_NO ) {
			continue;
		}
		// Very coarse but satisfiable
		SolidWorldTrace( &trace, traceStart.Data(), ent->s.origin );
		if( trace.fraction != 1.0f ) {
			continue;
		}
		return false;
	}

	return true;
}

bool Bot::IsShotBlockedBySolidWall( trace_t *tr,
									float distanceThreshold,
									const AimParams &aimParams,
									const GenericFireDef &fireDef ) {
	AimParams adjustedParams;
	memcpy( &adjustedParams, &aimParams, sizeof( AimParams ) );

	adjustedParams.fireTarget[0] -= 20.0f;
	adjustedParams.fireTarget[1] -= 20.0f;
	Vec3 adjustedLookDir( adjustedParams.fireTarget );
	adjustedLookDir -= adjustedParams.fireOrigin;
	adjustedLookDir.NormalizeFast();
	TryTraceShot( tr, adjustedLookDir, adjustedParams, fireDef );
	if( tr->fraction == 1.0f ) {
		return false;
	}
	if( DistanceSquared( tr->endpos, aimParams.fireTarget ) < distanceThreshold * distanceThreshold ) {
		return false;
	}

	adjustedParams.fireTarget[0] += 40.0f;
	adjustedParams.fireTarget[1] += 40.0f;
	adjustedLookDir.Set( adjustedParams.fireTarget );
	adjustedLookDir -= adjustedParams.fireOrigin;
	adjustedLookDir.NormalizeFast();
	TryTraceShot( tr, adjustedLookDir, adjustedParams, fireDef );
	if( tr->fraction == 1.0f ) {
		return false;
	}

	return DistanceSquared( tr->endpos, aimParams.fireTarget ) > distanceThreshold * distanceThreshold;
}

bool Bot::CheckShot( const AimParams &aimParams,
					 const BotInput *input,
					 const SelectedEnemies &selectedEnemies_,
					 const GenericFireDef &fireDef ) {
	// Convert modified angles to direction back (due to limited view speed it rarely will match given direction)
	Vec3 newLookDir( 0, 0, 0 );
	AngleVectors( input->AlreadyComputedAngles().Data(), newLookDir.Data(), nullptr, nullptr );

	Vec3 toTarget( aimParams.fireTarget );
	toTarget -= aimParams.fireOrigin;
	toTarget.NormalizeFast();
	float toTargetDotLookDir = toTarget.Dot( newLookDir );

	// Precache this result, it is not just a value getter
	const auto aimType = fireDef.AimType();

	// Cut off early
	if( aimType != AI_WEAPON_AIM_TYPE_DROP ) {
		if( toTargetDotLookDir < 0.5f ) {
			return false;
		}
	} else if( toTargetDotLookDir < 0 ) {
		return false;
	}

	// Do not shoot in enemies that are behind obstacles atm, bot may kill himself easily
	// We test directions factor first because it is cheaper to calculate

	trace_t tr;
	memset( &tr, 0, sizeof( trace_t ) );
	if( !TryTraceShot( &tr, newLookDir, aimParams, fireDef ) ) {
		return false;
	}

	float hitToBotDist = std::numeric_limits<float>::max();
	if( tr.fraction != 1.0f ) {
		// Do a generic check for team damage
		if( ( game.edicts[tr.ent].s.team == self->s.team ) && GS_TeamBasedGametype() && g_allow_teamdamage->integer ) {
			return false;
		}
		hitToBotDist = DistanceFast( self->s.origin, tr.endpos );
	}

	if( aimType == AI_WEAPON_AIM_TYPE_PREDICTION_EXPLOSIVE ) {
		if( tr.fraction == 1.0f ) {
			return true;
		}

		if( GS_SelfDamage() && hitToBotDist < fireDef.SplashRadius() ) {
			return false;
		}

		if( GS_TeamBasedGametype() && g_allow_teamdamage->integer ) {
			if( !CheckSplashTeamDamage( tr.endpos, aimParams, fireDef ) ) {
				return false;
			}
		}

		float testedSplashRadius = 0.5f * fireDef.SplashRadius();
		if( !this->ShouldKeepXhairOnEnemy() ) {
			testedSplashRadius *= 1.5f;
		}
		return DistanceSquared( tr.endpos, aimParams.fireTarget ) < testedSplashRadius * testedSplashRadius;
	}

	if( aimType == AI_WEAPON_AIM_TYPE_PREDICTION ) {
		if( tr.fraction == 1.0f ) {
			return true;
		}

		// Avoid suicide with PG
		if( hitToBotDist < fireDef.SplashRadius() && GS_SelfDamage() ) {
			return false;
		}

		if( IsShotBlockedBySolidWall( &tr, 96.0f, aimParams, fireDef ) ) {
			return false;
		}

		// Put very low restrictions on PG since spammy fire style is even adviced.
		if( fireDef.IsBuiltin() && fireDef.WeaponNum() == WEAP_PLASMAGUN ) {
			return toTargetDotLookDir > ( ( this->ShouldKeepXhairOnEnemy() ) ? 0.85f : 0.70f );
		}

		return toTargetDotLookDir > ( ( this->ShouldKeepXhairOnEnemy() ) ? 0.95f : 0.90f );
	}

	// Trajectory prediction is not accurate, also this adds some randomization in grenade spamming.
	if( aimType == AI_WEAPON_AIM_TYPE_DROP ) {
		// Allow shooting grenades in vertical walls
		if( DotProduct( tr.plane.normal, &axis_identity[AXIS_UP] ) < -0.1f ) {
			return false;
		}

		if( GS_TeamBasedGametype() && g_allow_teamdamage->integer ) {
			if( !CheckSplashTeamDamage( tr.endpos, aimParams, fireDef ) ) {
				return false;
			}
		}

		float testedSplashRadius = fireDef.SplashRadius();
		if( !this->ShouldKeepXhairOnEnemy() ) {
			testedSplashRadius *= 1.25f;
		}
		return DistanceSquared( tr.endpos, aimParams.fireTarget ) < testedSplashRadius * testedSplashRadius;
	}

	if( fireDef.IsBuiltin() && this->ShouldKeepXhairOnEnemy() ) {
		// For one-shot instant-hit weapons each shot is important, so check against a player bounding box
		// This is an extra hack for EB/IG, otherwise they miss too lot due to premature firing
		if( fireDef.WeaponNum() == WEAP_ELECTROBOLT || fireDef.WeaponNum() == WEAP_INSTAGUN ) {
			if( tr.fraction == 1.0f ) {
				return true;
			}

			// If trace hit pos is behind the target, fallback to generic
			// instant hit dot product tests which are fine in this case
			float distanceToTarget = DistanceFast( aimParams.fireOrigin, aimParams.fireTarget );
			// Add a distance offset to ensure the hit pos is really behind the target in the case described above
			distanceToTarget += 48.0f;
			float squareDistanceToHitPos = DistanceSquared( aimParams.fireOrigin, tr.endpos );
			if( squareDistanceToHitPos < distanceToTarget * distanceToTarget ) {
				Vec3 absMins( aimParams.fireTarget );
				Vec3 absMaxs( aimParams.fireTarget );
				absMins += playerbox_stand_mins;
				absMaxs += playerbox_stand_maxs;

				float factor = ( 1.0f - 0.75f * Skill() );
				if( this->ShouldKeepXhairOnEnemy() ) {
					factor *= 1.0f - 0.75 * Skill();
				}

				float radius = 1.0f + factor * 64.0f;
				return BoundsOverlapSphere( absMins.Data(), absMaxs.Data(), tr.endpos, radius );
			}
		}
	}

	// Generic instant-hit weapons without splash
	if( tr.fraction != 1.0f && IsShotBlockedBySolidWall( &tr, 64.0f, aimParams, fireDef ) ) {
		return false;
	}

	float dotThreshold = 0.97f;
	if( fireDef.IsBuiltin() ) {
		if( fireDef.WeaponNum() == WEAP_LASERGUN || fireDef.WeaponNum() == WEAP_RIOTGUN ) {
			dotThreshold -= ( this->ShouldKeepXhairOnEnemy() ) ? 0.10f : 0.20f;
		}
	} else {
		if( !this->ShouldKeepXhairOnEnemy() ) {
			dotThreshold -= 0.03f;
		}
	}

	return toTargetDotLookDir >= dotThreshold;
}
