#include "fsm.h"
#include "action.h"
#include "droid.h"
#include "order.h"
#include <memory>
#include <stdexcept>
static std::unordered_map<int, void*> droidStorage;


namespace FSM
{
	// be sure to explicitely specialize templates
	//template<> void idFunc<DROID*>(DROID*) {};
	//template<> void idFunc<STRUCTURE*>(STRUCTURE*) {};

	StateIdx StateMachineDroid::chooseTransition(DROID* psDroid) const
	{
		//auto v = cachedPreds.at(psDroid->curState);
		/*for (std::pair<PredDroid, bool> &p: v)
		{
			
		}*/

		/*for (const Transition<PredDroid> &tr: reactions)
		{
			if (tr.pred(psDroid))
			{
				return tr.to;
			}
		}*/
		auto ts = edges.at(psDroid->curState);
		for (const Transition<PredDroid> &tr: ts)
		{
			if (tr.pred(psDroid))
			{
				return tr.to;
			}
		}
		return psDroid->curState;
	}
	void StateMachineDroid::addTransition(StateIdx from, StateIdx to, PredDroid pred)
	{

	}
	void StateMachineDroid::tick(DROID* psDroid)
	{
		CHECK_DROID(psDroid);
		PROPULSION_STATS *psPropStats = asPropulsionStats + psDroid->asBits[COMP_PROPULSION];
		ASSERT_OR_RETURN(, psPropStats != nullptr, "Invalid propulsion stats pointer");
		//bool secHoldActive = secondaryGetState(psDroid, DSO_HALTTYPE) == DSS_HALT_HOLD;
		actionSanity(psDroid);
		// we have only 1 possible state, and no memory
		// so handle emp disabled droids here without introducing a new state
		// ...
		// end EMP disabled

		const StateIdx next = chooseTransition(psDroid);
		if (next == psDroid->curState)
		{
			knownStatesDroid.at(next).onTick(psDroid);
		}
		else
		{
			knownStatesDroid.at(psDroid->curState).onExit(psDroid);
			psDroid->curState = next;
			knownStatesDroid.at(next).onEntry(psDroid);
		}
	}
	void attack_onEntry(DROID *psDroid)
	{
		// j'aimerais bien éviter de faire le taf 2 fois
		// le prédicat a été évalué durant ce tick-là donc je peux skip
		// la vérification
		// mais le prédicat pourrait aussi retourner les infos "delta" à appliquer à l'état courant
		/*for (unsigned i = 0; i < psDroid->numWeaps; ++i)
		{
			if (nonNullWeapon[i])
			{
				BASE_OBJECT *psTemp = nullptr;
				WEAPON_STATS *const psWeapStats = &asWeaponStats[psDroid->asWeaps[i].nStat];
				if (psDroid->asWeaps[i].nStat > 0
					&& psWeapStats->rotate
					&& aiBestNearestTarget(psDroid, &psTemp, i) >= 0)
				{
					if (secondaryGetState(psDroid, DSO_ATTACK_LEVEL) == DSS_ALEV_ALWAYS)
					{
						setDroidActionTarget(psDroid, psTemp, i);
					}
				}
			}
		}*/
	}
	void attack_onTick(DROID *psDroid)
	{
		
	}
	void attack_onExit(DROID *psDroid)
	{
		
	}
	bool idle_to_attack(DROID* psDroid)
	{
		return false;
	}
	void idle_onEntry(DROID* psDroid)
	{

	}
	void idle_onExit(DROID* psDroid)
	{

	}
	void create_fsms()
	{
		/*some_unit->fsm = stateMachine0;
		auto idle = makeState<DROID*>("idle");
		auto moveAndFire = makeState<DROID*>("moveAndFire", &moveAndFire_onEntry, &moveAndFire_onTick, &moveAndFire_onExit);
		StateMachine<DROID*> fsm {idle};
		fsm.addTransition(idle, moveAndFire, [](){return true;});
		fsm.tick(nullptr);*/
	}
	/*void MoveAndFire::doTick()
	{

	}
	void MoveAndFire::doOnEntry()
	{

	}
	void MoveAndFire::doOnExit()
	{

	}*/

}