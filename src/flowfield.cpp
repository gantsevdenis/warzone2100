
/**
 * Because smallest occupied area is a TILE anyway, and because no dynamic obstacles are included into FF,
 * there is no need to calculate it using finer resolution... Use TILES.
 * 
 * Notes:
 * Also, need to clear results from caches
 * Maybe calculate flows with GPU

 * Notes:
 * CostField is initially set at 1
 * IntegrationField is initially set at 0xFFFF (IMPASSABLE)
 * CostField is set to 0xFFFF (IMPASSABLE) for every tile where there a structure, or impassable tile
 * CostField is set to 0x7FFF for every tile where there is a droid
 * If CostField is IMPASSABLE at cell C, then IntegrationField is IMPASSABLE at that cell too.
 * If FlowField is IMPASSABLE at cell C, then
 *    - either CostField is IMPASSABLE at that cell
 *    - or IntegrationField has not been computed yet at that cell
 */

// Must be before some stuff from headers from flowfield.h, otherwise "std has no member 'mutex'"
// That simply means someone else have messed up
#include <cstdint>
#include <mutex>

#include "flowfield.h"
#include <fstream>
#include <future>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>
#include <memory>
#include <chrono>
#include "lib/framework/wzapp.h"
#include "lib/framework/debug.h"
#include "lib/ivis_opengl/pieblitfunc.h"
#include "lib/framework/vector.h"
#include "lib/ivis_opengl/piepalette.h"
#include "lib/ivis_opengl/pietypes.h"
#include "lib/ivis_opengl/textdraw.h"
#include "lib/ivis_opengl/piematrix.h"

#include "move.h"
#include "geometry.h"
#include "objmem.h"
#include "src/display3ddef.h"
#include "structuredef.h"
#include "statsdef.h"
#include "display3d.h"
#include "map.h"
#include "order.h"
#include "lib/framework/wzapp.h"
#include <glm/gtx/transform.hpp>
#include "lib/framework/opengl.h"
#include "lib/ivis_opengl/piedef.h"
#include "lib/ivis_opengl/piefunc.h"
#include "lib/ivis_opengl/piestate.h"
#include "lib/ivis_opengl/piemode.h"
#include "lib/ivis_opengl/pieblitfunc.h"
#include "lib/ivis_opengl/pieclip.h"
#include "lib/wzmaplib/include/wzmaplib/map.h"
#include "lib/framework/trig.h"

#define DEG(degrees) ((degrees) * 8192 / 45)
static bool flowfieldEnabled = true;
// yeah...initialization sequence is a spaghetti plate
static bool costInitialized = false;
// only process structures once
static std::unordered_set<unsigned> seenStructures;
#define MAX_TILES_AREA 65536
// just for debug, remove for release builds
#ifdef DEBUG
	#define DEFAULT_EXTENT_X 2
	#define DEFAULT_EXTENT_Y 2
#else
	#define DEFAULT_EXTENT_X 1
	#define DEFAULT_EXTENT_Y 1
#endif

void flowfieldEnable()
{

	flowfieldEnabled = true;
}

bool isFlowfieldEnabled()
{
	return flowfieldEnabled;
}

void flowfieldToggle()
{
	flowfieldEnabled = !flowfieldEnabled;
}

constexpr const uint8_t COST_NOT_PASSABLE = 255;

// having minimum cost geater than 1 allows us to interpolate this value
// for sub-tile (="cell") resolution:
// instead of searching for a path in 1024x1024 grid,
// we can search in 256x256 grid, and then interpolate the results
// => higher resolution for (almost) free
// It's only possible with Structures, because they don't move
// and always fill at least one full tile
// I set this to 4, because one tile is 4x4 cells
constexpr const uint8_t COST_MIN = 4; // default cost

static bool _drawImpassableTiles = true;
// Decides how much slopes should be avoided
constexpr const float SLOPE_COST_BASE = 0.01f;
// Decides when terrain height delta is considered a slope
constexpr const uint16_t SLOPE_THRESOLD = 4;

const int propulsionIdx2[] = {
	0,//PROPULSION_TYPE_WHEELED,
	0,//PROPULSION_TYPE_TRACKED,
	0,//PROPULSION_TYPE_LEGGED,
	2,//PROPULSION_TYPE_HOVER,
	3,//PROPULSION_TYPE_LIFT,
	1,//PROPULSION_TYPE_PROPELLOR,
	0,//PROPULSION_TYPE_HALF_TRACKED,
};

void initCostFields();
void destroyCostFields();
void destroyflowfieldResults();

struct FLOWFIELDREQUEST
{
	/// Target position, cell coordinates
	uint16_t tile_goalX;
	uint16_t tile_goalY;
	PROPULSION_TYPE propulsion;
	// purely for debug
	int player;
};

void flowfieldInit() {
	if (!isFlowfieldEnabled()) return;
	if(mapWidth == 0 || mapHeight == 0)
	{
		// called by both stageTwoInitialise() and stageThreeInitialise().
		// in the case of both these being called, map will be unavailable the first time.
		return;
	}
	

	initCostFields();
}

void flowfieldDestroy()
{
	if (!isFlowfieldEnabled()) return;

	destroyCostFields();
	destroyflowfieldResults();
}


// If the path finding system is shutdown or not
static volatile bool ffpathQuit = false;
class Flowfield;
// threading stuff
static WZ_THREAD        *ffpathThread = nullptr;
static WZ_MUTEX         *ffpathMutex = nullptr;
static WZ_SEMAPHORE     *ffpathSemaphore = nullptr;
static std::list<FLOWFIELDREQUEST> flowfieldRequests;
// tile 2d units
static std::array< std::set<uint16_t>, 4> flowfieldCurrentlyActiveRequests
{
	std::set<uint16_t>(), // PROPULSION_TYPE_WHEELED
	std::set<uint16_t>(), // PROPULSION_TYPE_PROPELLOR
	std::set<uint16_t>(), // PROPULSION_TYPE_HOVER
	std::set<uint16_t>()  // PROPULSION_TYPE_LIFT
};

/// Flow field results. Key: Map Array index.
/// FPATH.cpp checks this continuously to decide when the path was calculated
/// when it is, droid is assigned MOVENAVIGATE
static std::array< std::unordered_map<uint16_t, Flowfield* >, 4> flowfieldResults 
{
	std::unordered_map<uint16_t, Flowfield* >(), // PROPULSION_TYPE_WHEELED
	std::unordered_map<uint16_t, Flowfield* >(), // PROPULSION_TYPE_PROPELLOR
	std::unordered_map<uint16_t, Flowfield* >(), // PROPULSION_TYPE_HOVER
	std::unordered_map<uint16_t, Flowfield* >()  // PROPULSION_TYPE_LIFT

};

std::mutex flowfieldMutex;

void processFlowfield(FLOWFIELDREQUEST request);

void calculateFlowfieldAsync(uint16_t worldx, uint16_t worldy, PROPULSION_TYPE propulsion, int player)
{
	uint8_t mapx, mapy;
	mapx = map_coord(worldx);
	mapy = map_coord(worldy);
	FLOWFIELDREQUEST request;
	request.tile_goalX = mapx;
	request.tile_goalY = mapy;
	request.player = player;
	request.propulsion = propulsion;

	const auto map_goal = tiles_2Dto1D(mapx, mapy);

	if (flowfieldCurrentlyActiveRequests[propulsionIdx2[request.propulsion]].count(map_goal))
	{
		if (request.player == 0) debug (LOG_FLOWFIELD, "already waiting for %i (mapx=%i mapy=%i)", map_goal, mapx, mapy);
		return;
	}

	if (player == 0) debug (LOG_FLOWFIELD, "new async request for %i (mapx=%i mapy=%i)", map_goal, mapx, mapy);
	wzMutexLock(ffpathMutex);

	bool isFirstRequest = flowfieldRequests.empty();
	flowfieldRequests.push_back(request);
	
	wzMutexUnlock(ffpathMutex);
	
	if (isFirstRequest)
	{
		wzSemaphorePost(ffpathSemaphore);  // Wake up processing thread.
	}
}

