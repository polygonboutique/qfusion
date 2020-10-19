#ifndef QFUSION_BOT_PERCEPTION_MANAGER_H
#define QFUSION_BOT_PERCEPTION_MANAGER_H

#include "ai_base_brain.h"
#include "static_deque.h"

struct Danger : public PoolItem {
	static constexpr unsigned TIMEOUT = 400;

	Danger( PoolBase *pool_ )
        : PoolItem( pool_ ),
        hitPoint( 0, 0, 0 ),
        direction( 0, 0, 0 ),
        damage( 0 ),
        timeoutAt( 0 ),
        attacker( nullptr ),
        splash( false ) {}

	// Sorting by this operator is fast but should be used only
	// to prepare most dangerous entities of the same type.
	// Ai decisions should be made by more sophisticated code.
	bool operator<( const Danger &that ) const { return this->damage < that.damage; }

	bool IsValid() const { return timeoutAt > level.time; }

	Vec3 hitPoint;
	Vec3 direction;
	float damage;
	int64_t timeoutAt;
	const edict_t *attacker;
	bool splash;
};

class EntitiesDetector
{
	friend class BotPerceptionManager;

	struct EntAndDistance {
		int entNum;
		float distance;

		EntAndDistance( int entNum_, float distance_ ) : entNum( entNum_ ), distance( distance_ ) {}
		bool operator<( const EntAndDistance &that ) const { return distance < that.distance; }
	};

	static constexpr float DETECT_ROCKET_SQ_RADIUS = 300 * 300;
	static constexpr float DETECT_PLASMA_SQ_RADIUS = 400 * 400;
	static constexpr float DETECT_GB_BLAST_SQ_RADIUS = 400 * 400;
	static constexpr float DETECT_GRENADE_SQ_RADIUS = 300 * 300;
	static constexpr float DETECT_LG_BEAM_SQ_RADIUS = 1000 * 1000;

	// There is a way to compute it in compile-time but it looks ugly
	static constexpr float MAX_RADIUS = 1000.0f;
	static_assert( MAX_RADIUS * MAX_RADIUS >= DETECT_ROCKET_SQ_RADIUS, "" );
	static_assert( MAX_RADIUS * MAX_RADIUS >= DETECT_PLASMA_SQ_RADIUS, "" );
	static_assert( MAX_RADIUS * MAX_RADIUS >= DETECT_GB_BLAST_SQ_RADIUS, "" );
	static_assert( MAX_RADIUS * MAX_RADIUS >= DETECT_GRENADE_SQ_RADIUS, "" );
	static_assert( MAX_RADIUS * MAX_RADIUS >= DETECT_LG_BEAM_SQ_RADIUS, "" );

	void Clear();

	static const auto MAX_NONCLIENT_ENTITIES = MAX_EDICTS - MAX_CLIENTS;
	typedef StaticVector<EntAndDistance, MAX_NONCLIENT_ENTITIES> EntsAndDistancesVector;
	typedef StaticVector<uint16_t, MAX_NONCLIENT_ENTITIES> EntNumsVector;

	inline void TryAddEntity( const edict_t *ent,
							  float squareDistanceThreshold,
							  EntsAndDistancesVector &dangerousEntities,
							  EntsAndDistancesVector &otherEntities );
	inline void TryAddGrenade( const edict_t *ent,
							   EntsAndDistancesVector &dangerousEntities,
							   EntsAndDistancesVector &otherEntities );


	// Returns false if not all entities were tested (some entities have been rejected by the limit)
	template<unsigned N, unsigned M, typename PvsFunc, typename VisFunc>
	bool FilterRawEntitiesWithDistances( StaticVector<EntAndDistance, N> &rawEnts,
										 StaticVector<uint16_t, M> &filteredEnts,
										 unsigned visEntsLimit,
										 PvsFunc pvsFunc, VisFunc visFunc );

	const edict_t *const self;

