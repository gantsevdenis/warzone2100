
/**
 * We have an indirection layer for Flow Field to reduce its size.
 * Since there 4 different body radii ( {Person =16}, {Cyborg, Small =32}, {Medium, Large =64}, {ExtraLarge =128} )
 * we need to calculate different flowfields for them: otherwise 1 Cyborg would block an entire tile
 * and another Cyborg wouldn't be able to pass through.
 * We break down the whole Map into 16x16 zones of the smallest size (16 worldunits)
 * and calculate Flowfield based on that:
 * - Map with 256x256 tiles at 128 worlunit each, => 1024x1024 cells, of 32x32 worldunits per cell.
 *   (this also supposes mouse-click precision should be at least 32x32 worldunits)
 *   So each regular tile is composed of 16 cells (each *side* 128 worldunits / FF_UNIT = 4 cells)
 * 
 * Cost field is complex: 
 * - for regular buildings, we want to mark all occupied tiles regardless of IMD model shape.
 * - However, scavanger walls, and Towers are really small, and it looks awkward to block the whole tile,
 *   so we need to actually take into account model shapes.
 * 
 * When structure or a feature gets destroyed, we should update existing flowfields by recalculating cost from
 * affected cells to neighbors.
 * 
 * Notes:
 * Might need to break down integration field into smaller sectors, so that 1MB array doesn't get recalculated ... 
 * Then, use some mean to go from one sector to another. 
 * However, how do we know those sectors excluded from calculation don't contain a better path? We don't, just assume.
 * Also, need to clear results from caches, they are getting heavy
 * May also reserve memory only for the *actual* Map size, not the maximum possible size (256x256 tiles), or maybe calculate
 * flows with GPU
 * 
*/

// Must be before some stuff from headers from flowfield.h, otherwise "std has no member 'mutex'"
// That simply means someone else have messed up
#include <mutex>

#include "flowfield.h"

#include <future>
#include <map>
#include <set>
#include <typeinfo>
#include <vector>
#include <memory>
#include <chrono>

#include "lib/framework/debug.h"
#include "lib/ivis_opengl/pieblitfunc.h"
#include "lib/ivis_opengl/piepalette.h"
#include "lib/ivis_opengl/textdraw.h"
#include "lib/ivis_opengl/piematrix.h"

#include "move.h"
#include "statsdef.h"
#include "display3d.h"
#include "map.h"
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
static bool flowfieldEnabled = false;
// yeah...initialization sequence is a spaghetti plate
static bool costInitialized = false;
// just for debug, remove for release builds
#ifdef DEBUG
	#define DEFAULT_EXTENT_X 2
	#define DEFAULT_EXTENT_Y 2
#else
	#define DEFAULT_EXTENT_X 1
	#define DEFAULT_EXTENT_Y 1
#endif

#define FF_TILE_SIZE 128
#define FF_GRID_1048576 1048576


void flowfieldEnable() {
	flowfieldEnabled = true;
}

bool isFlowfieldEnabled() {
	return flowfieldEnabled;
}

void flowfieldToggle()
{
	flowfieldEnabled = !flowfieldEnabled;
}

template<typename> struct TwiceWidth;
template<> struct TwiceWidth<uint8_t> { using type=uint16_t; };
template<> struct TwiceWidth<uint16_t> { using type=uint32_t; };

#define TILE_FACTOR  (mapWidth)
// number of world units on X axis of this Map
#define WORLD_FACTOR (mapWidth * FF_TILE_SIZE)
// number of Cells on X axis on this Map

#define CELL_X_LEN (mapWidth *  FF_TILE_SIZE / FF_UNIT) 
#define CELL_Y_LEN (mapHeight * FF_TILE_SIZE / FF_UNIT)
#define CELL_AREA  CELL_X_LEN * CELL_Y_LEN
#define IS_BETWEEN(X, LOW, HIGH) (LOW) < (X) && (X) < (HIGH)
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

static inline void tile_to_cell(uint8_t mapx, uint8_t mapy, uint16_t &cellx, uint16_t &celly)
{
	cellx = mapx * FF_TILE_SIZE / FF_UNIT;
	celly = mapy * FF_TILE_SIZE / FF_UNIT;
}

static inline uint32_t world_2Dto1D(uint16_t worldx, uint16_t worldy)
{
	return worldy * WORLD_FACTOR + worldx;
}

static inline void world_1Dto2D(uint32_t idx, uint16_t &worldx, uint16_t &worldy)
{
	worldx = idx % WORLD_FACTOR;
	worldy = idx / WORLD_FACTOR;
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
	celly = idx / CELL_X_LEN;
}

static inline uint16_t tiles_2Dto1D(uint8_t mapx, uint8_t mapy)
{
	return mapy * TILE_FACTOR + mapx;
}

static inline void tiles_1Dto2D(uint16_t idx, uint8_t &mapx, uint8_t &mapy)
{
	mapx = idx % TILE_FACTOR;
	mapy = idx / TILE_FACTOR;
}

