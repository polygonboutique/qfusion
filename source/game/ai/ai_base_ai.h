#ifndef QFUSION_AI_BASE_AI_H
#define QFUSION_AI_BASE_AI_H

#include "edict_ref.h"
#include "ai_frame_aware_updatable.h"
#include "ai_goal_entities.h"
#include "ai_aas_world.h"
#include "ai_aas_route_cache.h"
#include "static_vector.h"
#include "../../gameshared/q_comref.h"

class alignas ( 4 )AiEntityPhysicsState
{
	// Fields of this class are packed to allow cheap copying of class instances in bot movement prediction code
	friend class Ai;
	static constexpr float GROUND_TRACE_DEPTH = 128.0f;
	// These fields are accessed way too often, so packing benefits does not outweigh unpacking performance loss.
	vec3_t origin;
	vec3_t velocity;
	float speed;
	float speed2D;
	// Unpacking of these fields is much cheaper than calling AngleVectors() that uses the expensive fsincos instruction
	// 12 bytes totally
	int16_t forwardDir[3];
	int16_t rightDir[3];

	inline static void SetPackedDir( const vec3_t dir, int16_t *result ) {
		// Do not multiply by the exact 2 ^ 15 value, leave some space for vector components slightly > 1.0f
		result[0] = (signed short)( dir[0] * 30000 );
		result[1] = (signed short)( dir[1] * 30000 );
		result[2] = (signed short)( dir[2] * 30000 );
	}
	inline static Vec3 UnpackedDir( const int16_t *packedDir ) {
		float scale = 1.0f / 30000;
		return Vec3( scale * packedDir[0], scale * packedDir[1], scale * packedDir[2] );
	}

public:
	// CONTENTS flags, cannot be compressed
	int waterType;
private:
	int16_t angles[3];
	static_assert( MAX_EDICTS < ( 1 << 15 ), "Fields bits count assumes 2^15 as game entities count limit" );
	// Use a signed type for indicating an absent ground entity by a negative value
	int16_t groundEntNum;
	uint16_t selfEntNum;
	// This needs some precision (can be used to restore trace fraction if needed), so its packed into 2 bytes
	uint16_t heightOverGround;

	inline void SetHeightOverGround( float heightOverGround_ ) {
		if( heightOverGround_ <= GROUND_TRACE_DEPTH ) {
			this->heightOverGround = ( uint16_t )( heightOverGround_ * 256 );
		} else {
			this->heightOverGround = ( uint16_t )( ( GROUND_TRACE_DEPTH + 1 ) * 256 + 1 );
		}
	}

private:
	uint16_t currAasAreaNum;
	uint16_t droppedToFloorAasAreaNum;

	inline void SetSpeed( const vec3_t velocity_ ) {
		float squareSpeed2D = velocity_[0] * velocity_[0] + velocity_[1] * velocity_[1];
		float squareSpeed = squareSpeed2D + velocity_[2] * velocity_[2];
		this->speed = squareSpeed > 0.001f ? sqrtf( squareSpeed ) : 0;
		this->speed2D = squareSpeed2D > 0.001f ? sqrtf( squareSpeed2D ) : 0;
	}

	inline void UpdateDirs( const vec3_t angles_ ) {
		vec3_t dirs[2];
		AngleVectors( angles_, dirs[0], dirs[1], nullptr );
		SetPackedDir( dirs[0], forwardDir );
		SetPackedDir( dirs[1], rightDir );
	}

	void UpdateAreaNums();

public:
	int8_t droppedToFloorOriginOffset;
	uint8_t waterLevel;

	AiEntityPhysicsState()
		: speed( 0 ),
		speed2D( 0 ),
		waterType( 0 ),
		groundEntNum( 0 ),
		selfEntNum( 0 ),
		heightOverGround( 0 ),
		currAasAreaNum( 0 ),
		droppedToFloorAasAreaNum( 0 ),
		droppedToFloorOriginOffset( 0 ),
		waterLevel( 0 ) {}

	inline void UpdateFromEntity( const edict_t *ent ) {
		VectorCopy( ent->s.origin, this->origin );
		SetVelocity( ent->velocity );
		this->waterType = ent->watertype;
		this->waterLevel = ( decltype( this->waterLevel ) )ent->waterlevel;
		SetAngles( ent->s.angles );
		this->groundEntNum = -1;
		if( ent->groundentity ) {
			this->groundEntNum = ( decltype( this->groundEntNum ) )( ENTNUM( ent->groundentity ) );
		}
		this->selfEntNum = ( decltype( this->selfEntNum ) )ENTNUM( ent );

		UpdateAreaNums();
	}

	inline void UpdateFromPMove( const pmove_t *pmove ) {
		VectorCopy( pmove->playerState->pmove.origin, this->origin );
		SetVelocity( pmove->playerState->pmove.velocity );
		this->waterType = pmove->watertype;
		this->waterLevel = ( decltype( this->waterLevel ) )pmove->waterlevel;
		SetAngles( pmove->playerState->viewangles );
		this->groundEntNum = ( decltype( this->groundEntNum ) )pmove->groundentity;
		this->selfEntNum = ( decltype( this->selfEntNum ) )( pmove->playerState->playerNum + 1 );

		UpdateAreaNums();
	}

