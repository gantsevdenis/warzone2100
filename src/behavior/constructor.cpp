#include "constructor.h"
#include "../action.h"
#include "../order.h"
#include "../droiddef.h"
#include "../droid.h"
#include "../visibility.h"
#include "../ai.h"
#include "../structure.h"
#include "../structuredef.h"
#include "../mapgrid.h"

namespace {

void cancelBuild(DROID *psDroid)
{
	if (psDroid->order.type == DORDER_NONE ||
	psDroid->order.type == DORDER_PATROL ||
	psDroid->order.type == DORDER_HOLD ||
	psDroid->order.type == DORDER_SCOUT ||
	psDroid->order.type == DORDER_GUARD)
	{
		objTrace(psDroid->id, "Droid build action cancelled");
		psDroid->order.psObj = nullptr;
		psDroid->action = DACTION_NONE;
		setDroidActionTarget(psDroid, nullptr, 0);
		return;  // Don't cancel orders.
	}


}

void droidAddWeldSound(Vector3i iVecEffect)
{
	int iAudioID = ID_SOUND_CONSTRUCTION_1 + (rand() % 4);

	audio_PlayStaticTrack(iVecEffect.x, iVecEffect.z, iAudioID);
}

void addConstructorEffect(STRUCTURE *psStruct)
{
	if ((ONEINTEN) && (psStruct->visibleForLocalDisplay()))
	{
		/* This needs fixing - it's an arse effect! */
		const Vector2i size = psStruct->size() * TILE_UNITS / 4;
		Vector3i temp;
		temp.x = psStruct->pos.x + ((rand() % (2 * size.x)) - size.x);
		temp.y = map_TileHeight(map_coord(psStruct->pos.x), map_coord(psStruct->pos.y)) + (psStruct->sDisplay.imd->max.y / 6);
		temp.z = psStruct->pos.y + ((rand() % (2 * size.y)) - size.y);
		if (rand() % 2)
		{
			droidAddWeldSound(temp);
		}
	}
}

/** Update a construction droid while it is building
    returns true while building continues */
bool droidUpdateBuild(DROID *psDroid)
{

}

bool droidUpdateDemolishing(DROID *psDroid)
{
	CHECK_DROID(psDroid);

	ASSERT_OR_RETURN(false, psDroid->action == DACTION_DEMOLISH, "unit is not demolishing");
	STRUCTURE *psStruct = (STRUCTURE *)psDroid->order.psObj;
	ASSERT_OR_RETURN(false, psStruct->type == OBJ_STRUCTURE, "target is not a structure");

	int constructRate = 5 * constructorPoints(asConstructStats + psDroid->asBits[COMP_CONSTRUCT], psDroid->player);
	int pointsToAdd = gameTimeAdjustedAverage(constructRate);

	structureDemolish(psStruct, psDroid, pointsToAdd);

	addConstructorEffect(psStruct);

	CHECK_DROID(psDroid);

	return true;
}

/** This function checks if there are any structures to repair or help build in a given radius near the droid defined by REPAIR_RANGE if it is on hold, and REPAIR_MAXDIST if not on hold.
 * It returns a damaged or incomplete structure if any was found or nullptr if none was found.
 */
STRUCTURE* checkForDamagedStruct(const DROID &droid)
{
	STRUCTURE *psFailedTarget = nullptr;
	DROID *psDroid = &droid;
	if (psDroid->action == DACTION_SULK)
	{
		psFailedTarget = (STRUCTURE *)psDroid->psActionTarget[0];
	}
	unsigned radius = ((psDroid->order.type == DORDER_HOLD) ||
						(psDroid->order.type == DORDER_NONE && secondaryGetState(psDroid, DSO_HALTTYPE) == DSS_HALT_HOLD)) ? REPAIR_RANGE : REPAIR_MAXDIST;
	unsigned bestDistanceSq = radius * radius;
	STRUCTURE *best = nullptr;
	for (BASE_OBJECT *object : gridStartIterate(psDroid->pos.x, psDroid->pos.y, radius))
	{
		unsigned distanceSq = droidSqDist(psDroid, object);  // droidSqDist returns -1 if unreachable, (unsigned)-1 is a big number.
		STRUCTURE *structure = castStructure(object);
		if (structure == nullptr ||  // Must be a structure.
		    structure == psFailedTarget ||  // Must not have just failed to reach it.
		    distanceSq > bestDistanceSq ||  // Must be as close as possible.
		    !visibleObject(psDroid, structure, false) ||  // Must be able to sense it.
		    !aiCheckAlliances(psDroid->player, structure->player) ||  // Must be a friendly structure.
		    checkDroidsDemolishing(structure))  // Must not be trying to get rid of it.
		{
			continue;
		}
		if ((structure->status == SS_BUILD && structIsDamaged(structure))
			|| structure->status == SS_BEING_BUILT)
			bestDistanceSq = distanceSq;
	}
	return best;
}

// ========== Every tick updates
// EventStack is always ignored, because it has already been handled
// somewhat equivalent of actionDroidUpdate
struct Updates
{
	static void update (DROID &droid, Intentions::None it, Activity::Idle ac, EventStack)
	{
		
	};
	static void update (DROID &droid, Intentions::MoveToBuild it, Activity::Building ac, EventStack)
	{
		unsigned constructPoints =constructorPoints(asConstructStats + psDroid->asBits[COMP_CONSTRUCT], psDroid->player);
		unsigned pointsToAdd = constructPoints * (gameTime - psDroid->actionStarted) / GAME_TICKS_PER_SEC;
		structureBuild(psStruct, psDroid, pointsToAdd - psDroid->actionPoints, constructPoints);
		//store the amount just added
		psDroid->actionPoints = pointsToAdd;
		addConstructorEffect(psStruct);
	}
	