// ignore the first one when iterating!
// 8 neighbours for each cell
static const int neighbors[DIR_TO_VEC_SIZE][2] = {
	{-0xDEAD, -0xDEAD}, // DIR_NONE
	{-1, -1}, {0, -1}, {+1, -1},
	{-1,  0},          {+1,  0},
	{-1, +1}, {0, +1}, {+1, +1}
};

constexpr const uint16_t COST_NOT_PASSABLE = std::numeric_limits<uint16_t>::max();
constexpr const uint16_t COST_MIN = 1; // default cost 

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
	// purely for debug
	int player;
};

void flowfieldInit() {
	if (!isFlowfieldEnabled()) return;

	if(mapWidth == 0 || mapHeight == 0){
		// called by both stageTwoInitialise() and stageThreeInitialise().
		// in the case of both these being called, map will be unavailable the first time.
		return;
	}

	initCostFields();
}

void flowfieldDestroy() {
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
static std::array< std::set<uint32_t>, 4> flowfieldCurrentlyActiveRequests
{
	std::set<uint32_t>(), // PROPULSION_TYPE_WHEELED
	std::set<uint32_t>(), // PROPULSION_TYPE_PROPELLOR
	std::set<uint32_t>(), // PROPULSION_TYPE_HOVER
	std::set<uint32_t>()  // PROPULSION_TYPE_LIFT
};

/// Flow field results. Key: Cells Array index.
static std::array< std::unordered_map<uint32_t, Flowfield* >, 4> flowfieldResults 
{
	std::unordered_map<uint32_t, Flowfield* >(), // PROPULSION_TYPE_WHEELED
	std::unordered_map<uint32_t, Flowfield* >(), // PROPULSION_TYPE_PROPELLOR
	std::unordered_map<uint32_t, Flowfield* >(), // PROPULSION_TYPE_HOVER
	std::unordered_map<uint32_t, Flowfield* >()  // PROPULSION_TYPE_LIFT
};

static std::unordered_map<unsigned int, Flowfield*> flowfieldById;

std::mutex flowfieldMutex;

void processFlowfield(FLOWFIELDREQUEST request);

void calculateFlowfieldAsync(uint16_t worldx, uint16_t worldy, PROPULSION_TYPE propulsion, int player)
{
	uint16_t cellx, celly;
	world_to_cell(worldx, worldy, cellx, celly);
	FLOWFIELDREQUEST request;
	request.cell_goalX = cellx;
	request.cell_goalY = celly;
	request.player = player;
	request.propulsion = propulsion;

	const uint32_t cell_goal = cells_2Dto1D(cellx, celly);

	if(flowfieldCurrentlyActiveRequests[propulsionIdx2[request.propulsion]].count(cell_goal))
	{
		debug (LOG_FLOWFIELD, "already waiting for %i (cellx=%i celly=%i)", cell_goal, cellx, celly);
		return;
	}
	debug (LOG_FLOWFIELD, "new async request for %i (cellx=%i celly=%i)", cell_goal, cellx, celly);
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
			debug (LOG_FLOWFIELD, "processing took %li, erasing %i", duration.count(), cell_goal);
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

//////////////////////////////////////////////////////////////////////////////////////

/// auto increment state for flowfield ids. Used on psDroid->sMove.uint32_t. 
// 0 means no flowfield exists for the unit.
uint32_t flowfieldIdInc = 1;

/** Cost of movement for each one-sixteenth of a tile */
struct CostField
{
	std::vector<uint16_t> cost;
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
		for(int dx = 0; dx < cellradius; dx++)
		{
			for(int dy = 0; dy < cellradius; dy++)
			{
				int cellx = cell_startx + dx;
				int celly = cell_starty + dy;
				if (!(IS_BETWEEN(cellx, -1, CELL_X_LEN))) continue;
				if (!(IS_BETWEEN(celly, -1, CELL_Y_LEN))) continue;
				uint16_t cellIdx = cells_2Dto1D(cellx, celly);
				// NOTE: maybe use another value for droids, because they actually can move, unlike a rock
				cost.at(cellIdx) = COST_NOT_PASSABLE;
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

	void tile_setCost(uint8_t mapx, uint8_t mapy, uint16_t value)
	{
		uint16_t cellx, celly;
		tile_to_cell(mapx, mapy, cellx, celly);
		// mark everything "1 tile = 4 cells" accross
		for (int dx = 0; dx < (FF_TILE_SIZE / FF_UNIT); dx++)
		{
			for (int dy = 0; dy < (FF_TILE_SIZE / FF_UNIT); dy++)
			{
				// NOTE: assume that, because mapx and mapy are within bounds [0, 255]
				//       means no need to check bounds of cellx + dx, celly + dy
				this->cost.at(cells_2Dto1D(cellx + dx, celly + dy)) = value;
			}
		}
		
	}

	void tile_setImpassable(uint8_t mapx, uint8_t mapy)
	{
		tile_setCost(mapx, mapy, COST_NOT_PASSABLE);
	}

	uint16_t tile_getCost(uint8_t mapx, uint8_t mapy) const
	{
		uint16_t cellx, celly;
		// FIXME: not sure what semantics we need here, or remove if not needed
		// most likely, return IMPASSABLE when at least one of the underlying cells
		// is IMPASSABLE
		tile_to_cell(mapx, mapy, cellx, celly);
		return this->cost.at(cells_2Dto1D(cellx, celly));
	}

	void adjust()
	{
		cost.resize(CELL_AREA);
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
	// prefer straight lines over diagonals
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

const std::array<Directions, 3> quad1_vecs = {
	Directions::DIR_4,
	Directions::DIR_2,
	Directions::DIR_1
};
const std::array<Directions, 3> quad2_vecs = {
	Directions::DIR_1,
	Directions::DIR_0,
	Directions::DIR_3
};
const std::array<Directions, 3> quad3_vecs = {
	Directions::DIR_3,
	Directions::DIR_5,
	Directions::DIR_6
};
const std::array<Directions, 3> quad4_vecs = {
	Directions::DIR_6,
	Directions::DIR_7,
	Directions::DIR_4
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

	const PROPULSION_TYPE prop; // so that we know what CostField should be referenced
	// goal can be an area, which starts at "goal",
	// and of TotalSurface = (goalXExtent) x (goalYExtent)
	// a simple goal sized 1 tile will have both extents equal to 1
	// a square goal sized 4 will have both extends equal to 2
	// vertical line of len = 2 will have goalYExtent equal to 2, and goalXextent equal to 1
	const uint16_t goalXExtent = 4; // cells units
	const uint16_t goalYExtent = 4; // cells units
	std::vector<Directions> dirs;  // cells units
	std::vector<bool> impassable; // TODO use bitfield if used, or remove if not used
	
	Flowfield (uint32_t id_, uint16_t goalX_, uint16_t goalY_, PROPULSION_TYPE prop_, uint16_t goalXExtent_, uint16_t goalYExtent_)
	: id(id_), goalX(goalX_), goalY(goalY_), prop(prop_), goalXExtent(goalXExtent_), goalYExtent(goalYExtent_)
	{
		// = 16 cells per tile
		dirs.resize(CELL_AREA);
		impassable.resize(CELL_AREA);
	}
	
	bool world_isImpassable (uint16_t worldx, uint16_t worldy) const
	{
		uint16_t cellx, celly;
		world_to_cell(worldx, worldy, cellx, celly);
		return impassable.at(cells_2Dto1D(cellx, celly));
	}

	bool cell_isImpassable(uint16_t cellx, uint16_t celly) const 
	{
		return impassable.at(cells_2Dto1D(cellx, celly));
	}

	Directions cell_getDir (uint16_t cellx, uint16_t celly, uint8_t radius) const
	{
		if (radius == FF_UNIT)
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
					if (!(IS_BETWEEN(cellx + dx, -1, CELL_X_LEN))) continue;
					if (!(IS_BETWEEN(celly + dy, -1, CELL_Y_LEN))) continue;
					uint16_t cellIdx = cells_2Dto1D(cellx + dx, celly + dy);
					const Directions d = dirs.at(cellIdx);
					neighb_directions[(int) d]++;
				}
			}
			int min_sofar = 0xFFFF;
			int min_idx = 0xFFFF;
			for (int i = 0; i < 9; i++)
			{
				if (IS_BETWEEN(neighb_directions[i], -1, min_sofar))
				{
					min_sofar = neighb_directions[i];
					min_idx = i;
				}
			}
			return static_cast<Directions>(min_idx);
		}
		
	}

	Directions world_getDir (uint16_t worldx, uint16_t worldy, uint8_t radius) const
	{
		uint16_t cellx, celly;
		world_to_cell(worldx, worldy, cellx, celly);
		return cell_getDir(cellx, celly, radius);
	}

	Vector2f world_getVector(uint16_t worldx, uint16_t worldy) const
	{
		uint16_t cellx, celly;
		world_to_cell(worldx, worldy, cellx, celly);
		return dirToVec[(int) dirs.at(cells_2Dto1D(cellx, celly))];
	}

	void calculateFlows()
	{
		const std::vector<uint16_t> integrationField = integrateCosts();
		dirs.resize(CELL_AREA);
		ASSERT_OR_RETURN(, integrationField.size() == CELL_AREA, "invariant failed");
		static_assert((int) Directions::DIR_NONE == 0, "Invariant failed");
		static_assert(DIR_TO_VEC_SIZE == 9, "dirToVec must be sync with Directions!");
		//debug (LOG_FLOWFIELD, "flowfield size=%lu", integrationField.size());
		for (uint32_t cellIdx = 0; cellIdx < integrationField.size(); cellIdx++)
		{
			const auto cost = integrationField[cellIdx];
			if (cost == COST_NOT_PASSABLE) continue;
			uint16_t cellx, celly;
			cells_1Dto2D(cellIdx, cellx, celly);
			// debug (LOG_FLOWFIELD, "cellidx -> %i: %i %i", cellIdx, cellx, celly);
			if (isInsideGoal (goalX, goalY, cellx, celly, goalXExtent, goalYExtent)) continue;
			uint16_t costs[8] = {0};
			// we don't care about DIR_NONE
			for (int neighb = (int) Directions::DIR_0; neighb < DIR_TO_VEC_SIZE; neighb++)
			{
				if (!(IS_BETWEEN(cellx + neighbors[neighb][0], -1, CELL_X_LEN))) continue;
				if (!(IS_BETWEEN(celly + neighbors[neighb][1], -1, CELL_Y_LEN))) continue;
				const auto neighb_cellIdx = cells_2Dto1D (cellx + neighbors[neighb][0], celly + neighbors[neighb][1]);
				// debug (LOG_FLOWFIELD, "neighb #%i: neighb_cellidx=%i, vals=%i %i", neighb - 1, neighb_cellIdx, neighbors[neighb][0], neighbors[neighb][1]);
				// substract 1, because Directions::DIR_0 is 1
				costs[neighb - 1] = integrationField.at(neighb_cellIdx);
			}
			uint8_t minCostIdx = minIndex (costs, 8);
			ASSERT (minCostIdx >= 0 && minCostIdx < 8, "invariant failed");
			Directions dir = (Directions) (minCostIdx + (int) Directions::DIR_0);
			dirs.at(cellIdx) = dir;
		}
		return;
	}

private:
	std::vector<uint16_t> integrateCosts()
	{
		std::vector<uint16_t> integrationField;
		integrationField.resize(CELL_AREA);
		for (int i = 0; i < MAX_PLAYERS; i++)
		{
			for (DROID *psCurr = apsDroidLists[i]; psCurr != nullptr; psCurr = psCurr->psNext)
			{
				if(psCurr->sMove.Status == MOVEINACTIVE)
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
							if (!(IS_BETWEEN(cell_startx + dx, -1, CELL_X_LEN))) continue;
							if (!(IS_BETWEEN(cell_starty + dy, -1, CELL_Y_LEN))) continue;
							uint16_t cellIdx = cells_2Dto1D(cell_startx + dx, cell_starty + dy);
							// NOTE: maybe use another value for droids, because they actually can move, unlike a rock
							integrationField.at(cellIdx) = COST_NOT_PASSABLE;
						}
					}
				}
			}
		}
		// Thanks to priority queue, we get the water ripple effect - closest tile first.
		// First we go where cost is the lowest, so we don't discover better path later.
		std::priority_queue<Node> openSet;
		const auto predecessorCost = 0;
		for (int dx = 0; dx < goalXExtent; dx++)
		{
			for (int dy = 0; dy < goalYExtent; dy++)
			{
				if (!(IS_BETWEEN(goalX + dx, -1, CELL_X_LEN))) continue;
				if (!(IS_BETWEEN(goalY + dy, -1, CELL_Y_LEN))) continue;
				const auto index = cells_2Dto1D(goalX + dx, goalY + dy);
				openSet.push(Node { predecessorCost, index });
			}
		}
		while (!openSet.empty())
		{
			integrateNode(openSet, integrationField);
			openSet.pop();
		}
		return integrationField;
	}

	void integrateNode(std::priority_queue<Node> &openSet, std::vector<uint16_t> &integrationField)
	{
		const Node& node = openSet.top();
		auto cost = costFields.at(propulsionIdx2[prop])->cell_getCost(node.index);

		if (cost == COST_NOT_PASSABLE) return;
		// Go to the goal, no matter what
		if (node.predecessorCost == 0) cost = COST_MIN;
		const uint16_t integrationCost = node.predecessorCost + cost;
		const uint16_t oldIntegrationCost = integrationField.at(node.index);
		uint16_t cellx, celly;
		const uint32_t cellYlen = CELL_AREA / CELL_X_LEN;
		const uint32_t cellXlen = CELL_X_LEN;
		cells_1Dto2D (node.index, cellx, celly);
		if (integrationCost < oldIntegrationCost)
		{
			integrationField.at(node.index) = integrationCost;
			for (int neighb = (int) Directions::DIR_0; neighb < DIR_TO_VEC_SIZE; neighb++)
			{
				int neighbx, neighby;
				neighbx = cellx + neighbors[neighb][0];
				neighby = celly + neighbors[neighb][1];
				if (!(IS_BETWEEN(neighbx, -1, CELL_X_LEN))) continue;
				if (!(IS_BETWEEN(neighby, -1, CELL_Y_LEN))) continue;
				if (neighby > 0 && neighby < cellYlen &&
					neighbx > 0 && neighbx < cellXlen)
				{
					openSet.push({ integrationCost, cells_2Dto1D(neighbx, neighby) });
				}
			}
		}
	}
};

