#ifndef AI_DANGERS_DETECTOR_H
#define AI_DANGERS_DETECTOR_H

#include "ai_local.h"
#include "vec3.h"

struct Danger {
	static constexpr unsigned TIMEOUT = 400;

	Danger( const Vec3 &hitPoint_,
			const Vec3 &direction_,
			float damage_,
			const edict_t *attacker_ = nullptr,
			bool splash_ = false )
		: hitPoint( hitPoint_ ),
		direction( direction_ ),
		damage( damage_ ),
		timeoutAt( level.time + TIMEOUT ),
		attacker( attacker_ ),
		splash( splash_ )
	{}

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

class DangersDetector
{
	const edict_t *const self; // Bot reference

public:
	DangersDetector( const edict_t *bot ) : self( bot ), primaryDanger( nullptr ) {}

	static constexpr unsigned MAX_ROCKETS = 3;
	static constexpr unsigned MAX_PLASMA_BEAMS = 3;
	static constexpr unsigned MAX_GRENADES = 3;
	static constexpr unsigned MAX_BLASTS = 3;
	static constexpr unsigned MAX_LASER_BEAMS = 3;

	StaticVector<Danger, MAX_ROCKETS> rocketDangers;
	StaticVector<Danger, MAX_PLASMA_BEAMS> plasmaDangers;
	StaticVector<Danger, MAX_GRENADES> grenadeDangers;
	StaticVector<Danger, MAX_BLASTS> blastsDangers;
	StaticVector<Danger, MAX_LASER_BEAMS> laserDangers;

	const Danger *primaryDanger;

	bool FindDangers();

private:
	void Clear();
	void RegisterDanger( const Danger &danger );
	template<unsigned N>
	void ScaleAndRegisterDangers( StaticVector<Danger, N> &danger, float damageScale );
	template<unsigned N, unsigned M>
	bool FindProjectileDangers( StaticVector<Danger, N> &dangers, StaticVector<const edict_t *, M> &entities, float dangerRadius );
	template<unsigned N, unsigned M>
	bool FindPlasmaDangers( StaticVector<Danger, N> &dangers, StaticVector<const edict_t*, M> &plasmas, float plasmaSplashRadius );
	template<unsigned N, unsigned M>
	bool FindLaserDangers( StaticVector<Danger, N> &dangers, StaticVector<const edict_t*, M> &lasers );
};

#endif //QFUSION_DANGERS_DETECTOR_H
