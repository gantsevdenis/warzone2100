/*
	This file is part of Warzone 2100.
	Copyright (C) 1999-2004  Eidos Interactive
	Copyright (C) 2005-2020  Warzone 2100 Project

	Warzone 2100 is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	Warzone 2100 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Warzone 2100; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
*/
/*
 * Move.c
 *
 * Routines for moving units about the map
 *
 */
#include "lib/framework/frame.h"

#include "lib/framework/trig.h"
#include "lib/framework/math_ext.h"
#include "lib/framework/vector.h"
#include "lib/gamelib/gtime.h"
#include "lib/netplay/netplay.h"
#include "lib/sound/audio.h"
#include "lib/sound/audio_id.h"
#include "lib/ivis_opengl/ivisdef.h"
#include "console.h"

#include "move.h"

#include "objects.h"
#include "src/basedef.h"
#include "src/droid.h"
#include "src/droiddef.h"
#include "src/statsdef.h"
#include "visibility.h"
#include "map.h"
#include "fpath.h"
#include "flowfield.h"
#include "loop.h"
#include "geometry.h"
#include "action.h"
#include "order.h"
#include "astar.h"
#include "mapgrid.h"
#include "display.h"	// needed for widgetsOn flag.
#include "effects.h"
#include "power.h"
#include "scores.h"
#include "multiplay.h"
#include "multigifts.h"
#include "random.h"
#include "mission.h"
#include "qtscript.h"
#include "wzmaplib/map.h"
#include <cstdint>
#include <glm/fwd.hpp>
#include <limits>

/* max and min vtol heights above terrain */
#define	VTOL_HEIGHT_MIN				250
#define	VTOL_HEIGHT_LEVEL			300
#define	VTOL_HEIGHT_MAX				350

// Maximum size of an object for collision
#define OBJ_MAXRADIUS	(TILE_UNITS * 4)

// how long a shuffle can propagate before they all stop
#define MOVE_SHUFFLETIME	10000

// Length of time a droid has to be stationery to be considered blocked
#define BLOCK_TIME		6000
#define SHUFFLE_BLOCK_TIME	2000
// How long a droid has to be stationary before stopping trying to move
#define BLOCK_PAUSETIME	1500
#define BLOCK_PAUSERELEASE 500
// How far a droid has to move before it is no longer 'stationary'
#define BLOCK_DIST		64
// How far a droid has to rotate before it is no longer 'stationary'
#define BLOCK_DIR		90

// How far out from an obstruction to start avoiding it
#define AVOID_DIST		(TILE_UNITS*2)

// Speed to approach a final way point, if possible.
#define MIN_END_SPEED		60

// distance from final way point to start slowing
#define END_SPEED_RANGE		(3 * TILE_UNITS)

// how long to pause after firing a FOM_NO weapon
#define FOM_MOVEPAUSE		1500

// distance to consider droids for a shuffle
#define SHUFFLE_DIST		(3*TILE_UNITS/2)
// how far to move for a shuffle
#define SHUFFLE_MOVE		(2*TILE_UNITS/2)

/// Extra precision added to movement calculations.
#define EXTRA_BITS                              8
#define EXTRA_PRECISION                         (1 << EXTRA_BITS)


/* Function prototypes */
static void	moveUpdatePersonModel(DROID *psDroid, SDWORD speed, uint16_t direction);

const char *moveDescription(MOVE_STATUS status)
{
	switch (status)
	{
	case MOVEINACTIVE : return "Inactive";
	case MOVENAVIGATE : return "Navigate";
	case MOVETURN : return "Turn";
	case MOVEPAUSE : return "Pause";
	case MOVEPOINTTOPOINT : return "P2P";
	case MOVETURNTOTARGET : return "Turn2target";
	case MOVEHOVER : return "Hover";
	case MOVEWAITROUTE : return "Waitroute";
	case MOVESHUFFLE : return "Shuffle";
	}
	return "Error";	// satisfy compiler
}

// static bool isZero (Vector2i &v) { return v.x == 0 && v.y == 0; }

/** Set a target location in world coordinates for a droid to move to
 *  @return true if the routing was successful, if false then the calling code
 *          should not try to route here again for a while
 *  @todo Document what "should not try to route here again for a while" means.
 */
static bool moveDroidToBase(DROID *psDroid, UDWORD x, UDWORD y, bool bFormation, FPATH_MOVETYPE moveType)
{
	FPATH_RETVAL		retVal = FPR_OK;

	CHECK_DROID(psDroid);

	// in multiPlayer make Transporter move like the vtols
	if (isTransporter(psDroid) && game.maxPlayers == 0)
	{
		fpathSetDirectRoute(psDroid, x, y);
		psDroid->sMove.Status = MOVENAVIGATE;
		psDroid->sMove.pathIndex = 0;
		return true;
	}
	// NOTE: While Vtols can fly, then can't go through things, like the transporter.
	else if ((game.maxPlayers > 0 && isTransporter(psDroid)))
	{
		fpathSetDirectRoute(psDroid, x, y);
		retVal = FPR_OK;
	}
	else
	{
		retVal = fpathDroidRoute(psDroid, x, y, moveType);
	}

	if (retVal == FPR_OK)
	{
		// bit of a hack this - john
		// if astar doesn't have a complete route, it returns a route to the nearest clear tile.
		// the location of the clear tile is in DestinationX,DestinationY.
		// reset x,y to this position so the formation gets set up correctly
		x = psDroid->sMove.destination.x;
		y = psDroid->sMove.destination.y;

		objTrace(psDroid->id, "unit %d: path ok - base Speed %u, speed %d, target(%u|%d, %u|%d)",
		         (int)psDroid->id, psDroid->baseSpeed, psDroid->sMove.speed, x, map_coord(x), y, map_coord(y));

		psDroid->sMove.Status = MOVENAVIGATE;
		psDroid->sMove.pathIndex = 0;
	}
	else if (retVal == FPR_WAIT)
	{
		// the route will be calculated by the path-finding thread
		psDroid->sMove.Status = MOVEWAITROUTE;
		psDroid->sMove.destination.x = x;
		psDroid->sMove.destination.y = y;
	}
	else // if (retVal == FPR_FAILED)
	{
		objTrace(psDroid->id, "Path to (%d, %d) failed for droid %d", (int)x, (int)y, (int)psDroid->id);
		psDroid->sMove.Status = MOVEINACTIVE;
		actionDroid(psDroid, DACTION_SULK);
		return (false);
	}

	CHECK_DROID(psDroid);
	return true;
}

/** Move a droid to a location, joining a formation
 *  @see moveDroidToBase() for the parameter and return value specification
 */
bool moveDroidTo(DROID *psDroid, UDWORD x, UDWORD y, FPATH_MOVETYPE moveType)
{
	return moveDroidToBase(psDroid, x, y, true, moveType);
}

/** Move a droid to a location, not joining a formation
 *  @see moveDroidToBase() for the parameter and return value specification
 */
bool moveDroidToNoFormation(DROID *psDroid, UDWORD x, UDWORD y, FPATH_MOVETYPE moveType)
{
	ASSERT_OR_RETURN(false, x > 0 && y > 0, "Bad movement position");
	return moveDroidToBase(psDroid, x, y, false, moveType);
}


/** Move a droid directly to a location.
 *  @note This is (or should be) used for VTOLs only.
 */
void moveDroidToDirect(DROID *psDroid, UDWORD x, UDWORD y)
{
	ASSERT_OR_RETURN(, psDroid != nullptr && isVtolDroid(psDroid), "Only valid for a VTOL unit");

	fpathSetDirectRoute(psDroid, x, y);
	psDroid->sMove.Status = MOVENAVIGATE;
	psDroid->sMove.pathIndex = 0;
}


/** Turn a droid towards a given location.
 */
void moveTurnDroid(DROID *psDroid, UDWORD x, UDWORD y)
{
	uint16_t moveDir = calcDirection(psDroid->pos.x, psDroid->pos.y, x, y);

	if (psDroid->rot.direction != moveDir)
	{
		psDroid->sMove.target.x = x;
		psDroid->sMove.target.y = y;
		psDroid->sMove.Status = MOVETURNTOTARGET;
	}
}

// Tell a droid to move out the way for a shuffle
static void moveShuffleDroid(DROID *psDroid, Vector2i s)
{
	SDWORD  mx, my;
	bool	frontClear = true, leftClear = true, rightClear = true;
	SDWORD	lvx, lvy, rvx, rvy, svx, svy;
	SDWORD	shuffleMove;

	ASSERT_OR_RETURN(, psDroid != nullptr, "Bad droid pointer");
	CHECK_DROID(psDroid);

	uint16_t shuffleDir = iAtan2(s);
	int32_t shuffleMag = iHypot(s);

	if (shuffleMag == 0)
	{
		return;
	}

	shuffleMove = SHUFFLE_MOVE;

	// calculate the possible movement vectors
	svx = s.x * shuffleMove / shuffleMag;  // Straight in the direction of s.
	svy = s.y * shuffleMove / shuffleMag;

	lvx = -svy;  // 90° to the... right?
	lvy = svx;

	rvx = svy;   // 90° to the... left?
	rvy = -svx;

	// check for blocking tiles
	if (fpathBlockingTile(map_coord((SDWORD)psDroid->pos.x + lvx),
	                      map_coord((SDWORD)psDroid->pos.y + lvy), getPropulsionStats(psDroid)->propulsionType))
	{
		leftClear = false;
	}
	else if (fpathBlockingTile(map_coord((SDWORD)psDroid->pos.x + rvx),
	                           map_coord((SDWORD)psDroid->pos.y + rvy), getPropulsionStats(psDroid)->propulsionType))
	{
		rightClear = false;
	}
	else if (fpathBlockingTile(map_coord((SDWORD)psDroid->pos.x + svx),
	                           map_coord((SDWORD)psDroid->pos.y + svy), getPropulsionStats(psDroid)->propulsionType))
	{
		frontClear = false;
	}

	// find any droids that could block the shuffle
	static GridList gridList;  // static to avoid allocations.
	gridList = gridStartIterate(psDroid->pos.x, psDroid->pos.y, SHUFFLE_DIST);
	for (GridIterator gi = gridList.begin(); gi != gridList.end(); ++gi)
	{
		DROID *psCurr = castDroid(*gi);
		if (psCurr == nullptr || psCurr->died || psCurr == psDroid)
		{
			continue;
		}

		uint16_t droidDir = iAtan2((psCurr->pos - psDroid->pos).xy());
		int diff = angleDelta(shuffleDir - droidDir);
		if (diff > -DEG(135) && diff < -DEG(45))
		{
			leftClear = false;
		}
		else if (diff > DEG(45) && diff < DEG(135))
		{
			rightClear = false;
		}
	}

	// calculate a target
	if (leftClear)
	{
		mx = lvx;
		my = lvy;
	}
	else if (rightClear)
	{
		mx = rvx;
		my = rvy;
	}
	else if (frontClear)
	{
		mx = svx;
		my = svy;
	}
	else
	{
		// nowhere to shuffle to, quit
		return;
	}

	// check the location for vtols
	Vector2i tar = psDroid->pos.xy() + Vector2i(mx, my);
	if (isVtolDroid(psDroid))
	{
		actionVTOLLandingPos(psDroid, &tar);
	}


	// set up the move state
	if (psDroid->sMove.Status != MOVESHUFFLE)
	{
		psDroid->sMove.shuffleStart = gameTime;
	}
	psDroid->sMove.Status = MOVESHUFFLE;
	psDroid->sMove.src = psDroid->pos.xy();
	psDroid->sMove.target = tar;
	psDroid->sMove.asPath.clear();
	psDroid->sMove.pathIndex = 0;

	CHECK_DROID(psDroid);
}


/** Stop a droid from moving.
 */
void moveStopDroid(DROID *psDroid)
{
	CHECK_DROID(psDroid);
	PROPULSION_STATS *psPropStats = asPropulsionStats + psDroid->asBits[COMP_PROPULSION];
	ASSERT_OR_RETURN(, psPropStats != nullptr, "invalid propulsion stats pointer");

	if (psPropStats->propulsionType == PROPULSION_TYPE_LIFT)
	{
		psDroid->sMove.Status = MOVEHOVER;
	}
	else
	{
		psDroid->sMove.Status = MOVEINACTIVE;
	}
}

/** Stops a droid dead in its tracks.
 *  Doesn't allow for any little skidding bits.
 *  @param psDroid the droid to stop from moving
 */
void moveReallyStopDroid(DROID *psDroid)
{
	CHECK_DROID(psDroid);

	psDroid->sMove.Status = MOVEINACTIVE;
	psDroid->sMove.speed = 0;
}


#define PITCH_LIMIT 150

/* Get pitch and roll from direction and tile data */
void updateDroidOrientation(DROID *psDroid)
{
	int32_t hx0, hx1, hy0, hy1;
	int newPitch, deltaPitch, pitchLimit;
	int32_t dzdx, dzdy, dzdv, dzdw;
	const int d = 20;
	int32_t vX, vY;

	if (psDroid->droidType == DROID_PERSON || cyborgDroid(psDroid) || isTransporter(psDroid)
	    || isFlying(psDroid))
	{
		/* The ground doesn't affect the pitch/roll of these droids*/
		return;
	}

	// Find the height of 4 points around the droid.
	//    hy0
	// hx0 * hx1      (* = droid)
	//    hy1
	hx1 = map_Height(psDroid->pos.x + d, psDroid->pos.y);
	hx0 = map_Height(MAX(0, psDroid->pos.x - d), psDroid->pos.y);
	hy1 = map_Height(psDroid->pos.x, psDroid->pos.y + d);
	hy0 = map_Height(psDroid->pos.x, MAX(0, psDroid->pos.y - d));

	//update height in case were in the bottom of a trough
	psDroid->pos.z = MAX(psDroid->pos.z, (hx0 + hx1) / 2);
	psDroid->pos.z = MAX(psDroid->pos.z, (hy0 + hy1) / 2);

	// Vector of length 65536 pointing in direction droid is facing.
	vX = iSin(psDroid->rot.direction);
	vY = iCos(psDroid->rot.direction);

	// Calculate pitch of ground.
	dzdx = hx1 - hx0;                                    // 2*d*∂z(x, y)/∂x       of ground
	dzdy = hy1 - hy0;                                    // 2*d*∂z(x, y)/∂y       of ground
	dzdv = dzdx * vX + dzdy * vY;                        // 2*d*∂z(x, y)/∂v << 16 of ground, where v is the direction the droid is facing.
	newPitch = iAtan2(dzdv, (2 * d) << 16);              // pitch = atan(∂z(x, y)/∂v)/2π << 16

	deltaPitch = angleDelta(newPitch - psDroid->rot.pitch);

	// Limit the rate the front comes down to simulate momentum
	pitchLimit = gameTimeAdjustedIncrement(DEG(PITCH_LIMIT));
	deltaPitch = MAX(deltaPitch, -pitchLimit);

	// Update pitch.
	psDroid->rot.pitch += deltaPitch;

	// Calculate and update roll of ground (not taking pitch into account, but good enough).
	dzdw = dzdx * vY - dzdy * vX;				// 2*d*∂z(x, y)/∂w << 16 of ground, where w is at right angles to the direction the droid is facing.
	psDroid->rot.roll = iAtan2(dzdw, (2 * d) << 16);		// pitch = atan(∂z(x, y)/∂w)/2π << 16
}


struct BLOCKING_CALLBACK_DATA
{
	PROPULSION_TYPE propulsionType;
	bool blocking;
	Vector2i src;
	Vector2i dst;
};

static bool moveBlockingTileCallback(Vector2i pos, int32_t dist, void *data_)
{
	BLOCKING_CALLBACK_DATA *data = (BLOCKING_CALLBACK_DATA *)data_;
	data->blocking |= pos != data->src && pos != data->dst && fpathBlockingTile(map_coord(pos.x), map_coord(pos.y), data->propulsionType);
	return !data->blocking;
}