	inline float HeightOverGround() const {
		if( heightOverGround <= GROUND_TRACE_DEPTH * 256 ) {
			return heightOverGround / 256.0f;
		}
		return std::numeric_limits<float>::infinity();
	}

	// If true, reachability checks do not make sense, wait for landing.
	inline bool IsHighAboveGround() const {
		return heightOverGround > GROUND_TRACE_DEPTH * 256;
	}

	inline const edict_t *GroundEntity() const {
		return groundEntNum >= 0 ? game.edicts + groundEntNum : nullptr;
	}
	inline const edict_t *Self() const { return game.edicts + selfEntNum; }

	inline Vec3 Angles() const {
		return Vec3( (float)SHORT2ANGLE( angles[0] ), (float)SHORT2ANGLE( angles[1] ), (float)SHORT2ANGLE( angles[2] ) );
	}
	inline void SetAngles( const Vec3 &angles_ ) { SetAngles( angles_.Data() ); }
	inline void SetAngles( const vec3_t angles_ ) {
		this->angles[0] = (short)ANGLE2SHORT( angles_[0] );
		this->angles[1] = (short)ANGLE2SHORT( angles_[1] );
		this->angles[2] = (short)ANGLE2SHORT( angles_[2] );
		UpdateDirs( angles_ );
	}

	int CurrAasAreaNum() const { return (int)currAasAreaNum; }
	int DroppedToFloorAasAreaNum() const { return (int)droppedToFloorAasAreaNum; }

	inline Vec3 DroppedToFloorOrigin() const {
		return Vec3( origin[0], origin[1], origin[2] + droppedToFloorOriginOffset );
	}

	// Do not expose origin/velocity directly.
	// These accessors help to trace access to origin, and packing is yet an open question.
	// A bug have already been spotted using this access tracing.

	inline const float *Origin() const { return origin; }
	inline void SetOrigin( const vec3_t origin_ ) { VectorCopy( origin_, this->origin ); }
	inline void SetOrigin( const Vec3 &origin_ ) { SetOrigin( origin_.Data() ); }

	inline const float *Velocity() const { return velocity; }
	inline void SetVelocity( const vec3_t velocity_ ) {
		VectorCopy( velocity_, this->velocity );
		SetSpeed( velocity_ );
	}
	inline void SetVelocity( const Vec3 &velocity_ ) { SetVelocity( velocity_.Data() ); }

	inline float Speed() const { return speed; }
	inline float Speed2D() const { return speed2D; }
	// These getters are provided for compatibility with the other code
	inline float SquareSpeed() const {
		float unpackedSpeed = Speed();
		return unpackedSpeed * unpackedSpeed;
	}
	inline float SquareSpeed2D() const {
		float unpackedSpeed2D = Speed2D();
		return unpackedSpeed2D * unpackedSpeed2D;
	}

	inline Vec3 ForwardDir() const { return UnpackedDir( forwardDir ); }
	inline Vec3 RightDir() const { return UnpackedDir( rightDir ); }

	// Returns number of start areas to use in routing
	inline int PrepareRoutingStartAreas( int *areaNums ) const {
		int numAreas = 0;

		if( int areaNum = CurrAasAreaNum() ) {
			areaNums[numAreas++] = areaNum;
		}

		if( int areaNum = DroppedToFloorAasAreaNum() ) {
			if( numAreas ) {
				if( areaNums[0] != areaNum ) {
					areaNums[numAreas++] = areaNum;
				}
			} else {
				areaNums[numAreas++] = areaNum;
			}
		}

		return numAreas;
	}
};

class Ai : public EdictRef, public AiFrameAwareUpdatable
{
	friend class AiManager;
	friend class AiBaseTeamBrain;
	friend class AiBaseBrain;
	friend class AiBaseAction;
	friend class AiBaseActionRecord;
	friend class AiBaseGoal;

protected:
	// Must be set in a subclass constructor. A subclass manages memory for its brain
	// (it either has it as an intrusive member of allocates it on heap)
	// and provides a reference to it to this base class via this pointer.
	class AiBaseBrain *aiBaseBrain;
	// Must be set in a subclass constructor.
	// A subclass should decide whether a shared or separated route cache should be used.
	// A subclass should destroy the cache instance if necessary.
	AiAasRouteCache *routeCache;
	// A cached reference to an AAS world, set by this class
	AiAasWorld *aasWorld;
	// Must be set in a subclass constructor. Can be arbitrary changed later.
	// Can point to external (predicted) entity physics state during movement planning.
	AiEntityPhysicsState *entityPhysicsState;

	// Preferred and allowed travel flags
	int travelFlags[2];
	ArrayRange<int> travelFlagsRange;

	int64_t blockedTimeoutAt;

	vec3_t angularViewSpeed;

	void SetFrameAffinity( unsigned modulo, unsigned offset ) override;