bool tryGetFlowfieldForTarget(uint16_t worldx,
                              uint16_t worldy,
							  PROPULSION_TYPE propulsion)
{
	// caches needs to be updated each time there a structure built / destroyed
	uint16_t cellx, celly;
	world_to_cell(worldx, worldy, cellx, celly);
	uint32_t goal = cells_2Dto1D(cellx, celly);
	const auto results = flowfieldResults[propulsionIdx2[propulsion]];
	// this check is already done in fpath.cpp.
	// TODO: we should perhaps refresh the flowfield instead of just bailing here.
	return results.count(goal) > 0;
}

/** Position is world coordinates. Radius is world units too */
bool tryGetFlowfieldDirection(PROPULSION_TYPE prop, const Position &pos, uint8_t radius, Directions &out)
{
	const auto results = flowfieldResults[propulsionIdx2[prop]];
	uint16_t cellx, celly;
	uint32_t cell_goal;
	world_to_cell(pos.x, pos.y, cellx, celly);
	cell_goal = cells_2Dto1D(cellx, celly);
	if(!flowfieldById.count(cell_goal)) return false;
	auto flowfield = flowfieldById[cell_goal];
	out = flowfield->world_getDir(pos.x, pos.y, radius);
	return true;
}

// TODO: remove if not needed
// bool flowfieldIsImpassable(uint32_t id, uint16_t worldx, uint16_t worldy)
// {
// 	auto flowfield = flowfieldById.at(id);
// 	return flowfield->isImpassable(x, y);
// }