/** This runs in a separate thread */
static int ffpathThreadFunc(void *)
{
	wzMutexLock(ffpathMutex);
	
	while (!ffpathQuit)
	{
		if (flowfieldRequests.empty())
		{
			wzMutexUnlock(ffpathMutex);
			wzSemaphoreWait(ffpathSemaphore);  // Go to sleep until needed.	
			wzMutexLock(ffpathMutex);
			continue;
		}

		if(!flowfieldRequests.empty())
		{
			// Copy the first request from the queue.
			auto request = std::move(flowfieldRequests.front());
			const auto map_goal = tiles_2Dto1D(request.tile_goalX, request.tile_goalY);
			
			flowfieldRequests.pop_front();
			flowfieldCurrentlyActiveRequests[propulsionIdx2[request.propulsion]].insert(map_goal);
			wzMutexUnlock(ffpathMutex);
			auto start = std::chrono::high_resolution_clock::now();
			processFlowfield(request);
			auto end = std::chrono::high_resolution_clock::now();
			auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
			wzMutexLock(ffpathMutex);
			// debug (LOG_FLOWFIELD, "processing took %li, erasing %i from currently active requests", duration.count(), map_goal);
			flowfieldCurrentlyActiveRequests[propulsionIdx2[request.propulsion]].erase(map_goal);
		}
	}
	wzMutexUnlock(ffpathMutex);
	return 0;
}

// initialise the findpath module
bool ffpathInitialise()
{
	// The path system is up
	ffpathQuit = false;

	if (!ffpathThread)
	{
		ffpathMutex = wzMutexCreate();
		ffpathSemaphore = wzSemaphoreCreate(0);
		ffpathThread = wzThreadCreate(ffpathThreadFunc, nullptr);
		wzThreadStart(ffpathThread);
	}

	return true;
}


void ffpathShutdown()
{
	if (ffpathThread)
	{
		// Signal the path finding thread to quit
		ffpathQuit = true;
		wzSemaphorePost(ffpathSemaphore);  // Wake up thread.

		wzThreadJoin(ffpathThread);
		ffpathThread = nullptr;
		wzMutexDestroy(ffpathMutex);
		ffpathMutex = nullptr;
		wzSemaphoreDestroy(ffpathSemaphore);
		ffpathSemaphore = nullptr;
	}
}

/// auto increment state for flowfield ids. Used on psDroid->sMove.uint32_t. 
// 0 means no flowfield exists for the unit.
uint32_t flowfieldIdInc = 1;

/** Cost of movement of a tile */
struct CostField
{
	std::array<uint8_t, MAX_TILES_AREA> cost;
	void world_setCost(uint16_t worldx, uint16_t worldy, uint16_t value)
	{
		uint8_t mapx, mapy;
		mapx = map_coord(worldx);
		mapy = map_coord(worldy);
		this->cost.at(tiles_2Dto1D(mapx, mapy)) = value;
	}

	void world_setCost(const Position &pos, uint16_t value)
	{
		uint8_t mapx, mapy;
		mapx = map_coord(pos.x);
		mapy = map_coord(pos.y);
		tile_setCost(mapx, mapy, value);
	}
	
	void tile_setCost(uint8_t mapx, uint8_t mapy, uint16_t value)
	{
		cost.at(tiles_2Dto1D(mapx, mapy)) = value;
	}
	
	void world_setImpassable(const Position &pos)
	{
		world_setCost(pos, COST_NOT_PASSABLE);
	}

	uint8_t world_getCost(uint16_t worldx, uint16_t worldy) const
	{
		uint8_t mapx, mapy;
		mapx = map_coord(worldx);
		mapy = map_coord(worldy);
		return this->cost.at(tiles_2Dto1D(mapx, mapy));
	}

	void tile_setImpassable(uint8_t mapx, uint8_t mapy)
	{
		tile_setCost(mapx, mapy, COST_NOT_PASSABLE);
	}

	bool tile_isImpassable(uint8_t mapx, uint8_t mapy) const
	{
		return cost.at(tiles_2Dto1D(mapx, mapy)) == COST_NOT_PASSABLE;
	}
	
	uint8_t tile_getCost(uint8_t mapx, uint8_t mapy) const
	{
		return this->cost.at(tiles_2Dto1D(mapx, mapy));
	}

	uint8_t tile_getCost(uint16_t mapIdx) const
	{
		return this->cost.at(mapIdx);
	}

	/// For a given tile, return an 8 bit number where each bit is 0 if
	/// tile is free, 1 if blocked.
	/// DIR_0 is the most left bit, DIR_7 is the most right
	/// end of maps are regarded as obstacles
	uint8_t world_getImpassableArea (uint16_t worldx, uint16_t worldy) const
	{
		uint8_t mapx, mapy;
		mapx = map_coord(worldx);
		mapy = map_coord(worldy);
		uint8_t out = 255;
		for (int neighb = (int) Directions::DIR_0; neighb < DIR_TO_VEC_SIZE; neighb++)
		{
			const auto neighb_x = mapx + dir_neighbors[neighb][0];
			const auto neighb_y = mapy + dir_neighbors[neighb][1];
			bool blocked = false;
			if (!(IS_BETWEEN(neighb_x, 0, CELL_X_LEN))) blocked = true;
			if (!(IS_BETWEEN(neighb_y, 0, CELL_Y_LEN))) blocked = true;
			blocked |= tile_isImpassable(neighb_x, neighb_y);
			// substract 1, because Directions::DIR_0 is 1
			const int shift_to_position = 9 - neighb - 1;
			out &= (1 << shift_to_position) & ((int) blocked);
		}
		return out;
	}
	
	void adjust () {cost.fill(COST_MIN);}
};

// Cost fields
std::array<CostField*, 4> costFields
{
	new CostField(), // PROPULSION_TYPE_WHEELED
	new CostField(), // PROPULSION_TYPE_PROPELLOR
	new CostField(), // PROPULSION_TYPE_HOVER
	new CostField(), // PROPULSION_TYPE_LIFT
};

uint8_t world_getImpassableArea(uint16_t worldx, uint16_t worldy, PROPULSION_TYPE propulsion)
{
	return costFields[propulsionIdx2[propulsion]]->world_getImpassableArea(worldx, worldy);
}

uint8_t minIndex (uint16_t a[], size_t a_len)
{
	uint16_t min_so_far = std::numeric_limits<uint16_t>::max();
	uint8_t min_idx = a_len;
	for (int i = 0; i < a_len; i++)
	{
		if (a[i] < min_so_far)
		{
			min_so_far = a[i];
			min_idx = i;
		}
	}
	const int straight_idx[4] = {
		(int) ((int) Directions::DIR_1 - (int) Directions::DIR_0),
		(int) ((int) Directions::DIR_4 - (int) Directions::DIR_0),
		(int) ((int) Directions::DIR_6 - (int) Directions::DIR_0),
		(int) ((int) Directions::DIR_3 - (int) Directions::DIR_0)
	};
	// when a tie, prefer straight lines over diagonals
	for (int i = 0; i < 4; i ++)
	{
		if (min_so_far == a[straight_idx[i]])
		{
			return straight_idx[i];
		}
	}
	return min_idx;
}

struct Node
{
	uint16_t predecessorCost;
	uint16_t index; // map units

	bool operator<(const Node& other) const {
		// We want top element to have lowest cost
		return predecessorCost > other.predecessorCost;
	}
};

// works for anything, as long as all arguments are using same units
inline bool isInsideGoal (uint16_t goalX, uint16_t goalY, uint16_t atx, 
                          uint16_t aty, uint16_t extentX, uint16_t extentY)
{
	return  atx < goalX + extentX && atx >= goalX &&
            aty < goalY + extentY && aty >= goalY;
}

