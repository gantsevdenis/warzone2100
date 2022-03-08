#include "../basedef.h"

void GroundMoveFire(DROID &droid)
{
		// loop through weapons and look for target for each weapon
		bHasTarget = false;
		for (unsigned i = 0; i < psDroid->numWeaps; ++i)
		{
			bDirect = proj_Direct(asWeaponStats + psDroid->asWeaps[i].nStat);
			blockingWall = nullptr;
			// Does this weapon have a target?
			if (psDroid->psActionTarget[i] != nullptr)
			{
				// Is target worth shooting yet?
				if (aiObjectIsProbablyDoomed(psDroid->psActionTarget[i], bDirect))
				{
					setDroidActionTarget(psDroid, nullptr, i);
				}
				// Is target from our team now? (Electronic Warfare)
				else if (electronicDroid(psDroid) && psDroid->player == psDroid->psActionTarget[i]->player)
				{
					setDroidActionTarget(psDroid, nullptr, i);
				}
				// Is target blocked by a wall?
				else if (bDirect && visGetBlockingWall(psDroid, psDroid->psActionTarget[i]))
				{
					setDroidActionTarget(psDroid, nullptr, i);
				}
				// I have a target!
				else
				{
					bHasTarget = true;
				}
			}
			// This weapon doesn't have a target
			else
			{
				// Can we find a good target for the weapon?
				BASE_OBJECT *psTemp;
				if (aiBestNearestTarget(psDroid, &psTemp, i) >= 0) // assuming aiBestNearestTarget checks for electronic warfare
				{
					bHasTarget = true;
					setDroidActionTarget(psDroid, psTemp, i); // this updates psDroid->psActionTarget[i] to != NULL
				}
			}
			// If we have a target for the weapon: is it visible?
			if (psDroid->psActionTarget[i] != nullptr && visibleObject(psDroid, psDroid->psActionTarget[i], false) > UBYTE_MAX / 2)
			{
				hasVisibleTarget = true; // droid have a visible target to shoot
				targetVisibile[i] = true;// it is at least visible for this weapon
			}
		}
		// if there is at least one target
		if (bHasTarget)
		{
			// loop through weapons
			for (unsigned i = 0; i < psDroid->numWeaps; ++i)
			{
				const unsigned compIndex = psDroid->asWeaps[i].nStat;
				const WEAPON_STATS *psStats = asWeaponStats + compIndex;
				wallBlocked = false;

				// has weapon a target? is target valid?
				if (psDroid->psActionTarget[i] != nullptr && validTarget(psDroid, psDroid->psActionTarget[i], i))
				{
					// is target visible and weapon is not a Nullweapon?
					if (targetVisibile[i] && nonNullWeapon[i]) //to fix a AA-weapon attack ground unit exploit
					{
						BASE_OBJECT *psActionTarget = nullptr;
						blockingWall = visGetBlockingWall(psDroid, psDroid->psActionTarget[i]);

						if (proj_Direct(psStats) && blockingWall)
						{
							WEAPON_EFFECT weapEffect = psStats->weaponEffect;

							if (!aiCheckAlliances(psDroid->player, blockingWall->player)
								&& asStructStrengthModifier[weapEffect][blockingWall->pStructureType->strength] >= MIN_STRUCTURE_BLOCK_STRENGTH)
							{
								psActionTarget = blockingWall;
								setDroidActionTarget(psDroid, psActionTarget, i); // attack enemy wall
							}
							else
							{
								wallBlocked = true;
							}
						}
						else
						{
							psActionTarget = psDroid->psActionTarget[i];
						}

						// is the turret aligned with the target?
						if (!wallBlocked && actionTargetTurret(psDroid, psActionTarget, &psDroid->asWeaps[i]))
						{
							// In range - fire !!!
							combFire(&psDroid->asWeaps[i], psDroid, psActionTarget, i);
						}
					}
				}
			}
			// Droid don't have a visible target and it is not in pursue mode
			if (!hasVisibleTarget && secondaryGetState(psDroid, DSO_ATTACK_LEVEL) != DSS_ALEV_ALWAYS)
			{
				// Target lost
				psDroid->action = DACTION_MOVE;
			}
		}
		// it don't have a target, change to DACTION_MOVE
		else
		{
			psDroid->action = DACTION_MOVE;
		}
		//check its a VTOL unit since adding Transporter's into multiPlayer
		/* check vtol attack runs */
		if (isVtolDroid(psDroid))
		{
			actionUpdateVtolAttack(psDroid);
		}
}