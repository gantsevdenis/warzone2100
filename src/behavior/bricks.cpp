/*
Finite, basic "bricks" for behavior
These actions always pop back to whatever state was before */
#include "fsm.h"
#include "../action.h"
#include "../droid.h"
#include "../order.h"
#include "../move.h"
#include "../ai.h"
#include "../qtscript.h"
namespace FSM
{
    std::unordered_map<uint32_t, Vector2i> targetPositions;
    // ?? how should I handle multiple targets, one for each weapon ?
    // - when target is designated, then all weapon should try to shot at it
    // - when droid is freely choosing targets, then it should lock one valid
    //  target per weapon
    // ?? what if a target is designated, but we have 1 weapon which is invalid ?
    // - shoot with valid weapon, but leave the other one in "target-looking" state
    // ?? how is handled mix of direct & indirect weapons ?
    // - indirect weapons cannot rotate, so the whole droid must rotate to fire
    //   this is known immediately when droid is built: so we just set a bool to true
    //   to indicate that droids need rotation (important for Move-* states)
    // - while rotating, available weapons should fire
    // - when finished rotating, indirect weapon should fire
    // ?? how are handled walls ?
    // - when target is given, but is obstructed by wall, everything should happen 
    //   as if the wall was the real designated target, and original target gets
    //   saved somewhere
    // - once wall is destroyed, I should pop original target, and continue as usual
    std::unordered_map<uint32_t, BASE_OBJECT*> targetVictim;
    std::unordered_map<uint32_t, MoveBase&> returnMoveState;
    std::unordered_map<uint32_t, GroundAttackBase&> returnAttackState;
    // returnBuilderState? SensorState?...
    // il faudrait surement unifier tous ceux la sous la meme hiérarchie

    void MoveGotoPosition_OnEntry(DROID& droid)
    {
        DROID* psDroid = &droid;
        //psDroid->action = psAction->action;
		psDroid->actionPos.x = psAction->x;
		psDroid->actionPos.y = psAction->y;
		psDroid->actionStarted = gameTime;
		//setDroidActionTarget(psDroid, psAction->psObj, 0);
		bool thereIsPath = moveDroidTo(psDroid, psAction->x, psAction->y);
        if (!thereIsPath)
        {
            // can't go there!
            return returnMoveState[droid.id];
        }
    };
    MoveBase& MoveGotoPosition_OnTick(DROID& droid)
    {
        // src/move.cpp sets MOVEINACTIVE status
		if (DROID_STOPPED(psDroid))
		{
            /* notify scripts we have reached the destination
            *  also triggers when patrolling and reached a waypoint
            */
            triggerEventDroidIdle(psDroid);
            return returnMoveState[droid.id];
        }
        // FIXME return current state 
        return returnMoveState[droid.id];
    };
    void MoveGotoPosition_OnExit(DROID& droid){};

    void AttackShootTargetDesignated_OnEntry(DROID&){};
    GroundAttackBase& AttackShootTargetDesignated_OnTick(DROID& droid)
    {
        DROID* psDroid = &droid;
        BASE_OBJECT* targetObject = targetVictim[droid.id];
        //check the target hasn't become one the same player ID - Electronic Warfare
        if (electronicDroid(psDroid) && psDroid->player == targetObject->player)
        {
            for (unsigned i = 0; i < psDroid->numWeaps; ++i)
            {
                setDroidActionTarget(psDroid, nullptr, i);
            }
            psDroid->action = DACTION_NONE;
            break;
        }
        bHasTarget = false;
        wallBlocked = false;
        for (unsigned i = 0; i < psDroid->numWeaps && nonNullWeapon[i]; ++i)
        {
            BASE_OBJECT *psActionTarget = targetObject;
            if (!validTarget(psDroid, targetObject, i))
                continue;

            if (i > 0)
            {
                // If we're ordered to shoot something, and we can, shoot it
                if (psDroid->psActionTarget[i] != targetObject &&
                    actionInRange(psDroid, targetObject, i))
                {
                    setDroidActionTarget(psDroid, targetObject, i);
                }
            if (actionVisibleTarget(psDroid, psActionTarget, i)
                && actionInRange(psDroid, psActionTarget, i))
            {
                WEAPON_STATS *const psWeapStats = &asWeaponStats[psDroid->asWeaps[i].nStat];
                WEAPON_EFFECT weapEffect = psWeapStats->weaponEffect;
                blockingWall = visGetBlockingWall(psDroid, psActionTarget);

                // if a wall is inbetween us and the target, try firing at the wall if our
                // weapon is good enough
                if (proj_Direct(psWeapStats) && blockingWall)
                {
                    if (!aiCheckAlliances(psDroid->player, blockingWall->player)
                        && asStructStrengthModifier[weapEffect][blockingWall->pStructureType->strength] >= MIN_STRUCTURE_BLOCK_STRENGTH)
                    {
                        psActionTarget = (BASE_OBJECT *)blockingWall;
                        setDroidActionTarget(psDroid, psActionTarget, i);
                    }
                    else
                    {
                        wallBlocked = true;
                    }
                }

                if (!bHasTarget)
                {
                    bHasTarget = actionInRange(psDroid, psActionTarget, i, false);
                }

                if (!wallBlocked)
                {
                    int dirDiff = 0;

                    if (!psWeapStats->rotate)
                    {
                        // no rotating turret - need to check aligned with target
                        const uint16_t targetDir = calcDirection(psDroid->pos.x, psDroid->pos.y, psActionTarget->pos.x, psActionTarget->pos.y);
                        dirDiff = abs(angleDelta(targetDir - psDroid->rot.direction));
                    }

                    if (dirDiff > FIXED_TURRET_DIR)
                    {
                        if (i > 0)
                        {
                            if (psDroid->psActionTarget[i] != targetObject)
                            {
                                // Nope, can't shoot this, try something else next time
                                setDroidActionTarget(psDroid, nullptr, i);
                            }
                        }
                        else if (psDroid->sMove.Status != MOVESHUFFLE)
                        {
                            psDroid->action = DACTION_ROTATETOATTACK;
                            moveTurnDroid(psDroid, psActionTarget->pos.x, psActionTarget->pos.y);
                        }
                    }
                    else if (!psWeapStats->rotate ||
                                actionTargetTurret(psDroid, psActionTarget, &psDroid->asWeaps[i]))
                    {
                        /* In range - fire !!! */
                        combFire(&psDroid->asWeaps[i], psDroid, psActionTarget, i);
                    }
                }
                else if (i > 0)
                {
                    // Nope, can't shoot this, try something else next time
                    setDroidActionTarget(psDroid, nullptr, i);
                }
            }
            else if (i > 0)
            {
                // Nope, can't shoot this, try something else next time
                setDroidActionTarget(psDroid, nullptr, i);
            }
        }
    };
    void AttackShootTargetDesignated_OnExit(DROID& droid);


}