/// Contains direction vectors for each map tile
class Flowfield
{
public:
	const uint32_t id;
	const uint16_t goalX; // tiles
	const uint16_t goalY; // tiles
	const uint16_t goalXExtent = 1;
	const uint16_t goalYExtent = 1;
	const int player; // Debug only
	const PROPULSION_TYPE prop; // so that we know what CostField should be referenced
	std::array<uint16_t, MAX_TILES_AREA> dirs;
	std::array<bool, MAX_TILES_AREA> impassable;
	std::array<bool, MAX_TILES_AREA> hasLOS;
	std::vector<uint16_t> integrationField;
	Flowfield (uint32_t id_, uint16_t goalX_, uint16_t goalY_, int player_, PROPULSION_TYPE prop_, uint16_t goalXExtent_, uint16_t goalYExtent_) : id(id_), goalX(goalX_), goalY(goalY_),goalXExtent(goalXExtent_), goalYExtent(goalYExtent_), player(player_), prop(prop_)
	{
		if (goalXExtent <= 0 || goalYExtent <= 0)
		{
			debug (LOG_ERROR, "total length of goal at map (%i %i) must strictly > 0, was %i %i", goalX, goalY,  goalXExtent, goalYExtent);
			throw std::runtime_error("bad parameters to flowfield");
		}
		impassable.fill(false);
		hasLOS.fill(false);
		dirs.fill(0);
	}

	bool world_isImpassable (uint16_t worldx, uint16_t worldy) const
	{
		uint8_t mapx, mapy;
		mapx = map_coord(worldx);
		mapy = map_coord(worldy);
		return impassable.at(tiles_2Dto1D(mapx, mapy));
	}
	
	void tile_setImpassable (uint16_t tileIdx)
	{
		impassable.at(tileIdx) = COST_NOT_PASSABLE;
	}

	bool tile_isImpassable (uint16_t tileIdx) const { return impassable.at(tileIdx);}
	
	bool tile_isGoal (uint8_t mapx, uint8_t mapy) const
	{
		return isInsideGoal(goalX, goalY, mapx, mapy, goalXExtent, goalYExtent);
	}

	bool world_isGoal (uint16_t worldx, uint16_t worldy) const
	{
		uint16_t mapx, mapy;
		mapx = map_coord(worldx);
		mapy = map_coord(worldy);
		return tile_isGoal(mapx, mapy);
	}
	
	// calculate closest point
	static inline uint16_t _closest (uint16_t to, uint16_t from, uint16_t from_extent)
	{
		int16_t dist = to - from;
		if (dist >= 0 && dist < from_extent) { return to; }
		else { return dist < 0 ? from : ((0xFFFF) & (from + from_extent - 1)); }
	}

/// Calculates distance to the closest tile cell of Goal (which may be several tiles wide)
	Vector2i  calculateDistGoal (uint8_t at_mapx, uint8_t at_mapy) const
	{
		// find closest x
		uint16_t closestx = _closest(at_mapx, goalX, goalXExtent);
		uint16_t closesty = _closest(at_mapy, goalY, goalYExtent);
		
		// save distance for later use in flow calculation
		int distx = at_mapx - closestx;
		int disty = at_mapy - closesty;
		return Vector2i {distx, disty};
	}

	/// Sets LOS flag for those tiles, from which Goal is visible.
	///
	/// When we do have LOS, flow vector is trivial.
	/// Example : https://howtorts.github.io/2014/01/30/Flow-Fields-LOS.html
	///
	/// We need free flow vectors (as opposed to some predefined 8 directions) because that
	/// gives the most natural movement.
	///
	/// We also calculate for all 8 directions, not 4, because diagonals can directly reach
	/// Goal without passing thru neighbouring tiles.
	bool calculateLOS (const std::vector<uint16_t> integField, uint32_t at, uint8_t at_mapx, uint8_t at_mapy, Vector2i &dir) const
	{
		if (isInsideGoal(goalX, goalY, at_mapx, at_mapy, goalXExtent, goalYExtent))
		{
			dir.x = 0;
			dir.y = 0;
			return true;
		}

		ASSERT (hasLOS.at(tiles_2Dto1D(goalX, goalY)), "invariant failed: goal must have LOS");		
		// we want signed difference between closest point of "goal" and "at"
		int dx, dy;
		auto dist = calculateDistGoal(at_mapx, at_mapy);
		invert(dist);
		dir.x = dist.x;
		dir.y = dist.y;
	    dx = dist.x;
	    dy = dist.y;
	    
	    int dx_one, dy_one;
	    dx_one = dx > 0 ? 1 : -1; // cannot be zero, already checked above
	    dy_one = dy > 0 ? 1 : -1;
	    
	    int dx_abs, dy_abs;
	    dx_abs = std::abs(dx);
	    dy_abs = std::abs(dy);
	    bool has_los = false;
	
	    // if the tile which 1 closer to goal has LOS, then we *may* have it too
	    if (dx_abs >= dy_abs)
	    {
	        if (hasLOS.at(tiles_2Dto1D (at_mapx + dx_one, at_mapy))) has_los = true;
	    }
	    if (dy_abs >= dx_abs)
	    {
	        if (hasLOS.at(tiles_2Dto1D (at_mapx, at_mapy + dy_one))) has_los = true;
	    }
	    if (dy_abs > 0 && dx_abs > 0)
	    {
	        // if the diagonal doesn't have LOS, we don't
	        if (!hasLOS.at(tiles_2Dto1D (at_mapx + dx_one, at_mapy + dy_one)))
	        {
				has_los = false;

			}
	        else if (dx_abs == dy_abs)
	        {
	            if (COST_NOT_PASSABLE == (integField.at(tiles_2Dto1D(at_mapx + dx_one, at_mapy))) ||
	                COST_NOT_PASSABLE == (integField.at(tiles_2Dto1D(at_mapx, at_mapy + dy_one))))
	                has_los = false;
	        }
	    }
		return has_los;
	}

	uint16_t tile_getDir (uint8_t mapx, uint8_t mapy) const
	{
		return dirs.at(tiles_2Dto1D(mapx, mapy));
	}

	uint16_t tile_getDir (uint16_t mIdx) const { return dirs.at(mIdx); }

	uint16_t world_getDir (uint16_t worldx, uint16_t worldy) const
	{
		uint8_t mapx, mapy;
		mapx = map_coord(worldx);
		mapy = map_coord(worldy);
		return tile_getDir(mapx, mapy);
	}

	void calculateFlows()
	{
		if (!costInitialized) return;
		auto start = std::chrono::high_resolution_clock::now();
		integrateCosts();
		auto end = std::chrono::high_resolution_clock::now();
		auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
		debug (LOG_FLOWFIELD, "cost integration took %lu", duration.count());
#ifdef DEBUG
		std::stringstream ss;
		ss << std::this_thread::get_id();
		// debug (LOG_FLOWFIELD, "calculating flows... thread %s", ss.str().c_str());
		// we can't have all nodes equal to zero, that makes no sense
		bool all_zero = true;
		for (auto it = 0; it < integrationField.size(); it++)
		{
			all_zero = all_zero && integrationField.at(it) == 0;
		}
		ASSERT_OR_RETURN (, !all_zero, "broken integration field computations!");
		// debugPrintIntegField(integrationField);
#endif
		static_assert((int) Directions::DIR_NONE == 0, "Invariant failed");
		static_assert(DIR_TO_VEC_SIZE == 9, "dirToVec must be sync with Directions!");
		for (uint32_t tileIdx = 0; tileIdx < integrationField.size(); tileIdx++)
		{
			const auto cost = integrationField[tileIdx];
			if (cost == COST_NOT_PASSABLE) continue;
			if (hasLOS.at(tileIdx)) continue; // already computed
			uint8_t mapx, mapy;
			tiles_1Dto2D(tileIdx, mapx, mapy);
			// already checked
			uint16_t costs[DIR_TO_VEC_SIZE - 1] = {0xFFFF};
			// we don't care about DIR_NONE		
			for (int neighb = (int) Directions::DIR_0; neighb < DIR_TO_VEC_SIZE; neighb++)
			{
				if (!(IS_BETWEEN(mapx + dir_neighbors[neighb][0], 0, 256))) continue;
				if (!(IS_BETWEEN(mapy + dir_neighbors[neighb][1], 0, 256))) continue;
				const auto neighb_tileIdx = tiles_2Dto1D (mapx + dir_neighbors[neighb][0], mapy + dir_neighbors[neighb][1]);
				// substract 1, because Directions::DIR_0 is 1
				costs[neighb - 1] = integrationField.at(neighb_tileIdx);
			}
			int minCostIdx = minIndex (costs, 8);
			ASSERT (minCostIdx >= 0 && minCostIdx < 8, "invariant failed");
			Directions dir = (Directions) (minCostIdx + (int) Directions::DIR_0);
			dirs.at(tileIdx) = iAtan2(dirToVec[(int) dir]);
		}
		return;
	}
private:
	void integrateCosts()
	{
		integrationField.resize(MAX_TILES_AREA, COST_NOT_PASSABLE);
		// Thanks to priority queue, we get the water ripple effect - closest tile first.
		// First we go where cost is the lowest, so we don't discover better path later.
		std::priority_queue<Node> openSet;
		for (int dx = 0; dx < goalXExtent; dx++)
		{
			for (int dy = 0; dy < goalYExtent; dy++)
			{
				if (!(IS_BETWEEN(goalX + dx, 0, 256))) continue;
				if (!(IS_BETWEEN(goalY + dy, 0, 256))) continue;
				const auto index = tiles_2Dto1D(goalX + dx, goalY + dy);
				hasLOS.at(index) = true;
				openSet.push(Node { 0, index });
			}
		}
#ifdef DEBUG
		for (int i = 0; i < costFields.at(propulsionIdx2[prop])->cost.size(); i++)
		{
			if (costFields.at(propulsionIdx2[prop])->cost.at(i) == 0)
			{
				uint8_t mapx, mapy;
				tiles_1Dto2D (i, mapx, mapy);
				debug (LOG_FLOWFIELD, "cost cannot be zero (idx=%i %i %i). Only integration field can have zeros (at goal)",
						i, mapx, mapy);
			}
		}
#endif
		while (!openSet.empty())
		{
			integrateNode(openSet);
			openSet.pop();
		}
		return;
	}
	