	void OnNavTargetSet( NavTarget *navTarget );
	virtual void OnNavTargetTouchHandled() {};

	virtual void Frame() override;
	virtual void Think() override;

public:
	static constexpr unsigned MAX_REACH_CACHED = 20;
	struct alignas ( 2 )ReachAndTravelTime {
private:
		// Split an integer value in two parts to allow 2-byte alignment
		uint16_t reachNumHiPart;
		uint16_t reachNumLoPart;

public:
		// AAS travel time to a nav target in centiseconds (seconds ^-2).
		// Do not confuse with travel time required to pass the reach. itself.
		// The intrinsic reach. travel time can be retrieved from the reach. properties addressed by the reachNum.
		short aasTravelTimeToTarget;

		ReachAndTravelTime( int reachNum_, short aasTravelTimeToTarget_ )
		{
			this->reachNumHiPart = (uint16_t)( (unsigned)reachNum_ >> 16 );
			this->reachNumLoPart = (uint16_t)( (unsigned)reachNum_ & 0xFFFF );
			this->aasTravelTimeToTarget = aasTravelTimeToTarget_;
		}

		int ReachNum() const { return (int)( ( reachNumHiPart << 16 ) | reachNumLoPart ); }
	};
	static_assert( sizeof( ReachAndTravelTime ) == 6, "" );

	typedef StaticVector<ReachAndTravelTime, MAX_REACH_CACHED> ReachChainVector;

	Ai( edict_t *self_,
		AiBaseBrain *aiBaseBrain_,
		AiAasRouteCache *routeCache_,
		AiEntityPhysicsState *entityPhysicsState_,
		int preferredAasTravelFlags_,
		int allowedAasTravelFlags_,
		float yawSpeed = 330.0f,
		float pitchSpeed = 170.0f );

	virtual ~Ai() override {};

	inline bool IsGhosting() const { return G_ISGHOSTING( self ); }

	inline int CurrAreaNum() const { return entityPhysicsState->currAasAreaNum; }
	inline int DroppedToFloorAreaNum() const { return entityPhysicsState->droppedToFloorAasAreaNum; }
	int NavTargetAasAreaNum() const;
	Vec3 NavTargetOrigin() const;
	float NavTargetRadius() const;
	bool IsNavTargetBasedOnEntity( const edict_t *ent ) const;

	// Exposed for native and script actions
	int CheckTravelTimeMillis( const Vec3 &from, const Vec3 &to, bool allowUnreachable = true );

	inline int PreferredTravelFlags() const { return travelFlags[0]; }
	inline int AllowedTravelFlags() const { return travelFlags[1]; }

	inline const ArrayRange<int> TravelFlags() const { return travelFlagsRange; }

	// Accepts a touched entity and its old solid before touch
	void TouchedEntity( edict_t *ent );

	// TODO: Remove this, check item spawn time instead
	virtual void OnNavEntityReachedBy( const NavEntity *navEntity, const Ai *grabber ) {}
	virtual void OnEntityReachedSignal( const edict_t *entity ) {}

	void ResetNavigation();

	virtual void OnBlockedTimeout() {};

	static constexpr unsigned BLOCKED_TIMEOUT = 15000;

	unsigned MillisInBlockedState() const {
		int64_t diff = BLOCKED_TIMEOUT - ( blockedTimeoutAt - level.time );
		return diff >= 0 ? (unsigned)diff : 0;
	}

	bool IsBlocked() const {
		// Blocking is checked in Think() frames (usually every 64 millis),
		// so the blockedTimeoutAt value might be a bit outdated
		return MillisInBlockedState() > 64 + 16;
	}

	unsigned MillisUntilBlockedTimeout() const {
		// Returning a positive BLOCKED_TIMEOUT might be confusing in this case
		if( !IsBlocked() ) {
			return 0;
		}
		int64_t diff = level.time - blockedTimeoutAt;
		return diff >= 0 ? (unsigned)diff : 0;
	}
protected:
	const char *Nick() const {
		return self->r.client ? self->r.client->netname : self->classname;
	}

	virtual void TouchedOtherEntity( const edict_t *entity ) {}

	// This function produces very basic but reliable results.
	// Imitation of human-like aiming should be a burden of callers that prepare the desiredDirection.
	inline Vec3 GetNewViewAngles( const Vec3 &oldAngles, const Vec3 &desiredDirection,
								  unsigned frameTime, float angularSpeedMultiplier ) const {
		return GetNewViewAngles( oldAngles.Data(), desiredDirection, frameTime, angularSpeedMultiplier );
	}

	virtual Vec3 GetNewViewAngles( const vec3_t oldAngles, const Vec3 &desiredDirection,
								   unsigned frameTime, float angularSpeedMultiplier ) const;

	void UpdateReachChain( const ReachChainVector &oldReachChain,
						   ReachChainVector *currReachChain,
						   const AiEntityPhysicsState &state ) const;

private:
	float GetChangedAngle( float oldAngle, float desiredAngle, unsigned frameTime,
						   float angularSpeedMultiplier, int angleIndex ) const;
};

#endif
