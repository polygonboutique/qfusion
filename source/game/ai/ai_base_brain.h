#ifndef QFUSION_AI_BASE_BRAIN_H
#define QFUSION_AI_BASE_BRAIN_H

#include "ai_local.h"
#include "ai_goal_entities.h"
#include "ai_frame_aware_updatable.h"
#include "static_vector.h"
#include "ai_aas_route_cache.h"
#include "ai_base_ai.h"
#include "world_state.h"

class AiBaseGoal
{
	friend class Ai;
	friend class AiBaseBrain;

	static inline void Register( Ai *ai, AiBaseGoal *goal );

protected:
	edict_t *self;
	const char *name;
	const unsigned updatePeriod;

	float weight;

public:
	// Don't pass self as a constructor argument (self->ai ptr might not been set yet)
	inline AiBaseGoal( Ai *ai, const char *name_, unsigned updatePeriod_ )
		: self( ai->self ), name( name_ ), updatePeriod( updatePeriod_ ), weight( 0.0f ) {
		Register( ai, this );
	}

	virtual ~AiBaseGoal() {};

	virtual void UpdateWeight( const WorldState &worldState ) = 0;
	virtual void GetDesiredWorldState( WorldState *worldState ) = 0;
	virtual struct PlannerNode *GetWorldStateTransitions( const WorldState &worldState ) = 0;

	virtual void OnPlanBuildingStarted() {}
	virtual void OnPlanBuildingCompleted( const class AiBaseActionRecord *planHead ) {}

	inline bool IsRelevant() const { return weight > 0; }

	// More important goals are first after sorting goals array
	inline bool operator<( const AiBaseGoal &that ) const {
		return this->weight > that.weight;
	}

	inline const char *Name() const { return name; }
	inline unsigned UpdatePeriod() const { return updatePeriod; }
};

class alignas ( 8 )PoolBase
{
	friend class PoolItem;

	char *basePtr;
	const char *tag;
	unsigned itemSize;

	static constexpr auto FREE_LIST = 0;
	static constexpr auto USED_LIST = 1;

	short listFirst[2];

#ifdef _DEBUG
	inline const char *ListName( short index ) {
		switch( index ) {
			case FREE_LIST: return "FREE";
			case USED_LIST: return "USED";
			default: abort();
		}
	}
#endif

	inline class PoolItem &ItemAt( short index ) {
		return *(PoolItem *)( basePtr + itemSize * index );
	}
	inline short IndexOf( const class PoolItem *item ) const {
		return (short)( ( (const char *)item - basePtr ) / itemSize );
	}

	inline void Link( short itemIndex, short listIndex );
	inline void Unlink( short itemIndex, short listIndex );

protected:
	void *Alloc();
	void Free( class PoolItem *poolItem );

#ifndef _MSC_VER
	inline void Debug( const char *format, ... ) const __attribute__( ( format( printf, 2, 3 ) ) )
#else
	inline void Debug( _Printf_format_string_ const char *format, ... ) const
#endif
	{
		va_list va;
		va_start( va, format );
		AI_Debugv( tag, format, va );
		va_end( va );
	}

public:
	PoolBase( char *basePtr_, const char *tag_, unsigned itemSize_, unsigned itemsCount );

	void Clear();
};

class alignas ( 8 )PoolItem
{
	friend class PoolBase;
	PoolBase *pool;
	short prevInList;
	short nextInList;

public:
	PoolItem( PoolBase * pool_ ) : pool( pool_ ) {
	}
	virtual ~PoolItem() {
	}

	inline void DeleteSelf() {
		this->~PoolItem();
		pool->Free( this );
	}
};

template<class Item, unsigned N>
class alignas ( 8 )Pool : public PoolBase
{
	static constexpr unsigned ChunkSize() {
		return ( sizeof( Item ) % 8 ) ? sizeof( Item ) + 8 - ( sizeof( Item ) % 8 ) : sizeof( Item );
	}

	alignas( 8 ) char buffer[N * ChunkSize()];

public:
	Pool( const char *tag_ ) : PoolBase( buffer, tag_, sizeof( Item ), N ) {
	}

	inline Item *New() {
		if( void *mem = Alloc() ) {
			return new(mem) Item( this );
		}
		return nullptr;
	}

	template <typename Arg1>
	inline Item *New( Arg1 arg1 ) {
		if( void *mem = Alloc() ) {
			return new(mem) Item( this, arg1 );
		}
		return nullptr;
	}

