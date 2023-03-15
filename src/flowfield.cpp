
/**
 * Since there 4 different body radii ( {Person =16}, {Cyborg, Small =32}, {Medium, Large =64}, {ExtraLarge =128} )
 * we need to calculate different flowfields for them: otherwise 1 Cyborg would block an entire tile
 * and another Cyborg wouldn't be able to pass through.
 * We break down the whole Map into 16x16 zones of the smallest size (16 worldunits)
 * and calculate Flowfield based on that:
 * - Map with 256x256 tiles at 128 worldunit each, => 1024x1024 cells, of 32x32 worldunits per cell.
 *   (this also supposes mouse-click precision should be at least 32x32 worldunits)
 *   So each regular tile is composed of 16 cells (each *side* 128 worldunits / FF_UNIT = 4 cells)
 * 
 * When structure or a feature gets destroyed, we should update existing flowfields by recalculating cost from
 * affected cells to neighbors.
 * 
 * Notes:
 * Might need to break down integration field into smaller sectors, so that 1MB array doesn't get recalculated ... 
 * Then, use some mean to go from one sector to another. 
 * However, how do we know those sectors excluded from calculation don't contain a better path? We don't, just assume.
 * Also, need to clear results from caches, they are getting heavy
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

/// Given a tile, give back subcell indices.
/// worldx, worldx: top-left point
// TODO: maybe arrange cells into a contiguous 16-sized array 
//       to facilitate sequential access
static void cells_of_tile(uint16_t worldx, uint16_t worldy, std::vector<uint32_t> &out)
{
	for (int dx = 0; dx < FF_TILE_SIZE / FF_UNIT; dx++)
	{
		for (int dy = 0; dy < FF_TILE_SIZE / FF_UNIT; dy++)
		{
			uint16_t cellx, celly;
			uint32_t cellIdx;
			world_to_cell (worldx + dx, worldy + dy, cellx, celly);
			if (!(IS_BETWEEN(cellx, 0, CELL_X_LEN))) continue;
			if (!(IS_BETWEEN(celly, 0, CELL_Y_LEN))) continue;
			cellIdx = cells_2Dto1D(cellx, celly);
			out.push_back(cellIdx);
		}
	}
}

/// For each X-tile of the map, holds the ID of a structure on it (if any).
std::array<const STRUCTURE*, 256> structuresPositions = {nullptr};

constexpr const uint16_t COST_NOT_PASSABLE = std::numeric_limits<uint16_t>::max();
constexpr const uint16_t COST_DROID = COST_NOT_PASSABLE / 2;
constexpr const uint16_t COST_MIN = 1; // default cost 
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
	uint16_t cell_goalX;
	uint16_t cell_goalY;
	PROPULSION_TYPE propulsion;
	// cell units
	uint8_t resolution;
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
// cell 2d units
static std::array< std::set<uint32_t>, 4> flowfieldCurrentlyActiveRequests
{
	std::set<uint32_t>(), // PROPULSION_TYPE_WHEELED
	std::set<uint32_t>(), // PROPULSION_TYPE_PROPELLOR
	std::set<uint32_t>(), // PROPULSION_TYPE_HOVER
	std::set<uint32_t>()  // PROPULSION_TYPE_LIFT
};

/// Flow field results. Key: Cells Array index.
/// FPATH.cpp checks this continuously to decide when the path was calculated
/// when it is, droid is assigned MOVENAVIGATE
static std::array< std::unordered_map<uint32_t, Flowfield* >, 4> flowfieldResults 
{
	std::unordered_map<uint32_t, Flowfield* >(), // PROPULSION_TYPE_WHEELED
	std::unordered_map<uint32_t, Flowfield* >(), // PROPULSION_TYPE_PROPELLOR
	std::unordered_map<uint32_t, Flowfield* >(), // PROPULSION_TYPE_HOVER
	std::unordered_map<uint32_t, Flowfield* >()  // PROPULSION_TYPE_LIFT

};

std::mutex flowfieldMutex;

void processFlowfield(FLOWFIELDREQUEST request);

void calculateFlowfieldAsync(uint16_t worldx, uint16_t worldy, PROPULSION_TYPE propulsion, int player, uint8_t radius)
{
	uint16_t cellx, celly;
	world_to_cell(worldx, worldy, cellx, celly);
	FLOWFIELDREQUEST request;
	request.cell_goalX = cellx;
	request.cell_goalY = celly;
	request.player = player;
	request.resolution = radius / FF_UNIT;
	request.propulsion = propulsion;

	const uint32_t cell_goal = cells_2Dto1D(cellx, celly);

	if (flowfieldCurrentlyActiveRequests[propulsionIdx2[request.propulsion]].count(cell_goal))
	{
		// if (request.player == 0) debug (LOG_FLOWFIELD, "already waiting for %i (cellx=%i celly=%i)", cell_goal, cellx, celly);
		return;
	}

	// if (player == 0) debug (LOG_FLOWFIELD, "new async request for %i (cellx=%i celly=%i)", cell_goal, cellx, celly);
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
			const uint32_t cell_goal = cells_2Dto1D(request.cell_goalX, request.cell_goalY);
			
			flowfieldRequests.pop_front();
			flowfieldCurrentlyActiveRequests[propulsionIdx2[request.propulsion]].insert(cell_goal);
			wzMutexUnlock(ffpathMutex);
			auto start = std::chrono::high_resolution_clock::now();
			processFlowfield(request);
			auto end = std::chrono::high_resolution_clock::now();
			auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
			wzMutexLock(ffpathMutex);
			// debug (LOG_FLOWFIELD, "processing took %li, erasing %i from currently active requests", duration.count(), cell_goal);
			flowfieldCurrentlyActiveRequests[propulsionIdx2[request.propulsion]].erase(cell_goal);
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

/** Cost of movement for each one-sixteenth of a tile */
struct CostField
{
	std::vector<uint16_t> cost;  // cells units
	void world_setCost(uint16_t worldx, uint16_t worldy, uint16_t value)
	{
		uint16_t cellx, celly;
		world_to_cell(worldx, worldy, cellx, celly);
		this->cost.at(world_2Dto1D(worldx, worldy)) = value;
	}