// Returns (-1 - distance) if the direct path to the waypoint is blocked, 
// otherwise returns the distance to the waypoint.
static int32_t moveDirectPathToWaypoint(DROID *psDroid, unsigned positionIndex)
{
	Vector2i src(psDroid->pos.xy());
	Vector2i dst = psDroid->sMove.asPath[positionIndex];
	Vector2i delta = dst - src;
	int32_t dist = iHypot(delta);
	BLOCKING_CALLBACK_DATA data;
	data.propulsionType = getPropulsionStats(psDroid)->propulsionType;
	data.blocking = false;
	data.src = src;
	data.dst = dst;
	rayCast(src, dst, &moveBlockingTileCallback, &data);
	objTrace (psDroid->id, "moveDirectPathToWaypoint: %i", data.blocking);
	return data.blocking ? -1 - dist : dist;
}

// Returns true if still able to find the path.
static bool moveBestTarget(DROID *psDroid)
{
	int positionIndex = std::max(psDroid->sMove.pathIndex - 1, 0);
	int32_t dist = moveDirectPathToWaypoint(psDroid, positionIndex);
	if (dist >= 0)
	{
		// Look ahead in the path.
		while (dist >= 0 && dist < TILE_UNITS * 5)
		{
			++positionIndex;
			if (positionIndex >= (int)psDroid->sMove.asPath.size())
			{
				dist = -1;
				break;  // Reached end of path.
			}
			dist = moveDirectPathToWaypoint(psDroid, positionIndex);
		}
		if (dist < 0)
		{
			--positionIndex;
		}
	}
	else
	{
		// Lost sight of path, backtrack.
		while (dist < 0 && dist >= -TILE_UNITS * 7 && positionIndex > 0)
		{
			--positionIndex;
			dist = moveDirectPathToWaypoint(psDroid, positionIndex);
		}
		if (dist < 0)
		{
			objTrace (psDroid->id, "Couldn't find path, and backtracking didn't help");
			return false;  // Couldn't find path, and backtracking didn't help.
		}
	}
	psDroid->sMove.pathIndex = positionIndex + 1;
	psDroid->sMove.src = psDroid->pos.xy();
	psDroid->sMove.target = psDroid->sMove.asPath[positionIndex];
	return true;
}

/// Get the next target point from the route.
/// This is stopping condition to get from MOVENAVIGATE to MOVEINACTIVE
static bool moveNextTarget(DROID *psDroid)
{
	CHECK_DROID(psDroid);

	// See if there is anything left in the move list
	if (psDroid->sMove.pathIndex == (int)psDroid->sMove.asPath.size())
	{
		return false;
	}
	ASSERT_OR_RETURN(false, psDroid->sMove.pathIndex >= 0 && psDroid->sMove.pathIndex < (int)psDroid->sMove.asPath.size(), "psDroid->sMove.pathIndex out of bounds %d/%d.", psDroid->sMove.pathIndex, (int)psDroid->sMove.asPath.size());

	if (psDroid->sMove.pathIndex == 0)
	{
		psDroid->sMove.src = psDroid->pos.xy();
	}
	else
	{
		psDroid->sMove.src = psDroid->sMove.asPath[psDroid->sMove.pathIndex - 1];
	}
	psDroid->sMove.target = psDroid->sMove.asPath[psDroid->sMove.pathIndex];
	++psDroid->sMove.pathIndex;

	CHECK_DROID(psDroid);
	return true;
}

/// Returns cell units. Nb of cells this droid spans
uint8_t moveDroidSizeExtent (const DROID *psDroid)
{
	if (psDroid->droidType == DROID_PERSON)
	{
		return PersonRadius;
	}
	else if (cyborgDroid(psDroid))
	{
		return CyborgRadius;
	}
	const BODY_STATS *psBdyStats = &asBodyStats[psDroid->asBits[COMP_BODY]];
	static_assert(SIZE_NUM == 4, "Update this when adding new Body Sizes!");
	switch (psBdyStats->size)
	{
		case SIZE_LIGHT: return SmallRadius;
		case SIZE_MEDIUM: return MediumRadius;
		case SIZE_HEAVY: return LargeRadius;
		case SIZE_SUPER_HEAVY: return ExtraLargeRadius;
		// unreachable
		default: exit(1);
	}
}

// Watermelon:fix these magic number...the collision radius should be based on pie imd radius not some static int's...
// Note: tile size is 128
// static   int mvPersRad = 16, mvCybRad = 32, mvSmRad = 32, mvMedRad = 64, mvLgRad = 64, mvSuperHeavy = 128;
// static	int mvPersRad = 20, mvCybRad = 30, mvSmRad = 40, mvMedRad = 50, mvLgRad = 60;
/// Get radius (world units) of droid for collision calculations
SDWORD moveDroidRadius (const DROID *psDroid)
{
	return moveDroidSizeExtent(psDroid) * FF_UNIT;
}



/// Get the radius, in world units, of a base object for collision
SDWORD moveObjRadius(const BASE_OBJECT *psObj)
{
	switch (psObj->type)
	{
	case OBJ_DROID:
		{
			const DROID *psDroid = (const DROID *)psObj;
			return moveDroidRadius(psDroid);
		}
	case OBJ_STRUCTURE:
		return psObj->sDisplay.imd->radius / 2;

	case OBJ_FEATURE:
		return psObj->sDisplay.imd->radius / 2;

	default:
		ASSERT(false, "unknown object type");
		return 0;
	}
}


// see if a Droid has run over a person
static void moveCheckSquished(DROID *psDroid, int32_t emx, int32_t emy)
{
	int32_t		rad, radSq, objR, xdiff, ydiff, distSq;
	const int32_t	droidR = moveObjRadius((BASE_OBJECT *)psDroid);
	const int32_t   mx = gameTimeAdjustedAverage(emx, EXTRA_PRECISION);
	const int32_t   my = gameTimeAdjustedAverage(emy, EXTRA_PRECISION);

	static GridList gridList;  // static to avoid allocations.
	gridList = gridStartIterate(psDroid->pos.x, psDroid->pos.y, OBJ_MAXRADIUS);
	for (GridIterator gi = gridList.begin(); gi != gridList.end(); ++gi)
	{
		BASE_OBJECT *psObj = *gi;
		if (psObj->type != OBJ_DROID || ((DROID *)psObj)->droidType != DROID_PERSON)
		{
			// ignore everything but people
			continue;
		}

		ASSERT_OR_RETURN(, psObj->type == OBJ_DROID && ((DROID *)psObj)->droidType == DROID_PERSON, "squished - eerk");

		objR = moveObjRadius(psObj);
		rad = droidR + objR;
		radSq = rad * rad;

		xdiff = psDroid->pos.x + mx - psObj->pos.x;
		ydiff = psDroid->pos.y + my - psObj->pos.y;
		distSq = xdiff * xdiff + ydiff * ydiff;

		if (((2 * radSq) / 3) > distSq)
		{
			if ((psDroid->player != psObj->player) && !aiCheckAlliances(psDroid->player, psObj->player))
			{
				// run over a bloke - kill him
				destroyDroid((DROID *)psObj, gameTime);
				scoreUpdateVar(WD_BARBARIANS_MOWED_DOWN);
			}
		}
	}
}


// See if the droid has been stopped long enough to give up on the move
static bool moveBlocked(DROID *psDroid)
{
	SDWORD	xdiff, ydiff, diffSq;
	UDWORD	blockTime;

	if (psDroid->sMove.bumpTime == 0 || psDroid->sMove.bumpTime > gameTime)
	{
		// no bump - can't be blocked
		return false;
	}

	// See if the block can be cancelled
	if (abs(angleDelta(psDroid->rot.direction - psDroid->sMove.bumpDir)) > DEG(BLOCK_DIR))
	{
		// Move on, clear the bump
		psDroid->sMove.bumpTime = 0;
		psDroid->sMove.lastBump = 0;
		return false;
	}
	xdiff = (SDWORD)psDroid->pos.x - (SDWORD)psDroid->sMove.bumpPos.x;
	ydiff = (SDWORD)psDroid->pos.y - (SDWORD)psDroid->sMove.bumpPos.y;
	diffSq = xdiff * xdiff + ydiff * ydiff;
	if (diffSq > BLOCK_DIST * BLOCK_DIST)
	{
		// Move on, clear the bump
		psDroid->sMove.bumpTime = 0;
		psDroid->sMove.lastBump = 0;
		return false;
	}

	if (psDroid->sMove.Status == MOVESHUFFLE)
	{
		blockTime = SHUFFLE_BLOCK_TIME;
	}
	else
	{
		blockTime = BLOCK_TIME;
	}

	if (gameTime - psDroid->sMove.bumpTime > blockTime)
	{
		// Stopped long enough - blocked
		psDroid->sMove.bumpTime = 0;
		psDroid->sMove.lastBump = 0;
		if (!isHumanPlayer(psDroid->player) && bMultiPlayer)
		{
			psDroid->lastFrustratedTime = gameTime;
			objTrace(psDroid->id, "FRUSTRATED");
		}
		else
		{
			objTrace(psDroid->id, "BLOCKED");
		}
		// if the unit cannot see the next way point - reroute it's got stuck
		if ((bMultiPlayer || psDroid->player == selectedPlayer || psDroid->lastFrustratedTime == gameTime)
		    && psDroid->sMove.pathIndex != (int)psDroid->sMove.asPath.size())
		{
			objTrace(psDroid->id, "Trying to reroute to (%d,%d)", psDroid->sMove.destination.x, psDroid->sMove.destination.y);
			moveDroidTo(psDroid, psDroid->sMove.destination.x, psDroid->sMove.destination.y);
			return false;
		}

		return true;
	}

	return false;
}

// Droids closer than this (world coordinates) will be repelled with maximum force to avoid stepping on each other
const static int32_t D_1 = 4;
constexpr static int32_t D_1_SQ = D_1 * D_1;
// Droids between D_1 and D_2 will be repelled proportionnaly to their distance
// Droids further than D_2 will not be repelled
const static int32_t D_2 = FF_UNIT*2;
const static int32_t D_2_SQ = D_2 * D_2;

/// Calculate the actual avoidance vector to avoid obstacle
// should we take time-adjusted position?
#if 1

/// Adjusts droid's velocity vector wrt other encountered droids (avoid overlapping), maybe many of them.
/// Only modifies directions = leaves speed unchanged
///
/// Allowed: map of allowed directions
void droidHandleObstacle (DROID *psDroid, uint32_t adjx, uint32_t adjy, uint8_t allowed, int32_t *outx, int32_t *outy)
{
//	PROPULSION_STATS *propStats = asPropulsionStats + psDroid->asBits[COMP_PROPULSION];
//	int selfMaxSpeed = propStats->maxSpeed;
	uint16_t wantedDir = snapDirection8(psDroid->sMove.moveDir);
	uint16_t possibleDir = allowed & wantedDir;
	if (psDroid->player == 0) debug (LOG_FLOWFIELD, "handle obstacle %x %x %x", wantedDir, allowed, possibleDir);
	if (possibleDir == 0)
	{
		if (psDroid->player == 0) debug (LOG_FLOWFIELD, "nowhere to move %i", psDroid->id);
		psDroid->sMove.moveDir = 0; // ?? to modify speed?
	}
	psDroid->sMove.moveDir = possibleDir;
	
}

void droidRepulse (const DROID *psDroid, uint32_t adjx, uint32_t adjy, const BASE_OBJECT *psOther, int32_t *outx, int32_t *outy)
{
	// where center of sphere? Assume already in center
	const auto obsR = moveObjRadius(psOther); // world units
	const auto obsR_sq = obsR * obsR;
	const auto selfR = moveDroidRadius(psDroid); // world units
	const auto selfR_sq = selfR * selfR;
	const auto otherX = psOther->pos.x;
	const auto otherY = psOther->pos.y;
	// Calculate the vector to the obstruction
	const int32_t vobstX = otherX - adjx;
	const int32_t vobstY = otherY - adjy;
	const auto dist_sq = vobstX * vobstX + vobstY * vobstY;
	int ratio = 0;
	PROPULSION_STATS *propStats = asPropulsionStats + psDroid->asBits[COMP_PROPULSION];
	int selfMaxSpeed = propStats->maxSpeed;
	const auto angle = iAtan2({vobstX, vobstY});
	const auto projx = iSin(angle);
	const auto projy = iCos(angle);
	const auto total_radius_sq = obsR_sq + selfR_sq;
	Vector2i selfVel = iSinCosR(psDroid->sMove.moveDir, selfMaxSpeed);
	const auto velcx = iSin(psDroid->sMove.moveDir);
	const auto velcy = iCos(psDroid->sMove.moveDir);
	// Vector2i selfVelocity = iSinCosR(angle, selfMaxSpeed);
	// get the repulsion vector
	if (dist_sq < total_radius_sq)
	{
		// in floating: ratio = 1.0f
		//const uint32_t repulsion_dx = (int64_t)ratiox * selfVel.x  / 65536;
		//const uint32_t repulsion_dy = (int64_t)ratioy * selfVel.y  / 65536;
		*outx = 0; //-selfVel.x; // should go down when projx is low
		*outy = 0; //-selfVel.y; // should go down when projy is low
		// TODO hum move droid out, even if it's not moving currently
		if (psDroid->player == 0)
		debug (LOG_FLOWFIELD, "max repulsion: %i %i, adjxy %i %i, other %i %i, proj %i %i, velcxy %i %i", *outx, *outy, adjx, adjy, otherX, otherY,
			projx, projy, velcx, velcy);

	}
	#if 0
	else if (dist_sq < (obsR_sq + D_2_SQ))
	{
		// calculate linear interpolation
		ratio = (int64_t)65536 * (dist_sq - selfR_sq) / D_2_SQ;
		ratio = 65536 - ratio;
		// scale to angle between ourself and obstacle
		const auto ratiox = (int64_t)ratio * projx / 65536;
		const auto ratioy = (int64_t)ratio * projy / 65536;
		const uint32_t repulsion_dx = (int64_t)ratiox * selfVel.x  / 65536;
		const uint32_t repulsion_dy = (int64_t)ratioy * selfVel.y  / 65536;
		*outx = selfVel.x - repulsion_dx;
		*outy = selfVel.y - repulsion_dy;
		// repulsion: 5052 4781, rx ry 20558 -42700, adjxy 4964 13790, other 4925 13871
		if (psDroid->player == 0)
			debug (LOG_FLOWFIELD, "repulsion: %i %i, rx ry %li %li, adjxy %i %i, other %i %i", *outx, *outy, ratiox, ratioy, adjx, adjy, otherX, otherY);
	}
	#endif
	// else repulsion is zero. nothing to do
	if (psDroid->player == 0) debug (LOG_FLOWFIELD, "distance to obstacle was: %i<%i?=%i, (obsR_sq + D_2_SQ)=%i, otherxy %i %i",
	dist_sq, obsR_sq, dist_sq < obsR_sq, (obsR_sq + D_2_SQ), otherX, otherY);
}
#endif

bool moveCalcSlideVector(int32_t obstX, int32_t obstY, int32_t mx, int32_t my, Vector2i &out)
{
	int32_t dirX, dirY, dirMagSq, dotRes;
	// if the target dir is the same, don't need to slide
	if (obstX * mx + obstY * my >= 0)
	{
		return false;
	}

	// Choose the tangent vector to this on the same side as the target
	dotRes = obstY * mx - obstX * my;
	#if 0
	if (dotRes >= 0)
	{
		dirX = obstY;
		dirY = -obstX;
	}
	else
	{
		dirX = -obstY;
		dirY = obstX;
		dotRes = -dotRes;
	}
	#else
	const auto coef = dotRes >= 0 ? 1 : -1;
	dirX = coef * obstY;
	dirY = -coef * obstX;
	dotRes = coef * dotRes;	
	#endif
	dirMagSq = MAX(1, dirX * dirX + dirY * dirY);

	// Calculate the component of the movement in the direction of the tangent vector
	out.x = (int64_t)dirX * dotRes / dirMagSq;
	out.y = (int64_t)dirY * dotRes / dirMagSq;
	return true;
}