	void integrateNode(std::priority_queue<Node> &openSet)
	{
		const Node& node = openSet.top();
		uint8_t mapx, mapy;
		tiles_1Dto2D (node.index, mapx, mapy);
#ifdef DEBUG
		ASSERT_OR_RETURN(, node.index == tiles_2Dto1D(mapx, mapy), "fix that immediately! %i != (%i %i) != %i", node.index, mapx, mapy, tiles_2Dto1D(mapx, mapy));
#endif
		auto cost = costFields.at(propulsionIdx2[prop])->tile_getCost(node.index);
		if (cost == COST_NOT_PASSABLE)
		{
			tile_setImpassable(node.index);
			return;
		}
		// Go to the goal, no matter what
		if (node.predecessorCost == 0) cost = COST_MIN;
		const uint16_t integrationCost = node.predecessorCost + cost;
		const uint16_t oldIntegrationCost = integrationField.at(node.index);

		if (integrationCost < oldIntegrationCost)
		{
			integrationField.at(node.index) = integrationCost;
			Vector2i dirGoal;
			#if 0
			// LOS computation light be slow
			// TODO compute faster! or remove
			hasLOS.at(node.index) = false;// has_los;
			#else
			const bool has_los = calculateLOS(integrationField, node.index, mapx, mapy, dirGoal);
			hasLOS.at(node.index) = has_los;
			if (has_los)
			{
				dirs.at(node.index) = iAtan2(dirGoal);
			}
			#endif
			
			// only iterate over 4 neighbours, not 8
			// because otherwise diagonals in integration field arent yet initialized (they are still IMPASSABLE, and would block LOS)
			// we still check every tile, and still do have diagonal movement, no worries
			for (auto &neighb_it : dir_straight)
			{
				int neighb = (int) neighb_it;
				if (!(IS_BETWEEN(mapx + dir_neighbors[neighb][0], 0, 256))) continue;
				if (!(IS_BETWEEN( mapy + dir_neighbors[neighb][1], 0, 256))) continue;
				openSet.push(Node { integrationCost, tiles_2Dto1D(mapx + dir_neighbors[neighb][0],  mapy + dir_neighbors[neighb][1]) });
			}
		}
	}
};

bool droidReachedGoal (const DROID &psDroid)
{
	const auto dest = psDroid.sMove.destination;
	const auto goalx = map_coord (dest.x);
	const auto goaly = map_coord (dest.y);
	const auto selfx = map_coord (psDroid.pos.x);
	const auto selfy = map_coord (psDroid.pos.y);
	uint32_t map_goal = tiles_2Dto1D(goalx, goaly);
	const PROPULSION_STATS *psPropStats = asPropulsionStats + psDroid.asBits[COMP_PROPULSION];
	const PROPULSION_TYPE propulsion = psPropStats->propulsionType;
	const auto map_results = &flowfieldResults[propulsionIdx2[propulsion]];
	if (map_results->count(map_goal) > 0)
	{
		const auto ff = map_results->at(map_goal);
		const bool out = ff->tile_isGoal(selfx, selfy);
		// if (psDroid.player == 0) debug (LOG_INFO, "inside goal %i: %i %i (→) %i %i", out, selfx, selfy, ff->goalX, ff->goalY);
		return out;
	}
	//if (psDroid.player == 0) debug (LOG_ERROR, "asking for a flowfield for droid %i, but it has none", psDroid.id);
	return false;
}

// player for debug only
static Flowfield * _tryGetFlowfieldForTarget(uint16_t worldx, uint16_t worldy,
											PROPULSION_TYPE propulsion, int player)
{
	// caches needs to be updated each time there a structure built or destroyed
	uint16_t mapx,mapy;
	mapx = map_coord(worldx);
	mapy = map_coord(worldy);
	uint32_t map_goal = tiles_2Dto1D(mapx, mapy);
	const auto map_results = &flowfieldResults[propulsionIdx2[propulsion]];
	// FIXME: this is not technically correct because need to take into account radius
	if (map_results->count(map_goal) > 0)
		return map_results->at(map_goal);
	return nullptr;
}

/// Takes in world coordinates
// player is for debug only
bool tryGetFlowfieldForTarget(uint16_t worldx,
                              uint16_t worldy,
							  PROPULSION_TYPE propulsion, int player)
{
	const auto results = _tryGetFlowfieldForTarget(worldx, worldy, propulsion, player);
	// this check is already done in fpath.cpp.
	// TODO: we should perhaps refresh the flowfield instead of just bailing here.
	return results != nullptr;
}

/** Position is world coordinates. Radius is world units too */
bool tryGetFlowfieldDirection(PROPULSION_TYPE prop, const Position &pos,
							const Vector2i &dest, uint8_t radius, uint16_t &out)
{
	const std::unordered_map<uint16_t, Flowfield* > *results = &flowfieldResults[propulsionIdx2[prop]];
	uint8_t mapx, mapy;
	uint32_t map_goal;
	mapx = map_coord(dest.x);
	mapy = map_coord(dest.y);
	map_goal = tiles_2Dto1D(mapx, mapy);
	if (!results->count(map_goal))
	{
		// debug (LOG_FLOWFIELD, "not yet in results? %i, cellx=%i celly=%i", cell_goal, mapx, mapy);
		return false;
	}
	// get direction at current droid position
	// FIXME: should take radius into account
	out = results->at(map_goal)->world_getDir(pos.x, pos.y);
	// debug (LOG_FLOWFIELD, "found a flowfield for %i, DIR_%i", cell_goal, (int) out - (int) Directions::DIR_0);
	// TODO: calculate distance from pos to closest goal
	return true;
}

bool tryGetFlowfieldVector(const DROID &droid, uint16_t &out)
{
	const auto dest = droid.sMove.destination;
	const auto radius = moveObjRadius(&droid);
	PROPULSION_STATS *psPropStats = asPropulsionStats + droid.asBits[COMP_PROPULSION];
	bool found = tryGetFlowfieldDirection(psPropStats->propulsionType, droid.pos, dest, radius, out);
	if (!found) return false;
	return true;
}

