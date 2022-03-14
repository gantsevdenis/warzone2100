#include "../basedef.h"
#include "../action.h"
#include "../droid.h"
#include "../order.h"
#include "../move.h"
#include "../ai.h"
#include "../qtscript.h"
#include "fsm.h"
namespace FSM
{
    // parameterize target acquisition?
    class BuilderBase { };
    class MoveNone: public BuilderBase
    {

    };
    void MoveNone_OnEntry(DROID& droid)
    {
        clearMovement(droid);
    }
	const MoveBase& MoveNone_OnTick(DROID& droid)
    {
        // nothing to do? look for help his colleagues or build derricks
        return MoveNoneInstance;
    };
	void MoveNone_OnExit(DROID& droid) {}
    void Build_OnEntry(DROID& droid)
    {
        DROID* psDroid = &droid;
        DROID_ORDER_DATA *order = &psDroid->order;
        if (!order->psStats)
		{
			return MoveNoneInstance;
		}
		//ASSERT_OR_RETURN(, order->type == DORDER_BUILD || order->type == DORDER_HELPBUILD || order->type == DORDER_LINEBUILD, "cannot start build action without a build order");
		ASSERT_OR_RETURN(, psAction->x > 0 && psAction->y > 0, "Bad build order position");
		//psDroid->action = DACTION_MOVETOBUILD;
		psDroid->actionPos.x = psAction->x;
		psDroid->actionPos.y = psAction->y;
		moveDroidToNoFormation(psDroid, psDroid->actionPos.x, psDroid->actionPos.y);
        return MoveToBuildInstance;
    }
    void Build_OnTick(DROID& droid)
    {
        DROID* psDroid = &droid;
        DROID_ORDER_DATA *order = &psDroid->order;
        bool actionNone = false;
        if (!order->psStats)
		{
			objTrace(psDroid->id, "No target stats for build order - resetting");
			//psDroid->action = DACTION_NONE;
            actionNone = true;
			break;
		}
		if (DROID_STOPPED(psDroid) &&
		    !actionReachedBuildPos(psDroid, psDroid->actionPos.x, psDroid->actionPos.y, order->direction, order->psStats))
		{
			objTrace(psDroid->id, "DACTION_BUILD: Starting to drive toward construction site");
			moveDroidToNoFormation(psDroid, psDroid->actionPos.x, psDroid->actionPos.y);
		}
		else if (!DROID_STOPPED(psDroid) &&
		         psDroid->sMove.Status != MOVETURNTOTARGET &&
		         psDroid->sMove.Status != MOVESHUFFLE &&
		         actionReachedBuildPos(psDroid, psDroid->actionPos.x, psDroid->actionPos.y, order->direction, order->psStats))
		{
			objTrace(psDroid->id, "DACTION_BUILD: Stopped - at construction site");
			moveStopDroid(psDroid);
		}
		if (psDroid->action == DACTION_SULK)
		{
			objTrace(psDroid->id, "Failed to go to objective, aborting build action");
			psDroid->action = DACTION_NONE;
			break;
		}
		if (droidUpdateBuild(psDroid))
		{
			actionTargetTurret(psDroid, psDroid->psActionTarget[0], &psDroid->asWeaps[0]);
		}
    }
    void handleDemolishRepairRestore(DROID & droid)
    {
        if (!order->psStats)
		{
			psDroid->action = DACTION_NONE;
			break;
		}
		// set up for the specific action
		switch (psDroid->action)
		{
		case DACTION_DEMOLISH:
			// DACTION_MOVETODEMOLISH;
			actionUpdateFunc = droidUpdateDemolishing;
			break;
		case DACTION_REPAIR:
			// DACTION_MOVETOREPAIR;
			actionUpdateFunc = droidUpdateRepair;
			break;
		case DACTION_RESTORE:
			// DACTION_MOVETORESTORE;
			actionUpdateFunc = droidUpdateRestore;
			break;
		default:
			break;
		}

		// now do the action update
		if (DROID_STOPPED(psDroid) && !actionReachedBuildPos(psDroid, psDroid->actionPos.x, psDroid->actionPos.y, ((STRUCTURE *)psDroid->psActionTarget[0])->rot.direction, order->psStats))
		{
			if (order->type != DORDER_HOLD && (!secHoldActive || (secHoldActive && order->type != DORDER_NONE)))
			{
				objTrace(psDroid->id, "Secondary order: Go to construction site");
				moveDroidToNoFormation(psDroid, psDroid->actionPos.x, psDroid->actionPos.y);
			}
			else
			{
				psDroid->action = DACTION_NONE;
			}
		}
		else if (!DROID_STOPPED(psDroid) &&
		         psDroid->sMove.Status != MOVETURNTOTARGET &&
		         psDroid->sMove.Status != MOVESHUFFLE &&
		         actionReachedBuildPos(psDroid, psDroid->actionPos.x, psDroid->actionPos.y, ((STRUCTURE *)psDroid->psActionTarget[0])->rot.direction, order->psStats))
		{
			objTrace(psDroid->id, "Stopped - reached build position");
			moveStopDroid(psDroid);
		}
		else if (actionUpdateFunc(psDroid))
		{
			//use 0 for non-combat(only 1 'weapon')
			actionTargetTurret(psDroid, psDroid->psActionTarget[0], &psDroid->asWeaps[0]);
		}
		else
		{
			psDroid->action = DACTION_NONE;
		}
		break;

    }
    void MoveToBuild_OnTick()
    {
        DROID* psDroid = &droid;
        DROID_ORDER_DATA *order = &psDroid->order;
        if (!order->psStats)
		{
			return MoveNone
		}
        // Determine if the droid can still build or help to build the ordered structure at the specified location
        const STRUCTURE_STATS* const desiredStructure = order->psStats;
        const STRUCTURE* const structureAtBuildPosition = getTileStructure(map_coord(psDroid->actionPos.x), map_coord(psDroid->actionPos.y));

        if (nullptr != structureAtBuildPosition)
        {
            bool droidCannotBuild = false;

            if (!aiCheckAlliances(structureAtBuildPosition->player, psDroid->player))
            {
                // Not our structure
                droidCannotBuild = true;
            }
            else
            // There's an allied structure already there.  Is it a wall, and can the droid upgrade it to a defence or gate?
            if (isWall(structureAtBuildPosition->pStructureType->type) &&
                (desiredStructure->type == REF_DEFENSE || desiredStructure->type == REF_GATE))
            {
                // It's always valid to upgrade a wall to a defence or gate
                droidCannotBuild = false; // Just to avoid an empty branch
            }
            else
            if ((structureAtBuildPosition->pStructureType != desiredStructure) && // ... it's not the exact same type as the droid was ordered to build
                (structureAtBuildPosition->pStructureType->type == REF_WALLCORNER && desiredStructure->type != REF_WALL)) // and not a wall corner when the droid wants to build a wall
            {
                // And so the droid can't build or help with building this structure
                droidCannotBuild = true;
            }
            else
            // So it's a structure that the droid could help to build, but is it already complete?
            if (structureAtBuildPosition->status == SS_BUILT &&
                (!IsStatExpansionModule(desiredStructure) || !canStructureHaveAModuleAdded(structureAtBuildPosition)))
            {
                // The building is complete and the droid hasn't been told to add a module, or can't add one, so can't help with that.
                droidCannotBuild = true;
            }

            if (droidCannotBuild)
            {
                if (order->type == DORDER_LINEBUILD && map_coord(psDroid->order.pos) != map_coord(psDroid->order.pos2))
                {
                    // The droid is doing a line build, and there's more to build. This will force the droid to move to the next structure in the line build
                    objTrace(psDroid->id, "DACTION_MOVETOBUILD: line target is already built, or can't be built - moving to next structure in line");
                    psDroid->action = DACTION_NONE;
                    return MoveNoneInstance;
                }
                else
                {
                    // Cancel the current build order. This will move the truck onto the next order, if it has one, or halt in place.
                    objTrace(psDroid->id, "DACTION_MOVETOBUILD: target is already built, or can't be built - executing next order or halting");
                    cancelBuild(psDroid);
                }

                return MoveToBuildInstance;
            }
        }

		// The droid can still build or help with a build, and is moving to a location to do so - are we there yet, are we there yet, are we there yet?
		if (!actionReachedBuildPos(psDroid, psDroid->actionPos.x, psDroid->actionPos.y, order->direction, order->psStats))
        {
            if (DROID_STOPPED(psDroid))
            {
                objTrace(psDroid->id, "DACTION_MOVETOBUILD: Starting to drive toward construction site - move status was %d", (int)psDroid->sMove.Status);
                moveDroidToNoFormation(psDroid, psDroid->actionPos.x, psDroid->actionPos.y);
            }
            // implicitely return current state: keep moving
            return MoveToBuildInstance;
        }

        // We're there, go ahead and build or help to build the structure
        bool buildPosEmpty = actionRemoveDroidsFromBuildPos(psDroid->player, psDroid->actionPos, order->direction, order->psStats);
        if (!buildPosEmpty)
        {
            return MoveToBuildInstance;
        }

        bool helpBuild = false;
        // Got to destination - start building
        STRUCTURE_STATS *const psStructStats = order->psStats;
        uint16_t dir = order->direction;
        moveStopDroid(psDroid);
        objTrace(psDroid->id, "Halted in our tracks - at construction site");
        if (order->type == DORDER_BUILD && order->psObj == nullptr)
        {
            // Starting a new structure
            const Vector2i pos = psDroid->actionPos;

            //need to check if something has already started building here?
            //unless its a module!
            if (IsStatExpansionModule(psStructStats))
            {
                syncDebug("Reached build target: module");
                debug(LOG_NEVER, "DACTION_MOVETOBUILD: setUpBuildModule");
                setUpBuildModule(psDroid);
            }
            else if (TileHasStructure(worldTile(pos)))
            {
                // structure on the build location - see if it is the same type
                STRUCTURE *const psStruct = getTileStructure(map_coord(pos.x), map_coord(pos.y));
                if (psStruct->pStructureType == order->psStats ||
                    (order->psStats->type == REF_WALL && psStruct->pStructureType->type == REF_WALLCORNER))
                {
                    // same type - do a help build
                    syncDebug("Reached build target: do-help");
                    setDroidTarget(psDroid, psStruct);
                    helpBuild = true;
                }
                else if ((psStruct->pStructureType->type == REF_WALL ||
                            psStruct->pStructureType->type == REF_WALLCORNER) &&
                            (order->psStats->type == REF_DEFENSE ||
                            order->psStats->type == REF_GATE))
                {
                    // building a gun tower or gate over a wall - OK
                    if (droidStartBuild(psDroid))
                    {
                        syncDebug("Reached build target: tower");
                        //psDroid->action = DACTION_BUILD;
                        reutrn BuildInstance;
                    }
                }
                else
                {
                    syncDebug("Reached build target: already-structure");
                    objTrace(psDroid->id, "DACTION_MOVETOBUILD: tile has structure already");
                    cancelBuild(psDroid);
                }
            }
            else if (!validLocation(order->psStats, pos, dir, psDroid->player, false))
            {
                syncDebug("Reached build target: invalid");
                objTrace(psDroid->id, "DACTION_MOVETOBUILD: !validLocation");
                cancelBuild(psDroid);
            }
            else if (droidStartBuild(psDroid) == DroidStartBuildSuccess)  // If DroidStartBuildPending, then there's a burning oil well, and we don't want to change to DACTION_BUILD until it stops burning.
            {
                syncDebug("Reached build target: build");
                psDroid->actionStarted = gameTime;
                psDroid->actionPoints = 0;
                //psDroid->action = DACTION_BUILD;
                reutrn BuildInstance;
            }
        }
        else if (order->type == DORDER_LINEBUILD || order->type == DORDER_BUILD)
        {
            // building a wall.
            MAPTILE *const psTile = worldTile(psDroid->actionPos);
            syncDebug("Reached build target: wall");
            if (order->psObj == nullptr
                && (TileHasStructure(psTile)
                    || TileHasFeature(psTile)))
            {
                if (TileHasStructure(psTile))
                {
                    // structure on the build location - see if it is the same type
                    STRUCTURE *const psStruct = getTileStructure(map_coord(psDroid->actionPos.x), map_coord(psDroid->actionPos.y));
                    ASSERT(psStruct, "TileHasStructure, but getTileStructure returned nullptr");
#if defined(WZ_CC_GNU) && !defined(WZ_CC_INTEL) && !defined(WZ_CC_CLANG) && (7 <= __GNUC__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wnull-dereference"
#endif
                    if (psStruct->pStructureType == order->psStats)
                    {
                        // same type - do a help build
                        setDroidTarget(psDroid, psStruct);
                        helpBuild = true;
                    }
                    else if ((psStruct->pStructureType->type == REF_WALL || psStruct->pStructureType->type == REF_WALLCORNER) &&
                                (order->psStats->type == REF_DEFENSE || order->psStats->type == REF_GATE))
                    {
                        // building a gun tower over a wall - OK
                        if (droidStartBuild(psDroid))
                        {
                            objTrace(psDroid->id, "DACTION_MOVETOBUILD: start building defense");
                            psDroid->action = DACTION_BUILD;
                        }
                    }
                    else if ((psStruct->pStructureType->type == REF_FACTORY && order->psStats->type == REF_FACTORY_MODULE) ||
                            (psStruct->pStructureType->type == REF_RESEARCH && order->psStats->type == REF_RESEARCH_MODULE) ||
                            (psStruct->pStructureType->type == REF_POWER_GEN && order->psStats->type == REF_POWER_MODULE) ||
                            (psStruct->pStructureType->type == REF_VTOL_FACTORY && order->psStats->type == REF_FACTORY_MODULE))
                        {
                        // upgrade current structure in a row
                        if (droidStartBuild(psDroid))
                        {
                            objTrace(psDroid->id, "DACTION_MOVETOBUILD: start building module");
                            psDroid->action = DACTION_BUILD;
                        }
                    }
                    else
                    {
                        objTrace(psDroid->id, "DACTION_MOVETOBUILD: line build hit building");
                        cancelBuild(psDroid);
                    }
#if defined(WZ_CC_GNU) && !defined(WZ_CC_INTEL) && !defined(WZ_CC_CLANG) && (7 <= __GNUC__)
# pragma GCC diagnostic pop
#endif
                }
                else if (TileHasFeature(psTile))
                {
                    FEATURE *feature = getTileFeature(map_coord(psDroid->actionPos.x), map_coord(psDroid->actionPos.y));
                    objTrace(psDroid->id, "DACTION_MOVETOBUILD: tile has feature %d", feature->psStats->subType);
                    if (feature->psStats->subType == FEAT_OIL_RESOURCE && order->psStats->type == REF_RESOURCE_EXTRACTOR)
                    {
                        if (droidStartBuild(psDroid))
                        {
                            objTrace(psDroid->id, "DACTION_MOVETOBUILD: start building oil derrick");
                            psDroid->action = DACTION_BUILD;
                        }
                    }
                }
                else
                {
                    objTrace(psDroid->id, "DACTION_MOVETOBUILD: blocked line build");
                    cancelBuild(psDroid);
                }
            }
            else if (droidStartBuild(psDroid))
            {
                psDroid->action = DACTION_BUILD;
            }
        }
        else
        {
            syncDebug("Reached build target: planned-help");
            objTrace(psDroid->id, "DACTION_MOVETOBUILD: planned-help");
            helpBuild = true;
        }

        if (helpBuild)
        {
            // continuing a partially built structure (order = helpBuild)
            if (droidStartBuild(psDroid))
            {
                objTrace(psDroid->id, "DACTION_MOVETOBUILD: starting help build");
                psDroid->action = DACTION_BUILD;
            }
        }

    }
    void Demolish_OnEntry(DROID& droid)
    {
        ASSERT_OR_RETURN(, order->type == DORDER_DEMOLISH, "cannot start demolish action without a demolish order");
		//psDroid->action = DACTION_MOVETODEMOLISH;
		psDroid->actionPos.x = psAction->x;
		psDroid->actionPos.y = psAction->y;
		ASSERT_OR_RETURN(, (order->psObj != nullptr) && (order->psObj->type == OBJ_STRUCTURE), "invalid target for demolish order");
		order->psStats = ((STRUCTURE *)order->psObj)->pStructureType;
		setDroidActionTarget(psDroid, psAction->psObj, 0);
		moveDroidTo(psDroid, psAction->x, psAction->y);
    }