/// Calculate the actual avoidance vector to slide around
static void moveCalcSlideVector(DROID *psDroid, int32_t objX, int32_t objY, int32_t *pMx, int32_t *pMy)
{
	// Calculate the vector to the obstruction
	const int32_t obstX = psDroid->pos.x - objX;
	const int32_t obstY = psDroid->pos.y - objY;
	Vector2i out;
	const auto old_adjx = gameTimeAdjustedAverage(*pMx, EXTRA_PRECISION);
	const auto old_adjy = gameTimeAdjustedAverage(*pMy, EXTRA_PRECISION);
	const bool r = moveCalcSlideVector(obstX, obstY,  *pMx, *pMy, out);
	if (r)
	{
		const auto new_adjx = gameTimeAdjustedAverage(out.x, EXTRA_PRECISION);
		const auto new_adjy = gameTimeAdjustedAverage(out.y, EXTRA_PRECISION);
		if (psDroid->player == 0) debug (LOG_FLOWFIELD,
			"modified pmx pmy: new %i %i (%i %i), old %i %i (%i %i), obs %i %i",
			new_adjx, new_adjy, out.x, out.y, old_adjx, old_adjy, *pMx, *pMy, obstX, obstY);
		*pMx = out.x;
		*pMy = out.y;
	}
	else
	{

	}
}


static void moveOpenGates(DROID *psDroid, Vector2i tile)
{
	// is the new tile a gate?
	if (!worldOnMap(tile.x, tile.y))
	{
		return;
	}
	MAPTILE *psTile = mapTile(tile);
	if (!isFlying(psDroid) && psTile && psTile->psObject && psTile->psObject->type == OBJ_STRUCTURE && aiCheckAlliances(psTile->psObject->player, psDroid->player))
	{
		requestOpenGate((STRUCTURE *)psTile->psObject);  // If it's a friendly gate, open it. (It would be impolite to open an enemy gate.)
	}
}

static void moveOpenGates(DROID *psDroid)
{
	Vector2i pos = psDroid->pos.xy() + iSinCosR(psDroid->sMove.moveDir, psDroid->sMove.speed * SAS_OPEN_SPEED / GAME_TICKS_PER_SEC);
	moveOpenGates(psDroid, map_coord(pos));
}

// see if a droid has run into a blocking tile
// TODO See if this function can be simplified.
static void moveCalcBlockingSlide(DROID *psDroid, int32_t *pmx, int32_t *pmy, uint16_t tarDir, uint16_t *pSlideDir)
{
	PROPULSION_TYPE	propulsion = getPropulsionStats(psDroid)->propulsionType;
	SDWORD	horizX, horizY, vertX, vertY;
	uint16_t slideDir;
	// calculate the new coords and see if they are on a different tile
	const int32_t mx = gameTimeAdjustedAverage(*pmx, EXTRA_PRECISION);
	const int32_t my = gameTimeAdjustedAverage(*pmy, EXTRA_PRECISION);
	const int32_t tx = map_coord(psDroid->pos.x);
	const int32_t ty = map_coord(psDroid->pos.y);
	const int32_t nx = psDroid->pos.x + mx;
	const int32_t ny = psDroid->pos.y + my;
	const int32_t ntx = map_coord(nx);
	const int32_t nty = map_coord(ny);
	const int32_t blkCX = world_coord(ntx) + TILE_UNITS / 2;
	const int32_t blkCY = world_coord(nty) + TILE_UNITS / 2;

	CHECK_DROID(psDroid);

	// is the new tile a gate?
	moveOpenGates(psDroid, Vector2i(ntx, nty));

	// is the new tile blocking?
	if (!fpathBlockingTile(ntx, nty, propulsion))
	{
		// not blocking, don't change the move vector
		return;
	}

	// if the droid is shuffling - just stop
	if (psDroid->sMove.Status == MOVESHUFFLE)
	{
		objTrace(psDroid->id, "Was shuffling, now stopped");
		psDroid->sMove.Status = MOVEINACTIVE;
	}

	// note the bump time and position if necessary
	if (!isVtolDroid(psDroid) &&
	    psDroid->sMove.bumpTime == 0)
	{
		psDroid->sMove.bumpTime = gameTime;
		psDroid->sMove.lastBump = 0;
		psDroid->sMove.pauseTime = 0;
		psDroid->sMove.bumpPos = psDroid->pos;
		psDroid->sMove.bumpDir = psDroid->rot.direction;
	}

	if (tx != ntx && ty != nty)
	{
		// moved diagonally

		// figure out where the other two possible blocking tiles are
		horizX = mx < 0 ? ntx + 1 : ntx - 1;
		horizY = nty;

		vertX = ntx;
		vertY = my < 0 ? nty + 1 : nty - 1;

		if (fpathBlockingTile(horizX, horizY, propulsion) && fpathBlockingTile(vertX, vertY, propulsion))
		{
			// in a corner - choose an arbitrary slide
			if (gameRand(2) == 0)
			{
				*pmx = 0;
				*pmy = -*pmy;
			}
			else
			{
				*pmx = -*pmx;
				*pmy = 0;
			}
		}
		else if (fpathBlockingTile(horizX, horizY, propulsion))
		{
			*pmy = 0;
		}
		else if (fpathBlockingTile(vertX, vertY, propulsion))
		{
			*pmx = 0;
		}
		else
		{
			moveCalcSlideVector(psDroid, blkCX, blkCY, pmx, pmy);
		}
	}
	else if (tx != ntx)
	{
		// moved horizontally - see which half of the tile were in
		if ((psDroid->pos.y & TILE_MASK) > TILE_UNITS / 2)
		{
			// top half
			if (fpathBlockingTile(ntx, nty + 1, propulsion))
			{
				*pmx = 0;
			}
			else
			{
				moveCalcSlideVector(psDroid, blkCX, blkCY, pmx, pmy);
			}
		}
		else
		{
			// bottom half
			if (fpathBlockingTile(ntx, nty - 1, propulsion))
			{
				*pmx = 0;
			}
			else
			{
				moveCalcSlideVector(psDroid, blkCX, blkCY, pmx, pmy);
			}
		}
	}
	else if (ty != nty)
	{
		// moved vertically
		if ((psDroid->pos.x & TILE_MASK) > TILE_UNITS / 2)
		{
			// top half
			if (fpathBlockingTile(ntx + 1, nty, propulsion))
			{
				*pmy = 0;
			}
			else
			{
				moveCalcSlideVector(psDroid, blkCX, blkCY, pmx, pmy);
			}
		}
		else
		{
			// bottom half
			if (fpathBlockingTile(ntx - 1, nty, propulsion))
			{
				*pmy = 0;
			}
			else
			{
				moveCalcSlideVector(psDroid, blkCX, blkCY, pmx, pmy);
			}
		}
	}
	else // if (tx == ntx && ty == nty)
	{
		// on a blocking tile - see if we need to jump off
		int	intx = psDroid->pos.x & TILE_MASK;
		int	inty = psDroid->pos.y & TILE_MASK;
		bool	bJumped = false;
		int	jumpx = psDroid->pos.x;
		int	jumpy = psDroid->pos.y;

		if (intx < TILE_UNITS / 2)
		{
			if (inty < TILE_UNITS / 2)
			{
				// top left
				if ((mx < 0) && fpathBlockingTile(tx - 1, ty, propulsion))
				{
					bJumped = true;
					jumpy = (jumpy & ~TILE_MASK) - 1;
				}
				if ((my < 0) && fpathBlockingTile(tx, ty - 1, propulsion))
				{
					bJumped = true;
					jumpx = (jumpx & ~TILE_MASK) - 1;
				}
			}
			else
			{
				// bottom left
				if ((mx < 0) && fpathBlockingTile(tx - 1, ty, propulsion))
				{
					bJumped = true;
					jumpy = (jumpy & ~TILE_MASK) + TILE_UNITS;
				}
				if ((my >= 0) && fpathBlockingTile(tx, ty + 1, propulsion))
				{
					bJumped = true;
					jumpx = (jumpx & ~TILE_MASK) - 1;
				}
			}
		}
		else
		{
			if (inty < TILE_UNITS / 2)
			{
				// top right
				if ((mx >= 0) && fpathBlockingTile(tx + 1, ty, propulsion))
				{
					bJumped = true;
					jumpy = (jumpy & ~TILE_MASK) - 1;
				}
				if ((my < 0) && fpathBlockingTile(tx, ty - 1, propulsion))
				{
					bJumped = true;
					jumpx = (jumpx & ~TILE_MASK) + TILE_UNITS;
				}
			}
			else
			{
				// bottom right
				if ((mx >= 0) && fpathBlockingTile(tx + 1, ty, propulsion))
				{
					bJumped = true;
					jumpy = (jumpy & ~TILE_MASK) + TILE_UNITS;
				}
				if ((my >= 0) && fpathBlockingTile(tx, ty + 1, propulsion))
				{
					bJumped = true;
					jumpx = (jumpx & ~TILE_MASK) + TILE_UNITS;
				}
			}
		}

		if (bJumped)
		{
			psDroid->pos.x = MAX(0, jumpx);
			psDroid->pos.y = MAX(0, jumpy);
			*pmx = 0;
			*pmy = 0;
		}
		else
		{
			moveCalcSlideVector(psDroid, blkCX, blkCY, pmx, pmy);
		}
	}

	slideDir = iAtan2(*pmx, *pmy);
	if (ntx != tx)
	{
		// hit a horizontal block
		if ((tarDir < DEG(90) || tarDir > DEG(270)) &&
		    (slideDir >= DEG(90) && slideDir <= DEG(270)))
		{
			slideDir = tarDir;
		}
		else if ((tarDir >= DEG(90) && tarDir <= DEG(270)) &&
		         (slideDir < DEG(90) || slideDir > DEG(270)))
		{
			slideDir = tarDir;
		}
	}
	if (nty != ty)
	{
		// hit a vertical block
		if ((tarDir < DEG(180)) &&
		    (slideDir >= DEG(180)))
		{
			slideDir = tarDir;
		}
		else if ((tarDir >= DEG(180)) &&
		         (slideDir < DEG(180)))
		{
			slideDir = tarDir;
		}
	}
	*pSlideDir = slideDir;

	CHECK_DROID(psDroid);
}

/// Returns true of collision spheres do intersect
static bool collisionDroid_original (const DROID *psDroid, uint32_t adjx, uint32_t adjy, const DROID *psOther)
{
	#if 0
	// FIXME: looks like a hack, caller must handle it
	if (!bLegs && (psOther)->droidType == DROID_PERSON)
	{
		// everything else doesn't avoid people
		return false;
	}
	
	if (psOther->player == psDroid->player
	    && psDroid->lastFrustratedTime > 0
	    && gameTime - psDroid->lastFrustratedTime < FRUSTRATED_TIME)
	{
		return false; // clip straight through own units when sufficient frustrated -- using cheat codes!
	}
	#endif
	// TODO: this doesn't need to be called N times for each iteration
	const auto droidR = moveObjRadius(psDroid);
	const auto objR = moveObjRadius(psOther);
	auto rad = droidR + objR;
	auto radSq = rad * rad;
	auto xdiff = adjx - psOther->pos.x;
	auto ydiff = adjy - psOther->pos.y;
	auto distSq = xdiff * xdiff + ydiff * ydiff;
	return (radSq > distSq);
}

bool static cells_droidIntersects (uint16_t self_0x, uint16_t self_0y, uint16_t self_extent, uint16_t other_0x, uint16_t other_0y, uint16_t other_extent)
{
	const auto self_endx = self_0x + self_extent - 1;
	const auto self_endy = self_0y + self_extent - 1;
	const auto other_endx = other_0x + other_extent - 1;
	const auto other_endy = other_0y + other_extent - 1;
	bool x_intersects = ((self_0x <= other_endx) && (other_endx <= self_endx));
	bool y_intersects = ((self_0y <= other_endy) && (other_endy <= self_endy));
	return x_intersects && y_intersects;	
}

/// Adjx Adjy are in world coordinates.
/// Assumes droids are propulsion-comparable (air vs ground)
bool static droidIntersects (const DROID *psDroid, uint32_t adjx, uint32_t adjy, const DROID *psOther)
{
	const auto self_extent =  moveDroidSizeExtent (psDroid); // cells units
	const auto other_extent = moveDroidSizeExtent (psOther); // cells units
	uint16_t self_cellx, self_celly, other_cellx, other_celly;
	world_to_cell(adjx, adjy, self_cellx, self_celly);
	world_to_cell(psOther->pos.x, psOther->pos.y, other_cellx, other_celly);
	const bool out = cells_droidIntersects(self_cellx, self_celly, self_extent, other_cellx, other_celly, other_extent);
	if (out && psDroid->player == 0)
	debug (LOG_FLOWFIELD, "collision %i VS %i: (%i %i, %i) (%i %i, %i)", psDroid->id, psOther->id,
		self_cellx, self_celly, self_extent,
		other_cellx, other_celly, other_extent);
	return out;
}

/// New (stricter) collision detection. Returns true only are actually stepping on each other
/// FIXME Bad idea, collision is when adjacent cells are touching, no overlapping needed
/// Adjx Adjy are in world coordinates
static bool collisionDroid_overlap (const DROID *psDroid, uint32_t adjx, uint32_t adjy, const DROID *psOther)
{
	const auto droidR = moveObjRadius(psDroid);
	const auto objR = moveObjRadius(psOther);
	if (isTransporter(psOther))
	{
		// ignore transporters
		return false;
	}
	if ((!isFlying(psDroid) && isFlying(psOther) && psOther->pos.z > (psDroid->pos.z + droidR)) || 
	    (!isFlying(psOther) && isFlying(psDroid) && psDroid->pos.z > (psOther->pos.z + objR)))
	{
		// ground unit can't bump into a flying saucer..
		return false;

	}
	return droidIntersects(psDroid, adjx, adjy, psOther);
}

static inline uint16_t _closest (uint16_t to, uint16_t from, uint16_t from_extent)
{
	int16_t dist = to - from;
	if (dist >= 0 && dist < from_extent) { return to; }
	else { return dist < 0 ? from : ((0xFFFF) & (from + from_extent - 1)); }
}


static Directions closestDirection (uint16_t d)
{
	static int thresholds[8] = {
		4096
		, 12288
		, 20480
		, 28672
		, 36864
		, 45056
		, 53248
		, 61440
	};
	static Directions out[8] = {
		Directions::DIR_6,
		Directions::DIR_7,
		Directions::DIR_4,
		Directions::DIR_2,
		Directions::DIR_1,
		Directions::DIR_0,
		Directions::DIR_3,
		Directions::DIR_5
	};
	for (int i = 0; i < 8; i++)
	{
		if (d < thresholds[i]) return out[i];
	}
	return Directions::DIR_6;
}

/// Returns 255 if psDroid is NOT on an adjacent cell (=there is at least one cell buffer zone).
/// Otherwise returns number where each bit indicats whether it's an allowed Direction
/// (most significant = DIR_0)
static uint8_t collisionDroid_adjacent (const DROID *psDroid, uint32_t adjx, uint32_t adjy, const DROID *psOther)
{
	const auto self_extent =  moveDroidSizeExtent (psDroid); // cells units
	const auto other_extent = moveDroidSizeExtent (psOther); // cells units
	uint16_t self_cellx, self_celly, other_cellx, other_celly;
	world_to_cell(adjx, adjy, self_cellx, self_celly);
	world_to_cell(psOther->pos.x, psOther->pos.y, other_cellx, other_celly);
	if (self_extent == 1 && other_extent == 1)
	{
		return (std::abs(self_cellx - other_cellx) == 1) && (std::abs(self_celly - other_celly) == 1);
	}
	// try to translate Other position, and check if it would overlap with us
	uint8_t does_overlap = 255;
	for (int neighb = (int) Directions::DIR_0; neighb < DIR_TO_VEC_SIZE; neighb++)
	{
		const auto moved_other_x = other_cellx + dir_neighbors[neighb][0];
		const auto moved_other_y = other_celly + dir_neighbors[neighb][1];
		if (!(IS_BETWEEN(moved_other_x, 0, CELL_X_LEN))) continue;
		if (!(IS_BETWEEN(moved_other_y, 0, CELL_Y_LEN))) continue;
		const bool overlap = cells_droidIntersects(self_cellx, self_celly, self_extent, other_cellx, other_celly, other_extent);
		// substract 1, because Directions::DIR_0 is 1
		const int shift_to_position = 9 - neighb - 1;
		does_overlap |= (1 << shift_to_position) & ((int) overlap);
	}
	// return directions which do not overlap
	return ~does_overlap;
}

