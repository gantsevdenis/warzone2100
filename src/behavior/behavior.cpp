#include "fsm.h"
#include "../action.h"
#include "../droid.h"
#include "../order.h"
#include "../move.h"
#include "../ai.h"
#include "../qtscript.h"
#include "stateregistry.h"
#include <memory>
#include <stdexcept>
/*
    Important: 
    When delegating from implementation function to another, be careful: example:
        XXX_OnTick() { return YYY_OnTick(); } // Wrong
    Here, inside YYY_OnTick, have no idea where we came from, and there is no way to know what current state is for sure.
    If you want to reuse implementation, then refactor common behaviour into another function:
        void commonLogic() {};
        XXX_OnTick() { commnLogic(); }    // Correct, we know current state is XXX
        YYY_OnTick() { commonLogic(); }   // Correct, we know current state is YYY
    Now, there is no ambiguity about what state droid is in.

*/
static std::unordered_map<int, void*> droidStorage;
// Check if a droid has stopped moving
#define DROID_STOPPED(psDroid) \
	(psDroid->sMove.Status == MOVEINACTIVE || psDroid->sMove.Status == MOVEHOVER || \
	 psDroid->sMove.Status == MOVESHUFFLE)
#define UID_DECL()  static const uint32_t UID = 0x000010000 + __COUNTER__;\
                    uint32_t getUID() const { return UID;}
namespace FSM
{

    const Move MoveInstance;
    const MoveNone MoveNoneInstance;
    const Attack AttackInstance;
    const AttackNone AttackNoneInstance;
    const MoveWaitDuringRepair MoveWaitDuringRepairInstance;
    const MoveWaitForRepair MoveWaitForRepairInstance;
    const MoveGuard MoveGuardInstance;
    const MoveTransportWaitToFlyIn MoveTransportWaitToFlyInInstance;
    // see if we can attack something & set targets for each weapon

    void alignWeapons(DROID& droid)
    {
        for (unsigned i = 0; i < psDroid->numWeaps; ++i)
        {
            if (psDroid->asWeaps[i].rot.direction != 0 || psDroid->asWeaps[i].rot.pitch != 0)
            {
                actionAlignTurret(psDroid, i);
            }
        }
    }
    void setTargetsToNull(DROID& droid)
    {
        DROID *psDroid = &droid;
        for (int i = 0; i < psDroid->numWeaps; i++)
        {
            setDroidActionTarget(psDroid, nullptr, i);
        }
    }



    // ========================================================================================================================
    // ================= ATTACK related behavior ==============================================================================
    // ========================================================================================================================
    void AttackNone_OnEntry(DROID& droid)
    {
        setTargetsToNull(droid);
    }
	const GroundAttackBase& AttackNone_OnTick(DROID& droid)
    {
        // why this check?
        if (order->type == DORDER_NONE || order->type == DORDER_HOLD || order->type == DORDER_RTR || order->type == DORDER_GUARD)
        {
            const bool attack = anythingToShoot(droid);

            if (attack) return AttackInstance;
            return AttackNoneInstance;
        }

    }
	void AttackNone_OnExit(DROID& droid) {};

	void AttackReturnToPos_OnEntry(DROID& droid)
    {

    }
	const MoveBase& AttackReturnToPos_OnTick(DROID& droid)
    {
        const bool attack = anythingToShoot(droid);
        if (attack) return AttackInstance;
        return AttackNoneInstance;
    }

    void AttackFireSupportRetreat_OnEntry(DROID&) {};
	const MoveBase& AttackFireSupportRetreat_OnTick(DROID& droid)
    {
        const bool attack = anythingToShoot(droid);
        if (attack) return AttackInstance;
        return AttackNoneInstance;
    }
	void AttackFireSupportRetreat_OnExit(DROID&) {};

	void AttackReturnToPos_OnExit(DROID& droid) {};


    // ========================================================================================================================
    // ================= MOVE related behavior ================================================================================
    // ========================================================================================================================
    void MoveReturnToPos_OnEntry(DROID&);
	const MoveBase& MoveReturnToPos_OnTick(DROID& droid)
    {
        		// moving to a location
		if (DROID_STOPPED(psDroid))
		{
			return MoveNoneInstance;
		}
    }
	void MoveReturnToPos_OnExit(DROID&);
	void Move_OnEntry(DROID& droid)
    { }

	void MoveTransportWaitToFlyIn_OnEntry(DROID&) {};
	const MoveBase& MoveTransportWaitToFlyIn_OnTick(DROID& droid)
    {
        DROID* psDroid = &droid;
		//if we're moving droids to safety and currently waiting to fly back in, see if time is up
		if (psDroid->player == selectedPlayer && getDroidsToSafetyFlag())
		{
			bool enoughTimeRemaining = (mission.time - (gameTime - mission.startTime)) >= (60 * GAME_TICKS_PER_SEC);
			if (((SDWORD)(mission.ETA - (gameTime - missionGetReinforcementTime())) <= 0) && enoughTimeRemaining)
			{
				UDWORD droidX, droidY;

				if (!droidRemove(psDroid, mission.apsDroidLists))
				{
					ASSERT_OR_RETURN(, false, "Unable to remove transporter from mission list");
				}
				addDroid(psDroid, apsDroidLists);
				//set the x/y up since they were set to INVALID_XY when moved offWorld
				missionGetTransporterExit(selectedPlayer, &droidX, &droidY);
				psDroid->pos.x = droidX;
				psDroid->pos.y = droidY;
				//fly Transporter back to get some more droids
				orderDroidLoc(psDroid, DORDER_TRANSPORTIN,
				              getLandingX(selectedPlayer), getLandingY(selectedPlayer), ModeImmediate);
			}
		}
        return MoveTransportWaitToFlyIn;
    }
	void MoveTransportWaitToFlyIn_OnExit(DROID&) {};