Flowfield * droidGetFFDirections (const DROID &psDroid)
{
	const auto dest = psDroid.sMove.destination;
	PROPULSION_STATS *psPropStats = asPropulsionStats + psDroid.asBits[COMP_PROPULSION];
	return _tryGetFlowfieldForTarget(dest.x, dest.y, psPropStats->propulsionType, 0);	
}

void processFlowfield(FLOWFIELDREQUEST request)
{
	// NOTE for us noobs!!!! This function is executed on its own thread!!!!
	static_assert(PROPULSION_TYPE_NUM == 7, "new propulsions need to handled!!");
	std::unordered_map<uint16_t, Flowfield* > *results = &flowfieldResults[propulsionIdx2[request.propulsion]];
	const auto map_goal = tiles_2Dto1D (request.tile_goalX, request.tile_goalY);
 	// this check is already done in fpath.cpp.
	// TODO: we should perhaps refresh the flowfield instead of just bailing here.
	uint32_t ffid;
	if ((results->count(map_goal) > 0))
	{
		if (request.player == 0)
		{
			debug (LOG_FLOWFIELD, "already found in results %i (mapx=%i mapy=%i)",
					map_goal, request.tile_goalX, request.tile_goalY );
		}
		return;
	}
	// FIXME: remove, not needed; also check activeRequests for in-process calculations
	{
		std::lock_guard<std::mutex> lock(flowfieldMutex);
		ffid = flowfieldIdInc++;
	}
	Flowfield* flowfield = new Flowfield(ffid, request.tile_goalX, request.tile_goalY, request.player, request.propulsion, 1, 1);
	if (request.player == 0)
	{
		debug (LOG_FLOWFIELD, "calculating flowfield %i player=%i, at (mapx=%i len %i) (mapy=%i len %i)", 
		flowfield->id, 
		request.player, request.tile_goalX, flowfield->goalXExtent,
		request.tile_goalY, flowfield->goalYExtent);
	}
	flowfield->calculateFlows();
	{
		std::lock_guard<std::mutex> lock(flowfieldMutex);
		// store the result, this will be checked by fpath.cpp
		results->insert(std::make_pair(map_goal, flowfield));
	}
}

void cbFeatureDestroyed(const FEATURE *feature)
{
	if (!costInitialized) return;
	// NOTE: much copy pasta from feature.cpp: destroyFeature
	if (feature->psStats->subType == FEAT_SKYSCRAPER)
	{
		StructureBounds b = getStructureBounds(feature);
		for (int breadth = 0; breadth < b.size.y; ++breadth)
		{
			for (int width = 0; width < b.size.x; ++width)
			{
				uint16_t mapx, mapy;
				mapx = b.map.x + width;
				mapy = b.map.y + breadth;
				const MAPTILE *psTile = mapTile(mapx, mapy);
				// stops water texture changing for underwater features
				if (terrainType(psTile) != TER_WATER && terrainType(psTile) != TER_CLIFFFACE)
				{
					costFields[propulsionIdx2[PROPULSION_TYPE_WHEELED]]->tile_setCost(mapx, mapy, COST_MIN);
					costFields[propulsionIdx2[PROPULSION_TYPE_PROPELLOR]]->tile_setCost(mapx, mapy, COST_MIN);
					costFields[propulsionIdx2[PROPULSION_TYPE_HOVER]]->tile_setCost(mapx, mapy, COST_MIN);
					costFields[propulsionIdx2[PROPULSION_TYPE_LIFT]]->tile_setCost(mapx, mapy, COST_MIN);
				}
			}
		}
	}
}

// Note: this has 1 tile resolution, but that's best we can do
// because structure->pos is not consistent, so we can't calculate
// an impassable radius around it.
uint16_t calculateTileCost(uint16_t x, uint16_t y, PROPULSION_TYPE propulsion)
{
	// TODO: Current impl forbids VTOL from flying over short buildings
	if (!fpathBlockingTile(x, y, propulsion))
	{
		int pMax, pMin;
		getTileMaxMin(x, y, &pMax, &pMin);

		const auto delta = static_cast<uint16_t>(pMax - pMin);

		if (propulsion != PROPULSION_TYPE_LIFT && delta > SLOPE_THRESOLD)
		{
			// Yes, the cost is integer and we do not care about floating point tail
			return std::max(COST_MIN, static_cast<uint8_t>(SLOPE_COST_BASE * delta));
		}
		else
		{
			return COST_MIN;
		}
	}
	return COST_NOT_PASSABLE;
}


/// (Un)Mark tiles as (im)passable when structures are (destroyed)built.
/// All structures are at least 1-tile sized.
// Finer implementation was abandoned because structure->pos is not consistent
void updateTileCosts()
{
	for (int mapx = 0; mapx < mapWidth; mapx++)
	{
		for (int mapy = 0; mapy < mapHeight; mapy++)
		{
			auto cost_0 = calculateTileCost(mapx, mapy, PROPULSION_TYPE_WHEELED);
			auto cost_1 = calculateTileCost(mapx, mapy, PROPULSION_TYPE_PROPELLOR);
			auto cost_2 = calculateTileCost(mapx, mapy, PROPULSION_TYPE_HOVER);
			auto cost_3 = calculateTileCost(mapx, mapy, PROPULSION_TYPE_LIFT);
			costFields[propulsionIdx2[PROPULSION_TYPE_WHEELED]]->tile_setCost(mapx, mapy, cost_0);
			costFields[propulsionIdx2[PROPULSION_TYPE_PROPELLOR]]->tile_setCost(mapx, mapy, cost_1);
			costFields[propulsionIdx2[PROPULSION_TYPE_HOVER]]->tile_setCost(mapx, mapy, cost_2);
			costFields[propulsionIdx2[PROPULSION_TYPE_LIFT]]->tile_setCost(mapx, mapy, cost_3);
		
		}
	}
}

/** Update a given tile as impossible to cross.
 * Does NOT automatically get called for structure already present when game is loaded.
 * However we do call it from initCostfields to iterate over all *players* structures,
 * this excludes all scavenger structures (FXIME but it *should* include them!!)
 *
 * ONLY gets called for new structures built during gameplay.
 *
 * FIXME also must invalidate cached flowfields, because they
 * were all based on obsolete cost/integration fields!
 */
void cbStructureBuilt(const STRUCTURE *structure)
{
	if (!costInitialized) return;
	if (seenStructures.count(structure->id) > 0) return;
	seenStructures.insert(structure->id);
	updateTileCosts();
}

void cbStructureDestroyed(const STRUCTURE *structure)
{
	if (!costInitialized) return;
	if (seenStructures.count(structure->id) == 0 && structure->pStructureType->type != STRUCTURE_TYPE::REF_DEMOLISH)
	{
		debug (LOG_ERROR, "structure %i was unknown to pathfinder?! %s, %i %i",
		structure->id, structureTypeNames[structure->pStructureType->type],
		map_coord(structure->pos.x), map_coord(structure->pos.y));
		updateTileCosts();
	}
	else
	{
		updateTileCosts();
	}
}