static bool (*collisionFunction) (const DROID*, uint32_t, uint32_t, const DROID*) = &collisionDroid_original;
static std::vector<int> comfortField;

void toggleCollisionFunction ()
{
	if (collisionFunction == &collisionDroid_original)
	{
		collisionFunction = &collisionDroid_overlap;
	}
	else
	{
		collisionFunction = &collisionDroid_original;
	}
}
static bool _pdebug = false;
static const uint16_t DISCOMFORT_FORCE = 4;

static Directions _leastDiscomfortable2 (Directions to, uint16_t at_cellx, uint16_t at_celly, const Directions pref_dirs[], uint8_t pref_dir_sz, uint8_t free_tiles)
{
	int cur_comfort = std::numeric_limits<int32_t>::min();
	Directions best_comfort_dir = Directions::DIR_NONE;
	for (int i = 0; i < pref_dir_sz; i++)
	{
		Directions d = pref_dirs[i];
		// directions is blocked, skip
		if (! (dir_shift_pos[(int) d] & free_tiles)) continue;
		const auto dx_dy = dir_neighbors[(int) d];
		// we don't need to check bounds here, because out-of-map tiles
		// were marked as "blocked" previously, we inverted them, so only getting free tiles:
		// if a tile is free, then cell is also free, because it's smaller
		const auto neighb_idx = cells_2Dto1D(at_cellx + dx_dy[0], at_celly + dx_dy[1]);
		auto this_comfort = comfortField.at(neighb_idx);
		// Add up all discomfort fields and flowfield
		if (d == to)
		{
			// cancel out our own discomfort field (once cell ahead)
			this_comfort += DISCOMFORT_FORCE;
		}
		// only change dir if this one is strictly more comfortable
		if (this_comfort > cur_comfort)
		{
			if (_pdebug)
			{
				// debug (LOG_FLOWFIELD, "this_comfort %i in DIR_%i", this_comfort, (int) d - 1);
			}
			cur_comfort = this_comfort;
			best_comfort_dir = d;
		}
		else if (_pdebug)
		{
			// debug (LOG_FLOWFIELD, "ignored DIR_%i, comfort was %i", (int) d - 1, this_comfort);
		}
	}
	return best_comfort_dir;
}

// Given a field, and a point on that field, decide what is the cheapest move?
// - check that no static obstacles are preventing moving to preferred direction
// - then, for all free directions, choose the most comfortable one
// player is only for DEBUG
Directions leastDiscomfortable (Directions to, uint16_t at_worldx, uint16_t at_worldy, PROPULSION_TYPE propulsion, int player)
{
	ASSERT_OR_RETURN(Directions::DIR_6, to != Directions::DIR_NONE, "assumption failed");
	// check first equivalence group of 3 elements
	const auto preferences = dir_avoidance_preference[(int) to];
	ASSERT_OR_RETURN(Directions::DIR_NONE, preferences[0] == to, "assumption failed: most preferred direction must be the current one, %i != %i",
	(int) to, (int) preferences[0]);
	// free from *static* obstacles only : we are trying to determine the dynamic obstacles right now!
	const uint8_t free_tiles = ~world_getImpassableArea(at_worldx, at_worldy, propulsion);
	if (free_tiles == 0) return Directions::DIR_NONE;

	uint16_t at_cellx, at_celly;
	world_to_cell(at_worldx, at_worldy, at_cellx, at_celly);
	// first group is size 3
	Directions best_comfort_dir = _leastDiscomfortable2(to, at_cellx, at_celly, preferences, 3, free_tiles);
	// if (player == 0) debug (LOG_FLOWFIELD, "gr1: free, possible: %i %i, to=DIR_%i", free_tiles, possible_tiles, (int) to - 1);
	if (best_comfort_dir != Directions::DIR_NONE) return best_comfort_dir;

	// next eq group is size 2
	best_comfort_dir = _leastDiscomfortable2(to, at_cellx, at_celly, preferences + 3, 2, free_tiles);
	// if (player == 0) debug (LOG_FLOWFIELD, "gr2: free, possible: %i %i",free_tiles, possible_tiles);
	if (best_comfort_dir != Directions::DIR_NONE) return best_comfort_dir;
	if (dir_is_diagonal[(int) to])
	{
		// diagonal: next group is size 2
		best_comfort_dir = _leastDiscomfortable2(to, at_cellx, at_celly, preferences + 5, 2, free_tiles);
		if (best_comfort_dir != Directions::DIR_NONE) return best_comfort_dir;
		// lonely last dir
		best_comfort_dir = _leastDiscomfortable2(to, at_cellx, at_celly, preferences + 7, 1, free_tiles);
		if (best_comfort_dir != Directions::DIR_NONE) return best_comfort_dir;
	}
	else
	{
		// straight: next group is size 1, lonely dir
		best_comfort_dir = _leastDiscomfortable2(to, at_cellx, at_celly, preferences + 5, 1, free_tiles);
		if (best_comfort_dir != Directions::DIR_NONE) return best_comfort_dir;
		// last eqiv group
		best_comfort_dir = _leastDiscomfortable2(to, at_cellx, at_celly, preferences + 6, 2, free_tiles);
		if (best_comfort_dir != Directions::DIR_NONE) return best_comfort_dir;
	}
	// TODO: everything is blocked, just stop I guess 
	return Directions::DIR_NONE;	
}

static void moveGetDroidPosDiffs(DROID *psDroid, int32_t *pDX, int32_t *pDY)
{
	int32_t move = psDroid->sMove.speed * EXTRA_PRECISION;  // high precision

	*pDX = iSinR(psDroid->sMove.moveDir, move);
	*pDY = iCosR(psDroid->sMove.moveDir, move);
}

/// Droid collision detection (against other droids)
// see if a droid has run into another droid. Modifiy its movement vector to avoid overlapping
// pmx, pmy : components of velocity
static void moveCalcDroidSlide(DROID *psDroid, int *pmx, int *pmy)
{
	int32_t		droidR, spmx, spmy;
	bool        bLegs;
	ASSERT_OR_RETURN(, psDroid != nullptr, "Bad droid");
	CHECK_DROID(psDroid);

	bLegs = false;
	if (psDroid->droidType == DROID_PERSON || cyborgDroid(psDroid))
	{
		bLegs = true;
	}
	spmx = gameTimeAdjustedAverage(*pmx, EXTRA_PRECISION);
	spmy = gameTimeAdjustedAverage(*pmy, EXTRA_PRECISION);
	uint32_t adjx = psDroid->pos.x + spmx;
	uint32_t adjy = psDroid->pos.y + spmy;
	// if (psDroid->player == 0) debug (LOG_FLOWFIELD, "params: %i %i %i %i", *pmx, *pmy, spmx, spmy);
	droidR = moveObjRadius((BASE_OBJECT *) psDroid);
	BASE_OBJECT *psObst = nullptr;
	static GridList gridList;  // static to avoid allocations.
	static const int steerAwayRadius = TILE_UNITS * 3;
	// static const int steerAwayRadiusCells = 8;
	const uint16_t self_dir = psDroid->sMove.moveDir;
	const Directions approx_dir = closestDirection(self_dir);
	PROPULSION_STATS *psPropStats = asPropulsionStats + psDroid->asBits[COMP_PROPULSION];
	PROPULSION_TYPE propulsion = psPropStats->propulsionType;
	// the closer we are to obstacle, the greater discomfort is
	// static const uint16_t discomforts[9] = {65535, 57344, 49152, 40960, 32768, 24576, 16384, 8192, 0};
	gridList = gridStartIterate(psDroid->pos.x, psDroid->pos.y, steerAwayRadius);
	for (GridIterator gi = gridList.begin(); gi != gridList.end(); ++gi)
	{
		BASE_OBJECT *psObj = *gi;
		if (psObj->id == psDroid->id) continue;
		if (psObj->died)
		{
			ASSERT(psObj->type < OBJ_NUM_TYPES, "Bad pointer! type=%u", psObj->type);
			continue;
		}
		// TODO: also check for structures, and recalculate flowfield when unknown obstacles are detected
		if (psObj->type != OBJ_DROID) continue;
		DROID * psOther = static_cast<DROID*> (psObj);
		const auto droidR = moveObjRadius(psDroid);
		const auto objR = moveObjRadius(psOther);
		if (isTransporter(psOther))
		{
			// ignore transporters
			continue;
		}
		if ((!isFlying(psDroid) && isFlying(psOther) && psOther->pos.z > (psDroid->pos.z + droidR)) || 
		    (!isFlying(psOther) && isFlying(psDroid) && psDroid->pos.z > (psOther->pos.z + objR)))
		{
			// ground unit can't bump into a flying saucer..
			continue;
		}
		auto rad = droidR + objR;
		auto radSq = rad * rad;
		// Calculate the vector from obstruction to ourselves
		const int32_t xdiff = adjx - psOther->pos.x;
		const int32_t ydiff = adjy - psOther->pos.y;
		const auto distSq = xdiff * xdiff + ydiff * ydiff;
		// calculate what direction we are moving wrt obstacle
		// if moving in same direction as vector pointing from obstacle to us (>=0),
		// then our movement should be left inchanged
		const int dotMvObstCoeff = (*pmx) * xdiff + (*pmy) * ydiff >= 0 ? 1 : -1;
		if (dotMvObstCoeff > 0) continue;
		const bool riskOverlapping = radSq > distSq;
		if (riskOverlapping)
		{
			// collision detected
			// "hard" avoidance: prevent overlapping at all cost, will be handled below
			psObst = psOther;
			// continue;
		}
		// TODO: if not moving, then skip steering
		// first, get discretized direction
		if (psDroid->player == 0) _pdebug = true;
		const auto preferred_dir = leastDiscomfortable(approx_dir, adjx, adjy, propulsion, psDroid->player);
		psDroid->sMove.moveDir = dir_to_moveDir[(int) preferred_dir];
		moveGetDroidPosDiffs(psDroid, pmx, pmy);
		_pdebug = false;
		if (psDroid->player == 0 && approx_dir != preferred_dir) debug (LOG_FLOWFIELD, "pref dir was: DIR_%i, moving %i", (int) preferred_dir - 1, (int) approx_dir - 1);

		#if 0
		// "soft" avoidance: hint our droid to steer away from dynamic obstacles
		// check that we are in this droid's gravity pool
		uint16_t other_cellx, other_celly, self_cellx, self_celly;
		int diff_cellx, diff_celly, max_diff_cell;
		world_to_cell(psOther->pos.x, psOther->pos.y, other_cellx, other_celly);
		world_to_cell(adjx, adjy, self_cellx, self_celly);
		// first, calculate how many cells away we are from the obstacle
		diff_cellx = self_cellx - other_cellx;
		diff_celly = self_celly - other_celly;
		max_diff_cell = std::max(std::abs(diff_cellx), std::abs(diff_celly));
		// too far to make decisions
		
		if (max_diff_cell >= 9) continue;
//		ASSERT_OR_RETURN(, max_diff_cell <= 8, "wrong assumptions??");
		// ok now there is a risk that we could enter discomfort zone
		// check where are we heading, and choose the least discomfortable direction
		const auto discomfort = discomforts[max_diff_cell];


		
		// first, calculate linear interpolation of how far is obstacle compared
		// max possible steerawayradius
		const auto angle = iAtan2({xdiff, ydiff});
		const auto projx = iSin(angle);
		const auto projy = iCos(angle);
		// the closer this number is to 65536, the further is obstacle
		int ratio = (int64_t)65536 * (distSq - radSq) / (steerAwayRadius * steerAwayRadius);
		// invert it, because repulsion gets stronger when obstacle is nearer
		const auto repulsion = 65536 - ratio;
		// scale to angle between ourself and obstacle,
		// because repulsion is stronger when x/y projection is closer to 1
		// repulsion is a relative force: the sum of all repulsions can, at worst, only nullify droid's speed
		// but never push it in opposite direction
		const auto ratiox = (int64_t)repulsion * projx / 65536;
		const auto ratioy = (int64_t)repulsion * projy / 65536;
		// parts which gets nullified
		const uint32_t repulsion_dx = (int64_t)ratiox * (*pmx)  / 65536;
		const uint32_t repulsion_dy = (int64_t)ratioy * (*pmy)  / 65536;
		const auto distsq_cells = distSq / (FF_UNIT * FF_UNIT);
		//const auto npmx = *pmx + repulsion_dx;
		//const auto npmy = *pmy + repulsion_dy;
		// repulsion: 5052 4781, rx ry 20558 -42700, adjxy 4964 13790, other 4925 13871
		#if 0
		if (psDroid->player == 0)
			debug (LOG_FLOWFIELD, "repulsion against %i: %i %i (was %i %i), ratio %i %li %li, adjxy %i %i, obst %i %i, %i",
			psOther->id, repulsion_dx, repulsion_dy, *pmx, *pmy, ratio, ratiox, ratioy, adjx, adjy, xdiff, ydiff, distsq_cells);
		#endif
		*pmx = (*pmx) - repulsion_dx;
		*pmy = (*pmy) - repulsion_dy;
		#endif
		
	}
	
	
	// TODO: unify with above
	#if 0
	gridList = gridStartIterate(psDroid->pos.x, psDroid->pos.y, TILE_UNITS);
	for (GridIterator gi = gridList.begin(); gi != gridList.end(); ++gi)
	{
		BASE_OBJECT *psObj = *gi;
		if (psObj->id == psDroid->id) continue;
		if (psObj->died)
		{
			ASSERT(psObj->type < OBJ_NUM_TYPES, "Bad pointer! type=%u", psObj->type);
			continue;
		}
		
		if (psObj->type != OBJ_DROID) continue;
		DROID * psObjcast = static_cast<DROID*> (psObj);
		if (!collisionFunction(psDroid, adjx, adjy, psObjcast)) continue;
		if (psDroid->player == 0) debug (LOG_FLOWFIELD, "found obstacle droid %i collides with %i", psDroid->id, psObj->id);
		//droidRepulse(psDroid, adjx, adjy, psObst, pmx, pmy);
		#if 1
		//uint8_t allowed_dirs = collisionDroid_adjacent(psDroid, adjx, adjy, psObjcast);
		//if (allowed_dirs == 255) continue;
		if (psObst != nullptr)
		{
			// hit more than one droid - stop
			*pmx = 0;
			*pmy = 0;
			psObst = nullptr;
			break;
		}	
		else
		{
			// first collision with this particular droid detected
			psObst = psObj;
		}
		#endif
	}
	#endif 
	if (psObst != nullptr)
	{
		// Try to slide round it
		// moveCalcSlideVector(psDroid, psObst->pos.x, psObst->pos.y, pmx, pmy);
		// debug (LOG_FLOWFIELD, "get repulsion");
		
	}
	
	CHECK_DROID(psDroid);
}