	void world_setCost(const Position &pos, uint16_t value, uint16_t radius)
	{
		uint16_t cell_startx, cell_starty;
		world_to_cell(pos.x, pos.y, cell_startx, cell_starty);
		uint16_t cellradius;
		uint16_t radius_cells_mod = world_to_cell(radius, cellradius);
		// add 1 if spills to the next cell
		cellradius += (uint16_t) (radius_cells_mod > 0);
		// debug (LOG_FLOWFIELD, "cellradius was (startx=%i starty=%i) %i", cell_startx, cell_starty, cellradius);
		for(int dx = 0; dx < cellradius; dx++)
		{
			for(int dy = 0; dy < cellradius; dy++)
			{
				int cellx = cell_startx + dx;
				int celly = cell_starty + dy;
				if (!(IS_BETWEEN(cellx, 0, CELL_X_LEN))) continue;
				if (!(IS_BETWEEN(celly, 0, CELL_Y_LEN))) continue;
				auto cellIdx = cells_2Dto1D(cellx, celly);
				// NOTE: maybe use another value for droids, because they actually can move, unlike a rock
				this->cost.at(cellIdx) = value;
			}
	 	}
	}
	
	void tile_setCost(uint8_t mapx, uint8_t mapy, uint16_t value)
	{
		uint16_t cellx, celly;
		tile_to_cell(mapx, mapy, cellx, celly);
		for (int dx = 0; dx < (FF_TILE_SIZE / FF_UNIT); dx++)
		{
			for (int dy = 0; dy < (FF_TILE_SIZE / FF_UNIT); dy++)
			{
				// NOTE: assume that because mapx and mapy are within bounds [0, 255]
				this->cost.at(cells_2Dto1D(cellx + dx, celly + dy)) = value;
			}
		}
	}
	
	void world_setImpassable(const Position &pos, uint16_t radius)
	{
		world_setCost(pos, COST_NOT_PASSABLE, radius);
	}

	uint16_t world_getCost(uint16_t worldx, uint16_t worldy) const
	{
		uint16_t cellx, celly;
		world_to_cell(worldx, worldy, cellx, celly);
		return this->cost.at(cells_2Dto1D(cellx, celly));
	}

	uint16_t cell_getCost(uint16_t cellx, uint16_t celly) const
	{
		return this->cost.at(cells_2Dto1D(cellx, celly));
	}

	uint16_t cell_getCost(uint32_t index) const
	{
		return this->cost.at(index);
	}

	void tile_setImpassable(uint8_t mapx, uint8_t mapy)
	{
		tile_setCost(mapx, mapy, COST_NOT_PASSABLE);
	}

	uint16_t tile_getCost(uint8_t mapx, uint8_t mapy) const
	{
		uint16_t cellx, celly;
		tile_to_cell(mapx, mapy, cellx, celly);
		return this->cost.at(cells_2Dto1D(cellx, celly));
	}

	void adjust()
	{
		cost.resize(CELL_AREA, COST_MIN);
	}
};

// Cost fields
std::array<CostField*, 4> costFields
{
	new CostField(), // PROPULSION_TYPE_WHEELED
	new CostField(), // PROPULSION_TYPE_PROPELLOR
	new CostField(), // PROPULSION_TYPE_HOVER
	new CostField(), // PROPULSION_TYPE_LIFT
};

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
	uint32_t index; // cell units

	bool operator<(const Node& other) const {
		// We want top element to have lowest cost
		return predecessorCost > other.predecessorCost;
	}
};