	const MoveBase& Move_OnTick(DROID& droid)
    {
        DROID *psDroid = &droid;
        // moving to a location
		if (DROID_STOPPED(psDroid))
		{
			/* notify scripts we have reached the destination
			*  also triggers when patrolling and reached a waypoint
			*/
            triggerEventDroidIdle(psDroid);
            return MoveNoneInstance;
		}
		//added multiple weapon check
		else if (psDroid->numWeaps > 0)
		{
			for (unsigned i = 0; i < psDroid->numWeaps; ++i)
			{
				if (psDroid->asWeaps[i].nStat > 0)
				{
					BASE_OBJECT *psTemp = nullptr;

					//I moved psWeapStats flag update there
					WEAPON_STATS *const psWeapStats = &asWeaponStats[psDroid->asWeaps[i].nStat];
					if (!isVtolDroid(psDroid)
					    && psDroid->asWeaps[i].nStat > 0
					    && psWeapStats->rotate
					    && psWeapStats->fireOnMove
					    && aiBestNearestTarget(psDroid, &psTemp, i) >= 0)
					{
						if (secondaryGetState(psDroid, DSO_ATTACK_LEVEL) == DSS_ALEV_ALWAYS)
						{
							//psDroid->action = DACTION_MOVEFIRE;
							setDroidActionTarget(psDroid, psTemp, i);
						}
					}
				}
			}
		}
        return MoveInstance;
    }
	void Move_OnExit(DROID&);

    void MoveNone_OnEntry(DROID& droid)
    {
        DROID *psDroid = &droid;
        // Clear up what ever the droid was doing before if necessary
        if (!DROID_STOPPED(psDroid))
        {
            moveStopDroid(psDroid);
        }
        // psDroid->action = DACTION_NONE; // TODO remove that
        psDroid->actionPos = Vector2i(0, 0);
        psDroid->actionStarted = 0;
        psDroid->actionPoints = 0;
    }
	const MoveBase& MoveNone_OnTick(DROID& droid)
    {
        return MoveNoneInstance;
    };
	void MoveNone_OnExit(DROID& droid) {}
   
    void MoveWaitForRepair_OnEntry(DROID& droid)
    {
        return MoveNone_OnEntry(droid);
    }
	const MoveBase& MoveWaitForRepair_OnTick(DROID& droid)
    {
        return MoveNone_OnTick(droid);
    }
	void MoveWaitForRepair_OnExit(DROID& droid)
    {
        return MoveNone_OnExit(droid);
    }

   	void MoveWaitDuringRepair_OnEntry(DROID& droid) {}
	const MoveBase& MoveWaitDuringRepair_OnTick(DROID& droid)
    {
        DROID *psDroid = &droid;
        DROID_ORDER_DATA *order = &psDroid->order;
        // Check that repair facility still exists
        const bool targetDied = !order->psObj;
        if (order->type == DORDER_RTR && order->rtrType == RTR_TYPE_REPAIR_FACILITY)
        {
            // move back to the repair facility if necessary
            if (DROID_STOPPED(psDroid) &&
                !actionReachedBuildPos(psDroid,
                                    order->psObj->pos.x, order->psObj->pos.y, ((STRUCTURE *)order->psObj)->rot.direction,
                                    ((STRUCTURE *)order->psObj)->pStructureType))
            {
                moveDroidToNoFormation(psDroid, order->psObj->pos.x, order->psObj->pos.y);
            }
        }
        else if (order->type == DORDER_RTR && order->rtrType == RTR_TYPE_DROID && DROID_STOPPED(psDroid)) {
            if (!actionReachedDroid(psDroid, static_cast<DROID *> (order->psObj)))
            {
                moveDroidToNoFormation(psDroid, order->psObj->pos.x, order->psObj->pos.y);
            } else {
                moveStopDroid(psDroid);
            }
        }
        if (targetDied) return MoveNoneInstance;
        return MoveWaitDuringRepairInstance;
    }
	void MoveWaitDuringRepair_OnExit(DROID& droid) {};


	class GroundWeaponDroidFSM
	{
		public: 
		static const GroundWeaponDroidFSM& instance() { static const GroundWeaponDroidFSM INSTANCE; return INSTANCE; }
        MoveBase& getCurrentMoveState(uint32_t droidId)
        {
             // handle errors
            return move_states[droidId];
        }
        GroundAttackBase& getCurrentAttackState(uint32_t droidId)
        {
            // handle errors
            return attack_states[droidId];
        }
        private:
		std::unordered_map<uint32_t, const GroundAttackBase&> attack_states;
		std::unordered_map<uint32_t, const MoveBase&> move_states;
	};
}