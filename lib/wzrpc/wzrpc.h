
#ifndef __INCLUDED_WZRPC_H__
#define __INCLUDED_WZRPC_H__
#include "src/research.h"


namespace wzrpc
{   

    enum class RET_STATUS
    {
        RET_OK,
        RET_ERR_CALL_NOT_FOUND,
        RET_INCORRECT_NB_ARGS,
        RET_MISSING_ARG,
        RET_JSON_UNPARSABLE,
        RET_JSON_TYPE_ERROR,
        RET_JSON_OTHER_ERROR,
        RET_OBJECT_NOT_FOUND,
        RET_NO_APPROPRIATE_STRUCTURE,
        RET_UKNOWN_OBJ_TYPE,
    };
    enum class EVENT_TYPE
    {
        ET_DAMAGED,
        ET_DROID_BUILT,
        ET_DROID_DESTROYED,
        ET_DROID_FIRED,
        ET_GAME_LOADED,
        ET_GAME_PAUSE_STATUS,
        ET_GAME_TIME,
        ET_RESEARCHED,
        ET_START_LEVEL,
    };
    enum class AUX_FLAG
    {
        AUX_EMPTY = 0,
        AUX_POSITIONS = 1,
        AUX_HITPOINTS = 1<<1,
        AUX_LAST_TIME_HIT = 1<<2,
        AUX_LAST_HIT_WEAPON = 1<<3,
        AUX_TARGET_ID = 1<<4,

    };
    void start();

    void doAddDroidAt(const nlohmann::json &request);
    void doAddStructure(const nlohmann::json &request);
    void doAddTemplate(const nlohmann::json &request);
    void doBuildDroidAnyFact(const nlohmann::json &request);
    void doCenterView(const nlohmann::json &request);
    void doCompleteAllResearch(const nlohmann::json &request);
    void doCompleteResearch(const nlohmann::json &request);
    void doDestroyAllDroids(const nlohmann::json &request);
    void doDestroyAllStructures(const nlohmann::json &request);
    void doPursueResearch(const nlohmann::json &request);
    void doRemoveFeatures(const nlohmann::json &request);
    void doRemoveStructure(const nlohmann::json &request);
    void doRevealAll(const nlohmann::json &request);
    void doSpeedUp(const nlohmann::json &request);
    void doToggleDebug(const nlohmann::json &request);
    void getAllDroidIds(const nlohmann::json &request);
    void getAllFeatures(const nlohmann::json &request);
    void getAllStructures(const nlohmann::json &request);
    void getAllStructuresOfType(const nlohmann::json &request);
    void getAllTemplates(const nlohmann::json &request);
    void getAvailableResearch(const nlohmann::json &request);
    void getDroidDetails(const nlohmann::json &request);
    void getDroidPosition(const nlohmann::json &request);
    void getGameSpeedMod(const nlohmann::json &request);
    void getPlayerPower(const nlohmann::json &request);
    void getPropulsionCanReach(const nlohmann::json &request);
    void getResearch(const nlohmann::json &request);
    void getSelected(const nlohmann::json &request);
    void getStructureDetails(const nlohmann::json &request);
    void setDroidOrder(const nlohmann::json &request);
    void setDroidSecondary(const nlohmann::json &request);
    void setFiringStatus(const nlohmann::json &request);
    void setAssemblyPoint(const nlohmann::json &request);
    /*
    void getDroidPosition();
    void getDroidPath();
    void getAllDroidHealth();
    void getAllDroidInfo();
    uint32_t getGameTime();
    typedef std::unordered_map<std::string, std::string> MapOfStr;
    std::vector<MapOfStr> getStatsArray() noexcept;
    void completeResearch();
    void completeAllResearch();
    //-- Returns an array of all research objects that are currently and immediately available for research.
    void enumResearch();
    //-- Returns an array of structure objects.
    std::vector<uint32_t>  enumStruct(int player, int visibleForPlayer, STRUCTURE_TYPE stype);

    int getDroidLimit();
    int32_t playerPower(int player);
    void setDroidExperience();
    //-- Returns an array of game objects seen within range of given position that passes the optional filter
    void enumRange();
    void pursueResearch();
    void findResearch();
    //-- Give a droid an order to do something at the given location.
    void orderDroidLoc(int player, uint32_t droidId, DROID_ORDER order, int x, int y);
    // start producing droid
    void buildDroid();
    // create and place a droid
    void addDroid();
    void makeTemplate();
    void getDroidProduction();
    void centreView();
    void setPower();
    void orderDroid();
*/
    // getDroidMovePath();...
    
    void notifyResearched(int, const RESEARCH *, const STRUCTURE *);
    void notifyDroidBuilt(const STRUCTURE *, const DROID*);
    void notifyGameLoaded(uint32_t gameTime);
    void notifyDroidDestroyed(const DROID*psDroid, uint32_t gameTime);
    void notifyGamePauseStatus(bool status);
    void notifyGameTime(uint32_t);
    void notifyDamaged(uint32_t id, int damage, bool isDamagePerSecond, WEAPON_CLASS, WEAPON_SUBCLASS, uint32_t gameTime);
    void notifyStartLevel(uint32_t gameTime);
    void notifyDroidShotFired(PROJECTILE* psProj);
    void sendError(RET_STATUS, std::string details = "");

    void bind_all();
}
#endif