	template <typename Arg1, typename Arg2>
	inline Item *New( Arg1 arg1, Arg2 arg2 ) {
		if( void *mem = Alloc() ) {
			return new(mem) Item( this, arg1, arg2 );
		}
		return nullptr;
	};

	template <typename Arg1, typename Arg2, typename Arg3>
	inline Item *New( Arg1 arg1, Arg2 arg2, Arg3 arg3 ) {
		if( void *mem = Alloc() ) {
			return new(mem) Item( this, arg1, arg2, arg3 );
		}
		return nullptr;
	};

	template <typename Arg1, typename Arg2, typename Arg3, typename Arg4>
	inline Item *New( Arg1 arg1, Arg2 arg2, Arg3 arg3, Arg4 arg4 ) {
		if( void *mem = Alloc() ) {
			return new(mem) Item( this, arg1, arg2, arg3, arg4 );
		}
		return nullptr;
	};

	template <typename Arg1, typename Arg2, typename Arg3, typename Arg4, typename Arg5>
	inline Item *New( Arg1 arg1, Arg2 arg2, Arg3 arg3, Arg4 arg4, Arg5 arg5 ) {
		if( void *mem = Alloc() ) {
			return new(mem) Item( this, arg1, arg2, arg3, arg4, arg5 );
		}
		return nullptr;
	};
};

class AiBaseActionRecord : public PoolItem
{
	friend class AiBaseAction;

protected:
	edict_t *self;
	const char *name;

#ifndef _MSC_VER
	inline void Debug( const char *format, ... ) const __attribute__( ( format( printf, 2, 3 ) ) )
#else
	inline void Debug( _Printf_format_string_ const char *format, ... ) const
#endif
	{
		va_list va;
		va_start( va, format );
		AI_Debugv( name, format, va );
		va_end( va );
	}

public:
	AiBaseActionRecord *nextInPlan;

	inline AiBaseActionRecord( PoolBase *pool_, edict_t *self_, const char *name_ )
		: PoolItem( pool_ ), self( self_ ), name( name_ ), nextInPlan( nullptr ) {}

	virtual ~AiBaseActionRecord() {}

	virtual void Activate() {
		Debug( "About to activate\n" );
	};
	virtual void Deactivate() {
		Debug( "About to deactivate\n" );
	};

	const char *Name() const { return name; }

	enum Status {
		INVALID,
		VALID,
		COMPLETED
	};

	virtual Status CheckStatus( const WorldState &currWorldState ) const = 0;
};

struct PlannerNode : PoolItem {
	// World state after applying an action
	WorldState worldState;
	// An action record to apply
	AiBaseActionRecord *actionRecord;
	// Used to reconstruct a plan
	PlannerNode *parent;
	// Next in linked list of transitions for current node
	PlannerNode *nextTransition;

	// AStar edge "distance"
	float transitionCost;
	// AStar node G
	float costSoFar;
	// Priority queue parameter
	float heapCost;
	// Utility for retrieval an actual index in heap array by a node value
	unsigned heapArrayIndex;

	// Utilities for storing the node in a hash set
	PlannerNode *prevInHashBin;
	PlannerNode *nextInHashBin;
	uint32_t worldStateHash;

	inline PlannerNode( PoolBase *pool, edict_t *self )
		: PoolItem( pool ),
		worldState( self ),
		actionRecord( nullptr ),
		parent( nullptr ),
		nextTransition( nullptr ),
		transitionCost( std::numeric_limits<float>::max() ),
		costSoFar( std::numeric_limits<float>::max() ),
		heapCost( std::numeric_limits<float>::max() ),
		heapArrayIndex( ~0u ),
		prevInHashBin( nullptr ),
		nextInHashBin( nullptr ),
		worldStateHash( 0 ) {}

	~PlannerNode() override {
		if( actionRecord ) {
			actionRecord->DeleteSelf();
		}

		// Prevent use-after-free
		actionRecord = nullptr;
		parent = nullptr;
		nextTransition = nullptr;
		prevInHashBin = nullptr;
		nextInHashBin = nullptr;
	}
};

class AiBaseAction
{
	friend class Ai;
	friend class AiBaseBrain;

	static inline void Register( Ai *ai, AiBaseAction *action );

protected:
	edict_t *self;
	const char *name;

#ifndef _MSC_VER
	inline void Debug( const char *format, ... ) const __attribute__( ( format( printf, 2, 3 ) ) )
#else
	inline void Debug( _Printf_format_string_ const char *format, ... ) const
#endif
	{
		va_list va;
		va_start( va, format );
		AI_Debugv( name, format, va );
		va_end( va );
	}