/// Droid collision avoidance
// get an obstacle avoidance vector
static Vector2i moveGetObstacleVector(DROID *psDroid, Vector2i dest)
{
	int32_t                 numObst = 0, distTot = 0;
	Vector2i                dir(0, 0);
	PROPULSION_STATS       *psPropStats = asPropulsionStats + psDroid->asBits[COMP_PROPULSION];
	ASSERT_OR_RETURN(dir, psPropStats, "invalid propulsion stats pointer");

	int ourMaxSpeed = psPropStats->maxSpeed;
	int ourRadius = moveObjRadius(psDroid);
	if (ourMaxSpeed == 0)
	{
		return dest;  // No point deciding which way to go, if we can't move...
	}

	// scan the neighbours for obstacles
	static GridList gridList;  // static to avoid allocations.
	gridList = gridStartIterate(psDroid->pos.x, psDroid->pos.y, AVOID_DIST);
	for (GridIterator gi = gridList.begin(); gi != gridList.end(); ++gi)
	{
		if (*gi == psDroid)
		{
			continue;  // Don't try to avoid ourselves.
		}

		DROID *psObstacle = castDroid(*gi);
		if (psObstacle == nullptr)
		{
			// Object wrong type to worry about.
			continue;
		}

		// vtol droids only avoid each other and don't affect ground droids
		if (isVtolDroid(psDroid) != isVtolDroid(psObstacle))
		{
			continue;
		}

		if (isTransporter(psObstacle) ||
		    (psObstacle->droidType == DROID_PERSON &&
		     psObstacle->player != psDroid->player))
		{
			// don't avoid people on the other side - run over them
			continue;
		}

		PROPULSION_STATS *obstaclePropStats = asPropulsionStats + psObstacle->asBits[COMP_PROPULSION];
		int obstacleMaxSpeed = obstaclePropStats->maxSpeed;
		int obstacleRadius = moveObjRadius(psObstacle);
		int totalRadius = ourRadius + obstacleRadius;

		// Try to guess where the obstacle will be when we get close.
		// Velocity guess 1: Guess the velocity the droid is actually moving at.
		Vector2i obstVelocityGuess1 = iSinCosR(psObstacle->sMove.moveDir, psObstacle->sMove.speed);
		// Velocity guess 2: Guess the velocity the droid wants to move at.
		Vector2i obstTargetDiff = psObstacle->sMove.target - psObstacle->pos.xy();
		Vector2i obstVelocityGuess2 = iSinCosR(iAtan2(obstTargetDiff), obstacleMaxSpeed * std::min(iHypot(obstTargetDiff), AVOID_DIST) / AVOID_DIST);
		if (moveBlocked(psObstacle))
		{
			obstVelocityGuess2 = Vector2i(0, 0);  // This obstacle isn't going anywhere, even if it wants to.
			//obstVelocityGuess2 = -obstVelocityGuess2;
		}
		// Guess the average of the two guesses.
		Vector2i obstVelocityGuess = (obstVelocityGuess1 + obstVelocityGuess2) / 2;

		// Find the guessed obstacle speed and direction, clamped to half our speed.
		int obstSpeedGuess = std::min(iHypot(obstVelocityGuess), ourMaxSpeed / 2);
		uint16_t obstDirectionGuess = iAtan2(obstVelocityGuess);

		// Position of obstacle relative to us.
		Vector2i diff = (psObstacle->pos - psDroid->pos).xy();

		// Find very approximate position of obstacle relative to us when we get close, based on our guesses.
		Vector2i deltaDiff = iSinCosR(obstDirectionGuess, (int64_t)std::max(iHypot(diff) - totalRadius * 2 / 3, 0) * obstSpeedGuess / ourMaxSpeed);
		if (!fpathBlockingTile(map_coord(psObstacle->pos.x + deltaDiff.x), map_coord(psObstacle->pos.y + deltaDiff.y), obstaclePropStats->propulsionType))  // Don't assume obstacle can go through cliffs.
		{
			diff += deltaDiff;
		}

		if (dot(diff, dest) < 0)
		{
			// object behind
			continue;
		}

		int centreDist = std::max(iHypot(diff), 1);
		int dist = std::max(centreDist - totalRadius, 1);

		dir += diff * 65536 / (centreDist * dist);
		distTot += 65536 / dist;
		numObst += 1;
	}

	if (dir == Vector2i(0, 0) || numObst == 0)
	{
		return dest;
	}

	dir = Vector2i(dir.x / numObst, dir.y / numObst);
	distTot /= numObst;

	// Create the avoid vector
	Vector2i o(dir.y, -dir.x);
	Vector2i avoid = dot(dest, o) < 0 ? -o : o;

	// Normalise dest and avoid.
	dest = dest * 32767 / (iHypot(dest) + 1);
	avoid = avoid * 32767 / (iHypot(avoid) + 1);  // avoid.x and avoid.y are up to 65536, so we can multiply by at most 32767 here without potential overflow.

	// combine the avoid vector and the target vector
	int ratio = std::min(distTot * ourRadius / 2, 65536);

	return dest * (65536 - ratio) + avoid * ratio;
}


/*!
 * Get a direction for a droid to avoid obstacles etc.
 * \param psDroid Which droid to examine
 * \return The normalised direction vector
 */
static uint16_t moveGetDirection(DROID *psDroid)
{
	Vector2i src = psDroid->pos.xy();  // Do not want precise precision here, would overflow.
	Vector2i target = psDroid->sMove.target;
	Vector2i dest = target - src;

	// Transporters don't need to avoid obstacles, but everyone else should
	if (!isTransporter(psDroid))
	{
		dest = moveGetObstacleVector(psDroid, dest);
	}

	return iAtan2(dest);
}

/// Check if a droid has got to a way point
static bool moveReachedWayPoint(DROID *psDroid)
{
	// Calculate the vector to the droid
	const Vector2i droid = Vector2i(psDroid->pos.xy()) - psDroid->sMove.target;
	const bool last = psDroid->sMove.pathIndex == (int)psDroid->sMove.asPath.size();
	int sqprecision = last ? ((TILE_UNITS / 4) * (TILE_UNITS / 4)) : ((TILE_UNITS / 2) * (TILE_UNITS / 2));

	if (last && psDroid->sMove.bumpTime != 0)
	{
		// Make waypoint tolerance 1 tile after 0 seconds, 2 tiles after 3 seconds, X tiles after (X + 1)² seconds.
		sqprecision = (gameTime - psDroid->sMove.bumpTime + GAME_TICKS_PER_SEC) * (TILE_UNITS * TILE_UNITS / GAME_TICKS_PER_SEC);
	}

	// Else check current waypoint
	return dot(droid, droid) < sqprecision;
}

#define MAX_SPEED_PITCH  60

/** Calculate the new speed for a droid based on factors like pitch.
 *  @todo Remove hack for steep slopes not properly marked as blocking on some maps.
 */
SDWORD moveCalcDroidSpeed(DROID *psDroid)
{
	const uint16_t		maxPitch = DEG(MAX_SPEED_PITCH);
	UDWORD			mapX, mapY;
	int			speed, pitch;
	WEAPON_STATS		*psWStats;

	CHECK_DROID(psDroid);

	// NOTE: This screws up since the transporter is offscreen still (on a mission!), and we are trying to find terrainType of a tile (that is offscreen!)
	if (psDroid->droidType == DROID_SUPERTRANSPORTER && missionIsOffworld())
	{
		PROPULSION_STATS	*propulsion = asPropulsionStats + psDroid->asBits[COMP_PROPULSION];
		speed = propulsion->maxSpeed;
	}
	else
	{
		mapX = map_coord(psDroid->pos.x);
		mapY = map_coord(psDroid->pos.y);
		speed = calcDroidSpeed(psDroid->baseSpeed, terrainType(mapTile(mapX, mapY)), psDroid->asBits[COMP_PROPULSION], getDroidEffectiveLevel(psDroid));
	}


	// now offset the speed for the slope of the droid
	pitch = angleDelta(psDroid->rot.pitch);
	speed = (maxPitch - pitch) * speed / maxPitch;
	if (speed <= 10)
	{
		// Very nasty hack to deal with buggy maps, where some cliffs are
		// not properly marked as being cliffs, but too steep to drive over.
		// This confuses the heck out of the path-finding code! - Per
		speed = 10;
	}

	// stop droids that have just fired a no fire while moving weapon
	if (psDroid->numWeaps > 0)
	{
		if (psDroid->asWeaps[0].nStat > 0 && psDroid->asWeaps[0].lastFired + FOM_MOVEPAUSE > gameTime)
		{
			psWStats = asWeaponStats + psDroid->asWeaps[0].nStat;
			if (!psWStats->fireOnMove)
			{
				speed = 0;
			}
		}
	}

	// slow down shuffling VTOLs
	if (isVtolDroid(psDroid) &&
	    (psDroid->sMove.Status == MOVESHUFFLE) &&
	    (speed > MIN_END_SPEED))
	{
		speed = MIN_END_SPEED;
	}

	CHECK_DROID(psDroid);

	return speed;
}

/** Determine whether a droid has stopped moving.
 *  @return true if the droid doesn't move, false if it's moving.
 */
static bool moveDroidStopped(DROID *psDroid, SDWORD speed)
{
	if (psDroid->sMove.Status == MOVEINACTIVE && speed == 0 && psDroid->sMove.speed == 0)
	{
		return true;
	}
	else
	{
		return false;
	}
}

/*
static bool ff_moveDroidStopped (DROID *psDroid, Vector2i &v)
{
	return isZero(psDroid->sMove.physics.velocity);
}
*/

// Direction is target direction (= new "moveDir")
// iSpinAngle = DEG(psPropStats->spinAngle);
// iSpinSpeed = psDroid->baseSpeed * psPropStats->spinSpeed;
// iTurnSpeed = psDroid->baseSpeed * psPropStats->turnSpeed;
// pDroidDir = new direction
static void moveUpdateDroidDirection(DROID *psDroid, SDWORD *pSpeed, uint16_t direction,
                                     uint16_t iSpinAngle, int iSpinSpeed, int iTurnSpeed, uint16_t *pDroidDir)
{
	*pDroidDir = psDroid->rot.direction;

	// don't move if in MOVEPAUSE state
	if (psDroid->sMove.Status == MOVEPAUSE)
	{
		return;
	}

	int diff = angleDelta(direction - *pDroidDir);
	// Turn while moving - slow down speed depending on target angle so that we can turn faster
	*pSpeed = std::max<int>(*pSpeed * (iSpinAngle - abs(diff)) / iSpinAngle, 0);

	// iTurnSpeed is turn speed at max velocity, increase turn speed up to iSpinSpeed when slowing down
	int turnSpeed = std::min<int>(iTurnSpeed + int64_t(iSpinSpeed - iTurnSpeed) * abs(diff) / iSpinAngle, iSpinSpeed);

	// Calculate the maximum change in direction
	int maxChange = gameTimeAdjustedAverage(turnSpeed);

	// Move *pDroidDir towards target, by at most maxChange.
	*pDroidDir += clip(diff, -maxChange, maxChange);
}
/*
static void ff_moveUpdateDroidDirection(DROID *psDroid, Vector2i &vel, uint16_t direction,
										uint16_t iSpinAngle, int iSpinSpeed, int iTurnSpeed, uint16_t *pDroidDir)
{
	// don't move if in MOVEPAUSE state
	if (psDroid->sMove.Status == MOVEPAUSE)
	{
		return;
	}
	int diff = angleDelta(direction - *pDroidDir);
	// Turn while moving - slow down speed depending on target angle so that we can turn faster
	auto velocityFactor = std::max<int>((iSpinAngle - abs(diff)) / iSpinAngle, 0);
	multiply(vel, velocityFactor);
	// iTurnSpeed is turn speed at max velocity, increase turn speed up to iSpinSpeed when slowing down
	int turnSpeed = std::min<int>(iTurnSpeed + int64_t(iSpinSpeed - iTurnSpeed) * abs(diff) / iSpinAngle, iSpinSpeed);
	// Calculate the maximum change in direction
	int maxChange = gameTimeAdjustedAverage(turnSpeed);
	// Move *pDroidDir towards target, by at most maxChange.
	*pDroidDir += clip(diff, -maxChange, maxChange);
}*/

// Calculate current speed perpendicular to droids direction
static int moveCalcPerpSpeed(DROID *psDroid, uint16_t iDroidDir, SDWORD iSkidDecel)
{
	int adiff = angleDelta(iDroidDir - psDroid->sMove.moveDir);
	int perpSpeed = iSinR(abs(adiff), psDroid->sMove.speed);

	// decelerate the perpendicular speed
	perpSpeed = MAX(0, perpSpeed - gameTimeAdjustedAverage(iSkidDecel));
	return perpSpeed;
}


static void moveCombineNormalAndPerpSpeeds(DROID *psDroid, int fNormalSpeed, int fPerpSpeed, uint16_t iDroidDir)
{
	int16_t         adiff;
	int		relDir;
	int		finalSpeed;

	/* set current direction */
	psDroid->rot.direction = iDroidDir;

	/* set normal speed and direction if perpendicular speed is zero */
	if (fPerpSpeed == 0)
	{
		psDroid->sMove.speed = fNormalSpeed;
		psDroid->sMove.moveDir = iDroidDir;
		return;
	}

	finalSpeed = iHypot(fNormalSpeed, fPerpSpeed);

	// calculate the angle between the droid facing and movement direction
	relDir = iAtan2(fPerpSpeed, fNormalSpeed);

	// choose the finalDir on the same side as the old movement direction
	adiff = angleDelta(iDroidDir - psDroid->sMove.moveDir);

	psDroid->sMove.moveDir = adiff < 0 ? iDroidDir + relDir : iDroidDir - relDir;  // Cast wrapping intended.
	psDroid->sMove.speed = finalSpeed;
}


// Calculate the current speed in the droids normal direction
static int moveCalcNormalSpeed(DROID *psDroid, int fSpeed, uint16_t iDroidDir, SDWORD iAccel, SDWORD iDecel)
{
	uint16_t        adiff;
	int		normalSpeed;

	adiff = (uint16_t)(iDroidDir - psDroid->sMove.moveDir);  // Cast wrapping intended.
	normalSpeed = iCosR(adiff, psDroid->sMove.speed);

	if (normalSpeed < fSpeed)
	{
		// accelerate
		normalSpeed += gameTimeAdjustedAverage(iAccel);
		if (normalSpeed > fSpeed)
		{
			normalSpeed = fSpeed;
		}
	}
	else
	{
		// decelerate
		normalSpeed -= gameTimeAdjustedAverage(iDecel);
		if (normalSpeed < fSpeed)
		{
			normalSpeed = fSpeed;
		}
	}

	return normalSpeed;
}

/// Adjust speed wrt distance to target
// see if the droid is close to the final way point
static void moveCheckFinalWaypoint(DROID *psDroid, SDWORD *pSpeed)
{
	int minEndSpeed = (*pSpeed + 2) / 3;
	minEndSpeed = std::min(minEndSpeed, MIN_END_SPEED);

	// don't do this for VTOLs doing attack runs
	if (isVtolDroid(psDroid) && (psDroid->action == DACTION_VTOLATTACK))
	{
		return;
	}

	if (psDroid->sMove.Status != MOVESHUFFLE &&
	    psDroid->sMove.pathIndex == (int)psDroid->sMove.asPath.size())
	{
		Vector2i diff = psDroid->pos.xy() - psDroid->sMove.target;
		int distSq = dot(diff, diff);
		if (distSq < END_SPEED_RANGE * END_SPEED_RANGE)
		{
			*pSpeed = (*pSpeed - minEndSpeed) * distSq / (END_SPEED_RANGE * END_SPEED_RANGE) + minEndSpeed;
		}
	}
}

static void moveUpdateDroidPos(DROID *psDroid, int32_t dx, int32_t dy)
{
	CHECK_DROID(psDroid);

	if (psDroid->sMove.Status == MOVEPAUSE || isDead((BASE_OBJECT *)psDroid))
	{
		// don't actually move if the move is paused
		return;
	}
	const auto _dx = gameTimeAdjustedAverage(dx, EXTRA_PRECISION);
	const auto _dy = gameTimeAdjustedAverage(dy, EXTRA_PRECISION);
	// if (psDroid->player == 0) debug (LOG_FLOWFIELD, "speed for this frame was %i %i, %i %i", dx, dy, _dx, _dy);
	psDroid->pos.x += _dx;
	psDroid->pos.y += _dy;

	/* impact if about to go off map else update coordinates */
	if (worldOnMap(psDroid->pos.x, psDroid->pos.y) == false)
	{
		/* transporter going off-world will trigger next map, and is ok */
		ASSERT(isTransporter(psDroid), "droid trying to move off the map!");
		if (!isTransporter(psDroid))
		{
			/* dreadful last-ditch crash-avoiding hack - sort this! - GJ */
			destroyDroid(psDroid, gameTime);
			return;
		}
	}

	// lovely hack to keep transporters just on the map
	// two weeks to go and the hacks just get better !!!
	if (isTransporter(psDroid))
	{
		if (psDroid->pos.x == 0)
		{
			psDroid->pos.x = 1;
		}
		if (psDroid->pos.y == 0)
		{
			psDroid->pos.y = 1;
		}
	}
	CHECK_DROID(psDroid);
}