// works for anything, as long as all arguments are using same units
inline bool isInsideGoal (uint16_t goalX, uint16_t goalY, uint16_t cellx, 
                          uint16_t celly, uint16_t extentX, uint16_t extentY)
{
	return  cellx < goalX + extentX && cellx >= goalX &&
            celly < goalY + extentY && celly >= goalY;
}

/** Contains direction vectors for each map tile
 * we have a 1024*1024 sized array here,
 * because each Map can be up to 256 tiles, and each tile is 128 worldunits size.
 * We split each tile into 4 subtiles of 32 worldunits for a finer grained movement.
 */
class Flowfield
{
public:
	const uint32_t id;
	const uint16_t goalX; // cells units
	const uint16_t goalY; // cells units
	const int player; // Debug only
	const PROPULSION_TYPE prop; // so that we know what CostField should be referenced
	// goal can be an area, which starts at "goal",
	// and of TotalSurface = (goalXExtent) x (goalYExtent)
	// a simple goal sized 1 tile will have both extents equal to 1
	// a square goal sized 4 will have both extends equal to 2
	const uint16_t goalXExtent = 1; 	// cells units
	const uint16_t goalYExtent = 1; 	// cells units
	std::vector<uint16_t> dirs;         // cells units. Directions only, no magnitude
	std::vector<bool> impassable;   	// cells units
	std::vector<bool> hasLOS;       	// cells units
	std::vector<Vector2i> distGoal;   	// cells units
	
	Flowfield (uint32_t id_, uint16_t goalX_, uint16_t goalY_, int player_, PROPULSION_TYPE prop_, uint16_t goalXExtent_, uint16_t goalYExtent_)
	: id(id_), goalX(goalX_), goalY(goalY_), player(player_), prop(prop_), goalXExtent(goalXExtent_), goalYExtent(goalYExtent_)
	{
		dirs.resize(CELL_AREA);
		impassable.resize(CELL_AREA, false);
		hasLOS.resize(CELL_AREA, false);
		distGoal.resize(CELL_AREA, {0, 0});
		if (goalXExtent <= 0 || goalYExtent <= 0)
		{
			debug (LOG_ERROR, "total length of goal at cells (%i %i) must strictly > 0, was %i %i", goalX, goalY,  goalXExtent, goalYExtent);
			throw std::runtime_error("bad parameters to flowfield");
		}
	}
	
	bool world_isImpassable (uint16_t worldx, uint16_t worldy) const
	{
		uint16_t cellx, celly;
		world_to_cell(worldx, worldy, cellx, celly);
		return impassable.at(cells_2Dto1D(cellx, celly));
	}

	void cell_setImpassable (uint32_t cellIdx)
	{
		impassable.at(cellIdx) = true;
	}
	
	bool cell_isImpassable (uint16_t cellx, uint16_t celly) const 
	{
		return impassable.at(cells_2Dto1D(cellx, celly));
	}

	bool cell_isGoal (uint16_t cellx, uint16_t celly) const
	{
		return isInsideGoal(goalX, goalY, cellx, celly, goalXExtent, goalYExtent);
	}

	bool world_isGoal (uint16_t worldx, uint16_t worldy) const
	{
		uint16_t cellx, celly;
		world_to_cell(worldx, worldy, cellx, celly);
		return cell_isGoal(cellx, celly);
	}
	
	// calculate closest point
	static inline uint16_t _closest (uint16_t to, uint16_t from, uint16_t from_extent)
	{
		int16_t dist = to - from;
		if (dist >= 0 && dist < from_extent) { return to; }
		else { return dist < 0 ? from : ((0xFFFF) & (from + from_extent - 1)); }
	}

	/// Calculates distance to the closest tile cell of Goal (which may be several cells wide)
	Vector2i  calculateDistGoal (uint32_t at, uint16_t at_cellx, uint16_t at_celly) const
	{
		// find closest x
		uint16_t closestx = _closest(at_cellx, goalX, goalXExtent);
		uint16_t closesty = _closest(at_celly, goalY, goalYExtent);
		
		// save distance for later use in flow calculation
		int distx = at_cellx - closestx;
		int disty = at_celly - closesty;
		return Vector2i {distx, disty};
	}
	
