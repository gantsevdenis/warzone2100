#ifndef _fsm_h_
#define _fsm_h_
#include <functional>
#include <unordered_set>
#include <unordered_map> 
/*#include <vector>
#include <cstdint>
#include <cstddef>
#include <string>*/
#include "basedef.h"
//#include "droiddef.h"
typedef size_t StateIdx;

struct DROID;
struct STRUCTURE;


namespace FSM
{

	enum Category
	{
		// each state machine is designed exclusively for a type of unit/structure
		ANY = 0,
		FOR_DROID_WEAPON,
		FOR_DROID_SENSOR,
		FOR_DROID_CONSTRUCT,
		FOR_DROID_PERSON,
		FOR_DROID_TRANSPORTER,
		FOR_DROID_COMMAND,
		FOR_DROID_REPAIR,
		FOR_DROID_SUPERTRANSPORTER,
		FOR_DROID_ANY,
		FOR_REF_DEFENSE,
		FOR_REF_REPAIR_FACILITY,
		FOR_REF_ANY,
	};
	enum States
	{
		STATE_NONE,
		STATE_MOVE,
	}
	// do-nothing action
	template<class T> void idFunc(T t);

	template <class T, class U>
	struct State
	{
		//State(StateIdx _uid, U _onEntry, U _onTick, U _onExit);
		//State(StateIdx);

		// unique ID for this particular State
		//StateIdx uid;
		// called once when state entered
		std::function<void(T)> onEntry;
		// = onUpdate: every game tick
		// returns next state
		std::function<uint32_t(T)> onTick;
		// called once when state exited
		std::function<void(T)> onExit;
		
	};
	typedef std::function<bool()> Pred0;
	typedef std::function<bool(DROID*)> PredDroid;
	typedef std::function<bool(STRUCTURE*)> PredStructure;
	typedef std::function<void(DROID*)> ActionDroid;
	typedef std::function<void(STRUCTURE*)> ActionStructure;
	/** Storage for internal state of FSM for droids/structures */

	// ObjectId -> StateIdx
	static std::unordered_map<StateIdx, std::string> stateNames;
	// static std::unordered_map<uint32_t, Vector2i> actionPos;
	// static std::unordered_map<uint32_t, BASE_OBJECT*[MAX_WEAPONS]> actionTarget;
	// ObjectId -> (actionStarted, lastFrustratedTime, actionPoints)
	static std::unordered_map<uint32_t, UDWORD[3]> actionStartedFrustratedCumulated;

	
	static std::vector<State<DROID*, ActionDroid>> knownStatesDroid;
	static std::vector<State<STRUCTURE*, ActionStructure>> knownStatesStructure;
	StateIdx addStateDroid(const std::string &name, ActionDroid _onEntry, ActionDroid _onTick, ActionDroid _onExit);
	StateIdx addStateStructure(const std::string &name, ActionStructure _onEntry, ActionStructure _onTick, ActionStructure _onExit);

	template<class T>
	struct Transition
	{
		Transition(T, StateIdx);
		T pred;
		StateIdx to;
	};

	// One StateMachine per unit/structure category:
	// for Commanders, Builders, VTOL, Repair Stations, etc...
	class StateMachineDroid
	{
		public:
		StateMachineDroid() = default;
		// If "pred" returns true and if "curState" == "from", then FSM will transition to the "to" state
		void addTransition(StateIdx from, StateIdx to, PredDroid pred);
		// If "pred" returns true and if "curState" == "from", then FSM will cache the result for further evaluation
		void addCachedPredicate(StateIdx from, PredDroid);

		// Called on each tick. Looks for a transition to be applied coming out of current state.
		// Calls onExit, modify current state, call onEntry
		void tick(DROID*);
		protected:
		// Where to go next? Evaluate all predicates and return the first matching
		// May return same state as the current one.
		StateIdx chooseTransition(DROID*) const;
		// Reactions to world events. Always evaluated first, on every tick
		//std::vector<Transition<PredDroid>> reactions;

		// for a given state, what are the transition it can take?
		std::unordered_map<StateIdx, std::vector<Transition<PredDroid>>> edges;
	};

	class StateMachineStruct
	{
		public:
		StateMachineStruct() = default;
		void addTransition(StateIdx from, StateIdx to, PredStructure pred);
		void addReaction(StateIdx from, StateIdx to, PredStructure pred);
		void tick(STRUCTURE*);

		protected:
		StateIdx chooseTransition(STRUCTURE*) const;
		//std::vector<Transition<PredStructure>> reactions;
		std::unordered_map<StateIdx, std::vector<Transition<PredStructure>>> edges;

	};
	/*#define DeriveState() 	protected:\
							virtual void doTick() override;\
							virtual void doOnExit() override;\
							virtual void doOnEntry() override
							
	// Do nothing specific: DACTION_NONE
	class Idle final: public State { public: ~Idle() override = default; DeriveState();	};
	// DACTION_MOVE moving to a location
	class MoveAndFire final: public State { public: ~MoveAndFire() override = default; DeriveState(); };
	// DACTION_MOVE moving to a location
	class MoveNoFire final: public State { public: ~MoveNoFire() override = default; DeriveState(); };
	// DACTION_BUILD repairing a structure 
	class Build final: public State {public: ~Build() override = default; DeriveState(); };
	// DACTION_ATTACK attacking something
	class Attack final: public State {public: ~Attack() override = default; DeriveState(); };
	// DACTION_OBSERVE observing something
	class Observe final: public State {public: ~Observe() override = default; DeriveState(); };
	// DACTION_FIRESUPPORT attacking something visible by a sensor droid
	class FireSupport final: public State {public: ~FireSupport() override = default; DeriveState(); };
	// DACTION_TRANSPORTOUT move transporter offworld
	class TransportOut final: public State {public: ~TransportOut() override = default; DeriveState(); };
	// DACTION_TRANSPORTWAITTOFLYIN wait for timer to move reinforcements in
	class TransportWaitToFlyIn final: public State {public: ~TransportWaitToFlyIn() override = default; DeriveState(); };
	// DACTION_TRANSPORTIN move transporter onworld
	class TransportIn final: public State {public: ~TransportIn() override = default; DeriveState(); };
	// DACTION_DROIDREPAIR repairing a droid
	class DroidRepair final: public State {public: ~DroidRepair() override = default; DeriveState(); };
	// DACTION_RESTORE restore resistance points of a structure
	class Restore final: public State {public: ~Restore() override = default; DeriveState(); };
	*/
}

#endif