/* Update a tracked droids position and speed given target values */
static void moveUpdateGroundModel(DROID *psDroid, SDWORD speed, uint16_t direction)
{
	int			fPerpSpeed, fNormalSpeed;
	uint16_t                iDroidDir;
	uint16_t                slideDir = 0;
	PROPULSION_STATS	*psPropStats;
	int32_t                 spinSpeed, spinAngle, turnSpeed, dx, dy, bx, by;

	CHECK_DROID(psDroid);

	// nothing to do if the droid is stopped
	if (moveDroidStopped(psDroid, speed) == true)
	{
		return;
	}

	psPropStats = asPropulsionStats + psDroid->asBits[COMP_PROPULSION];
	spinSpeed = psDroid->baseSpeed * psPropStats->spinSpeed;
	turnSpeed = psDroid->baseSpeed * psPropStats->turnSpeed;
	spinAngle = DEG(psPropStats->spinAngle);

	moveCheckFinalWaypoint(psDroid, &speed);

	moveUpdateDroidDirection(psDroid, &speed, direction, spinAngle, spinSpeed, turnSpeed, &iDroidDir);
	fNormalSpeed = iCosR( (uint16_t)(iDroidDir - psDroid->sMove.moveDir), psDroid->sMove.speed);
	fPerpSpeed = iSinR( (uint16_t)(iDroidDir - psDroid->sMove.moveDir), psDroid->sMove.speed);
	fNormalSpeed = moveCalcNormalSpeed(psDroid, speed, iDroidDir, psPropStats->acceleration, psPropStats->deceleration);
	fPerpSpeed   = moveCalcPerpSpeed(psDroid, iDroidDir, psPropStats->skidDeceleration);

	moveCombineNormalAndPerpSpeeds(psDroid, fNormalSpeed, fPerpSpeed, iDroidDir);
	moveGetDroidPosDiffs(psDroid, &dx, &dy);
	moveOpenGates(psDroid);
	moveCheckSquished(psDroid, dx, dy);
	moveCalcDroidSlide(psDroid, &dx, &dy);
	bx = dx;
	by = dy;
	moveCalcBlockingSlide(psDroid, &bx, &by, direction, &slideDir);
	if (bx != dx || by != dy)
	{
		moveUpdateDroidDirection(psDroid, &speed, slideDir, spinAngle, psDroid->baseSpeed * DEG(1), psDroid->baseSpeed * DEG(1) / 3, &iDroidDir);
		psDroid->rot.direction = iDroidDir;
	}

	moveUpdateDroidPos(psDroid, bx, by);

	//set the droid height here so other routines can use it
	psDroid->pos.z = map_Height(psDroid->pos.x, psDroid->pos.y); //jps 21july96
	updateDroidOrientation(psDroid);
}

/* Update a persons/cyborg position and speed given target values */
static void moveUpdatePersonModel(DROID *psDroid, SDWORD speed, uint16_t direction)
{
	int			fPerpSpeed, fNormalSpeed;
	int32_t                 spinSpeed, turnSpeed, dx, dy;
	uint16_t                iDroidDir;
	uint16_t                slideDir;
	PROPULSION_STATS	*psPropStats;

	CHECK_DROID(psDroid);

	// if the droid is stopped, only make sure animations are set correctly
	if (moveDroidStopped(psDroid, speed))
	{
		if (psDroid->droidType == DROID_PERSON &&
			(psDroid->action == DACTION_ATTACK ||
			psDroid->action == DACTION_ROTATETOATTACK)
			&& psDroid->animationEvent != ANIM_EVENT_DYING
			&& psDroid->animationEvent != ANIM_EVENT_FIRING)
		{
			psDroid->timeAnimationStarted = gameTime;
			psDroid->animationEvent = ANIM_EVENT_FIRING;
		}
		else if (psDroid->animationEvent == ANIM_EVENT_ACTIVE)
		{
			psDroid->timeAnimationStarted = 0; // turn off movement animation, since we stopped
			psDroid->animationEvent = ANIM_EVENT_NONE;
		}
		return;
	}

	psPropStats = asPropulsionStats + psDroid->asBits[COMP_PROPULSION];
	spinSpeed = psDroid->baseSpeed * psPropStats->spinSpeed;
	turnSpeed = psDroid->baseSpeed * psPropStats->turnSpeed;

	moveUpdateDroidDirection(psDroid, &speed, direction, DEG(psPropStats->spinAngle), spinSpeed, turnSpeed, &iDroidDir);

	fNormalSpeed = moveCalcNormalSpeed(psDroid, speed, iDroidDir, psPropStats->acceleration, psPropStats->deceleration);

	/* people don't skid at the moment so set zero perpendicular speed */
	fPerpSpeed = 0;

	moveCombineNormalAndPerpSpeeds(psDroid, fNormalSpeed, fPerpSpeed, iDroidDir);	
	moveGetDroidPosDiffs(psDroid, &dx, &dy);
	moveOpenGates(psDroid);
	// commenting this completely removes collisions
	moveCalcDroidSlide(psDroid, &dx, &dy);

	moveCalcBlockingSlide(psDroid, &dx, &dy, direction, &slideDir);
	moveUpdateDroidPos(psDroid, dx, dy);

	//set the droid height here so other routines can use it
	psDroid->pos.z = map_Height(psDroid->pos.x, psDroid->pos.y);//jps 21july96

	/* update anim if moving */
	if (psDroid->droidType == DROID_PERSON && speed != 0 && (psDroid->animationEvent != ANIM_EVENT_ACTIVE && psDroid->animationEvent != ANIM_EVENT_DYING))
	{
		psDroid->timeAnimationStarted = gameTime;
		psDroid->animationEvent = ANIM_EVENT_ACTIVE;
	}

	CHECK_DROID(psDroid);
}

#define	VTOL_VERTICAL_SPEED		(((psDroid->baseSpeed / 4) > 60) ? ((SDWORD)psDroid->baseSpeed / 4) : 60)

/* primitive 'bang-bang' vtol height controller */
static void moveAdjustVtolHeight(DROID *psDroid, int32_t iMapHeight)
{
	int32_t	iMinHeight, iMaxHeight, iLevelHeight;
	if (isTransporter(psDroid) && !bMultiPlayer)
	{
		iMinHeight   = 2 * VTOL_HEIGHT_MIN;
		iLevelHeight = 2 * VTOL_HEIGHT_LEVEL;
		iMaxHeight   = 2 * VTOL_HEIGHT_MAX;
	}
	else
	{
		iMinHeight   = VTOL_HEIGHT_MIN;
		iLevelHeight = VTOL_HEIGHT_LEVEL;
		iMaxHeight   = VTOL_HEIGHT_MAX;
	}

	if (psDroid->pos.z >= (iMapHeight + iMaxHeight))
	{
		psDroid->sMove.iVertSpeed = (SWORD) - VTOL_VERTICAL_SPEED;
	}
	else if (psDroid->pos.z < (iMapHeight + iMinHeight))
	{
		psDroid->sMove.iVertSpeed = (SWORD)VTOL_VERTICAL_SPEED;
	}
	else if ((psDroid->pos.z < iLevelHeight) &&
	         (psDroid->sMove.iVertSpeed < 0))
	{
		psDroid->sMove.iVertSpeed = 0;
	}
	else if ((psDroid->pos.z > iLevelHeight) &&
	         (psDroid->sMove.iVertSpeed > 0))
	{
		psDroid->sMove.iVertSpeed = 0;
	}
}

static void moveUpdateVtolModel(DROID *psDroid, SDWORD speed, uint16_t direction)
{
	int fPerpSpeed, fNormalSpeed;
	uint16_t   iDroidDir;
	uint16_t   slideDir;
	int32_t spinSpeed, turnSpeed, iMapZ, iSpinSpeed, iTurnSpeed, dx, dy;
	uint16_t targetRoll;
	PROPULSION_STATS	*psPropStats;

	CHECK_DROID(psDroid);

	// nothing to do if the droid is stopped
	if (moveDroidStopped(psDroid, speed) == true)
	{
		return;
	}

	psPropStats = asPropulsionStats + psDroid->asBits[COMP_PROPULSION];
	spinSpeed = DEG(psPropStats->spinSpeed);
	turnSpeed = DEG(psPropStats->turnSpeed);

	moveCheckFinalWaypoint(psDroid, &speed);

	if (isTransporter(psDroid))
	{
		moveUpdateDroidDirection(psDroid, &speed, direction, DEG(psPropStats->spinAngle), spinSpeed, turnSpeed, &iDroidDir);
	}
	else
	{
		iSpinSpeed = std::max<int>(psDroid->baseSpeed * DEG(1) / 2, spinSpeed);
		iTurnSpeed = std::max<int>(psDroid->baseSpeed * DEG(1) / 8, turnSpeed);
		moveUpdateDroidDirection(psDroid, &speed, direction, DEG(psPropStats->spinAngle), iSpinSpeed, iTurnSpeed, &iDroidDir);
	}

	fNormalSpeed = moveCalcNormalSpeed(psDroid, speed, iDroidDir, psPropStats->acceleration, psPropStats->deceleration);
	fPerpSpeed   = moveCalcPerpSpeed(psDroid, iDroidDir, psPropStats->skidDeceleration);

	moveCombineNormalAndPerpSpeeds(psDroid, fNormalSpeed, fPerpSpeed, iDroidDir);

	moveGetDroidPosDiffs(psDroid, &dx, &dy);

	/* set slide blocking tile for map edge */
	if (!isTransporter(psDroid))
	{
		moveCalcBlockingSlide(psDroid, &dx, &dy, direction, &slideDir);
	}

	moveUpdateDroidPos(psDroid, dx, dy);

	/* update vtol orientation */
	targetRoll = clip(4 * angleDelta(psDroid->sMove.moveDir - psDroid->rot.direction), -DEG(60), DEG(60));
	psDroid->rot.roll = psDroid->rot.roll + (uint16_t)gameTimeAdjustedIncrement(3 * angleDelta(targetRoll - psDroid->rot.roll));

	/* do vertical movement - only if on the map */
	if (worldOnMap(psDroid->pos.x, psDroid->pos.y))
	{
		iMapZ = map_Height(psDroid->pos.x, psDroid->pos.y);
		psDroid->pos.z = MAX(iMapZ, psDroid->pos.z + gameTimeAdjustedIncrement(psDroid->sMove.iVertSpeed));
		moveAdjustVtolHeight(psDroid, iMapZ);
	}
}

static void moveUpdateCyborgModel(DROID *psDroid, SDWORD moveSpeed, uint16_t moveDir, UBYTE oldStatus)
{
	CHECK_DROID(psDroid);

	// nothing to do if the droid is stopped
	if (moveDroidStopped(psDroid, moveSpeed))
	{
		if (psDroid->animationEvent == ANIM_EVENT_ACTIVE)
		{
			psDroid->timeAnimationStarted = 0;
			psDroid->animationEvent = ANIM_EVENT_NONE;
		}
		return;
	}

	if (psDroid->animationEvent == ANIM_EVENT_NONE)
	{
		psDroid->timeAnimationStarted = gameTime;
		psDroid->animationEvent = ANIM_EVENT_ACTIVE;
	}

	/* use baba person movement */
	moveUpdatePersonModel(psDroid, moveSpeed, moveDir);

	psDroid->rot.pitch = 0;
	psDroid->rot.roll  = 0;
}

static void moveDescending(DROID *psDroid)
{
	int32_t iMapHeight = map_Height(psDroid->pos.x, psDroid->pos.y);

	psDroid->sMove.speed = 0;

	if (psDroid->pos.z > iMapHeight)
	{
		/* descending */
		psDroid->sMove.iVertSpeed = (SWORD) - VTOL_VERTICAL_SPEED;
	}
	else
	{
		/* on floor - stop */
		psDroid->pos.z = iMapHeight;
		psDroid->sMove.iVertSpeed = 0;

		/* reset move state */
		psDroid->sMove.Status = MOVEINACTIVE;

		/* conform to terrain */
		updateDroidOrientation(psDroid);
	}
}


bool moveCheckDroidMovingAndVisible(void *psObj)
{
	DROID	*psDroid = (DROID *)psObj;

	if (psDroid == nullptr)
	{
		return false;
	}

	/* check for dead, not moving or invisible to player */
	if (psDroid->died || moveDroidStopped(psDroid, 0) ||
	    (isTransporter(psDroid) && psDroid->order.type == DORDER_NONE) ||
	    !(psDroid->visibleForLocalDisplay()))
	{
		psDroid->iAudioID = NO_SOUND;
		return false;
	}

	return true;
}


static void movePlayDroidMoveAudio(DROID *psDroid)
{
	SDWORD				iAudioID = NO_SOUND;
	PROPULSION_TYPES	*psPropType;
	UBYTE				iPropType = 0;

	ASSERT_OR_RETURN(, psDroid != nullptr, "Unit pointer invalid");

	if ((psDroid != nullptr) &&
	    (psDroid->visibleForLocalDisplay()))
	{
		PROPULSION_STATS *psPropStats = asPropulsionStats + psDroid->asBits[COMP_PROPULSION];
		ASSERT_OR_RETURN(, psPropStats != nullptr, "Invalid propulsion stats pointer");
		iPropType = asPropulsionStats[(psDroid)->asBits[COMP_PROPULSION]].propulsionType;
		psPropType = &asPropulsionTypes[iPropType];

		/* play specific wheeled and transporter or stats-specified noises */
		if (iPropType == PROPULSION_TYPE_WHEELED && psDroid->droidType != DROID_CONSTRUCT)
		{
			iAudioID = ID_SOUND_TREAD;
		}
		else if (isTransporter(psDroid))
		{
			iAudioID = ID_SOUND_BLIMP_FLIGHT;
		}
		else if (iPropType == PROPULSION_TYPE_LEGGED && cyborgDroid(psDroid))
		{
			iAudioID = ID_SOUND_CYBORG_MOVE;
		}
		else
		{
			iAudioID = psPropType->moveID;
		}

		if (iAudioID != NO_SOUND)
		{
			if (audio_PlayObjDynamicTrack(psDroid, iAudioID,
			                              moveCheckDroidMovingAndVisible))
			{
				psDroid->iAudioID = iAudioID;
			}
		}
	}
}


static bool moveDroidStartCallback(void *psObj)
{
	DROID *psDroid = (DROID *)psObj;

	if (psDroid == nullptr)
	{
		return false;
	}

	movePlayDroidMoveAudio(psDroid);

	return true;
}


static void movePlayAudio(DROID *psDroid, bool bStarted, bool bStoppedBefore, SDWORD iMoveSpeed)
{
	UBYTE				propType;
	PROPULSION_STATS	*psPropStats;
	PROPULSION_TYPES	*psPropType;
	bool				bStoppedNow;
	SDWORD				iAudioID = NO_SOUND;
	AUDIO_CALLBACK		pAudioCallback = nullptr;

	/* get prop stats */
	psPropStats = asPropulsionStats + psDroid->asBits[COMP_PROPULSION];
	ASSERT_OR_RETURN(, psPropStats != nullptr, "Invalid propulsion stats pointer");
	propType = psPropStats->propulsionType;
	psPropType = &asPropulsionTypes[propType];

	/* get current droid motion status */
	bStoppedNow = moveDroidStopped(psDroid, iMoveSpeed);

	if (bStarted)
	{
		/* play start audio */
		if ((propType == PROPULSION_TYPE_WHEELED && psDroid->droidType != DROID_CONSTRUCT)
		    || psPropType->startID == NO_SOUND)
		{
			movePlayDroidMoveAudio(psDroid);
			return;
		}
		else if (isTransporter(psDroid))
		{
			iAudioID = ID_SOUND_BLIMP_TAKE_OFF;
		}
		else
		{
			iAudioID = psPropType->startID;
		}

		pAudioCallback = moveDroidStartCallback;
	}
	else if (!bStoppedBefore && bStoppedNow &&
	         (psPropType->shutDownID != NO_SOUND))
	{
		/* play stop audio */
		if (isTransporter(psDroid))
		{
			iAudioID = ID_SOUND_BLIMP_LAND;
		}
		else if (propType != PROPULSION_TYPE_WHEELED || psDroid->droidType == DROID_CONSTRUCT)
		{
			iAudioID = psPropType->shutDownID;
		}
	}
	else if (!bStoppedBefore && !bStoppedNow && psDroid->iAudioID == NO_SOUND)
	{
		/* play move audio */
		movePlayDroidMoveAudio(psDroid);
		return;
	}

	if ((iAudioID != NO_SOUND) &&
	    (psDroid->visibleForLocalDisplay()))
	{
		if (audio_PlayObjDynamicTrack(psDroid, iAudioID,
		                              pAudioCallback))
		{
			psDroid->iAudioID = iAudioID;
		}
	}
}