	static void update (DROID &droid, Intentions::MoveToBuild it, Activity::MovingToStructure ac, EventStack) {};
	static void update (DROID &droid, Intentions::MoveToRecycle it, Activity::MovingToStructure ac, EventStack) {};
	static void update (DROID &droid, Intentions::MoveToPlace it, Activity::MovingToPlace ac, EventStack) {};
	static void update (DROID &droid, Intentions::MoveForRepairs it, Activity::MovingToStructure ac, EventStack) {};
	static void update (DROID &droid, Intentions::MoveForRepairs it, Activity::MovingToDroid ac, EventStack) {};
	static void update (DROID &droid, Intentions::MoveForRepairs it, Activity::BeingRepaired ac, EventStack) {};
	static void update (DROID &droid, Intentions::Patrol it, Activity::MovingToPlace ac, EventStack) {};
	
};

  // ============= Consumers
  // returns true if intention/state was changed
  bool on_event (DROID &droid, Intentions::None it, Activity::Idle ac, Events::HealthBelowThreashold event)
{
	auto where = decideWhereToRepairAndBalance(&droid);
	Activity::Type activity = Activity::Type::Idle;
	if (where.type == RTR_TYPE_REPAIR_FACILITY || where.type == RTR_TYPE_HQ)
	{
	//activity = new Activity::MovingToStructure{(STRUCTURE*)where.psObj};
		activity = Activity::Type::MovingToStructure;
	}
	else if (where.type == RTR_TYPE_DROID)
	{
	//	activity = new Activity::MovingToDroid {(DROID*)where.psObj};
		activity = Activity::Type::MovingToDroid;
	}
	else
	{
		debug(LOG_ERROR, "cannot happen");
		return;
	}
	droidSetActivity(droid, activity);
	droidPushIntention(droid, Intentions::Type::MoveForRepairs);
	Intentions::Data_MoveForRepairs data {where};
	Backstorage::Intentions1::move_for_repairs.at(droid.id).push (data);
}

// Try go to help building it
bool on_event (DROID &droid, Intentions::None it, Activity::Idle ac, Events::DamagedStructInVicinity event, STRUCTURE &structure)
{
	//STRUCTURE *target = Backstorage::Events1::damaged_struct_in_vicinity.at (droid.id).m_structure;
}

// Should we go to repairs?
bool on_event (DROID &droid, Intentions::MoveToBuild it, Activity::Building ac, Events::HealthBelowThreashold event){}

// Clean everything, return to previous intention
bool on_event (DROID &droid, Intentions::MoveToBuild it, Activity::Building ac, Events::TargetDied event)
{
	//droid.psActionTarget = nullptr;
	droidSetActivity(droid, Activity::Type::Idle);
	droidPopIntention(droid);
}

// Clean everything, return to previous intention
bool on_event (DROID &droid, Intentions::MoveToBuild it, Activity::Building ac, Events::BuildFinished event)
{
	// Check if line order build is completed, or we are not carrying out a line order build
	if (psDroid->order.type != DORDER_LINEBUILD || map_coord(psDroid->order.pos) == map_coord(psDroid->order.pos2))
	{
		// cancelBuild(psDroid);
		// goes to the droid's order list and sets a new order to it from its order list
		if (orderDroidList(psDroid))
		{
			objTrace(psDroid->id, "Droid build order cancelled - changing to next order");
			return false;
		}
		else
		{
			// nothing to do, well Idle i guess
			objTrace(psDroid->id, "Droid build order cancelled");
			droidPopIntention (droid);
			return true;
		}
	}
	
	//psDroid->action = DACTION_NONE;	// make us continue line build
	//setDroidTarget(psDroid, nullptr);
	//setDroidActionTarget(psDroid, nullptr, 0);
	droidPopIntention (droid);
	return true;
}

// Find a repair point, assign new psActiontarget, push new intention
bool on_event (DROID &droid, Intentions::MoveToBuild it, Activity::MovingToStructure ac, Events::HealthBelowThreashold event)
{
	debug (LOG_INFO, "health below threashold!");
}
bool on_event (DROID &droid, Intentions::MoveToBuild it, Activity::MovingToStructure ac, Events::ArrivedAtDestination event){}
bool on_event (DROID &droid, Intentions::MoveToBuild it, Activity::MovingToStructure ac, Events::NoRouteToPlace event){}
bool on_event (DROID &droid, Intentions::MoveToBuild it, Activity::MovingToStructure ac, Events::TargetDied event){}
bool on_event (DROID &droid, Intentions::MoveToBuild it, Activity::MovingToStructure ac, Events::Timeout event){}
bool on_event (DROID &droid, Intentions::MoveToRecycle it, Activity::MovingToStructure ac, Events::HealthBelowThreashold event){}
bool on_event (DROID &droid, Intentions::MoveToRecycle it, Activity::MovingToStructure ac, Events::ArrivedAtDestination event){}
bool on_event (DROID &droid, Intentions::MoveToRecycle it, Activity::MovingToStructure ac, Events::NoRouteToPlace event){}
bool on_event (DROID &droid, Intentions::MoveToRecycle it, Activity::MovingToStructure ac, Events::TargetDied event){}
bool on_event (DROID &droid, Intentions::MoveToRecycle it, Activity::MovingToStructure ac, Events::Timeout event){}
bool on_event (DROID &droid, Intentions::MoveToPlace it, Activity::MovingToPlace ac, Events::HealthBelowThreashold event){}
bool on_event (DROID &droid, Intentions::MoveToPlace it, Activity::MovingToPlace ac, Events::ArrivedAtDestination event){}
bool on_event (DROID &droid, Intentions::MoveToPlace it, Activity::MovingToPlace ac, Events::NoRouteToPlace event){}
bool on_event (DROID &droid, Intentions::MoveToPlace it, Activity::MovingToPlace ac, Events::Timeout event){}
bool on_event (DROID &droid, Intentions::MoveForRepairs it, Activity::MovingToStructure ac, Events::ArrivedAtDestination event){}
bool on_event (DROID &droid, Intentions::MoveForRepairs it, Activity::MovingToStructure ac, Events::NoRouteToPlace event){}
bool on_event (DROID &droid, Intentions::MoveForRepairs it, Activity::MovingToStructure ac, Events::TargetDied event){}
bool on_event (DROID &droid, Intentions::MoveForRepairs it, Activity::MovingToStructure ac, Events::Timeout event){}
bool on_event (DROID &droid, Intentions::MoveForRepairs it, Activity::MovingToStructure ac, Events::Repaired event){}
bool on_event (DROID &droid, Intentions::MoveForRepairs it, Activity::MovingToDroid ac, Events::Repaired event){}
bool on_event (DROID &droid, Intentions::MoveForRepairs it, Activity::MovingToDroid ac, Events::ArrivedAtDestination event){}
bool on_event (DROID &droid, Intentions::MoveForRepairs it, Activity::MovingToDroid ac, Events::NoRouteToTarget event){}
bool on_event (DROID &droid, Intentions::MoveForRepairs it, Activity::MovingToDroid ac, Events::TargetDied event){}
bool on_event (DROID &droid, Intentions::MoveForRepairs it, Activity::MovingToDroid ac, Events::Timeout event){}
bool on_event (DROID &droid, Intentions::MoveForRepairs it, Activity::BeingRepaired ac, Events::Repaired event){}
bool on_event (DROID &droid, Intentions::MoveForRepairs it, Activity::BeingRepaired ac, Events::RepairerDied event){}
bool on_event (DROID &droid, Intentions::Patrol it, Activity::MovingToPlace ac, Events::HealthBelowThreashold event){}
bool on_event (DROID &droid, Intentions::Patrol it, Activity::MovingToPlace ac, Events::ArrivedAtDestination event){}
bool on_event (DROID &droid, Intentions::Patrol it, Activity::MovingToPlace ac, Events::NoRouteToPlace event){}
bool on_event (DROID &droid, Intentions::Patrol it, Activity::MovingToPlace ac, Events::Timeout event){}

#define RETURN_IF_TRUE(x) if (x) return

struct ProduceEvents
{

