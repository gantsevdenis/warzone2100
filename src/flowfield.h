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
 *  Definitions for flowfield pathfinding.
 */

#ifndef __INCLUDED_SRC_FLOWFIELD_H__
#define __INCLUDED_SRC_FLOWFIELD_H__

#include "fpath.h"
#include "movedef.h"
#include "lib/framework/frame.h" // for statsdef.h
#include "statsdef.h"
#include "featuredef.h"
#include <deque>
#include <glm/gtx/transform.hpp>

/*
	Concept: http://www.gameaipro.com/GameAIPro/GameAIPro_Chapter23_Crowd_Pathfinding_and_Steering_Using_Flow_Field_Tiles.pdf
*/

////////////////////////////////////////////////////////////////////// SCRATCHPAD
// Useful functions found:
// getTileMaxMin <- maybe?
// TileIsOccupied <- maybe, to check if there is a building on that tile
// TileIsKnownOccupied <- like above
// tileIsExplored

// MAPTILE.limitedContinent - if I understand it correctly, if there is no ground path between two tiles, then limitedContinent1 != limitedContinent2
// MAPTILE.hoverContinent - like above, but what is continent for hover?

/////////////////////////////////////////////////////////////////////
// TODO: maybe prefer visible tiles, or just discovered tiles (for player)
// Both things would go into integration field probably. Note that adding visibility stuff would quickly require most integration and flow fields to be thrown away, since visibility changes all the time.

#define FF_UNIT 32
// possible directions
enum class Directions : uint16_t
{
	DIR_NONE,
	DIR_0,  DIR_1,    DIR_2,
	DIR_3,  /*O*/     DIR_4,
	DIR_5,  DIR_6,    DIR_7
};
enum class Quadrant { Q1, Q2, Q3, Q4};

// Y grows down, X grows to the right -0.70711
//  Quad2    | Quad1
//  -        |       +x
//  ------------------>
//   Quad3   |   Quad4
//           |+y
#define DIR_TO_VEC_SIZE 9
const Vector2i dirToVec[DIR_TO_VEC_SIZE] = {
	Vector2i { 0,              0}, // NONE
	Vector2i {-1, -1}, // DIR_0
	Vector2i { 0,             -1}, // DIR_1
	Vector2i { 1, -1}, // DIR_2
	Vector2i {-1,              0}, // DIR_3
	Vector2i { 1,              0}, // DIR_4
	Vector2i {-1,        1}, // DIR_5
	Vector2i { 0,              1}, // DIR_6
	Vector2i { 1,  1}  // DIR_7
};


/// Enable flowfield pathfinding.
void flowfieldEnable();
/// Checks if flowfield pathfinding is enabled.
bool isFlowfieldEnabled();

void flowfieldToggle();

/// Initialises flowfield pathfinding for a map.
void flowfieldInit();
/// Deinitialises flowfield pathfinding.
void flowfieldDestroy();

void cbStructureBuilt(const STRUCTURE *structure);

void cbStructureDestroyed(const STRUCTURE *structure);

void cbFeatureDestroyed(const FEATURE *feature);

/// Returns true and populates flowfieldId if a flowfield exists for the specified target.
bool tryGetFlowfieldForTarget(uint16_t worldx, uint16_t worldy, PROPULSION_TYPE propulsion, int player);
/// Starts to generate a flowfield for the specified target.
void calculateFlowfieldAsync(uint16_t worldx, uint16_t worldy, PROPULSION_TYPE propulsion, int player, uint8_t radius);
/// Returns true and populates vector if a directional vector exists for the specified flowfield and target position.
// bool tryGetFlowfieldVector(unsigned int flowfieldId, uint8_t x, uint8_t y, Vector2f& vector);

// bool tryGetFlowfieldDirection(PROPULSION_TYPE prop, const Position &pos, const Vector2i &dest, uint8_t radius, Directions &out);
bool tryGetFlowfieldVector(DROID &droid, uint16_t &out);

/// is tile (x, y) passable? We don't need propulsion argument, it's implicit for this particular flowfield
bool flowfieldIsImpassable(unsigned int flowfieldId, uint8_t x, uint8_t y);

/// Draw debug data for flowfields.
void debugDrawFlowfields(const glm::mat4 &mvp);

/// Initialise infrastructure (threads ...) for flowfield pathfinding.
bool ffpathInitialise();
/// Shut down infrastructure (threads ...) for flowfield pathfinding.
void ffpathShutdown();

void toggleYellowLines();
void toggleDrawSquare();
void toggleVectors();
void toogleImpassableTiles();
void exportFlowfieldSelected();
uint16_t directionToUint16(Directions dir);

#endif // __INCLUDED_SRC_FLOWFIELD_H__