static bool pickupOilDrum(int toPlayer, int fromPlayer)
{
	unsigned int power = OILDRUM_POWER;

	if (!bMultiPlayer && !bInTutorial)
	{
		// Let Beta and Gamma campaign oil drums give a little more power
		if (getCampaignNumber() == 2)
		{
			power = OILDRUM_POWER + (OILDRUM_POWER / 2);
		}
		else if (getCampaignNumber() == 3)
		{
			power = OILDRUM_POWER * 2;
		}
	}

	addPower(toPlayer, power);  // give power

	if (toPlayer == selectedPlayer)
	{
		CONPRINTF(_("You found %u power in an oil drum."), power);
	}

	return true;
}

// called when a droid moves to a new tile.
// use to pick up oil, etc..
static void checkLocalFeatures(DROID *psDroid)
{
	// NOTE: Why not do this for AI units also?
	if ((!isHumanPlayer(psDroid->player) && psDroid->order.type != DORDER_RECOVER) || isVtolDroid(psDroid) || isTransporter(psDroid))  // VTOLs or transporters can't pick up features!
	{
		return;
	}

	// scan the neighbours
#define DROIDDIST ((TILE_UNITS*5)/2)
	static GridList gridList;  // static to avoid allocations.
	gridList = gridStartIterate(psDroid->pos.x, psDroid->pos.y, DROIDDIST);
	for (GridIterator gi = gridList.begin(); gi != gridList.end(); ++gi)
	{
		BASE_OBJECT *psObj = *gi;
		bool pickedUp = false;

		if (psObj->type == OBJ_FEATURE && !psObj->died)
		{
			switch (((FEATURE *)psObj)->psStats->subType)
			{
			case FEAT_OIL_DRUM:
				pickedUp = pickupOilDrum(psDroid->player, psObj->player);
				triggerEventPickup((FEATURE *)psObj, psDroid);
				break;
			case FEAT_GEN_ARTE:
				pickedUp = pickupArtefact(psDroid->player, psObj->player);
				triggerEventPickup((FEATURE *)psObj, psDroid);
				break;
			default:
				break;
			}
		}

		if (!pickedUp)
		{
			// Object is not a living oil drum or artefact.
			continue;
		}

		turnOffMultiMsg(true);
		removeFeature((FEATURE *)psObj);  // remove artifact+.
		turnOffMultiMsg(false);
	}
}
/** For debugging only */
// ================== moveUpdateDroid_original START ==================
void moveUpdateDroid_original(DROID *psDroid)
{
	UDWORD				oldx, oldy;
	UBYTE				oldStatus = psDroid->sMove.Status;
	SDWORD				moveSpeed;
	uint16_t			moveDir;
	PROPULSION_STATS	*psPropStats;
	Vector3i 			pos(0, 0, 0);
	bool				bStarted = false, bStopped;

	CHECK_DROID(psDroid);

	psPropStats = asPropulsionStats + psDroid->asBits[COMP_PROPULSION];
	ASSERT_OR_RETURN(, psPropStats != nullptr, "Invalid propulsion stats pointer");

	// If the droid has been attacked by an EMP weapon, it is temporarily disabled
	if (psDroid->lastHitWeapon == WSC_EMP)
	{
		if (gameTime - psDroid->timeLastHit < EMP_DISABLE_TIME)
		{
			// Get out without updating
			return;
		}
	}

	/* save current motion status of droid */
	bStopped = moveDroidStopped(psDroid, 0);

	moveSpeed = 0;
	moveDir = psDroid->rot.direction;

	switch (psDroid->sMove.Status)
	{
	case MOVEINACTIVE:
		if (psDroid->animationEvent == ANIM_EVENT_ACTIVE)
		{
			psDroid->timeAnimationStarted = 0;
			psDroid->animationEvent = ANIM_EVENT_NONE;
		}
		break;
	case MOVESHUFFLE:
		if (moveReachedWayPoint(psDroid) || (psDroid->sMove.shuffleStart + MOVE_SHUFFLETIME) < gameTime)
		{
			if (psPropStats->propulsionType == PROPULSION_TYPE_LIFT)
			{
				psDroid->sMove.Status = MOVEHOVER;
			}
			else
			{
				psDroid->sMove.Status = MOVEINACTIVE;
			}
		}
		else
		{
			// Calculate a target vector
			moveDir = moveGetDirection(psDroid);

			moveSpeed = moveCalcDroidSpeed(psDroid);
		}
		break;
	case MOVEWAITROUTE:
		moveDroidTo(psDroid, psDroid->sMove.destination.x, psDroid->sMove.destination.y);
		moveSpeed = MAX(0, psDroid->sMove.speed - 1);
		if (psDroid->sMove.Status != MOVENAVIGATE)
		{
			break;
		}
		// fallthrough
	case MOVENAVIGATE:
		// Get the next control point
		if (!moveNextTarget(psDroid))
		{
			// No more waypoints - finish
			if (psPropStats->propulsionType == PROPULSION_TYPE_LIFT)
			{
				psDroid->sMove.Status = MOVEHOVER;
			}
			else
			{
				psDroid->sMove.Status = MOVEINACTIVE;
			}
			break;
		}

		if (isVtolDroid(psDroid))
		{
			psDroid->rot.pitch = 0;
		}

		psDroid->sMove.Status = MOVEPOINTTOPOINT;
		psDroid->sMove.bumpTime = 0;
		moveSpeed = MAX(0, psDroid->sMove.speed - 1);

		/* save started status for movePlayAudio */
		if (psDroid->sMove.speed == 0)
		{
			bStarted = true;
		}
		// fallthrough
	case MOVEPOINTTOPOINT:
	case MOVEPAUSE:
		// moving between two way points
		if (psDroid->sMove.asPath.size() == 0)
		{
			debug(LOG_WARNING, "No path to follow, but psDroid->sMove.Status = %d", psDroid->sMove.Status);
		}

		// Get the best control point.
		if (psDroid->sMove.asPath.size() == 0 || !moveBestTarget(psDroid))
		{
			// Got stuck somewhere, can't find the path.
			moveDroidTo(psDroid, psDroid->sMove.destination.x, psDroid->sMove.destination.y);
		}

		// See if the target point has been reached
		if (moveReachedWayPoint(psDroid))
		{
			// Got there - move onto the next waypoint
			if (!moveNextTarget(psDroid))
			{
				// No more waypoints - finish
				if (psPropStats->propulsionType == PROPULSION_TYPE_LIFT)
				{
					// check the location for vtols
					Vector2i tar = psDroid->pos.xy();
					if (psDroid->order.type != DORDER_PATROL && psDroid->order.type != DORDER_CIRCLE  // Not doing an order which means we never land (which means we might want to land).
					    && psDroid->action != DACTION_MOVETOREARM && psDroid->action != DACTION_MOVETOREARMPOINT
					    && actionVTOLLandingPos(psDroid, &tar)  // Can find a sensible place to land.
					    && map_coord(tar) != map_coord(psDroid->sMove.destination))  // We're not at the right place to land.
					{
						psDroid->sMove.destination = tar;
						moveDroidTo(psDroid, psDroid->sMove.destination.x, psDroid->sMove.destination.y);
					}
					else
					{
						psDroid->sMove.Status = MOVEHOVER;
					}
				}
				else
				{
					// this is equivalent to MOVEINACTIVE for non-lift droids
					psDroid->sMove.Status = MOVETURN;
				}
				objTrace(psDroid->id, "Arrived at destination!");
				break;
			}
		}

		moveDir = moveGetDirection(psDroid);
		moveSpeed = moveCalcDroidSpeed(psDroid);

		if ((psDroid->sMove.bumpTime != 0) &&
		    (psDroid->sMove.pauseTime + psDroid->sMove.bumpTime + BLOCK_PAUSETIME < gameTime))
		{
			if (psDroid->sMove.Status == MOVEPOINTTOPOINT)
			{
				psDroid->sMove.Status = MOVEPAUSE;
			}
			else
			{
				psDroid->sMove.Status = MOVEPOINTTOPOINT;
			}
			psDroid->sMove.pauseTime = (UWORD)(gameTime - psDroid->sMove.bumpTime);
		}

		if ((psDroid->sMove.Status == MOVEPAUSE) &&
		    (psDroid->sMove.bumpTime != 0) &&
		    (psDroid->sMove.lastBump > psDroid->sMove.pauseTime) &&
		    (psDroid->sMove.lastBump + psDroid->sMove.bumpTime + BLOCK_PAUSERELEASE < gameTime))
		{
			psDroid->sMove.Status = MOVEPOINTTOPOINT;
		}

		break;
	case MOVETURN:
		// Turn the droid to it's final facing
		if (psPropStats->propulsionType == PROPULSION_TYPE_LIFT)
		{
			psDroid->sMove.Status = MOVEPOINTTOPOINT;
		}
		else
		{
			psDroid->sMove.Status = MOVEINACTIVE;
		}
		break;
	case MOVETURNTOTARGET:
		moveSpeed = 0;
		moveDir = iAtan2(psDroid->sMove.target - psDroid->pos.xy());
		break;
	case MOVEHOVER:
		moveDescending(psDroid);
		break;

	default:
		ASSERT(false, "unknown move state");
		return;
		break;
	}

	// Update the movement model for the droid
	oldx = psDroid->pos.x;
	oldy = psDroid->pos.y;

	if (psDroid->droidType == DROID_PERSON)
	{
		moveUpdatePersonModel(psDroid, moveSpeed, moveDir);
	}
	else if (cyborgDroid(psDroid))
	{
		moveUpdateCyborgModel(psDroid, moveSpeed, moveDir, oldStatus);
	}
	else if (psPropStats->propulsionType == PROPULSION_TYPE_LIFT)
	{
		moveUpdateVtolModel(psDroid, moveSpeed, moveDir);
	}
	else
	{
		moveUpdateGroundModel(psDroid, moveSpeed, moveDir);
	}

	if (map_coord(oldx) != map_coord(psDroid->pos.x)
	    || map_coord(oldy) != map_coord(psDroid->pos.y))
	{
		visTilesUpdate((BASE_OBJECT *)psDroid);

		// object moved from one tile to next, check to see if droid is near stuff.(oil)
		checkLocalFeatures(psDroid);

		triggerEventDroidMoved(psDroid, oldx, oldy);
	}

	// See if it's got blocked
	if ((psPropStats->propulsionType != PROPULSION_TYPE_LIFT) && moveBlocked(psDroid))
	{
		objTrace(psDroid->id, "status: id %d blocked", (int)psDroid->id);
		psDroid->sMove.Status = MOVETURN;
	}

//	// If were in drive mode and the droid is a follower then stop it when it gets within
//	// range of the driver.
//	if(driveIsFollower(psDroid)) {
//		if(DoFollowRangeCheck) {
//			if(driveInDriverRange(psDroid)) {
//				psDroid->sMove.Status = MOVEINACTIVE;
////				ClearFollowRangeCheck = true;
//			} else {
//				AllInRange = false;
//			}
//		}
//	}

	/* If it's sitting in water then it's got to go with the flow! */
	if (worldOnMap(psDroid->pos.x, psDroid->pos.y) && terrainType(mapTile(map_coord(psDroid->pos.x), map_coord(psDroid->pos.y))) == TER_WATER)
	{
		updateDroidOrientation(psDroid);
	}

	if (psDroid->sMove.Status == MOVETURNTOTARGET && psDroid->rot.direction == moveDir)
	{
		if (psPropStats->propulsionType == PROPULSION_TYPE_LIFT)
		{
			psDroid->sMove.Status = MOVEPOINTTOPOINT;
		}
		else
		{
			psDroid->sMove.Status = MOVEINACTIVE;
		}
		objTrace(psDroid->id, "MOVETURNTOTARGET complete");
	}

	if (psDroid->periodicalDamageStart != 0 && psDroid->droidType != DROID_PERSON && psDroid->visibleForLocalDisplay()) // display-only check for adding effect
	{
		pos.x = psDroid->pos.x + (18 - rand() % 36);
		pos.z = psDroid->pos.y + (18 - rand() % 36);
		pos.y = psDroid->pos.z + (psDroid->sDisplay.imd->max.y / 3);
		addEffect(&pos, EFFECT_EXPLOSION, EXPLOSION_TYPE_SMALL, false, nullptr, 0, gameTime - deltaGameTime + 1);
	}

	movePlayAudio(psDroid, bStarted, bStopped, moveSpeed);
	ASSERT(droidOnMap(psDroid), "%s moved off map (%u, %u)->(%u, %u)", droidGetName(psDroid), oldx, oldy, (UDWORD)psDroid->pos.x, (UDWORD)psDroid->pos.y);
	CHECK_DROID(psDroid);
}

// ================== moveUpdateDroid_original END ==================
Vector2i ff_iNormalize(Vector2i v)
{
	int32_t magnitude = iSqrt(v.x * v.x + v.y * v.y);
	return {v.x / magnitude, v.y / magnitude};
}

Vector2i ff_iLimit(Vector2i v, int32_t limit)
{
	return (ff_iNormalize(v) * limit);
}

// called every cycle
void ff_update(DROID &droid)
{
	droid.sMove.physics.velocity += droid.sMove.physics.acceleration;
	// TODO cap velocity to some ceiling
	Vector2i &wV = droid.sMove.physics.velocity;
	droid.pos = {droid.pos.x + wV.x, droid.pos.y + wV.y, droid.pos.z};
	droid.sMove.physics.acceleration = {0, 0};
	if (droid.player == 0)
	{
	  debug(LOG_FLOWFIELD, "physics: %i %i", wV.x, wV.y);
	}
	
}
// called when needed
void ff_applyForce(DROID &droid, Vector2f force)
{
	droid.sMove.physics.acceleration = force; // (force / droid.sMove.physics.mass);
}

/// Adds discomfort only at where obstacle is located, and that's it.
/// useful for currently immobile dynamic obstacles
static inline void cells_addDynamicObstacle(uint16_t cellx, uint16_t celly, uint8_t cell_radius)
{
	// the cell right under dynamic obstacle have highest discomfort
	static const uint16_t dynamic_obstacle = 65535;
	for (int dy = 0; dy < cell_radius - 1; dy++)
		for (int dx = 0; dx < cell_radius - 1; dx++)
	{
		comfortField.at(cells_2Dto1D(cellx + dx, celly + dy)) = -dynamic_obstacle;		
	}
}

void cells_addDynamicObstacleArea_straight(uint16_t cellx, uint16_t celly, uint8_t cell_radius, Directions dir)
{
	#define FORCE(df) -(DISCOMFORT_FORCE + 1 - df)
	switch(dir)
	{
		case Directions::DIR_1:
		{
			for (int df = 1; df < DISCOMFORT_FORCE + 1; df++)
			for (int dx = 0; dx < cell_radius; dx++)
			{
				if (!(IS_BETWEEN(cellx + dx, 0, CELL_X_LEN))) continue;
				if (!(IS_BETWEEN(celly - df, 0, CELL_Y_LEN))) continue;
				comfortField.at(cells_2Dto1D(cellx + dx, celly - df)) += FORCE(df);
			}
		break;
		}
		case Directions::DIR_3:
		{
			for (int df = 1; df < DISCOMFORT_FORCE + 1; df++)
			for (int dy = 0; dy < cell_radius; dy++)
			{
				if (!(IS_BETWEEN(cellx - df, 0, CELL_X_LEN))) continue;
				if (!(IS_BETWEEN(celly + dy, 0, CELL_Y_LEN))) continue;
				comfortField.at(cells_2Dto1D(cellx - df, celly + dy)) += FORCE(df);
			}
		break;
		}
		case Directions::DIR_4:
		{
			for (int df = 1; df < DISCOMFORT_FORCE + 1; df++)
			for (int dy = 0; dy < cell_radius; dy++)
			{
				if (!(IS_BETWEEN(cellx + df + cell_radius - 1, 0, CELL_X_LEN))) continue;
				if (!(IS_BETWEEN(celly + dy, 0, CELL_Y_LEN))) continue;
				comfortField.at(cells_2Dto1D(cellx + df + cell_radius - 1, celly + dy)) += FORCE(df);
			}
		break;
		}
		case Directions::DIR_6:
		{
			for (int df = 1; df < DISCOMFORT_FORCE + 1; df++)
			for (int dx = 0; dx < cell_radius; dx++)
			{
				if (!(IS_BETWEEN(cellx + dx, 0, CELL_X_LEN))) continue;
				if (!(IS_BETWEEN(celly + df + cell_radius - 1, 0, CELL_Y_LEN))) continue;
				comfortField.at(cells_2Dto1D(cellx + dx, celly + df + cell_radius - 1)) += FORCE(df);
			}
		break;
		}
		// unreachable
		default: exit(1);
	}
}