	/// Sets LOS flag for those cells, from which Goal is visible.
	///
	/// When we do have LOS, flow vector is trivial.
	/// Example : https://howtorts.github.io/2014/01/30/Flow-Fields-LOS.html
	///
	/// We need free flow vectors (as opposed to some predefined 8 directions) because that
	/// gives the most natural movement.
	///
	/// We also calculate for all 8 directions, not 4, because diagonals can directly reach
	/// Goal without passing thru neighbouring tiles.
	///
	/// NOTE: very expensive, I have better idea
	bool calculateLOS (const std::vector<uint16_t> integField, uint32_t at, uint16_t at_cellx, uint16_t at_celly, Vector2i &dir) const
	{
		if (isInsideGoal(goalX, goalY, at_cellx, at_celly, goalXExtent, goalYExtent))
		{
			dir.x = 0;
			dir.y = 0;
			return true;
		}

		ASSERT (hasLOS.at(cells_2Dto1D(goalX, goalY)), "invariant failed: goal must have LOS");		
		// we want signed difference between closest point of "goal" and "at"
		int dx, dy;
		auto dist = calculateDistGoal(at, at_cellx, at_celly);
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
	
	    // if the cell which 1 closer to goal has LOS, then we *may* have it too
	    if (dx_abs >= dy_abs)
	    {
	        if (hasLOS.at(cells_2Dto1D (at_cellx + dx_one, at_celly))) has_los = true;
	    }
	    if (dy_abs >= dx_abs)
	    {
	        if (hasLOS.at(cells_2Dto1D (at_cellx, at_celly + dy_one))) has_los = true;
	    }
	    if (dy_abs > 0 && dx_abs > 0)
	    {
	        // if the diagonal doesn't have LOS, we don't
	        if (!hasLOS.at(cells_2Dto1D (at_cellx + dx_one, at_celly + dy_one)))
	        {
				has_los = false;

			}
	        else if (dx_abs == dy_abs)
	        {
	            if (COST_NOT_PASSABLE == (integField.at(cells_2Dto1D(at_cellx + dx_one, at_celly))) ||
	                COST_NOT_PASSABLE == (integField.at(cells_2Dto1D(at_cellx, at_celly + dy_one))))
	                has_los = false;
	        }
	    }
		return has_los;
	}

	uint16_t cell_getDir (uint16_t cellx, uint16_t celly, uint8_t radius) const
	{
		return dirs.at(cells_2Dto1D(cellx, celly));
		/*if (radius == FF_UNIT)
		{
			return dirs.at(cells_2Dto1D(cellx, celly));
		}
		else
		{
			// NOTE: when the radius is larger than FF_UNITS, we need to decide where to go
			// proposal: just count the most represented direction, and go there
			// TODO: fix duplicated iteration code, like in CostField.world_setCost
			uint16_t cellradius;
			uint16_t radius_cells_mod = world_to_cell(radius, cellradius);
			static_assert((int) Directions::DIR_0 == 1, "invariant broken");
			int neighb_directions[9] = {-1};
			// add 1 if spills to the next cell
			cellradius += (uint16_t) (radius_cells_mod > 0);
			for(int dx = 0; dx < cellradius; dx++)
			{
				for(int dy = 0; dy < cellradius; dy++)
				{
					if (!(IS_BETWEEN(cellx + dx, 0, CELL_X_LEN))) continue;
					if (!(IS_BETWEEN(celly + dy, 0, CELL_Y_LEN))) continue;
					uint16_t cellIdx = cells_2Dto1D(cellx + dx, celly + dy);
					const Directions d = dirs.at(cellIdx);
					neighb_directions[(int) d]++;
				}
			}
			int min_sofar = 0xFFFF;
			int min_idx = 0xFFFF;
			for (int i = 0; i < 9; i++)
			{
				if (IS_BETWEEN(neighb_directions[i], 0, min_sofar))
				{
					min_sofar = neighb_directions[i];
					min_idx = i;
				}
			}
			return static_cast<Directions>(min_idx);
		}*/
		
	}

	uint16_t world_getDir (uint16_t worldx, uint16_t worldy, uint8_t radius) const
	{
		uint16_t cellx, celly;
		world_to_cell(worldx, worldy, cellx, celly);
		return cell_getDir(cellx, celly, radius);
	}

	uint16_t world_getVector(uint16_t worldx, uint16_t worldy) const
	{
		uint16_t cellx, celly;
		world_to_cell(worldx, worldy, cellx, celly);
		return dirs.at(cells_2Dto1D(cellx, celly));
	}
	
