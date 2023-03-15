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

extern SDWORD mapWidth; // defined in map.cpp
extern SDWORD mapHeight;

#define FF_UNIT 32
#define FF_TILE_SIZE 128
#define FF_GRID_1048576 1048576
#define CELL_X_LEN (mapWidth *  FF_TILE_SIZE / FF_UNIT) 
#define CELL_Y_LEN (mapHeight * FF_TILE_SIZE / FF_UNIT)
#define CELL_AREA  (CELL_X_LEN * CELL_Y_LEN)
#define WORLD_FACTOR (mapWidth * FF_TILE_SIZE)

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
	Vector2i { 0,  0}, // NONE
	Vector2i {-1, -1}, // DIR_0
	Vector2i { 0, -1}, // DIR_1
	Vector2i { 1, -1}, // DIR_2
	Vector2i {-1,  0}, // DIR_3
	Vector2i { 1,  0}, // DIR_4
	Vector2i {-1,  1}, // DIR_5
	Vector2i { 0,  1}, // DIR_6
	Vector2i { 1,  1}  // DIR_7
};

#define IS_BETWEEN(X, LOW, HIGH) (LOW) <= (X) && (X) < (HIGH)

// ignore the first one when iterating!
// 8 neighbours for each cell
static const int dir_neighbors[DIR_TO_VEC_SIZE][2] = {
	{-0xDEAD, -0xDEAD}, // DIR_NONE
	{-1, -1}, {0, -1}, {+1, -1},
	{-1,  0},          {+1,  0},
	{-1, +1}, {0, +1}, {+1, +1}
};

// dx| dy // distance to target
// -----
// - | - // strictly the best: we are getting closer to target in both dimensions
// - | = 
// = | - 
// + | - // at least in one dimension we are actually getting further away
// - | + // we might want to start marking cells as "visited" so that we dont visit them again
// + | = // meh ... Keeping one dimension constant, moving away in the other
// = | + // ...
// + | + // strictly the worst: we are actually moving away from target, in both dimensions
         // might be necessary to unblock in congested places 

// Which direction to prefer in presence of dynamic obstacles?
// When dynamic obstacles around, we can no longer guarantee optimal path to target...
// just trying our best, we can totally get stuck.
// Preferences are grouped by their equivalence groups (EqGr), all directions in the same group
// have the same priority (=you need some heuristic to prefer one over the others)
//
// The first direction is always the current one. It's not in its own group,
// because that would imply that a droid will only steer sideway (=take a direction from the next EqGr) when the next cell is blocked,
// while what we actually want, is to start steering in advance.
//
// In order to pass from one EqGr to another we must have a solid argument: for ex. all
// possible directions are blocked.
//
// When resorting to the last two EqGr, we should remember visited cells, as if they were occupied
// by some dynamic obstacle, so that droids are not tempted to visit them multiple times
static const Directions dir_avoidance_preference[8][8] = {
  // ↖: ↖↑← ↗↙ →↓ ↘
  {Directions::DIR_0, Directions::DIR_1, Directions::DIR_3, Directions::DIR_2, Directions::DIR_5, Directions::DIR_4, Directions::DIR_6, Directions::DIR_7},
  // ↑: ↑↖↗ ←→ ↓ ↙↘
  {Directions::DIR_1, Directions::DIR_0, Directions::DIR_2, Directions::DIR_3, Directions::DIR_4, Directions::DIR_6, Directions::DIR_5, Directions::DIR_7},
  // ↗: ↗↑→ ↖↘ ↓← ↙
  {Directions::DIR_2, Directions::DIR_1, Directions::DIR_4, Directions::DIR_0, Directions::DIR_7, Directions::DIR_6, Directions::DIR_3, Directions::DIR_5},
  // ←: ←↖↙ ↑↓ → ↗↘
  {Directions::DIR_3, Directions::DIR_0, Directions::DIR_5, Directions::DIR_1, Directions::DIR_6, Directions::DIR_4, Directions::DIR_2, Directions::DIR_7},
  // →: →↗↘ ↑↓ ← ↖↙
  {Directions::DIR_4, Directions::DIR_2, Directions::DIR_7, Directions::DIR_1, Directions::DIR_6, Directions::DIR_3, Directions::DIR_0, Directions::DIR_5},
  // ↙: ↙←↓ ↖↘ ↑→ ↗
  {Directions::DIR_5, Directions::DIR_3, Directions::DIR_6, Directions::DIR_0, Directions::DIR_7, Directions::DIR_1, Directions::DIR_4, Directions::DIR_2},
  // ↓: ↓↙↘ ←→ ↑ ↖↗
  {Directions::DIR_6, Directions::DIR_5, Directions::DIR_7, Directions::DIR_3, Directions::DIR_4, Directions::DIR_1, Directions::DIR_0, Directions::DIR_2},
  // ↘: ↘↓→ ↗↙ ←↑ ↖
  {Directions::DIR_7, Directions::DIR_6, Directions::DIR_4, Directions::DIR_2, Directions::DIR_5, Directions::DIR_3, Directions::DIR_1, Directions::DIR_0}
};

static const auto dir_straight = {Directions::DIR_1, Directions::DIR_3, Directions::DIR_4, Directions::DIR_6};
static const auto dir_diagonal = {Directions::DIR_0, Directions::DIR_2, Directions::DIR_5, Directions::DIR_7}; 

// equivalence groups
// note how all straight/diagonal directions have the same partition
static const int dir_groups_straight[] = {3, 2, 1, 2};
static const int dir_groups_diagonal[] = {3, 2, 2, 1};
// last 2 eqgr needs special marking for cells
static const bool dir_group_backtracks[] = {false, false, true, true};
static const bool dir_is_diagonal[9] = {
  false,
  true ,  false, true,
  false,         false,
  true ,  false, true
};


static inline uint16_t world_to_cell(uint16_t world, uint16_t &cell)
{
	cell = world / FF_UNIT;
	return world % FF_UNIT;
}

static inline void world_to_cell(uint16_t worldx, uint16_t worldy, uint16_t &cellx, uint16_t &celly)
{
	cellx = worldx / FF_UNIT;
	celly = worldy / FF_UNIT;
}

/// Gives upper left world-coordinate
static inline void cell_to_world(uint16_t cellx, uint16_t celly, uint16_t &worldx, uint16_t &worldy)
{
	worldx = cellx * FF_UNIT;
	worldy = celly * FF_UNIT;
}

static inline void tile_to_cell(uint8_t mapx, uint8_t mapy, uint16_t &cellx, uint16_t &celly)
{
	cellx = mapx * FF_TILE_SIZE / FF_UNIT;
	celly = mapy * FF_TILE_SIZE / FF_UNIT;
}

static inline uint32_t world_2Dto1D(uint16_t worldx, uint16_t worldy)
{
	return worldy * WORLD_FACTOR + worldx;
}

static inline uint32_t cells_2Dto1D(int cellx, int celly)
{
	// Note: this assumes we have 1 Flowfield per entire Map.
	if (cellx < 0 || celly < 0)
	{
		char what[128] = {0};
		sprintf(what, "out of range when addressing cells array: cellx=%i celly=%i", cellx, celly);
		throw std::out_of_range(what);
	}
	return celly * CELL_X_LEN + cellx;
}

static inline void cells_1Dto2D(uint32_t idx, uint16_t &cellx, uint16_t &celly)
{
	cellx = idx % CELL_X_LEN;
	celly = (idx - cellx) / CELL_X_LEN;
}

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