void initCostFields()
{
	costFields[propulsionIdx2[PROPULSION_TYPE_WHEELED]]->adjust();
	costFields[propulsionIdx2[PROPULSION_TYPE_PROPELLOR]]->adjust();
	costFields[propulsionIdx2[PROPULSION_TYPE_HOVER]]->adjust();
	costFields[propulsionIdx2[PROPULSION_TYPE_LIFT]]->adjust();
	costInitialized = true;
	// init costs
	updateTileCosts();
#ifdef DEBUG
	Vector2i z, w;
	z = Vector2i{15, 8} + Vector2i {-1, 8};
	Vector2i _11 = {-1, 1};
	iNorm (_11);
	debug (LOG_FLOWFIELD, "z: %i %i", z.x, z.y);
	debug (LOG_FLOWFIELD, "iAtan2(16384,   16384)=%i", iAtan2( 16384,   16384));
	debug (LOG_FLOWFIELD, "iAtan2(-16384, -16384)=%i", iAtan2(-16384,  -16384));
	debug (LOG_FLOWFIELD, "iAtan2( 40960,  40960)=%i", iAtan2( 40960,   40960));
	debug (LOG_FLOWFIELD, "iAtan2(-16384,  16384)=%i", iAtan2(-16384,   16384));
	debug (LOG_FLOWFIELD, "iAtan2(16384,  -16384)=%i", iAtan2( 16384,  -16384));
	debug (LOG_FLOWFIELD, "iAtan2(-46341, -46341)=%i", iAtan2(-46341,  -46341));
	debug (LOG_FLOWFIELD, "iAtan2(-4,  -4)=%i", iAtan2(-4,  -4));
	debug (LOG_FLOWFIELD, "iAtan2(2,  1)=%i", iAtan2(2,  1));
	debug (LOG_FLOWFIELD, "iAtan2(1, 2)=%i", iAtan2(1, 2));
	debug (LOG_FLOWFIELD, "iAtan2({0, 0})=%i ", iAtan2({0, 0}));
	debug (LOG_FLOWFIELD, "iAtan2({0, 1})=%i ", iAtan2({0, 1}));
	debug (LOG_FLOWFIELD, "iAtan2({1, 0})=%i ", iAtan2({1, 0}));
	debug (LOG_FLOWFIELD, "iAtan2({1, 1})=%i ", iAtan2({1, 1}));
	debug (LOG_FLOWFIELD, "iAtan2({0, -1})=%i ", iAtan2({0, -1}));
	debug (LOG_FLOWFIELD, "iAtan2({-1, 0})=%i ", iAtan2({-1, 0}));
	debug (LOG_FLOWFIELD, "iAtan2({-1, -1})=%i ", iAtan2({-1, -1}));
	debug (LOG_FLOWFIELD, "iAtan2({-1,  1})=%i ", iAtan2({-1, 1}));
	debug (LOG_FLOWFIELD, "iNorm({-1,  1})={%i, %i}", _11.x, _11.y);
	debug (LOG_FLOWFIELD, "iAtan2({1, -1})=%i ", iAtan2({1, -1}));
	debug (LOG_FLOWFIELD, "iSin({-1, -1})=%i",  iSin(iAtan2({-1, -1})));
	debug (LOG_FLOWFIELD, "iCos({-1, -1})=%i",  iCos(iAtan2({-1, -1})));
	debug (LOG_FLOWFIELD, "iSin({1, 0})=%i",  iSin(iAtan2({1, 0})));
	debug (LOG_FLOWFIELD, "iCos({1, 0})=%i",  iCos(iAtan2({1, 0})));
	debug (LOG_FLOWFIELD, "iSin({1, 1})=%i",   iSin(iAtan2({1, 1})));
	debug (LOG_FLOWFIELD, "iCos({1, 1})=%i",   iCos(iAtan2({1, 1})));
	debug (LOG_FLOWFIELD, "iSin({0, -1})=%i",  iSin(iAtan2({0, -1})));
	debug (LOG_FLOWFIELD, "iCos({0, -1})=%i",  iCos(iAtan2({0, -1})));
	debug (LOG_FLOWFIELD, "iSin({0, 1})=%i",  iSin(iAtan2({0, 1})));
	debug (LOG_FLOWFIELD, "iCos({0, 1})=%i",  iCos(iAtan2({0, 1})));
	debug (LOG_FLOWFIELD, "iSin({0, 0})=%i",  iSin(iAtan2({0, 0})));
	debug (LOG_FLOWFIELD, "iCos({0, 0})=%i",  iCos(iAtan2({0, 0})));
	debug (LOG_FLOWFIELD, "iSin({-1, 1})=%i",  iSin(iAtan2({-1, 1})));
	debug (LOG_FLOWFIELD, "iCos({-1, 1})=%i",  iCos(iAtan2({-1, 1})));
	Vector2i slide;
	bool r = false;
	r = moveCalcSlideVector(-1, -1, 1, 1, slide);
	debug (LOG_FLOWFIELD, "-1, -1, 1, 1: %i %i %i", r, slide.x, slide.y);
	r = moveCalcSlideVector(-1, -1, 1, 0, slide);
	debug (LOG_FLOWFIELD, "-1, -1, 1, 0: %i %i %i", r, slide.x, slide.y);
	r = moveCalcSlideVector(-15000, -15000, 0, 15000, slide);
	debug (LOG_FLOWFIELD, "-1, -1, 0, 1: %i %i %i", r, slide.x, slide.y);
	r = moveCalcSlideVector(-1, -1, -1, -1, slide);
	debug (LOG_FLOWFIELD, "-1, -1, -1, -1: %i %i %i", r, slide.x, slide.y);

	r = moveCalcSlideVector(-1, 0, 1, 1, slide);
	debug (LOG_FLOWFIELD, "-1, 0, 1, 1: %i %i %i", r, slide.x, slide.y);
	r = moveCalcSlideVector(-1, 0, 1, 0, slide);
	debug (LOG_FLOWFIELD, "-1, 0, 1, 0: %i %i %i", r, slide.x, slide.y);
	r = moveCalcSlideVector(-1, 0, 0, 1, slide);
	debug (LOG_FLOWFIELD, "-1, 0, 0, 1: %i %i %i", r, slide.x, slide.y);
	r = moveCalcSlideVector(-1, 0, 0, -1, slide);
	debug (LOG_FLOWFIELD, "-1, 0, 0, -1: %i %i %i", r, slide.x, slide.y);

	w = Vector2i {1, 1};
	int32_t ratio = (int64_t)65536 * 60 / 100;
	auto wr = w * ratio;
	int32_t scaled = (int64_t)100 * 39321 / 65536;
	Vector2i scaledV ={(int64_t) 100 * 39321 / 65536, (int64_t) 100 * 39321 / 65536};
	debug (LOG_FLOWFIELD, "scaling: %i, %i %i, %i, %i %i", ratio, wr.x, wr.y, scaled, scaledV.x, scaledV.y);
	debug (LOG_FLOWFIELD, "init cost field done.");
#endif
}

void destroyCostFields()
{
	// ?
}

void destroyflowfieldResults() 
{
	// for (auto&& pair : propulsionToIndexUnique) {
	// 	flowfieldResults[pair.second]->clear();
	// }
}

// draw a square where half of sidelen goes in each direction
static void drawSquare (const glm::mat4 &mvp, int sidelen, int startX, int startY, int height, PIELIGHT color)
{
	iV_PolyLine({
		{ startX - (sidelen / 2), height, -startY - (sidelen / 2) },
		{ startX - (sidelen / 2), height, -startY + (sidelen / 2) },
		{ startX + (sidelen / 2), height, -startY + (sidelen / 2) },
		{ startX + (sidelen / 2), height, -startY - (sidelen / 2) },
		{ startX - (sidelen / 2), height, -startY - (sidelen / 2) },
	}, mvp, color);
}

// no half-side translation
// world coordinates
static void drawSquare2 (const glm::mat4 &mvp, int sidelen, int startX, int startY, int height, PIELIGHT color)
{
	iV_PolyLine({
		{ startX, height, -startY },
		{ startX, height, -startY - (sidelen) },
		{ startX + (sidelen), height, -startY - (sidelen) },
		{ startX + (sidelen), height, -startY },
		{ startX, height, -startY},
	}, mvp, color);
}

static void (*curDraw) (const glm::mat4&, int, int, int, int, PIELIGHT) = &drawSquare;
static bool isOne = false;
static bool drawYellowLines = false;
static bool drawVectors = false;

void toggleYellowLines() { drawYellowLines = !drawYellowLines; }

void toggleVectors() {drawVectors = !drawVectors;}

void toggleDrawSquare()
{
	if (isOne)
	{
		curDraw = &drawSquare2;
		isOne = false;
		
	}
	else
	{
		curDraw = &drawSquare;
		isOne = true;
	}
}

// alpha fully opaque
static const PIELIGHT WZ_WHITE {0xFF, 0xFF, 0xFF, 0xFF};

static void debugRenderText (const char *txt, int vert_idx)
{
	const int TEXT_SPACEMENT = 20;
	WzText t(WzString (txt), font_regular);
	t.render(20, 80 + TEXT_SPACEMENT * vert_idx, WZ_WHITE);
}