 // ======== Producers: in combination with "on_event", somewhat equivalent to the old "actionDroidBase"
	static void call (DROID &droid, Intentions::None it, Activity::Idle ac, EventStack events)
	{
		auto damaged = checkForDamagedStruct(droid);
		if (damaged) { RETURN_IF_TRUE (::on_event (droid, it, ac, Events::DamagedStructInVicinity, *damaged)); }
	}
	// "droidUpdateBuild"
	static void call (const DROID &droid, Intentions::MoveToBuild it, Activity::Building ac, EventStack events)
	{
		STRUCTURE *psStruct = castStructure(droid.order.psObj);
		if (psStruct == nullptr) { RETURN_IF_TRUE(::on_event (droid, it, ac, Events::TargetDied {})); }
		if (psStruct->status == SS_BUILT) { RETURN_IF_TRUE(::on_event (droid, it, ac, Events::BuildFinished {})); }
		//CHECK_DROID(psDroid);
		//ASSERT_OR_RETURN(false, psStruct->type == OBJ_STRUCTURE, "target is not a structure");
		//ASSERT_OR_RETURN(false, psDroid->asBits[COMP_CONSTRUCT] < numConstructStats, "Invalid construct pointer for unit");
		// First check the structure hasn't been completed by another droid
		if (psStruct->status == SS_BUILT)
		{

		}
		// make sure we still 'own' the building in question
		if (!aiCheckAlliances(psStruct->player, psDroid->player))
		{
			RETURN_IF_TRUE(::on_event (droid, it, ac, Events::TargetChangedOwner));
		}
		return;
		
	}
	static void call (const DROID &droid, Intentions::MoveToBuild it, Activity::MovingToStructure ac, EventStack events){}
	static void call (const DROID &droid, Intentions::MoveToRecycle it, Activity::MovingToStructure ac, EventStack events){}
	static void call (const DROID &droid, Intentions::MoveToPlace it, Activity::MovingToPlace ac, EventStack events){}
	static void call (const DROID &droid, Intentions::MoveForRepairs it, Activity::MovingToStructure ac, EventStack events){}
	static void call (const DROID &droid, Intentions::MoveForRepairs it, Activity::MovingToDroid ac, EventStack events){}
	static void call (const DROID &droid, Intentions::MoveForRepairs it, Activity::BeingRepaired ac, EventStack events){}
	static void call (const DROID &droid, Intentions::Patrol it, Activity::MovingToPlace ac, EventStack events){}
	
};

struct HandleEvents
{
    EVENT_HANDLER_2(on_event, None,           Idle,              HealthBelowThreashold, DamagedStructInVicinity)
    EVENT_HANDLER_5(on_event, MoveToBuild,    MovingToStructure, HealthBelowThreashold, ArrivedAtDestination, NoRouteToPlace, TargetDied, Timeout)
    EVENT_HANDLER_3(on_event, MoveToBuild,    Building,          HealthBelowThreashold, BuildFinished, TargetDied)
    EVENT_HANDLER_5(on_event, MoveToRecycle,  MovingToStructure, HealthBelowThreashold, ArrivedAtDestination, NoRouteToPlace, TargetDied, Timeout)
    EVENT_HANDLER_4(on_event, MoveToPlace,    MovingToPlace,     HealthBelowThreashold, ArrivedAtDestination, NoRouteToPlace, Timeout)
    EVENT_HANDLER_5(on_event, MoveForRepairs, MovingToStructure, ArrivedAtDestination, NoRouteToPlace, TargetDied, Repaired,  Timeout)
    EVENT_HANDLER_5(on_event, MoveForRepairs, MovingToDroid,     ArrivedAtDestination, NoRouteToTarget, TargetDied, Repaired, Timeout)
    EVENT_HANDLER_2(on_event, MoveForRepairs, BeingRepaired,     Repaired, RepairerDied)
    EVENT_HANDLER_4(on_event, Patrol,         MovingToPlace,     HealthBelowThreashold, ArrivedAtDestination, NoRouteToPlace, Timeout)
};