void cells_addDynamicObstacleArea_diagonal(uint16_t cellx, uint16_t celly, uint8_t cell_radius, Directions dir)
{
	#define FORCE(df) -(DISCOMFORT_FORCE + 1 - df)
	uint16_t rby = celly + cell_radius - 1;
	uint16_t rbx = cellx + cell_radius - 1;
	switch(dir)
	{
		case Directions::DIR_0:
		{
			for (int df = 1; df < DISCOMFORT_FORCE + 1; df++)
			for (int dx = 0; dx < cell_radius; dx++)
			{
				if (!(IS_BETWEEN(cellx + dx, 0, CELL_X_LEN))) continue;
				if (!(IS_BETWEEN(celly + df + cell_radius - 1, 0, CELL_Y_LEN))) continue;
				comfortField.at(cells_2Dto1D(cellx - df + dx, celly - df)) += FORCE(df);
			}
			for (int df = 1; df < DISCOMFORT_FORCE + 1; df++)
			for (int dy = 0; dy < cell_radius; dy++)
			{
				if (!(IS_BETWEEN(cellx - df, 0, CELL_X_LEN))) continue;
				if (!(IS_BETWEEN(celly - df + dy, 0, CELL_Y_LEN))) continue;
				comfortField.at(cells_2Dto1D(cellx - df, celly - df + dy)) += FORCE(df);
			}
		break;
		}
		case Directions::DIR_2:
		{
			for (int df = 1; df < DISCOMFORT_FORCE + 1; df++)
			for (int dx = 0; dx < cell_radius; dx++)
			{
				if (!(IS_BETWEEN(cellx + df + dx, 0, CELL_X_LEN))) continue;
				if (!(IS_BETWEEN(celly - df, 0, CELL_Y_LEN))) continue;
				comfortField.at(cells_2Dto1D(cellx + df + dx, celly - df)) += FORCE(df);
			}
			for (int df = 1; df < DISCOMFORT_FORCE + 1; df++)
			for (int dy = 0; dy < cell_radius; dy++)
			{
				if (!(IS_BETWEEN(rbx + df, 0, CELL_X_LEN))) continue;
				if (!(IS_BETWEEN(rby - df - dy, 0, CELL_Y_LEN))) continue;
				comfortField.at(cells_2Dto1D(rbx + df, rby - df - dy)) += FORCE(df);
			}
			break;
		}
		case Directions::DIR_5:
		{
			for (int df = 1; df < DISCOMFORT_FORCE + 1; df++)
			for (int dx = 0; dx < cell_radius; dx++)
			{
				if (!(IS_BETWEEN(cellx - df + dx, 0, CELL_X_LEN))) continue;
				if (!(IS_BETWEEN(rby + df, 0, CELL_Y_LEN))) continue;
				comfortField.at(cells_2Dto1D(cellx - df + dx, rby + df)) = FORCE(df);
			}
			for (int df = 1; df < DISCOMFORT_FORCE + 1; df++)
			for (int dy = 0; dy < cell_radius; dy++)
			{
				if (!(IS_BETWEEN(cellx - df, 0, CELL_X_LEN))) continue;
				if (!(IS_BETWEEN(celly + df + dy, 0, CELL_Y_LEN))) continue;
				comfortField.at(cells_2Dto1D(cellx - df, celly + df + dy)) = FORCE(df);
			}
			break;
		}
		case Directions::DIR_7:
		{
			for (int df = 1; df < DISCOMFORT_FORCE + 1; df++)
			for (int dx = 0; dx < cell_radius; dx++)
			{
				if (!(IS_BETWEEN(cellx + df + dx, 0, CELL_X_LEN))) continue;
				if (!(IS_BETWEEN(rby + df, 0, CELL_Y_LEN))) continue;
				comfortField.at(cells_2Dto1D(cellx + df + dx, rby + df)) += FORCE(df);
			}
			for (int df = 1; df < DISCOMFORT_FORCE + 1; df++)
			for (int dy = 0; dy < cell_radius; dy++)
			{
				if (!(IS_BETWEEN(rbx + df, 0, CELL_X_LEN))) continue;
				if (!(IS_BETWEEN(celly + df + dy, 0, CELL_Y_LEN))) continue;
				comfortField.at(cells_2Dto1D(rbx + df, celly + df + dy)) += FORCE(df);
			}
			break;
		}
		// unreachable
		default: exit(1);
	}
}

/// Adds a discomfort zone in the direction of movement.
/// The zone will be cell_radius-wide and 4 cells-(=one tile)-long
void cells_addDynamicObstacleArea(uint16_t cellx, uint16_t celly, uint8_t cell_radius, uint16_t moveDir)
{
	Directions dir = closestDirection(moveDir);
	ASSERT_OR_RETURN(, dir != Directions::DIR_NONE, "assumption failed");
	if (dir_is_diagonal[(int) dir])
		cells_addDynamicObstacleArea_diagonal(cellx, celly, cell_radius, dir);
	else
		cells_addDynamicObstacleArea_straight(cellx, celly, cell_radius, dir);
}

static void droidAddDynamicObstacle (const DROID &psCurr)
{
	uint16_t cellx, celly;
	world_to_cell(psCurr.pos.x, psCurr.pos.y, cellx, celly);
	if (psCurr.sMove.speed == 0)
	{
		cells_addDynamicObstacle(cellx, celly, moveDroidSizeExtent(&psCurr));
	}
	else
	{
		cells_addDynamicObstacleArea(cellx, celly, moveDroidSizeExtent(&psCurr), psCurr.sMove.moveDir);
		cells_addDynamicObstacle(cellx, celly, moveDroidSizeExtent(&psCurr));
	}
}

void beforeUpdateDroid()
{
	comfortField.clear();
	// TODO: resize doesn't need to be done every tick
	// only once, when map size is known. But I don't think it's hurting.
	comfortField.resize(CELL_AREA, 0);
	auto start = std::chrono::high_resolution_clock::now();
	int counter = 0;
	// being discomfortable when in the way of other droids:
	for (unsigned i = 0; i < MAX_PLAYERS; i++)
	{
		for (const DROID *psCurr = apsDroidLists[i]; psCurr != nullptr; psCurr = psCurr->psNext)
		{
			if (isVtolDroid(psCurr)) continue;
			droidAddDynamicObstacle(*psCurr);
			counter++;
		}

		for (DROID *psCurr = mission.apsDroidLists[i]; psCurr != nullptr; psCurr = psCurr->psNext)
		{
			if (isVtolDroid(psCurr)) continue;
			droidAddDynamicObstacle(*psCurr);
			counter++;
		}
	}
	// TODO: being discomfortable when has full HP, is doing nothing, and is within Repair Station range
	// ...
	// TODO: being discomfortable when ordered to build, and too far away from building target (? maybe not needed)
	// ...
	//auto end = std::chrono::high_resolution_clock::now();
	//auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
	//debug (LOG_FLOWFIELD, "beforeUpdate took %li for %i droids", duration.count(), counter);
}

/* Frame update for the movement of a tracked droid */
void moveUpdateDroid(DROID *psDroid)
{
	UDWORD				oldx, oldy;
	UBYTE				oldStatus = psDroid->sMove.Status;
	// moveSpeed + moveDir = velocity
	SDWORD				moveSpeed; // magnitude of the moveDir vector
	uint16_t			moveDir; // movemement vector
	PROPULSION_STATS	*psPropStats;
	Vector3i 			pos(0, 0, 0);
	bool				bStarted = false, bStopped;
	uint8_t				mapx, mapy;


	CHECK_DROID(psDroid);
	mapx = map_coord (psDroid->pos.x);
	mapy = map_coord (psDroid->pos.y);
	psPropStats = asPropulsionStats + psDroid->asBits[COMP_PROPULSION];
	ASSERT_OR_RETURN(, psPropStats != nullptr, "Invalid propulsion stats pointer");

	// If the droid has been attacked by an EMP weapon, it is temporarily disabled
	if (psDroid->lastHitWeapon == WSC_EMP)
	{
		if (gameTime - psDroid->timeLastHit < EMP_DISABLE_TIME)
		{
			// Get out without updating
			return;
		}
	}

	/* save current motion status of droid */
	bStopped = moveDroidStopped(psDroid, 0);
	// Directions flowDir = Directions::DIR_NONE;
	Vector2i v2i;
	moveSpeed = 0;
	moveDir = psDroid->rot.direction;

	switch (psDroid->sMove.Status)
	{
	case MOVEINACTIVE:
		if (psDroid->animationEvent == ANIM_EVENT_ACTIVE)
		{
			psDroid->timeAnimationStarted = 0;
			psDroid->animationEvent = ANIM_EVENT_NONE;
		}
		break;
	case MOVESHUFFLE:
		if (moveReachedWayPoint(psDroid) || (psDroid->sMove.shuffleStart + MOVE_SHUFFLETIME) < gameTime)
		{
			if (psPropStats->propulsionType == PROPULSION_TYPE_LIFT)
			{
				psDroid->sMove.Status = MOVEHOVER;
			}
			else
			{
				psDroid->sMove.Status = MOVEINACTIVE;
			}
		}
		else
		{
			// Calculate a target vector
			moveDir = moveGetDirection(psDroid);

			moveSpeed = moveCalcDroidSpeed(psDroid);
		}
		break;
	case MOVEWAITROUTE:
		moveDroidTo(psDroid, psDroid->sMove.destination.x, psDroid->sMove.destination.y);
		moveSpeed = MAX(0, psDroid->sMove.speed - 1);
		if (psDroid->sMove.Status != MOVENAVIGATE)
		{
			break;
		}
		// fallthrough
	case MOVENAVIGATE:
		// Get the next control point
		if (moveReachedWayPoint(psDroid) && !moveNextTarget(psDroid))
		{
			// No more waypoints - finish
			if (psPropStats->propulsionType == PROPULSION_TYPE_LIFT)
			{
				psDroid->sMove.Status = MOVEHOVER;
			}
			else
			{
				debug (LOG_FLOWFIELD, "reached destination : marking as inactive %i", psDroid->id);
				psDroid->sMove.Status = MOVEINACTIVE;
			}
			break;
		}

		if (isVtolDroid(psDroid))
		{
			psDroid->rot.pitch = 0;
		}

		psDroid->sMove.Status = MOVEPOINTTOPOINT;
		psDroid->sMove.bumpTime = 0;
		moveSpeed = MAX(0, psDroid->sMove.speed - 1);

		/* save started status for movePlayAudio */
		if (psDroid->sMove.speed == 0)
		{
			bStarted = true;
		}
		// fallthrough
	case MOVEPOINTTOPOINT:
	case MOVEPAUSE:
		if (droidReachedGoal(*psDroid))
		{
			debug (LOG_FLOWFIELD, "time to stop! %i", psDroid->id);
			psDroid->sMove.Status = MOVETURN;
			break;
		}
		if (tryGetFlowfieldVector(*psDroid, moveDir))
		{
			//moveSpeed = 200; //psDroid->sMove.physics.maxSpeed;// iHypot(v2i); //moveCalcDroidSpeed(psDroid);
			moveSpeed = moveCalcDroidSpeed(psDroid);
			// if (psDroid->player == 0) debug (LOG_FLOWFIELD, "moveDir %i from (%i %i)", moveDir, v2i.x, v2i.y);
		}
		else
		{
			if (psDroid->player == 0)
			{
				debug (LOG_FLOWFIELD, "FF not found for droid %i", psDroid->id);
			}
			// flowfield not found??
		}

		// psDroid->sMove.physics.velocity = v2i;
		// ff_update(*psDroid);
		break;
		// return;
	case MOVETURN:
		// Turn the droid to it's final facing
		if (psPropStats->propulsionType == PROPULSION_TYPE_LIFT)
		{
			psDroid->sMove.Status = MOVEPOINTTOPOINT;
		}
		else
		{
			psDroid->sMove.Status = MOVEINACTIVE;
		}
		break;
	case MOVETURNTOTARGET:
		moveSpeed = 0;
		moveDir = iAtan2(psDroid->sMove.target - psDroid->pos.xy());
		break;
	case MOVEHOVER:
		moveDescending(psDroid);
		break;

	default:
		ASSERT(false, "unknown move state");
		return;
		break;
	}

	// Update the movement model for the droid
	oldx = psDroid->pos.x;
	oldy = psDroid->pos.y;

	if (psDroid->droidType == DROID_PERSON)
	{
		moveUpdatePersonModel(psDroid, moveSpeed, moveDir);
	}
	else if (cyborgDroid(psDroid))
	{
		moveUpdateCyborgModel(psDroid, moveSpeed, moveDir, oldStatus);
	}
	else if (psPropStats->propulsionType == PROPULSION_TYPE_LIFT)
	{
		moveUpdateVtolModel(psDroid, moveSpeed, moveDir);
	}
	else
	{
		moveUpdateGroundModel(psDroid, moveSpeed, moveDir);
	}

	if (map_coord(oldx) != map_coord(psDroid->pos.x)
	    || map_coord(oldy) != map_coord(psDroid->pos.y))
	{
		visTilesUpdate((BASE_OBJECT *)psDroid);

		// object moved from one tile to next, check to see if droid is near stuff.(oil)
		checkLocalFeatures(psDroid);

		triggerEventDroidMoved(psDroid, oldx, oldy);
	}

	// See if it's got blocked
	/*if ((psPropStats->propulsionType != PROPULSION_TYPE_LIFT) && moveBlocked(psDroid))
	{
		objTrace(psDroid->id, "status: id %d blocked", (int)psDroid->id);
		psDroid->sMove.Status = MOVETURN;
	}*/

	/* If it's sitting in water then it's got to go with the flow! */
	/*if (worldOnMap(psDroid->pos.x, psDroid->pos.y) && terrainType(mapTile(map_coord(psDroid->pos.x), map_coord(psDroid->pos.y))) == TER_WATER)
	{
		updateDroidOrientation(psDroid);
	}*/

	if (psDroid->sMove.Status == MOVETURNTOTARGET && psDroid->rot.direction == moveDir)
	{
		if (psPropStats->propulsionType == PROPULSION_TYPE_LIFT)
		{
			psDroid->sMove.Status = MOVEPOINTTOPOINT;
		}
		else
		{
			psDroid->sMove.Status = MOVEINACTIVE;
		}
	}

	if (psDroid->periodicalDamageStart != 0 && psDroid->droidType != DROID_PERSON && psDroid->visibleForLocalDisplay()) // display-only check for adding effect
	{
		pos.x = psDroid->pos.x + (18 - rand() % 36);
		pos.z = psDroid->pos.y + (18 - rand() % 36);
		pos.y = psDroid->pos.z + (psDroid->sDisplay.imd->max.y / 3);
		addEffect(&pos, EFFECT_EXPLOSION, EXPLOSION_TYPE_SMALL, false, nullptr, 0, gameTime - deltaGameTime + 1);
	}

	movePlayAudio(psDroid, bStarted, bStopped, moveSpeed);
	ASSERT(droidOnMap(psDroid), "%s moved off map (%u, %u)->(%u, %u)", droidGetName(psDroid), oldx, oldy, (UDWORD)psDroid->pos.x, (UDWORD)psDroid->pos.y);
	CHECK_DROID(psDroid);
}