static void drawLines(const glm::mat4& mvp, std::vector<Vector3i> pts, PIELIGHT color)
{
	std::vector<glm::ivec4> grid2D;
	for (int i = 0; i < pts.size(); i += 2)
	{
		Vector2i _a, _b;
		pie_RotateProjectWithPerspective(&pts[i],     mvp, &_a);
		pie_RotateProjectWithPerspective(&pts[i + 1], mvp, &_b);
		grid2D.push_back({_a.x, _a.y, _b.x, _b.y});
	}
	iV_Lines(grid2D, color);
}

// red contour tiles which are fpathImpassable (terrain + structures)
static void debugDrawImpassableTile(const glm::mat4 &mvp, uint8_t mapx, uint8_t mapy)
{
	uint16_t wx, wy;
	wx = map_coord(mapx);
	wy = map_coord(mapy);
	std::vector<Vector3i> pts;
	int slice = FF_TILE_SIZE / 8;
	auto height = map_TileHeight(mapx, mapy) + 10;
	for (int i = 0; i < 16; i++)
	{
		
		pts.push_back({wx, height, -(wy + slice * i)});
		pts.push_back({wx + FF_TILE_SIZE, height, -(wy + slice * i)});
	}
	drawSquare2 (mvp, 128, world_coord(mapx), world_coord(mapy), height, WZCOL_RED);
	drawLines (mvp, pts, WZCOL_RED);
}

void debugDrawImpassableTiles(const glm::mat4 &mvp)
{
	const auto playerXTile = map_coord(playerPos.p.x);
	const auto playerYTile = map_coord(playerPos.p.z); // on 2D Map, this is actually Y
 	for (int dx = -6; dx <= 6; dx++)
	{
		for (int dy = 0; dy <=6; dy++)
		{
			const auto x = playerXTile + dx;
			const auto y = playerYTile + dy;
			if (isTerrainBlocked(x, y))
			{
				debugDrawImpassableTile(mvp, x, y);
			}
		}
	}
}

void debugDrawFlowfield(const DROID *psDroid, const glm::mat4 &mvp) 
{
	const auto playerXTile = map_coord(playerPos.p.x);
	const auto playerYTile = map_coord(playerPos.p.z); // on 2D Map, this is actually Y
	// pie_UniTransBoxFill(psDroid->pos.x, psDroid->pos.y, psDroid->pos.x+400, psDroid->pos.y+400, WZ_WHITE);
	PROPULSION_STATS       *psPropStats = asPropulsionStats + psDroid->asBits[COMP_PROPULSION];
	
	int height_xy = map_TileHeight(playerXTile, playerYTile);
	uint16_t destwx, destwy;
	destwx = psDroid->sMove.destination.x;
	destwy = psDroid->sMove.destination.y;
	Flowfield* flowfield = _tryGetFlowfieldForTarget(destwx, destwy, psPropStats->propulsionType, 0);
	// draw vector lines
	#define HALF_SIDE_FF_DEBUG 6
	if (drawVectors && flowfield && flowfield->integrationField.size() > 0)
	{
		// int radius = moveObjRadius(psDroid);
		for (auto dx = -HALF_SIDE_FF_DEBUG; dx < HALF_SIDE_FF_DEBUG; dx++)
		for (auto dy = -HALF_SIDE_FF_DEBUG; dy < HALF_SIDE_FF_DEBUG; dy++)
		{
			const auto mx = playerXTile + dx;
			const auto my = playerYTile + dy;
			const auto mIdx = tiles_2Dto1D(mx, my);
			if (!(IS_BETWEEN(mx, 0, 256))) continue;
			if (!(IS_BETWEEN(my, 0, 256))) continue;
			if (flowfield->tile_isImpassable(mIdx))
			{
				debugDrawImpassableTile(mvp, mx, my);
				continue;
			}
			auto angle = flowfield->tile_getDir(mIdx);
			auto dirx = iSin(angle);
			auto diry = iCos(angle);
			// for debug only, use floats
			int idirx = std::floor((float) dirx / float(0xFFFF) * (float) FF_TILE_SIZE);
			int idiry = std::floor((float) diry / float(0xFFFF) * (float) FF_TILE_SIZE);
			PIELIGHT c;
			// draw the origin/head of the vector
			if (flowfield->hasLOS.at(mIdx))
			{ c = WZCOL_GREEN; }
			else
			{ c = WZCOL_WHITE; }
			uint32_t startx, starty;
			startx = world_coord(mx);
			starty = world_coord(my);
			drawSquare(mvp, FF_TILE_SIZE, startx, starty, height_xy, c);
			// draw vector line itself
			iV_PolyLine({
				{ startx, height_xy, -starty },
				// { startx + vector.x * FF_UNIT / 2, height_xy, -starty - vector.y * FF_UNIT / 2 },
				{ startx + idirx, height_xy, - (starty + idiry) },
			}, mvp, c);

			Vector3i a;
			Vector2i b;
			a = {startx, height_xy, -(starty + 10)};
			pie_RotateProjectWithPerspective(&a, mvp, &b);
			auto cost = flowfield->integrationField.at(mIdx);
			if (cost != COST_NOT_PASSABLE)
			{
				WzText cost_text(WzString (std::to_string(cost).c_str()), font_small);
				cost_text.render(b.x, b.y, WZCOL_TEXT_BRIGHT);
			}
			else { debugDrawImpassableTile(mvp, mx, my);}
			
		}
	}
	
	for (auto deltaX = -6; deltaX <= 6; deltaX++)
	{
		const auto x = playerXTile + deltaX;

		if (x < 0) continue;
		
		for (auto deltaY = -6; deltaY <= 6; deltaY++)
		{
			const auto y = playerYTile + deltaY;

			if (y < 0) continue;

			const int X = world_coord(x);
			const int Y = world_coord(y);
			height_xy = map_TileHeight(x, y);
			const int height_x1y = map_TileHeight(x + 1, y);
			const int X1 = world_coord(x + 1);
			const int Y1 = world_coord(y + 1);
			const int height_xy1 = map_TileHeight(x, y + 1);
			const int height_x1y1 = map_TileHeight(x + 1, y + 1);
			
			// int height = map_TileHeight(x, y);
			// tile
			iV_PolyLine({
				{ X, height_xy, -Y },
				{ X, height_xy1, -Y1 },
				{ X1, height_x1y1, -Y1 },
			}, mvp, WZCOL_GREY);

			if (drawYellowLines)
			{
				std::vector<Vector3i> pts {
					// 3 vertical lines ...
					{ (X + FF_UNIT), height_xy, -Y },
					{ (X + FF_UNIT), height_xy1, -(Y + FF_TILE_SIZE) },

					{ (X + FF_UNIT * 2), height_xy, -Y },
					{ (X + FF_UNIT * 2), height_xy1, -(Y + FF_TILE_SIZE) },
					
					{ (X + FF_UNIT * 3), height_xy, -Y },
					{ (X + FF_UNIT * 3), height_xy1, -(Y + FF_TILE_SIZE) },

					// 3 horizontal lines
					{ X, height_xy, -(Y + FF_UNIT)},
					{ X + FF_TILE_SIZE, height_x1y, -(Y + FF_UNIT)},

					{ X, height_xy, -(Y + FF_UNIT * 2)},
					{ X + FF_TILE_SIZE, height_x1y, -(Y + FF_UNIT * 2)},

					{ X, height_xy, -(Y + FF_UNIT * 3)},
					{ X + FF_TILE_SIZE, height_x1y, -(Y + FF_UNIT * 3)},
				};
				std::vector<glm::ivec4> grid2D;
				for (int i = 0; i < pts.size(); i += 2)
				{
					Vector2i _a, _b;
					pie_RotateProjectWithPerspective(&pts[i],     mvp, &_a);
					pie_RotateProjectWithPerspective(&pts[i + 1], mvp, &_b);
					grid2D.push_back({_a.x, _a.y, _b.x, _b.y});
				}
				iV_Lines(grid2D, WZCOL_YELLOW);
			}

	 	}
	}
}

static DROID *lastSelected = nullptr;

