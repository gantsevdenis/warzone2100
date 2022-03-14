#ifndef _fsm_h_
#define _fsm_h_
#include <functional>
#include <unordered_set>
#include <unordered_map> 
/*#include <vector>
#include <cstdint>
#include <cstddef>
#include <string>*/
#include "../basedef.h"
#include "../droiddef.h"
typedef size_t StateIdx;

struct DROID;
struct STRUCTURE;
/*
We want to have separated SM for each droid type so that their behavior
doesn't overlap, and it's easier to debug/extend/reassign at runtime
Each *_OnTick can only return another state from the same State Machine
Some StateMachines may use memory to remember what as previous, and return to it
*/

namespace FSM
{
	#define UID_DECL()  static const uint32_t UID = 0x000010000 + __COUNTER__;\
                    uint32_t getUID() const { return UID;}
	// everything is const because all state is kept elsewhere
	// this is only logic, no reason to be mutable!
	#define BASE_DECL(T)    public:\
							virtual ~T() {}; \
							virtual void         onEntry(DROID& self) const { };\
							const virtual T&     onTick (DROID& self) const { return *this;};\
							virtual void         onExit (DROID& self) const { };\
							virtual uint32_t     getUID() const { return 0;}

	#define BODY_DECL(T, BASE)      void onEntry(DROID& droid) const override final \
									{ return T ## _OnEntry(droid); };\
									const BASE& onTick(DROID& droid) const final\
									{ return T ## _OnTick(droid); };\
									void onExit(DROID& droid) const final \
									{ return T ## _OnExit(droid); }

	class MoveBase
	{
		BASE_DECL(MoveBase);
	};
	class GroundAttackBase
	{
		BASE_DECL(GroundAttackBase);
	};

	void MoveNone_OnEntry(DROID&);
	const MoveBase& MoveNone_OnTick(DROID&);
	void MoveNone_OnExit(DROID&);
	class MoveNone: public MoveBase
	{
        public:
        UID_DECL();
		BODY_DECL(MoveNone, MoveBase);
	};

	void Move_OnEntry(DROID&);
	const MoveBase& Move_OnTick(DROID&);
	void Move_OnExit(DROID&);
	class Move: public MoveBase
	{
        public:
        UID_DECL();
		BODY_DECL(Move, MoveBase);
	};

	void MoveWaitDuringRepair_OnEntry(DROID&);
	const MoveBase& MoveWaitDuringRepair_OnTick(DROID&);
	void MoveWaitDuringRepair_OnExit(DROID&);
	class MoveWaitDuringRepair: public MoveBase
	{
        public:
        UID_DECL();
		BODY_DECL(MoveWaitDuringRepair, MoveBase);
	};

	void MoveWaitForRepair_OnEntry(DROID&);
	const MoveBase& MoveWaitForRepair_OnTick(DROID&);
	void MoveWaitForRepair_OnExit(DROID&);
	class MoveWaitForRepair: public MoveBase
	{
        public:
        UID_DECL();
		BODY_DECL(MoveWaitForRepair, MoveBase);
	};

	void MoveReturnToPos_OnEntry(DROID&);
	const MoveBase& MoveReturnToPos_OnTick(DROID&);
	void MoveReturnToPos_OnExit(DROID&);
	class MoveReturnToPos: public MoveBase
	{
        public:
        UID_DECL();
		BODY_DECL(MoveReturnToPos, MoveBase);
	};

	void MoveGuard_OnEntry(DROID&);
	const MoveBase& MoveGuard_OnTick(DROID&);
	void MoveGuard_OnExit(DROID&);
	class MoveGuard: public MoveBase
	{
        public:
        UID_DECL();
		BODY_DECL(MoveGuard, MoveBase);
	};

	void MoveTransportWaitToFlyIn_OnEntry(DROID&);
	const MoveBase& MoveTransportWaitToFlyIn_OnTick(DROID&);
	void MoveTransportWaitToFlyIn_OnExit(DROID&);
	class MoveTransportWaitToFlyIn: public MoveBase
	{
        public:
        UID_DECL();
		BODY_DECL(MoveTransportWaitToFlyIn, MoveBase);
	};

	

	void AttackReturnToPos_OnEntry(DROID&);
	const MoveBase& AttackReturnToPos_OnTick(DROID&);
	void AttackReturnToPos_OnExit(DROID&);
	class AttackReturnToPos: public MoveBase
	{
        public:
        UID_DECL();
		BODY_DECL(AttackReturnToPos, MoveBase);
	};
	
	void AttackFireSupportRetreat_OnEntry(DROID&);
	const MoveBase& AttackFireSupportRetreat_OnTick(DROID&);
	void AttackFireSupportRetreat_OnExit(DROID&);
	class AttackFireSupportRetreat: public MoveBase
	{
        public:
        UID_DECL();
		BODY_DECL(AttackFireSupportRetreat, MoveBase);
	};
	
	void AttackNone_OnEntry(DROID&);
	const GroundAttackBase& AttackNone_OnTick(DROID&);
	void AttackNone_OnExit(DROID&);
    class AttackNone: public GroundAttackBase
    {
        public:
        UID_DECL();
		BODY_DECL(AttackNone, GroundAttackBase);
    };
	
	void Attack_OnEntry(DROID&);
	const GroundAttackBase& Attack_OnTick(DROID&);
	void Attack_OnExit(DROID&);
    class Attack: public GroundAttackBase
	{
		public:
		UID_DECL();
		BODY_DECL(Attack, GroundAttackBase);
	};
	bool anythingToShoot(DROID& droid);
    // extern const MoveNone* MoveNoneInstance;
    // extern const Attack* AttackInstance;
    // extern const AttackNone* AttackNoneInstance;
    // extern const MoveWaitDuringRepair* MoveWaitDuringRepairInstance;
    // extern const MoveGuard* MoveGuardInstance;


	/*#define DeriveState() 	protected:\
							virtual void doTick() override;\
							virtual void doOnExit() override;\
							virtual void doOnEntry() override
							*/
}

#endif