void processFlowfield(FLOWFIELDREQUEST request)
{
	// NOTE for us noobs!!!! This function is executed on its own thread!!!!
	static_assert(PROPULSION_TYPE_NUM == 7, "new propulsions need to handled!!");
	// const auto costField = costFields[propulsionIdx2[request.propulsion]];
	std::unordered_map<uint32_t, Flowfield* > *results = &flowfieldResults[propulsionIdx2[request.propulsion]];
	uint16_t cell_extentX, cell_extentY;
	tile_to_cell(DEFAULT_EXTENT_X, DEFAULT_EXTENT_Y, cell_extentX, cell_extentY);
	const uint32_t cell_goal = cells_2Dto1D (request.cell_goalX, request.cell_goalY);
 	// this check is already done in fpath.cpp.
	// TODO: we should perhaps refresh the flowfield instead of just bailing here.
	uint32_t ffid;
	if (results->count(cell_goal))
	{
		debug (LOG_FLOWFIELD, "already found in results");
		return;
	}
	debug (LOG_FLOWFIELD, "not found %i in results", cell_goal);
	// FIXME: remove, not needed; also check activeRequests for in-process calculations
	{
		std::lock_guard<std::mutex> lock(flowfieldMutex);
		ffid = flowfieldIdInc++;
	}
	Flowfield* flowfield = new Flowfield(ffid, request.cell_goalX, request.cell_goalY, request.propulsion, cell_extentX, cell_extentY);
	flowfieldById.insert(std::make_pair(flowfield->id, flowfield));
	debug (LOG_FLOWFIELD, "calculating flowfield %i player=%i, at (cellx=%i len %i) (celly=%i len %i)", flowfield->id, 
		request.player, request.cell_goalX, flowfield->goalXExtent,
		request.cell_goalY, flowfield->goalYExtent);
	flowfield->calculateFlows();
	{
		std::lock_guard<std::mutex> lock(flowfieldMutex);
		// store the result, this will be checked by fpath.cpp
		// NOTE: we are storing Goal in cell units
		debug (LOG_FLOWFIELD, "inserting %i into results, results size=%li", cell_goal, results->size());
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

/** Update a given tile as impossible to cross.
 * TODO also must invalidate cached flowfields, because they
 * were all based on obsolete cost/integration fields!
 */
void cbStructureBuilt(const STRUCTURE *structure)
{
	if (!costInitialized) return;
	if (structCBSensor(structure))
	{
		// take real size into account
		auto radius = structure->sDisplay.imd->radius / 2;
		costFields[PROPULSION_TYPE_WHEELED]->world_setImpassable(structure->pos, radius);
	}
	else
	{
		// assume it takes at least 1 whole tile
		StructureBounds b = getStructureBounds(structure);
		uint8_t mapx, mapy;
		mapx = map_coord(structure->pos.x);
		mapy = map_coord(structure->pos.y);
		debug (LOG_FLOWFIELD, "marking impassable: %i %i (%i %i), player=%i",
		       mapx, mapy, b.size.x, b.size.y, structure->player);
		for (int dx = 0; dx < b.size.x; ++dx)
		{
			for (int dy = 0; dy < b.size.y; ++dy)
			{
				// assume structures have correct bounds, no need for bound check, mapx + dx,  mapy + dy
				costFields[PROPULSION_TYPE_WHEELED]->tile_setImpassable(mapx + dx, mapy + dy);
			}
		}
	}
	
}

void cbStructureDestroyed(const STRUCTURE *structure)
{
	if (!costInitialized) return;
	if (structCBSensor(structure))
	{
		// take real size into account
		auto radius = structure->sDisplay.imd->radius / 2;
		costFields[propulsionIdx2[PROPULSION_TYPE_HOVER]]->world_setCost(structure->pos, COST_MIN, radius);
		costFields[propulsionIdx2[PROPULSION_TYPE_WHEELED]]->world_setCost(structure->pos, COST_MIN, radius);
	}
	else
	{
		// assume it takes at least 1 whole tile
		StructureBounds b = getStructureBounds(structure);
		uint8_t mapx, mapy;
		mapx = map_coord(structure->pos.x);
		mapy = map_coord(structure->pos.y);
		for (int dx = 0; dx < b.size.x; ++dx)
		{
			for (int dy = 0; dy < b.size.y; ++dy)
			{
				// assume structures have correct bounds, no need for bound check, mapx + dx,  mapy + dy
				costFields[propulsionIdx2[PROPULSION_TYPE_HOVER]]->tile_setCost(mapx + dx, mapy + dy, COST_MIN);
				costFields[propulsionIdx2[PROPULSION_TYPE_WHEELED]]->tile_setCost(mapx + dx, mapy + dy, COST_MIN);
			}
		}
	}
}

uint16_t calculateTileCost(uint16_t x, uint16_t y, PROPULSION_TYPE propulsion)
{
	// TODO: Current impl forbids VTOL from flying over short buildings
	// Note: this might need refactoring as we may not want to rely on old path finding
	// logic to decide what is passable, and what isn't
	// I leave it as is for now
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

void initCostFields()
{
	costInitialized = true;
	debug (LOG_FLOWFIELD, "Cell Area=%i MapWidth=%i MapHeight=%i, CELL_X_LEN=%i CELL_Y_LEN=%i",
	       CELL_AREA, mapWidth, mapHeight, CELL_X_LEN, CELL_Y_LEN);
	costFields[propulsionIdx2[PROPULSION_TYPE_WHEELED]]->adjust();
	costFields[propulsionIdx2[PROPULSION_TYPE_PROPELLOR]]->adjust();
	costFields[propulsionIdx2[PROPULSION_TYPE_HOVER]]->adjust();
	costFields[propulsionIdx2[PROPULSION_TYPE_LIFT]]->adjust();
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
	debug (LOG_FLOWFIELD, "init cost field done.");
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
static void drawSquare (const glm::mat4 &mvp, int sidelen, int startPointX, int startPointY, int height, PIELIGHT color)
{
	iV_PolyLine({
		{ startPointX - (sidelen / 2), height, -startPointY - (sidelen / 2) },
		{ startPointX - (sidelen / 2), height, -startPointY + (sidelen / 2) },
		{ startPointX + (sidelen / 2), height, -startPointY + (sidelen / 2) },
		{ startPointX + (sidelen / 2), height, -startPointY - (sidelen / 2) },
		{ startPointX - (sidelen / 2), height, -startPointY - (sidelen / 2) },
	}, mvp, color);
}

static void renderDebugText (const char *txt, int vert_idx)
{
	const int TEXT_SPACEMENT = 20;
	WzText t(WzString (txt), font_regular);
	t.render(20, 80 + TEXT_SPACEMENT * vert_idx, WZCOL_TEXT_DARK);
}

void debugDrawFlowfield(const DROID *psDroid, const glm::mat4 &mvp) 
{
	const auto playerXTile = map_coord(playerPos.p.x);
	const auto playerZTile = map_coord(playerPos.p.z);
	//Flowfield* flowfield = nullptr;
	PROPULSION_STATS       *psPropStats = asPropulsionStats + psDroid->asBits[COMP_PROPULSION];
	if (!tryGetFlowfieldForTarget(psDroid->pos.x, psDroid->pos.y, psPropStats->propulsionType)) return;
	
	// const auto& costField = costFields[propulsionIdx2[PROPULSION_TYPE_WHEELED]];
	for (auto deltaX = -6; deltaX <= 6; deltaX++)
	{
		const auto x = playerXTile + deltaX;

		if (x < 0) continue;
		
		for (auto deltaZ = -6; deltaZ <= 6; deltaZ++)
		{
			const auto z = playerZTile + deltaZ;

			if(z < 0) continue;

			const int XA = world_coord(x);
			const int XB = world_coord(x + 1);
			const int ZA = world_coord(z);
			const int ZB = world_coord(z + 1);
			const int YAA = map_TileHeight(x, z);
			const int YBA = map_TileHeight(x + 1, z);
			const int YAB = map_TileHeight(x, z + 1);
			const int YBB = map_TileHeight(x + 1, z + 1);
			
			// int height = map_TileHeight(x, z);

			// tile
			iV_PolyLine({
				{ XA, YAA, -ZA },
				{ XA, YAB, -ZB },
				{ XB, YBB, -ZB },
				{ XB, YBA, -ZA },
				{ XA, YAA, -ZA },
			}, mvp, WZCOL_GREY);

			// cost
			// const Vector3i a = { (XA + 20), height, -(ZA + 20) };
			// Vector2i b;
			// pie_RotateProjectWithPerspective(&a, mvp, &b);
			// auto cost = costField->getCost(x, z);
			// if (!flowfield->isImpassable(x, z))
			// {
			// 	WzText costText(WzString::fromUtf8(std::to_string(cost)), font_small);
			// 	costText.render(b.x, b.y, WZCOL_TEXT_BRIGHT);
			// 	// position
			// 	if(x < 999 && z < 999){
			// 		char positionString[7];
			// 		ssprintf(positionString, "%i,%i", x, z);
			// 		const Vector3i positionText3dCoords = { (XA + 20), height, -(ZB - 20) };
			// 		Vector2i positionText2dCoords;
			// 		pie_RotateProjectWithPerspective(&positionText3dCoords, mvp, &positionText2dCoords);
			// 		WzText positionText(positionString, font_small);
			// 		positionText.render(positionText2dCoords.x, positionText2dCoords.y, WZCOL_RED);
			// 	}
			// }
	 	}
	}
	/*
	// flowfields
	for (auto deltaX = -6; deltaX <= 6; deltaX++)
	{
		const int x = playerXTile + deltaX;
		
		if (x < 0) continue;

		for (auto deltaZ = -6; deltaZ <= 6; deltaZ++)
		{
			const int z = playerZTile + deltaZ;

			if (z < 0) continue;
			
			
			const float XA = world_coord(x);
			const float ZA = world_coord(z);

			auto vector = flowfield->getVector(x, z);
			
			auto startPointX = XA + FF_TILE_SIZE / 2;
			auto startPointY = ZA + FF_TILE_SIZE / 2;

			auto height = map_TileHeight(x, z) + 10;

			// origin
			if (!flowfield->isImpassable(x, z))
			{
				drawSquare(mvp, 16, startPointX, startPointY, height, WZCOL_WHITE);
				// direction
				iV_PolyLine({
					{ startPointX, height, -startPointY },
					{ startPointX + vector.x * 75, height, -startPointY - vector.y * 75 },
				}, mvp, WZCOL_WHITE);
			}

			// integration fields
			const Vector3i integrationFieldText3dCoordinates { (XA + 20), height, -(ZA + 40) };
			Vector2i integrationFieldText2dCoordinates;

			pie_RotateProjectWithPerspective(&integrationFieldText3dCoordinates, mvp, &integrationFieldText2dCoordinates);
			auto integrationCost = flowfield->integrationField->getCost(x, z);
			if (!flowfield->isImpassable(x, z))
			{
				WzText costText(WzString::fromUtf8 (std::to_string(integrationCost)), font_small);
				costText.render(integrationFieldText2dCoordinates.x, integrationFieldText2dCoordinates.y, WZCOL_TEXT_BRIGHT);
			}
		}
	}*/

	// for (int dx = 0; dx < flowfield->goalXExtent; dx++)
	// {
	// 	for (int dy = 0; dy < flowfield->goalYExtent; dy++)
	// 	{
	// 		uint16_t _goalx, _goaly;
	// 		tileFromIndex32 (flowfield->goal, _goalx, _goaly);
	// 		_goalx += dx;
	// 		_goaly += dy;
	// 		auto goalX = world_coord(_goalx) + FF_TILE_SIZE / 2;
	// 		auto goalY = world_coord(_goaly) + FF_TILE_SIZE / 2;
	// 		auto height = map_TileHeight(_goalx, _goaly) + 10;
	// 		drawSquare(mvp, 16, goalX, goalY, height, WZCOL_RED);
	// 	}
	// }
}

// even if flowfield disabled, just for sake of information
static void drawUnitDebugInfo (const DROID *psDroid, const glm::mat4 &mvp)
{
	// some droid sinfo
	auto startPointX = psDroid->pos.x;
	auto startPointY = psDroid->pos.y;
	auto target = (psDroid->pos.xy() - psDroid->sMove.target);
	auto destination = (psDroid->pos.xy() - psDroid->sMove.destination);
	auto height = map_TileHeight(map_coord(startPointX), map_coord(startPointY)) + 10;
	iV_PolyLine({
		{ startPointX, height, -startPointY },
		{ startPointX + static_cast<int>(target.x), height, 
		 -startPointY - static_cast<int>(target.y)},
		}, mvp, WZCOL_LBLUE);
	iV_PolyLine({
		{ startPointX, height, -startPointY },
		{ startPointX + static_cast<int>(destination.x), height, 
		 -startPointY - static_cast<int>(destination.y)},
		}, mvp, WZCOL_DBLUE);

	int idx = 0;
	char tmpBuff[64] = {0};
	ssprintf(tmpBuff, "Selected Droid %i", psDroid->id);
	renderDebugText(tmpBuff, idx++);

	tmpBuff[64] = {0};
	ssprintf(tmpBuff, "Flowfield is %s", flowfieldEnabled ? "ON" : "OFF");
	renderDebugText(tmpBuff, idx++);

	tmpBuff[64] = {0};
	ssprintf(tmpBuff, "Target (light blue): %i %i (%i %i)", 
		psDroid->sMove.target.x, psDroid->sMove.target.y, 
		map_coord(psDroid->sMove.target.x), map_coord(psDroid->sMove.target.y));
	renderDebugText(tmpBuff, idx++);

	tmpBuff[64] = {0};
	ssprintf(tmpBuff, "Destination (dark blue): %i %i (%i %i)", psDroid->sMove.destination.x, psDroid->sMove.destination.y, 
		map_coord(psDroid->sMove.destination.x), map_coord(psDroid->sMove.destination.y));
	renderDebugText(tmpBuff, idx++);

	tmpBuff[64] = {0};
	ssprintf(tmpBuff, "Src: %i %i (%i %i)", psDroid->sMove.src.x, psDroid->sMove.src.y, 
		map_coord(psDroid->sMove.src.x), map_coord(psDroid->sMove.src.y));
	renderDebugText(tmpBuff, idx++);

	tmpBuff[64] = {0};
	sprintf(tmpBuff, "moveDir: %i (%.2f %.2f)", psDroid->sMove.moveDir, 
		static_cast<float>(iSin(psDroid->sMove.moveDir)) / static_cast<float>((1 << 16)), 
		static_cast<float>(iCos(psDroid->sMove.moveDir)) / static_cast<float>((1 << 16)));
	renderDebugText(tmpBuff, idx++);

	PROPULSION_STATS       *psPropStats = asPropulsionStats + psDroid->asBits[COMP_PROPULSION];
	tmpBuff[64] = {0};
	sprintf(tmpBuff, "speed: %i (Prop maxspeed=%i)", psDroid->sMove.speed, psPropStats->maxSpeed);
	renderDebugText(tmpBuff, idx++);

	tmpBuff[64] = {0};
	if (psDroid->sMove.pathIndex != (int)psDroid->sMove.asPath.size())
	{
		Vector2i next = psDroid->sMove.asPath[psDroid->sMove.pathIndex];
		sprintf(tmpBuff, "Next path target: %i %i, path len %lu", map_coord(next.x), map_coord(next.y), psDroid->sMove.asPath.size());
		renderDebugText(tmpBuff, idx++);
	}
	else if (psDroid->sMove.asPath.size() == 1)
	{
		Vector2i next = psDroid->sMove.asPath[0];
		sprintf(tmpBuff, "Next path (and only) target: %i %i (%i %i)", next.x, next.y, map_coord(next.x), map_coord(next.y));
		renderDebugText(tmpBuff, idx++);
	}

	tmpBuff[64] = {0};
	uint16_t cell_radius;
	bool mod = world_to_cell(moveObjRadius(psDroid), cell_radius);
	cell_radius += (int) mod > 0;
	sprintf(tmpBuff, "Radius: %i (cells %i)", moveObjRadius(psDroid), cell_radius);
	renderDebugText(tmpBuff, idx++);
	
	tmpBuff[64] = {0};
	sprintf(tmpBuff, "Prop SpinSpeed (DEG %i): %i", psPropStats->spinSpeed, DEG(psPropStats->spinSpeed));
	renderDebugText(tmpBuff, idx++);

	tmpBuff[64] = {0};
	sprintf(tmpBuff, "Prop SpinAngle DEG: %i", psPropStats->spinAngle);
	renderDebugText(tmpBuff, idx++);

	tmpBuff[64] = {0};
	sprintf(tmpBuff, "Prop TurnSpeed: %i", psPropStats->turnSpeed);
	renderDebugText(tmpBuff, idx++);

	tmpBuff[64] = {0};
	sprintf(tmpBuff, "Prop Acceleration: %i", psPropStats->acceleration);
	renderDebugText(tmpBuff, idx++);

	tmpBuff[64] = {0};
	sprintf(tmpBuff, "Prop Deceleration: %i", psPropStats->deceleration);
	renderDebugText(tmpBuff, idx++);
	
	auto collisionRadius = moveObjRadius(psDroid);
	drawSquare (mvp, collisionRadius, startPointX, startPointY, height, WZCOL_RED);

	tmpBuff[64] = {0};
	uint16_t cells_radius;
	world_to_cell(collisionRadius, cells_radius);
	sprintf(tmpBuff, "Collision radius: %i (cells %i)", collisionRadius, cells_radius);
	renderDebugText(tmpBuff, idx++);

	tmpBuff[64] = {0};
	sprintf(tmpBuff, "Flowfield size (ground): %li", flowfieldResults[0].size());
	renderDebugText(tmpBuff, idx++);
}


static DROID *lastSelected = nullptr;
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
	if (!lastSelected) return;
	drawUnitDebugInfo(lastSelected, mvp);
	if (isFlowfieldEnabled()) 
		debugDrawFlowfield(lastSelected, mvp);
}