void exportFlowfieldSelected()
{
	if (!lastSelected) return;
	DROID *psDroid = lastSelected;
	PROPULSION_STATS *psPropStats = asPropulsionStats + psDroid->asBits[COMP_PROPULSION];
	Vector2i destination = psDroid->sMove.destination;
	Flowfield *ff = _tryGetFlowfieldForTarget(destination.x, destination.y, psPropStats->propulsionType, 0);
	if (!ff)
	{
		debug (LOG_FLOWFIELD, "no flowfield found for droid %i, exiting", psDroid->id);
		return;
	}
	// dump info about map first, then flowfield
	const auto tmpfile = "/tmp/ffexport";
	std::ofstream ofs;
	ofs.open(tmpfile);
	ofs << "lorem ipsum";
	ofs.flush();
	ofs.close();

	debug (LOG_FLOWFIELD, "ff export finished.");
}


// even if flowfield disabled, just for sake of information
static void drawUnitDebugInfo (const DROID *psDroid, const glm::mat4 &mvp)
{
	// some droid sinfo
	auto startX = psDroid->pos.x;
	auto startY = psDroid->pos.y;
	auto target = (psDroid->pos.xy() - psDroid->sMove.target);
	auto destination = (psDroid->pos.xy() - psDroid->sMove.destination);
	auto height = map_TileHeight(map_coord(startX), map_coord(startY)) + 10;

	iV_PolyLine({
		{ startX, height, -startY },
		{ startX + static_cast<int>(target.x), height, 
		 -startY - static_cast<int>(target.y)},
		}, mvp, WZCOL_LBLUE);
	iV_PolyLine({
		{ startX, height, -startY },
		{ startX + static_cast<int>(destination.x), height, 
		 -startY - static_cast<int>(destination.y)},
		}, mvp, WZCOL_DBLUE);

	int idx = 0;
	char tmpBuff[64] = {0};
	ssprintf(tmpBuff, "Selected Droid %i", psDroid->id);
	debugRenderText(tmpBuff, idx++);

	memset(tmpBuff, 0, 64);
	ssprintf(tmpBuff, "Flowfield is %s", flowfieldEnabled ? "ON" : "OFF");
	debugRenderText(tmpBuff, idx++);

	memset(tmpBuff, 0, 64);
	ssprintf(tmpBuff, "Pos: %i %i (%i %i)", 
		psDroid->pos.x, psDroid->pos.y, 
		map_coord(psDroid->pos.x), map_coord(psDroid->pos.y));
	debugRenderText(tmpBuff, idx++);
	
	memset(tmpBuff, 0, 64);
	ssprintf(tmpBuff, "Target (LB): %i %i (%i %i)", 
		psDroid->sMove.target.x, psDroid->sMove.target.y, 
		map_coord(psDroid->sMove.target.x), map_coord(psDroid->sMove.target.y));
	debugRenderText(tmpBuff, idx++);

	memset(tmpBuff, 0, 64);
	ssprintf(tmpBuff, "Destination (DB): %i %i (%i %i)", 
		psDroid->sMove.destination.x, psDroid->sMove.destination.y, 
		map_coord(psDroid->sMove.destination.x), map_coord(psDroid->sMove.destination.y));
	debugRenderText(tmpBuff, idx++);

	memset(tmpBuff, 0, 64);
	ssprintf(tmpBuff, "Src: %i %i (%i %i)", psDroid->sMove.src.x, psDroid->sMove.src.y, 
		map_coord(psDroid->sMove.src.x), map_coord(psDroid->sMove.src.y));
	debugRenderText(tmpBuff, idx++);

	memset(tmpBuff, 0, 64);
	sprintf(tmpBuff, "moveDir: %i (%.2f %.2f)", psDroid->sMove.moveDir, 
		static_cast<float>(iSin(psDroid->sMove.moveDir)) / static_cast<float>((1 << 16)), 
		static_cast<float>(iCos(psDroid->sMove.moveDir)) / static_cast<float>((1 << 16)));
	debugRenderText(tmpBuff, idx++);

	PROPULSION_STATS       *psPropStats = asPropulsionStats + psDroid->asBits[COMP_PROPULSION];
	memset(tmpBuff, 0, 64);
	sprintf(tmpBuff, "speed: %i (Prop maxspeed=%i)", psDroid->sMove.speed, psPropStats->maxSpeed);
	debugRenderText(tmpBuff, idx++);

	memset(tmpBuff, 0, 64);
	if (psDroid->sMove.pathIndex != (int)psDroid->sMove.asPath.size())
	{
		Vector2i next = psDroid->sMove.asPath[psDroid->sMove.pathIndex];
		sprintf(tmpBuff, "Next path target: %i %i, path len %lu", map_coord(next.x), map_coord(next.y), psDroid->sMove.asPath.size());
		debugRenderText(tmpBuff, idx++);
	}
	else if (psDroid->sMove.asPath.size() == 1)
	{
		Vector2i next = psDroid->sMove.asPath[0];
		sprintf(tmpBuff, "Next path (and only) target: %i %i (%i %i)", next.x, next.y, map_coord(next.x), map_coord(next.y));
		debugRenderText(tmpBuff, idx++);
	}

	memset(tmpBuff, 0, 64);
	sprintf(tmpBuff, "Radius: %i", moveObjRadius(psDroid));
	debugRenderText(tmpBuff, idx++);
	
	memset(tmpBuff, 0, 64); 
	sprintf(tmpBuff, "Prop SpinSpeed (DEG %i): %i", psPropStats->spinSpeed, DEG(psPropStats->spinSpeed));
	debugRenderText(tmpBuff, idx++);

	memset(tmpBuff, 0, 64);
	sprintf(tmpBuff, "Prop SpinAngle DEG: %i", psPropStats->spinAngle);
	debugRenderText(tmpBuff, idx++);

	memset(tmpBuff, 0, 64);
	sprintf(tmpBuff, "Prop TurnSpeed: %i", psPropStats->turnSpeed);
	debugRenderText(tmpBuff, idx++);

	memset(tmpBuff, 0, 64);
	sprintf(tmpBuff, "Prop Acceleration: %i", psPropStats->acceleration);
	debugRenderText(tmpBuff, idx++);

	memset(tmpBuff, 0, 64);
	sprintf(tmpBuff, "Prop Deceleration: %i", psPropStats->deceleration);
	debugRenderText(tmpBuff, idx++);
	
	auto collisionRadius = moveObjRadius(psDroid);
	curDraw (mvp, collisionRadius, startX, startY, height, WZCOL_RED);

	memset(tmpBuff, 0, 64);
	sprintf(tmpBuff, "Collision radius: %i", collisionRadius);
	debugRenderText(tmpBuff, idx++);

	memset(tmpBuff, 0, 64);
	sprintf(tmpBuff, "Flowfield size (ground): %li", flowfieldResults[0].size());
	debugRenderText(tmpBuff, idx++);

	memset(tmpBuff, 0, 64);
	sprintf(tmpBuff, "Radius drawing func: %i", isOne ? 1 : 2);
	debugRenderText(tmpBuff, idx++);

	memset(tmpBuff, 0, 64);
	sprintf(tmpBuff, "Coloring impassable tiles: %s", _drawImpassableTiles ? "ON" : "OFF");
	debugRenderText(tmpBuff, idx++);
}

void toogleImpassableTiles()
{
	_drawImpassableTiles = !_drawImpassableTiles;
}


void debugDrawFlowfields(const glm::mat4 &mvp)
{
	// transports cannot be selected?
	for (DROID *psDroid = apsDroidLists[selectedPlayer]; psDroid; psDroid = psDroid->psNext)
	{
		if (!psDroid->selected) continue;
		// PROPULSION_STATS       *psPropStats = asPropulsionStats + psDroid->asBits[COMP_PROPULSION];
		lastSelected = psDroid;
		break;
	}
	if (isFlowfieldEnabled() && lastSelected)  debugDrawFlowfield(lastSelected, mvp);
	if (_drawImpassableTiles) debugDrawImpassableTiles(mvp);
	if (!lastSelected) return;
	drawUnitDebugInfo(lastSelected, mvp);
}