	EntsAndDistancesVector maybeDangerousRockets;
	EntNumsVector dangerousRockets;
	EntsAndDistancesVector maybeDangerousPlasmas;
	EntNumsVector dangerousPlasmas;
	EntsAndDistancesVector maybeDangerousBlasts;
	EntNumsVector dangerousBlasts;
	EntsAndDistancesVector maybeDangerousGrenades;
	EntNumsVector dangerousGrenades;
	StaticVector<EntAndDistance, MAX_NONCLIENT_ENTITIES> maybeDangerousLasers;
	StaticVector<uint16_t, MAX_NONCLIENT_ENTITIES> dangerousLasers;

	EntsAndDistancesVector maybeVisibleOtherRockets;
	EntNumsVector visibleOtherRockets;
	EntsAndDistancesVector maybeVisibleOtherPlasmas;
	EntNumsVector visibleOtherPlasmas;
	EntsAndDistancesVector maybeVisibleOtherBlasts;
	EntNumsVector visibleOtherBlasts;
	EntsAndDistancesVector maybeVisibleOtherGrenades;
	EntNumsVector visibleOtherGrenades;
	EntsAndDistancesVector maybeVisibleOtherLasers;
	EntNumsVector visibleOtherLasers;

	EntitiesDetector( const edict_t *self_ ) : self( self_ ) {}

	void Run();
};

class BotBrain;

class BotPerceptionManager: public AiFrameAwareUpdatable
{
	friend class PlasmaBeamsBuilder;
	friend class EntitiesDetector;
	friend class JumppadUsersTracker;

	EntitiesDetector entitiesDetector;

	edict_t *const self;

	// Currently there is no more than a single active danger. It might be changed in future.
	static constexpr auto MAX_CLASS_DANGERS = 1;
	typedef Pool<Danger, MAX_CLASS_DANGERS> DangersPool;

	DangersPool rocketDangersPool;
	DangersPool plasmaBeamDangersPool;
	DangersPool grenadeDangersPool;
	DangersPool blastDangersPool;
	DangersPool laserBeamsPool;

	Danger *primaryDanger;

	float viewDirDotTeammateDir[MAX_CLIENTS];
	float distancesToTeammates[MAX_CLIENTS];
	uint8_t testedTeammatePlayerNums[MAX_CLIENTS];
	int8_t teammatesVisStatus[MAX_CLIENTS];
	unsigned numTestedTeamMates;
	bool hasComputedTeammatesVisData;
	bool areAllTeammatesInFov;

	struct DetectedEvent {
		vec3_t origin;
		int enemyEntNum;
		DetectedEvent( const vec3_t origin_, int enemyEntNum_ ) {
			VectorCopy( origin_, this->origin );
			this->enemyEntNum = enemyEntNum_;
		}
	};

	StaticDeque<DetectedEvent, 16> eventsQueue;

	static const auto MAX_NONCLIENT_ENTITIES = EntitiesDetector::MAX_NONCLIENT_ENTITIES;
	typedef EntitiesDetector::EntAndDistance EntAndDistance;

	void ClearDangers();

	bool TryAddDanger( float damageScore, const vec3_t hitPoint, const vec3_t direction,
					   const edict_t *owner, bool splash = false );

	typedef StaticVector<uint16_t, MAX_NONCLIENT_ENTITIES> EntNumsVector;
	void FindProjectileDangers( const EntNumsVector &entNums, float dangerRadius, float damageScale );

	void FindPlasmaDangers( const EntNumsVector &entNums );
	void FindLaserDangers( const EntNumsVector &entNums );

	// The failure chance is specified mainly to throttle excessive plasma spam
	void TryGuessingBeamOwnersOrigins( const EntNumsVector &dangerousEntsNums, float failureChance );
	void TryGuessingProjectileOwnersOrigins( const EntNumsVector &dangerousEntNums, float failureChance );