	class PlannerNodePtr
	{
		PlannerNode *node;
		PlannerNodePtr( const PlannerNodePtr &that ) = delete;
		PlannerNodePtr &operator=( const PlannerNodePtr &that ) = delete;

public:
		inline explicit PlannerNodePtr( PlannerNode *node_ ) : node( node_ ) {}
		inline PlannerNodePtr( PlannerNodePtr &&that ) : node( that.node ) {
			that.node = nullptr;
		}
		inline PlannerNodePtr &operator=( PlannerNodePtr &&that ) {
			node = that.node;
			that.node = nullptr;
			return *this;
		}
		inline PlannerNode *ReleaseOwnership() {
			PlannerNode *result = node;
			// Clear node reference to avoid being deleted in the destructor
			node = nullptr;
			return result;
		}
		inline ~PlannerNodePtr();
		inline PlannerNode *PrepareActionResult();
		inline class WorldState &WorldState();
		inline float &Cost();
		inline operator bool() const { return node != nullptr; }
	};

	inline PlannerNodePtr NewNodeForRecord( AiBaseActionRecord *record );

public:
	// Don't pass self as a constructor argument (self->ai ptr might not been set yet)
	inline AiBaseAction( Ai *ai, const char *name_ ) : self( ai->self ), name( name_ ) {
		Register( ai, this );
	}

	virtual ~AiBaseAction() {}

	const char *Name() const { return name; }

	virtual PlannerNode *TryApply( const WorldState &worldState ) = 0;
};

class AiBaseBrain : public AiFrameAwareUpdatable
{
	friend class Ai;
	friend class AiManager;
	friend class AiBaseTeamBrain;
	friend class AiBaseGoal;
	friend class AiBaseAction;
	friend class AiBaseActionRecord;
	friend class BotGutsActionsAccessor;

public:
	static constexpr unsigned MAX_GOALS = 12;
	static constexpr unsigned MAX_ACTIONS = 36;

protected:
	edict_t *self;

	// Its mainly used as a storage for nav targets set by scripts
	NavTarget localNavTarget;
	NavTarget *navTarget;
	AiBaseActionRecord *planHead;
	AiBaseGoal *activeGoal;
	int64_t nextActiveGoalUpdateAt;

	const NavTarget *lastReachedNavTarget;
	int64_t lastNavTargetReachedAt;

	int64_t prevThinkAt;

	float decisionRandom;
	int64_t nextDecisionRandomUpdateAt;

	StaticVector<AiBaseGoal *, MAX_GOALS> goals;
	StaticVector<AiBaseAction *, MAX_ACTIONS> actions;

	static constexpr unsigned MAX_PLANNER_NODES = 384;
	Pool<PlannerNode, MAX_PLANNER_NODES> plannerNodesPool;

	signed char attitude[MAX_EDICTS];
	// Used to detect attitude change
	signed char oldAttitude[MAX_EDICTS];

	int CurrAasAreaNum() const { return self->ai->aiRef->entityPhysicsState->CurrAasAreaNum(); };
	int DroppedToFloorAasAreaNum() const { return self->ai->aiRef->entityPhysicsState->DroppedToFloorAasAreaNum(); }
	Vec3 DroppedToFloorOrigin() const { return self->ai->aiRef->entityPhysicsState->DroppedToFloorOrigin(); }

	int PreferredAasTravelFlags() const { return self->ai->aiRef->preferredAasTravelFlags; }
	int AllowedAasTravelFlags() const { return self->ai->aiRef->allowedAasTravelFlags; }

	const AiAasWorld *AasWorld() const { return self->ai->aiRef->aasWorld; }
	AiAasRouteCache *RouteCache() { return self->ai->aiRef->routeCache; }
	const AiAasRouteCache *RouteCache() const { return self->ai->aiRef->routeCache; }

	AiBaseBrain( edict_t *self );

	virtual void PrepareCurrWorldState( WorldState *worldState ) = 0;

	virtual bool ShouldSkipPlanning() const = 0;

	bool UpdateGoalAndPlan( const WorldState &currWorldState );

	bool FindNewGoalAndPlan( const WorldState &currWorldState );

	// Allowed to be overridden in a subclass for class-specific optimization purposes
	virtual AiBaseActionRecord *BuildPlan( AiBaseGoal *goal, const WorldState &startWorldState );

	AiBaseActionRecord *ReconstructPlan( PlannerNode *lastNode ) const;

	void SetGoalAndPlan( AiBaseGoal *goal_, AiBaseActionRecord *planHead_ );