	void calculateFlows()
	{
		if (!costInitialized) return;
		auto start = std::chrono::high_resolution_clock::now();
		integrateCosts();
		auto end = std::chrono::high_resolution_clock::now();
		auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
		//debug (LOG_FLOWFIELD, "cost integration took %lu", duration.count());
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
		ASSERT_OR_RETURN(, integrationField.size() == CELL_AREA, "invariant failed");
		static_assert((int) Directions::DIR_NONE == 0, "Invariant failed");
		static_assert(DIR_TO_VEC_SIZE == 9, "dirToVec must be sync with Directions!");
		for (uint32_t cellIdx = 0; cellIdx < integrationField.size(); cellIdx++)
		{
			const auto cost = integrationField[cellIdx];
			if (cost == COST_NOT_PASSABLE) continue;
			if (hasLOS.at(cellIdx)) continue; // already computed
			uint16_t cellx, celly;
			cells_1Dto2D(cellIdx, cellx, celly);
			// already checked
			uint16_t costs[DIR_TO_VEC_SIZE - 1] = {0xFFFF};
			// we don't care about DIR_NONE		
			for (int neighb = (int) Directions::DIR_0; neighb < DIR_TO_VEC_SIZE; neighb++)
			{
				if (!(IS_BETWEEN(cellx + dir_neighbors[neighb][0], 0, CELL_X_LEN))) continue;
				if (!(IS_BETWEEN(celly + dir_neighbors[neighb][1], 0, CELL_Y_LEN))) continue;
				const auto neighb_cellIdx = cells_2Dto1D (cellx + dir_neighbors[neighb][0], celly + dir_neighbors[neighb][1]);
				// substract 1, because Directions::DIR_0 is 1
				costs[neighb - 1] = integrationField.at(neighb_cellIdx);
			}
			int minCostIdx = minIndex (costs, 8);
			ASSERT (minCostIdx >= 0 && minCostIdx < 8, "invariant failed");
			Directions dir = (Directions) (minCostIdx + (int) Directions::DIR_0);
			// FIXME: no need to compute every single time, just point to correct angle
			dirs.at(cellIdx) = iAtan2(dirToVec[(int) dir]);
		}
		return;
	}
	std::vector<uint16_t> integrationField;

private:
	void integrateCosts()
	{
		integrationField.resize(CELL_AREA, COST_NOT_PASSABLE);
		for (int i = 0; i < MAX_PLAYERS; i++)
		{
			for (DROID *psCurr = apsDroidLists[i]; psCurr != nullptr; psCurr = psCurr->psNext)
			{
				if (((psCurr->order.type == DORDER_HOLD) ||
					(psCurr->order.type == DORDER_NONE && secondaryGetState(psCurr, DSO_HALTTYPE) == DSS_HALT_HOLD)))
				{
					// account for real size of	droids!!
					auto radius = moveObjRadius(psCurr);
					uint16_t cell_startx, cell_starty;
					world_to_cell(psCurr->pos.x, psCurr->pos.y, cell_startx, cell_starty);
					uint16_t radius_cells;
					world_to_cell(radius, radius_cells);
					radius_cells += (uint16_t) (radius % FF_UNIT > 0);
					for(int dx = 0; dx < radius_cells; dx++)
					{
						for(int dy = 0; dy < radius_cells; dy++)
						{
							if (!(IS_BETWEEN(cell_startx + dx, 0, CELL_X_LEN))) continue;
							if (!(IS_BETWEEN(cell_starty + dy, 0, CELL_Y_LEN))) continue;
							uint16_t cellIdx = cells_2Dto1D(cell_startx + dx, cell_starty + dy);
							// NOTE: maybe use another value for droids,
							// because they actually can move, unlike a rock
							// What about the droid itself?
							integrationField.at(cellIdx) = COST_DROID;
						}
					}
				}
			}
		}
		// Thanks to priority queue, we get the water ripple effect - closest tile first.
		// First we go where cost is the lowest, so we don't discover better path later.
		std::priority_queue<Node> openSet;
		for (int dx = 0; dx < goalXExtent; dx++)
		{
			for (int dy = 0; dy < goalYExtent; dy++)
			{
				if (!(IS_BETWEEN(goalX + dx, 0, CELL_X_LEN))) continue;
				if (!(IS_BETWEEN(goalY + dy, 0, CELL_Y_LEN))) continue;
				const auto index = cells_2Dto1D(goalX + dx, goalY + dy);
				hasLOS.at(index) = true;
				openSet.push(Node { 0, index });
			}
		}
#ifdef DEBUG
		for (int i = 0; i < costFields.at(propulsionIdx2[prop])->cost.size(); i++)
		{
			if (costFields.at(propulsionIdx2[prop])->cost.at(i) == 0)
			{
				uint16_t cellx, celly;
				cells_1Dto2D (i, cellx, celly);
				debug (LOG_FLOWFIELD, "cost cannot be zero (idx=%i %i %i). Only integration field can have zeros (at goal)",
						i, cellx, celly);
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
		uint16_t cellx, celly;
		cells_1Dto2D (node.index, cellx, celly);
#ifdef DEBUG
		ASSERT_OR_RETURN(, node.index == cells_2Dto1D(cellx, celly), "fix that immediately!");
#endif
		auto cost = costFields.at(propulsionIdx2[prop])->cell_getCost(node.index);
		if (cost == COST_NOT_PASSABLE)
		{
			cell_setImpassable(node.index);
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
			// LOS computation is very slow and easily adds 1 second on debug build
			// TODO compute faster! or remove
			hasLOS.at(node.index) = false;// has_los;
			#else
			const bool has_los = calculateLOS(integrationField, node.index, cellx, celly, dirGoal);
			hasLOS.at(node.index) = has_los;
			if (has_los)
			{
				dirs.at(node.index) = iAtan2(dirGoal);
			}
			#endif
			
			// only iterate over 4 neighbours, not 8
			// because otherwise diagonals in integration field arent yet initialized (they are still IMPASSABLE, and would block LOS)
			for (auto &neighb_it : dir_straight)
			{
				int neighbx, neighby;
				int neighb = (int) neighb_it;
				neighbx = cellx + dir_neighbors[neighb][0];
				neighby = celly + dir_neighbors[neighb][1];
				if (!(IS_BETWEEN(neighbx, 0, CELL_X_LEN))) continue;
				if (!(IS_BETWEEN(neighby, 0, CELL_Y_LEN))) continue;
				openSet.push(Node { integrationCost, cells_2Dto1D(neighbx, neighby) });
			}
		}
	}
};

// player for debug only
static Flowfield * _tryGetFlowfieldForTarget(uint16_t worldx, uint16_t worldy,
											PROPULSION_TYPE propulsion, int player)
{
	// caches needs to be updated each time there a structure built or destroyed
	uint16_t cellx, celly;
	world_to_cell(worldx, worldy, cellx, celly);
	uint32_t cell_goal = cells_2Dto1D(cellx, celly);
	const auto map_results = &flowfieldResults[propulsionIdx2[propulsion]];
	/*if (player == 0 && (map_results->count(cell_goal) <= 0 ||  map_results->at(cell_goal) == nullptr))
	{
		debug (LOG_FLOWFIELD, "no results for %i (cellx=%i celly=%i) %p
		", cell_goal, cellx, celly, (void*) map_results);
	}*/
	if (map_results->count(cell_goal) > 0)
		return map_results->at(cell_goal);
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
	const std::unordered_map<uint32_t, Flowfield* > *results = &flowfieldResults[propulsionIdx2[prop]];
	uint16_t cellx, celly;
	uint32_t cell_goal;
	//world_to_cell(pos.x, pos.y, cell_posx, cell_posy);
	world_to_cell(dest.x, dest.y, cellx, celly);
	cell_goal = cells_2Dto1D(cellx, celly);
	if (!results->count(cell_goal))
	{
		// debug (LOG_FLOWFIELD, "not yet in results? %i, cellx=%i celly=%i", cell_goal, cellx, celly);
		return false;
	}
	// get direction at current droid position
	out = results->at(cell_goal)->world_getDir(pos.x, pos.y, radius);
	// debug (LOG_FLOWFIELD, "found a flowfield for %i, DIR_%i", cell_goal, (int) out - (int) Directions::DIR_0);
	// TODO: calculate distance from pos to closest goal
	return true;
}

bool tryGetFlowfieldVector(DROID &droid, uint16_t &out)
{
	const auto dest = droid.sMove.destination;
	const auto radius = moveObjRadius(&droid);
	PROPULSION_STATS *psPropStats = asPropulsionStats + droid.asBits[COMP_PROPULSION];
	bool found = tryGetFlowfieldDirection(psPropStats->propulsionType, droid.pos, dest, radius, out);
	if (!found) return false;
	// TODO somehow reincorporate magnitude into resulting vector
	return true;
}

void processFlowfield(FLOWFIELDREQUEST request)
{
	// NOTE for us noobs!!!! This function is executed on its own thread!!!!
	static_assert(PROPULSION_TYPE_NUM == 7, "new propulsions need to handled!!");
	std::unordered_map<uint32_t, Flowfield* > *results = &flowfieldResults[propulsionIdx2[request.propulsion]];
	uint16_t cell_extentX, cell_extentY;
	cell_extentX = request.resolution;
	cell_extentY = request.resolution;
	const uint32_t cell_goal = cells_2Dto1D (request.cell_goalX, request.cell_goalY);
 	// this check is already done in fpath.cpp.
	// TODO: we should perhaps refresh the flowfield instead of just bailing here.
	uint32_t ffid;
	if ((results->count(cell_goal) > 0))
	{
		if (request.player == 0)
		{
			debug (LOG_FLOWFIELD, "already found in results %i (cellx=%i celly=%i)",
					cell_goal, request.cell_goalX, request.cell_goalY );
		}
		return;
	}
	// FIXME: remove, not needed; also check activeRequests for in-process calculations
	{
		std::lock_guard<std::mutex> lock(flowfieldMutex);
		ffid = flowfieldIdInc++;
	}
	Flowfield* flowfield = new Flowfield(ffid, request.cell_goalX, request.cell_goalY, request.player,
										request.propulsion, cell_extentX, cell_extentY);
	if (request.player == 0)
	{
		debug (LOG_FLOWFIELD, "calculating flowfield %i player=%i, at (cellx=%i len %i) (celly=%i len %i)", 
		flowfield->id, 
		request.player, request.cell_goalX, flowfield->goalXExtent,
		request.cell_goalY, flowfield->goalYExtent);
	}
	flowfield->calculateFlows();
	{
		std::lock_guard<std::mutex> lock(flowfieldMutex);
		// store the result, this will be checked by fpath.cpp
		// NOTE: we are storing Goal in cell units
		/*if (request.player == 0)
			debug (LOG_FLOWFIELD, "inserting %i (cellx=%i celly=%i) into results, results size=%li", cell_goal,
		request.cell_goalX, request.cell_goalY, results->size());*/
		results->insert(std::make_pair(cell_goal, flowfield));
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
			return std::max(COST_MIN, static_cast<uint16_t>(SLOPE_COST_BASE * delta));
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
	debug (LOG_FLOWFIELD, "Cell Area=%i MapWidth=%i MapHeight=%i, CELL_X_LEN=%i CELL_Y_LEN=%i",
	       CELL_AREA, mapWidth, mapHeight, CELL_X_LEN, CELL_Y_LEN);
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

static void (*curDraw) (const glm::mat4&, int, int, int, int, PIELIGHT) = &drawSquare2;
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

static void debugDrawCellWithColor (const glm::mat4 &mvp,  uint16_t cellx, uint16_t celly, uint16_t height, PIELIGHT c)
{
	uint16_t wx, wy;
	cell_to_world(cellx, celly, wx, wy);
	std::vector<Vector3i> pts;
	int slice = FF_UNIT / 8;
	for (int i = 0; i < 8; i++)
	{

		pts.push_back({wx, height, -(wy + slice * i)});
		pts.push_back({wx + FF_UNIT, height, -(wy + slice * i)});
	}
	drawLines(mvp, pts, c);
}

// ugly hack to fill a tile *partially* because I can't
// display a filled rectangle on a tile with right projection ... 
static void debugDrawImpassableCell (const glm::mat4& mvp, uint16_t cellx, uint16_t celly, uint16_t height)
{
	debugDrawCellWithColor(mvp, cellx, celly, height, WZCOL_RED);
}

static void debugDrawGoal (const glm::mat4& mvp, uint16_t cellx, uint16_t celly, uint16_t height)
{
	debugDrawCellWithColor(mvp, cellx, celly, height, WZCOL_GREEN);
}

// red contour tiles which are fpathImpassable (terrain + structures)
static void debugDrawImpassableTile(const glm::mat4 &mvp, uint16_t mapx, uint16_t mapy)
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
	uint16_t player_cellx, player_celly;
	tile_to_cell(playerXTile, playerYTile, player_cellx, player_celly);
	// pie_UniTransBoxFill(psDroid->pos.x, psDroid->pos.y, psDroid->pos.x+400, psDroid->pos.y+400, WZ_WHITE);
	// const auto& costField = costFields[propulsionIdx2[PROPULSION_TYPE_WHEELED]];
	PROPULSION_STATS       *psPropStats = asPropulsionStats + psDroid->asBits[COMP_PROPULSION];
	
	int height_xy = map_TileHeight(playerXTile, playerYTile);
	uint16_t destwx, destwy, destcellx, destcelly;
	destwx = psDroid->sMove.destination.x;
	destwy = psDroid->sMove.destination.y;
	Flowfield* flowfield = _tryGetFlowfieldForTarget(destwx, destwy, psPropStats->propulsionType, 0);
	tile_to_cell(destwx, destwy, destcellx, destcelly);
	// uint32_t cell_target = cells_2Dto1D(destcellx, destcelly);
	
	// draw vector lines
	#define HALF_SIDE_FF_DEBUG 6
	if (drawVectors && flowfield && flowfield->integrationField.size() > 0)
	{
		int radius = moveObjRadius(psDroid);
		for (auto dx = -HALF_SIDE_FF_DEBUG; dx < HALF_SIDE_FF_DEBUG; dx++)
		for (auto dy = -HALF_SIDE_FF_DEBUG; dy < HALF_SIDE_FF_DEBUG; dy++)
		{
			uint16_t cellx = player_cellx + dx;
			uint16_t celly = player_celly + dy;
			auto cellIdx = cells_2Dto1D(cellx, celly);
			if (!(IS_BETWEEN(cellx, 0, CELL_X_LEN))) continue;
			if (!(IS_BETWEEN(celly, 0, CELL_Y_LEN))) continue;
			if (flowfield->cell_isImpassable(cellx, celly))
			{
				debugDrawImpassableCell(mvp, cellx, celly, height_xy);
				continue;
			}
			if (flowfield->cell_isGoal(cellx, celly))
			{
				debugDrawGoal(mvp, cellx, celly, height_xy);
				continue;
			}
			auto angle = flowfield->cell_getDir(cellx, celly, radius);
			auto dirx = iSin(angle);
			auto diry = iCos(angle);
			// for debug only, use floats
			int idirx = std::floor((float) dirx / float(0xFFFF) * (float) FF_TILE_SIZE);
			int idiry = std::floor((float) diry / float(0xFFFF) * (float) FF_TILE_SIZE);
			uint16_t wx, wy, startx, starty;
			cell_to_world(cellx, celly, wx, wy);
			startx = wx + FF_UNIT / 2;
			starty = wy + FF_UNIT / 2;
			//int distx, disty;
			//distx = std::abs(cellx - flowfield->_closest(cellx, flowfield->goalX, flowfield->goalXExtent));
			//disty = std::abs(celly - flowfield->_closest(celly, flowfield->goalY, flowfield->goalYExtent));
			PIELIGHT c;
			// draw the origin/head of the vector
			if (flowfield->hasLOS.at(cellIdx))
			{ c = WZCOL_GREEN; }
			else
			{ c = WZCOL_WHITE; }
			drawSquare(mvp, 8, startx, starty, height_xy, c);
			// draw vector line itself
			iV_PolyLine({
				{ startx, height_xy, -starty },
				// { startx + vector.x * FF_UNIT / 2, height_xy, -starty - vector.y * FF_UNIT / 2 },
				{ startx + idirx, height_xy, - (starty + idiry) },
			}, mvp, c);

			Vector3i a;
			Vector2i b;
			a = {wx, height_xy, -(wy + 10)};
			pie_RotateProjectWithPerspective(&a, mvp, &b);
			auto cost = flowfield->integrationField.at(cellIdx);
			if (cost != COST_NOT_PASSABLE)
			{
				WzText cost_text(WzString (std::to_string(cost).c_str()), font_small);
				cost_text.render(b.x, b.y, WZCOL_TEXT_BRIGHT);
			}
			else { debugDrawImpassableCell(mvp, cellx, celly, height_xy);}
			
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
	uint16_t cell_startx, cell_starty;
	world_to_cell(startX, startY, cell_startx, cell_starty);
	//cell_startx = startX / FF_UNIT;
	//cell_starty = startY / FF_UNIT;
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
	uint16_t cellx, celly;
	world_to_cell(psDroid->pos.x, psDroid->pos.y, cellx, celly);
	ssprintf(tmpBuff, "Pos: %i %i (%i %i %i %i)", 
		psDroid->pos.x, psDroid->pos.y, 
		map_coord(psDroid->pos.x), map_coord(psDroid->pos.y), cellx, celly);
	debugRenderText(tmpBuff, idx++);
	
	memset(tmpBuff, 0, 64);
	ssprintf(tmpBuff, "Target (LB): %i %i (%i %i)", 
		psDroid->sMove.target.x, psDroid->sMove.target.y, 
		map_coord(psDroid->sMove.target.x), map_coord(psDroid->sMove.target.y));
	debugRenderText(tmpBuff, idx++);

	memset(tmpBuff, 0, 64);
	uint16_t cell_destx, cell_desty;
	world_to_cell(psDroid->sMove.destination.x, psDroid->sMove.destination.y, cell_destx, cell_desty);
	ssprintf(tmpBuff, "Destination (DB): %i %i (%i %i; %i %i)", 
		psDroid->sMove.destination.x, psDroid->sMove.destination.y, 
		map_coord(psDroid->sMove.destination.x), map_coord(psDroid->sMove.destination.y),
		cell_destx, cell_desty);
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
	uint16_t cell_radius;
	bool mod = world_to_cell(moveObjRadius(psDroid), cell_radius);
	cell_radius += (int) mod > 0;
	sprintf(tmpBuff, "Radius: %i (cells %i)", moveObjRadius(psDroid), cell_radius);
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
	uint16_t nwx, nwy;
	cell_to_world(cell_startx, cell_starty, nwx, nwy);
	curDraw (mvp, collisionRadius, nwx, nwy, height, WZCOL_RED);
	// debugDrawImpassableCell(mvp, cell_startx, cell_starty, height);

	memset(tmpBuff, 0, 64);
	uint16_t cells_radius;
	world_to_cell(collisionRadius, cells_radius);
	sprintf(tmpBuff, "Collision radius: %i (cells %i)", collisionRadius, cells_radius);
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
	std::vector<uint32_t> out;
	if (false) cells_of_tile(0,0, out);
}