    void Demolish_OnTick(DROID& droid)
    {
        DROID* psDroid = &droid;
        DROID_ORDER_DATA *order = &psDroid->order;
        if (!order->psStats)
		{
			psDroid->action = DACTION_NONE;
			break;
		}

		// now do the action update
		if (DROID_STOPPED(psDroid) && !actionReachedBuildPos(psDroid, psDroid->actionPos.x, psDroid->actionPos.y, ((STRUCTURE *)psDroid->psActionTarget[0])->rot.direction, order->psStats))
		{
			if (order->type != DORDER_HOLD && (!secHoldActive || (secHoldActive && order->type != DORDER_NONE)))
			{
				objTrace(psDroid->id, "Secondary order: Go to construction site");
				moveDroidToNoFormation(psDroid, psDroid->actionPos.x, psDroid->actionPos.y);
			}
			else
			{
				psDroid->action = DACTION_NONE;
			}
		}
		else if (!DROID_STOPPED(psDroid) &&
		         psDroid->sMove.Status != MOVETURNTOTARGET &&
		         psDroid->sMove.Status != MOVESHUFFLE &&
		         actionReachedBuildPos(psDroid, psDroid->actionPos.x, psDroid->actionPos.y, ((STRUCTURE *)psDroid->psActionTarget[0])->rot.direction, order->psStats))
		{
			objTrace(psDroid->id, "Stopped - reached build position");
			moveStopDroid(psDroid);
		}
		else if (droidUpdateDemolishing(psDroid))
		{
			actionTargetTurret(psDroid, psDroid->psActionTarget[0], &psDroid->asWeaps[0]);
		}
		else
		{
			psDroid->action = DACTION_NONE;
		}
    }

    void MoveToDemolish_OnTick(DROID& droid)
    {
        return handleDemolishRepairRestore(droid);
    }
    void MoveToRepair_OnTick(DROID &droid)
    {
        return handleDemolishRepairRestore(droid);
    }
    void MoveToRestore_OnTick(DROID& droid)
    {
        return handleAbandonedStructures(droid);
    }
}