  // Handlers and Producers must be kept in-sync!
  DISPATCH_ACTIVITY_1(None,           HandleEvents, Idle)
  DISPATCH_ACTIVITY_2(MoveToBuild,    HandleEvents, Building, MovingToStructure)
  DISPATCH_ACTIVITY_1(MoveToRecycle,  HandleEvents, MovingToStructure)
  DISPATCH_ACTIVITY_1(MoveToPlace,    HandleEvents, MovingToPlace)
  DISPATCH_ACTIVITY_3(MoveForRepairs, HandleEvents, MovingToStructure, MovingToDroid, BeingRepaired)
  DISPATCH_ACTIVITY_1(Patrol,         HandleEvents, MovingToPlace)

  DISPATCH_ACTIVITY_1(None,           ProduceEvents, Idle)
  DISPATCH_ACTIVITY_2(MoveToBuild,    ProduceEvents, Building, MovingToStructure)
  DISPATCH_ACTIVITY_1(MoveToRecycle,  ProduceEvents, MovingToStructure)
  DISPATCH_ACTIVITY_1(MoveToPlace,    ProduceEvents, MovingToPlace)
  DISPATCH_ACTIVITY_3(MoveForRepairs, ProduceEvents, MovingToStructure, MovingToDroid, BeingRepaired)
  DISPATCH_ACTIVITY_1(Patrol,         ProduceEvents, MovingToPlace)