	inline void SetNavTarget( NavTarget *navTarget_ ) {
		this->navTarget = navTarget_;
		self->ai->aiRef->OnNavTargetSet( this->navTarget );
	}

	inline void SetNavTarget( const Vec3 &navTargetOrigin, float reachRadius ) {
		localNavTarget.SetToTacticalSpot( navTargetOrigin, reachRadius );
		self->ai->aiRef->OnNavTargetSet( &localNavTarget );
	}

	inline void ResetNavTarget() {
		this->navTarget = nullptr;
		self->ai->aiRef->OnNavTargetTouchHandled();
	}

	int FindAasParamToGoalArea( int goalAreaNum, int ( AiAasRouteCache::*pathFindingMethod )( int, int, int ) const ) const;

	int FindReachabilityToGoalArea( int goalAreaNum ) const;
	int FindTravelTimeToGoalArea( int goalAreaNum ) const;

	virtual void PreThink() override;

	virtual void Think() override;

	virtual void PostThink() override {
		prevThinkAt = level.time;
	}

	virtual void OnAttitudeChanged( const edict_t *ent, int oldAttitude_, int newAttitude_ ) {}

public:
	virtual ~AiBaseBrain() override {}

	void SetAttitude( const edict_t *ent, int attitude );

	inline bool HasNavTarget() const { return navTarget != nullptr; }

	inline bool HasPlan() const { return planHead != nullptr; }

	void ClearGoalAndPlan();

	void DeletePlan( AiBaseActionRecord *head );

	bool IsCloseToNavTarget( float proximityThreshold ) const {
		return DistanceSquared( self->s.origin, navTarget->Origin().Data() ) < proximityThreshold * proximityThreshold;
	}
	// Calling it when there is no nav target is legal
	int NavTargetAasAreaNum() const {
		return navTarget ? navTarget->AasAreaNum() : 0;
	}
	Vec3 NavTargetOrigin() const { return navTarget->Origin(); }
	float NavTargetRadius() const { return navTarget->RadiusOrDefault( 12.0f ); }
	bool IsNavTargetBasedOnEntity( const edict_t *ent ) const {
		return navTarget ? navTarget->IsBasedOnEntity( ent ) : false;
	}

	bool HandleNavTargetTouch( const edict_t *ent );
	bool TryReachNavTargetByProximity();

	// Helps to reject non-feasible enemies quickly.
	// A false result does not guarantee that enemy is feasible.
	// A true result guarantees that enemy is not feasible.
	bool MayNotBeFeasibleEnemy( const edict_t *ent ) const;
};

inline void AiBaseGoal::Register( Ai *ai, AiBaseGoal *goal ) {
	ai->aiBaseBrain->goals.push_back( goal );
}

inline void AiBaseAction::Register( Ai *ai, AiBaseAction *action ) {
	ai->aiBaseBrain->actions.push_back( action );
}

inline AiBaseAction::PlannerNodePtr::~PlannerNodePtr() {
	if( this->node ) {
		this->node->DeleteSelf();
	}
}

inline PlannerNode *AiBaseAction::PlannerNodePtr::PrepareActionResult() {
	PlannerNode *result = this->node;
	this->node = nullptr;

#ifndef PUBLIC_BUILD
	if( !result->worldState.IsCopiedFromOtherWorldState() ) {
		AI_FailWith( "PlannerNodePtr::PrepareActionResult()", "World state has not been copied from parent one" );
	}
#endif

	// Compute modified world state hash
	// This computation have been put here to avoid error-prone copy-pasting.
	// Another approach is to use lazy hash code computation but it adds branching on each hash code access
	result->worldStateHash = result->worldState.Hash();
	return result;
}

inline WorldState &AiBaseAction::PlannerNodePtr::WorldState() {
	return node->worldState;
}

inline float &AiBaseAction::PlannerNodePtr::Cost() {
	return node->transitionCost;
}

inline AiBaseAction::PlannerNodePtr AiBaseAction::NewNodeForRecord( AiBaseActionRecord *record ) {
	if( !record ) {
		Debug( "Can't allocate an action record\n" );
		return PlannerNodePtr( nullptr );
	}

	PlannerNode *node = self->ai->aiRef->aiBaseBrain->plannerNodesPool.New( self );
	if( !node ) {
		Debug( "Can't allocate a planner node\n" );
		record->DeleteSelf();
		return PlannerNodePtr( nullptr );
	}

	node->actionRecord = record;
	return PlannerNodePtr( node );
}

#endif
