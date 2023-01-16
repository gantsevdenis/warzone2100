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
/** @file
 *  Interface for the unit movement system
 */

#ifndef __INCLUDED_SRC_MOVE_H__
#define __INCLUDED_SRC_MOVE_H__

#include "objectdef.h"
#include "fpath.h"

// doesn't have have to a power of two: just being a factor of FF_UNIT (flowfield.h) is enough
static const uint16_t PersonRadius      = 1; // FF_UNITS = 32, Worldunits
static const uint16_t CyborgRadius      = 1; // FF_UNITS = 32, probably make this bigger like 3 * FF_UNITS
static const uint16_t SmallRadius       = 1; // FF_UNITS = 32,
static const uint16_t MediumRadius      = 2; // FF_UNITS = 64,
static const uint16_t LargeRadius       = 2; // FF_UNITS = 64, probably make this bigger, like 5 * FF_UNITS
static const uint16_t ExtraLargeRadius  = 4; // FF_UNITS = 128,

/* Set a target location for a droid to move to  - returns a bool based on if there is a path to the destination (true if there is a path)*/
bool moveDroidTo(DROID *psDroid, UDWORD x, UDWORD y, FPATH_MOVETYPE moveType = FMT_MOVE);

/* Set a target location for a droid to move to  - returns a bool based on if there is a path to the destination (true if there is a path)*/
// the droid will not join a formation when it gets to the location
bool moveDroidToNoFormation(DROID *psDroid, UDWORD x, UDWORD y, FPATH_MOVETYPE moveType = FMT_MOVE);

// move a droid directly to a location (used by vtols only)
void moveDroidToDirect(DROID *psDroid, UDWORD x, UDWORD y);

// Get a droid to turn towards a locaton
void moveTurnDroid(DROID *psDroid, UDWORD x, UDWORD y);

/* Stop a droid */
void moveStopDroid(DROID *psDroid);

/*Stops a droid dead in its tracks - doesn't allow for any little skidding bits*/
void moveReallyStopDroid(DROID *psDroid);

/* Get a droid to do a frame's worth of moving */
void moveUpdateDroid(DROID *psDroid);

/* Master-version for debugging */
void moveUpdateDroid_original(DROID *psDroid);

SDWORD moveCalcDroidSpeed(DROID *psDroid);

// get collision radius
SDWORD moveObjRadius(const BASE_OBJECT *psObj);

/* update body and turret to local slope */
void updateDroidOrientation(DROID *psDroid);

/* audio callback used to kill movement sounds */
bool moveCheckDroidMovingAndVisible(void *psObj);

const char *moveDescription(MOVE_STATUS status);

#endif // __INCLUDED_SRC_MOVE_H__