  DISPATCH_ACTIVITY_1(None,           Updates, Idle)
  DISPATCH_ACTIVITY_2(MoveToBuild,    Updates, Building, MovingToStructure)
  DISPATCH_ACTIVITY_1(MoveToRecycle,  Updates, MovingToStructure)
  DISPATCH_ACTIVITY_1(MoveToPlace,    Updates, MovingToPlace)
  DISPATCH_ACTIVITY_3(MoveForRepairs, Updates, MovingToStructure, MovingToDroid, BeingRepaired)
  DISPATCH_ACTIVITY_1(Patrol,         Updates, MovingToPlace)
  
  // must be kept in-sync!
  DISPATCH_INTENTIONS_6(HandleEvents, None, MoveToBuild, MoveToRecycle, MoveToPlace, MoveForRepairs, Patrol)
  DISPATCH_INTENTIONS_6(ProduceEvents, None, MoveToBuild, MoveToRecycle, MoveToPlace, MoveForRepairs, Patrol)
  DISPATCH_INTENTIONS_6(Updates, None, MoveToBuild, MoveToRecycle, MoveToPlace, MoveForRepairs, Patrol)

} // ========= end of anonymous namespace

void Constructor::tick (DROID& droid)
{
	EventStack events; // somehow get them
	::dispatch_intentions_HandleEvents(droid, events);
	// 	::dispatch_intentions<HandleEvents>(droid);
}
void Constructor::gen_events (DROID& droid)
{
	EventStack events; // somehow get them
	::dispatch_intentions_ProduceEvents(droid, events);
	//	::dispatch_intentions<ProduceEvents>(droid);
}

