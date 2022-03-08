#include "../basedef.h"

void attack(DROID &droid)
{
    if (psDroid->psActionTarget[0] == nullptr &&  psDroid->psActionTarget[1] != nullptr)
    {
        break;
    }
    ASSERT_OR_RETURN(, psDroid->psActionTarget[0] != nullptr, "target is NULL while attacking");

    //check the target hasn't become one the same player ID - Electronic Warfare
    if (electronicDroid(psDroid) && psDroid->player == psDroid->psActionTarget[0]->player)
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
    for (unsigned i = 0; i < psDroid->numWeaps; ++i)
    {
        BASE_OBJECT *psActionTarget;

        if (i > 0)
        {
            // If we're ordered to shoot something, and we can, shoot it
            if ((order->type == DORDER_ATTACK || order->type == DORDER_ATTACKTARGET) &&
                psDroid->psActionTarget[i] != psDroid->psActionTarget[0] &&
                validTarget(psDroid, psDroid->psActionTarget[0], i) &&
                actionInRange(psDroid, psDroid->psActionTarget[0], i))
            {
                setDroidActionTarget(psDroid, psDroid->psActionTarget[0], i);
            }
            // If we still don't have a target, try to find one
            else
            {
                if (psDroid->psActionTarget[i] == nullptr &&
                    aiChooseTarget(psDroid, &psTargets[i], i, false, nullptr))  // Can probably just use psTarget instead of psTargets[i], and delete the psTargets variable.
                {
                    setDroidActionTarget(psDroid, psTargets[i], i);
                }
            }
        }

        if (psDroid->psActionTarget[i])
        {
            psActionTarget = psDroid->psActionTarget[i];
        }
        else
        {
            psActionTarget = psDroid->psActionTarget[0];
        }

        if (nonNullWeapon[i]
            && actionVisibleTarget(psDroid, psActionTarget, i)
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

            if (validTarget(psDroid, psActionTarget, i) && !wallBlocked)
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
                        if (psDroid->psActionTarget[i] != psDroid->psActionTarget[0])
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

    if (!bHasTarget || wallBlocked)
    {
        BASE_OBJECT *psTarget;
        bool supportsSensorTower = !isVtolDroid(psDroid) && (psTarget = orderStateObj(psDroid, DORDER_FIRESUPPORT)) && psTarget->type == OBJ_STRUCTURE;

        if (secHoldActive && (order->type == DORDER_ATTACKTARGET || order->type == DORDER_FIRESUPPORT))
        {
            psDroid->action = DACTION_NONE; // secondary holding, cancel the order.
        }
        else if (secondaryGetState(psDroid, DSO_HALTTYPE) == DSS_HALT_PURSUE &&
            !supportsSensorTower &&
            !(order->type == DORDER_HOLD ||
            order->type == DORDER_RTR))
        {
            //We need this so pursuing doesn't stop if a unit is ordered to move somewhere while
            //it is still in weapon range of the target when reaching the end destination.
            //Weird case, I know, but keeps the previous pursue order intact.
            psDroid->action = DACTION_MOVETOATTACK;	// out of range - chase it
        }
        else if (supportsSensorTower ||
            order->type == DORDER_NONE ||
            order->type == DORDER_HOLD ||
            order->type == DORDER_RTR)
        {
            // don't move if on hold or firesupport for a sensor tower
            // also don't move if we're holding position or waiting for repair
            psDroid->action = DACTION_NONE; // holding, cancel the order.
        }
        //Units attached to commanders are always guarding the commander
        else if (secHoldActive && order->type == DORDER_GUARD && hasCommander(psDroid))
        {
            DROID *commander = psDroid->psGroup->psCommander;

            if (commander->order.type == DORDER_ATTACKTARGET ||
                commander->order.type == DORDER_FIRESUPPORT ||
                commander->order.type == DORDER_ATTACK)
            {
                psDroid->action = DACTION_MOVETOATTACK;
            }
            else
            {
                psDroid->action = DACTION_NONE;
            }
        }
        else if (secondaryGetState(psDroid, DSO_HALTTYPE) != DSS_HALT_HOLD)
        {
            psDroid->action = DACTION_MOVETOATTACK;	// out of range - chase it
        }
        else
        {
            psDroid->order.psObj = nullptr;
            psDroid->action = DACTION_NONE;
        }
    }
}