	void ResetTeammatesVisData();
	void ComputeTeammatesVisData( const vec3_t forwardDir, float fovDotFactor );
	bool CanDistinguishEnemyShotsFromTeammates( const vec_t *guessedEnemyOrigin );

	void RegisterVisibleEnemies();

	void PushEnemyEventOrigin( const edict_t *enemy, const vec3_t origin ) {
		if( eventsQueue.size() == eventsQueue.capacity() ) {
			eventsQueue.pop_back();
		}
		eventsQueue.emplace_front( DetectedEvent( origin, ENTNUM( enemy ) ) );
	}

	bool CanPlayerBeHeardAsEnemy( const edict_t *ent, float distanceThreshold ) {
		if( self->s.team != ent->s.team || self->s.team == TEAM_PLAYERS ) {
			if( DistanceSquared( self->s.origin, ent->s.origin ) < distanceThreshold * distanceThreshold ) {
				return true;
			}
		}
		return false;
	}

	bool CanEntityBeHeardAsEnemy( const edict_t *ent, float distanceThreshold ) {
		const edict_t *owner = game.edicts + ent->s.ownerNum;
		if( self->s.team != owner->s.team || self->s.team == TEAM_PLAYERS ) {
			if( DistanceSquared( self->s.origin, ent->s.origin ) < distanceThreshold * distanceThreshold ) {
				return true;
			}
		}
		return false;
	}

	void HandleGenericPlayerEntityEvent( const edict_t *player, float distanceThreshold );
	void HandleGenericEventAtPlayerOrigin( const edict_t *event, float distanceThreshold );
	void HandleGenericImpactEvent( const edict_t *event, float visibleDistanceThreshold );
	void HandleDummyEvent( const edict_t *, float ) {}
	void HandleJumppadEvent( const edict_t *player, float );
	void HandlePlayerTeleportOutEvent( const edict_t *player, float );

	// We are not sure what code a switch statement produces.
	// Event handling code is quite performance-sensitive since its is called for each bot for each event.
	// So we set up a lookup table manually.
	typedef void ( BotPerceptionManager::*EventHandler )( const edict_t *, float );
	EventHandler eventHandlers[MAX_EVENTS];
	float eventHandlingParams[MAX_EVENTS];

	void SetupEventHandlers();
	void SetEventHandler( int event, EventHandler handler, float param = 0.0f ) {
		eventHandlers[event] = handler;
		eventHandlingParams[event] = param;
	}

	class JumppadUsersTracker: public AiFrameAwareUpdatable {
		friend class BotPerceptionManager;
		BotPerceptionManager *perceptionManager;
		// An i-th element corresponds to an i-th client
		bool isTrackedUser[MAX_CLIENTS];
	public:
		JumppadUsersTracker( BotPerceptionManager *perceptionManager_ ) {
			this->perceptionManager = perceptionManager_;
			memset( isTrackedUser, 0, sizeof( isTrackedUser ) );
		}

		void Register( const edict_t *ent ) {
			assert( ent->r.client );
			isTrackedUser[PLAYERNUM( ent )] = true;
		}

		void Think() override;
		void Frame() override;
	};

	JumppadUsersTracker jumppadUsersTracker;

	void ProcessEvents();
public:
	BotPerceptionManager( edict_t *self_ );

	const Danger *PrimaryDanger() const { return primaryDanger; }

	void RegisterEvent( const edict_t *ent, int event, int parm );

	void Think() override;

	void Frame() override {
		AiFrameAwareUpdatable::Frame();
		// Always calls Frame() and calls Think() if needed
		jumppadUsersTracker.Update();
	}

	void SetFrameAffinity( unsigned modulo, unsigned offset ) override {
		AiFrameAwareUpdatable::SetFrameAffinity( modulo, offset );
		jumppadUsersTracker.SetFrameAffinity( modulo, offset );
	}
};

#endif
