#include <unordered_set>
#include "lib/framework/frame.h"
#include "lib/framework/debug.h"
#include "lib/gamelib/gtime.h"
//#include "lib/wzmaplib/include/wzmaplib/map.h"
#include "wzrpc.h"
#include "src/wzapi.h"
#include "src/objmem.h"
#include "src/droid.h"
#include "src/order.h"
#include "src/structure.h"
#include "src/feature.h"
#include "src/template.h"
#include "src/display.h"
#include "src/keybind.h"

//#include "lib/map.h" // can't include this..
#include "tcp_server.h"
#include "udp_server.h"

using wzrpc::RET_STATUS;
using wzrpc::EVENT_TYPE;
using wzrpc::AUX_FLAG;
// defined in structure.cpp
extern std::unordered_set<uint32_t> bannedFromFiring;
static bool validate_arguments(const nlohmann::json &request, const char *argnames[], int count);
static DROID *findById(int player, uint32_t id);
#define LEN_ARGS(args) sizeof(args)/sizeof(args[0])
#define FOR_EACH_DROID(player) for (DROID *psDroid = apsDroidLists[player]; psDroid; psDroid = psDroid->psNext)
#define FOR_EACH_STRUCTURE(player) for (STRUCTURE *psStruct = apsStructLists[player]; psStruct; psStruct = psStruct->psNext)
#define SEND_ERROR_0(status) wzrpc::sendError(status); return
#define SEND_ERROR_1(status, details) wzrpc::sendError(status, details); return
class wzrpc_context : public wzapi::execution_context
{
public:
    wzrpc_context(){};
    int _player;
    void throwError(const char *expr, int line, const char *function) const override
    {

    }
    wzapi::scripting_instance* currentInstance() const override
    {
        return  nullptr;
    }
    playerCallbackFunc getNamedScriptCallback(const WzString& func) const override
    {
        throw ;
    }
    void doNotSaveGlobal(const std::string &global) const override
    {

    }
    int player() {return _player;};
    // not const, we want modify them
    std::unordered_map<uint32_t, DROID*> idToDroid[MAX_PLAYERS];
    std::unordered_map<uint32_t, STRUCTURE*> idToStructure[MAX_PLAYERS];
};
static wzrpc_context ctx;


void wzrpc::start()
{
    udp_start();
    tcp_start();
    bind_all();
    wzrpc_context _ctx;
    ctx = _ctx;
}


static bool validate_arguments(const nlohmann::json &request, const char *argnames[], int count)
{
    if (request.size() < count)
    {
        SEND_ERROR_0(RET_STATUS::RET_INCORRECT_NB_ARGS) false;
    }
    for (size_t i = 0; i < count; i++)
    {
        if (!request.contains(argnames[i]))
        {

            SEND_ERROR_1(RET_STATUS::RET_MISSING_ARG, argnames[i]) false;
        }
    }
    return true;
}
/*static void interpet_aux_flag(const nlohmann::json &request)
{
    auto flag = request.at("flag").get<AUX_FLAG>();
    if (flag == AUX_FLAG::AUX_EMPTY)
    {
        return;
    }

}*/
// -------------------- RPC --------------------------------
void wzrpc::bind_all()
{
    tcp_bind("doAddDroidAt",&wzrpc::doAddDroidAt);
    tcp_bind("doAddStructure", &wzrpc::doAddStructure);
    tcp_bind("doAddTemplate", &wzrpc::doAddTemplate);
    tcp_bind("doBuildDroidAnyFact", &wzrpc::doBuildDroidAnyFact);
    tcp_bind("doCenterView", &wzrpc::doCenterView);
    tcp_bind("doCompleteResearch", &wzrpc::doCompleteResearch);
    tcp_bind("doDestroyAllDroids", &wzrpc::doDestroyAllDroids);
    tcp_bind("doDestroyAllStructures", &wzrpc::doDestroyAllStructures);
    tcp_bind("doPursueResearch", &wzrpc::doPursueResearch);
    tcp_bind("doRevealAll", &wzrpc::doRevealAll);
    tcp_bind("doRemoveFeatures", &wzrpc::doRemoveFeatures);
    tcp_bind("doSpeedUp", &wzrpc::doSpeedUp);
    tcp_bind("doToggleDebug", &wzrpc::doToggleDebug);
    tcp_bind("getAllDroidIds", &wzrpc::getAllDroidIds);
    tcp_bind("getAllFeatures", &wzrpc::getAllFeatures);
    tcp_bind("getAllStructuresOfType",&wzrpc::getAllStructuresOfType);
    tcp_bind("getAllTemplates", &wzrpc::getAllTemplates);
    tcp_bind("getAvailableResearch", &wzrpc::getAvailableResearch);
    tcp_bind("getGameSpeedMod", &wzrpc::getGameSpeedMod);
    tcp_bind("getDroidDetails", &wzrpc::getDroidDetails);
    tcp_bind("getPropulsionCanReach", &wzrpc::getPropulsionCanReach);
    tcp_bind("getSelected", &wzrpc::getSelected);
    tcp_bind("getStructureDetails", &wzrpc::getStructureDetails);
    tcp_bind("setDroidOrder", &wzrpc::setDroidOrder);
    tcp_bind("setDroidSecondary", &wzrpc::setDroidSecondary);
    tcp_bind("setFiringStatus", &wzrpc::setFiringStatus);
    tcp_bind("setAssemblyPoint", &wzrpc::setAssemblyPoint);
    //tcp_bind("getTerrainType", &wzrpc::getTerrainType);
}
void wzrpc::sendError(wzrpc::RET_STATUS status, std::string details)
{
    auto o = nlohmann::json::object();
    o["error"] = status;
    if (!details.empty())
    {
        o["details"] = details;
    }
    const std::vector<std::uint8_t> out = json::to_msgpack(o);
    tcp_send_data(o);
    return;
}

void wzrpc::getSelected(const nlohmann::json &request)
{
    std::vector<const BASE_OBJECT *> selected = wzapi::enumSelected();
    auto array = nlohmann::json::array();
    for (int i = 0; i < selected.size(); i++)
    {
        auto o = nlohmann::json::object();
        o["id"] = selected[i]->id;
        o["type"] = selected[i]->type;
        array.insert(array.end(), o);       
    }
    tcp_send_data(array);
    return;
}


void wzrpc::getAllDroidIds(const nlohmann::json &request)
{
    const char *args[] = {"player"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    const auto player = request.at("player").get<int>();
    auto flag = request.at("flag").get<uint8_t>();

    std::vector<const BASE_OBJECT *> selected = wzapi::enumSelected();
    auto array = nlohmann::json::array();

    FOR_EACH_DROID(player)
	{
        auto o = nlohmann::json::object();
        o["id"] = psDroid->id;
        o["hp"] = psDroid->body;
        o["periodicalDamage"] = psDroid->periodicalDamage;
        array.insert(array.end(), o);
	}
    tcp_send_data(array);
    return;
}

void wzrpc::getAllStructuresOfType(const nlohmann::json &request)
{
    const char *args[] = {"player", "visibleForPlayer", "stype"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    STRUCTURE_TYPE stype = request.at("stype").get<STRUCTURE_TYPE>();
    const int player = request.at("player").get<int>();
    int visibleForPlayer = request.at("visibleForPlayer").get<int>();

    wzapi::STRUCTURE_TYPE_or_statsName_string _type;
    _type.type = stype;
    std::vector<const STRUCTURE *> structures = wzapi::enumStruct(ctx, player, _type, visibleForPlayer);
    auto array = nlohmann::json::array();
    for (auto s: structures)
    {
        auto o = nlohmann::json::object();
        o["id"] = s->id;
        array.insert(array.end(), s->id);
    }
    tcp_send_data(array);
    return;
}
void wzrpc::getAllStructures(const nlohmann::json &request)
{
    const char *args[] = {"player", "visibleForPlayer"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    const int player = request.at("player").get<int>();
    int visibleForPlayer = request.at("visibleForPlayer").get<int>();

    wzapi::STRUCTURE_TYPE_or_statsName_string _type;
    std::vector<const STRUCTURE *> structures = wzapi::enumStruct(ctx, player, nonstd::optional_lite::nullopt, visibleForPlayer);
    auto array = nlohmann::json::array();
    for (auto s: structures)
    {
        auto o = nlohmann::json::object();
        o["id"] = s->id;
        array.insert(array.end(), s->id);
    }
    tcp_send_data(array);
}

void wzrpc::doDestroyAllDroids(const nlohmann::json &request)
{
    const char *args[] = {"player"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    const int player = request.at("player").get<int>();
    int count = 0;
	for (DROID *psDroid = apsDroidLists[player], *psTemp = nullptr; psDroid; )
    {
        // destroying object invalidates its pointer to the next, so save it before commiting the crime
        psTemp = psDroid->psNext;
        destroyDroid(psDroid, gameTime + 10);
        ++count;
        psDroid = psTemp;

    }
    auto o = nlohmann::json::object();
    o["count"] = count;
    tcp_send_data(o);
    return;
}
// Add droid at position
// Position is in world coordinates. If you need map coordinates, use  x*128, y*128
void wzrpc::doAddDroidAt(const nlohmann::json &request)
{
    const char *args[] = {"player", "name", "body", "propulsion", "turrets", "x", "y"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    const int player = request.at("player").get<int>();
    auto name = request.at("name").get<std::string>();
    auto body = request.at("body").get<std::string>();
    auto propulsion = request.at("propulsion").get<std::string>();
    int x = request.at("x").get<int>();
    int y = request.at("y").get<int>();

    // hmmm do we really need all these conversions?..
    wzapi::string_or_string_list body_list;
    wzapi::string_or_string_list propulsion_list;
    wzapi::string_or_string_list turrets_list;
    body_list.strings.push_back(body);
    propulsion_list.strings.push_back(propulsion);
    for (const auto &it: request.at("turrets"))
    {
        turrets_list.strings.push_back(it.get<std::string>());
    }
    const wzapi::reservedParam p;
    wzapi::va_list<wzapi::string_or_string_list> va_turrets_list;
    va_turrets_list.va_list.push_back(turrets_list);
    const auto ptr = wzapi::addDroid(ctx, player, x, y, name,body_list, propulsion_list, p, p, va_turrets_list);
    auto o = nlohmann::json::object();
    if (ptr)
    {
        // ... how do I use this "ptr"??
        o["id"] = ptr.pt->id;
        const auto d = findById(player, ptr.pt->id);
        ctx.idToDroid->insert(std::pair<uint32_t, DROID*>(d->id, d));
    }
    tcp_send_data(o);
    return;
}

void wzrpc::doBuildDroidAnyFact(const nlohmann::json &request)
{
    const char *args[] = {"player", "name", "body", "propulsion", "turrets"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    const int player = request.at("player").get<int>();
    auto name = request.at("name").get<std::string>();
    auto body = request.at("body").get<std::string>();
    auto propulsion = request.at("propulsion").get<std::string>();
    STRUCTURE *aFactory = nullptr;
    const wzapi::reservedParam p;
	for (STRUCTURE *psStruct = apsStructLists[player]; psStruct; psStruct = psStruct->psNext)
	{
		if (!psStruct->died && STRUCTURE_TYPE::REF_FACTORY == psStruct->pStructureType->type)
		{
			aFactory = psStruct;
            // hmmm do we really need all these conversions?..
            wzapi::string_or_string_list body_list;
            wzapi::string_or_string_list propulsion_list;
            wzapi::string_or_string_list turrets_list;
            body_list.strings.push_back(body);
            propulsion_list.strings.push_back(propulsion);
            for (const auto &it: request.at("turrets"))
            {
                turrets_list.strings.push_back(it.get<std::string>());
            }

            wzapi::va_list<wzapi::string_or_string_list> va_turrets_list;
            va_turrets_list.va_list.push_back(turrets_list);
            bool ok = wzapi::buildDroid(ctx, aFactory, name, body_list, propulsion_list, p , p, va_turrets_list);
            if (ok)
            {
                auto o = nlohmann::json::object();
                o["status"] = ok;
                tcp_send_data(o);
                return;
            }
		}
	}
    sendError(RET_STATUS::RET_NO_APPROPRIATE_STRUCTURE);
    return;
}
static DROID *findById(int player, uint32_t id)
{
    const auto list = apsDroidLists[player];
    FOR_EACH_DROID(player)
    {
        if (psDroid->id == id && !psDroid->died)
        {
            return psDroid;
        }
    }
    return nullptr;
}
static STRUCTURE *findStructureById(int player, uint32_t id)
{
    const auto list = apsStructLists[player];
    FOR_EACH_STRUCTURE(player)
    {
        if (psStruct->id == id && !psStruct->died)
        {
            return psStruct;
        }
    }
    return nullptr;
}
void wzrpc::setDroidSecondary(const nlohmann::json &request)
{
    const char *args[] = {"player", "droidId", "secondaryOrder", "secondaryState"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    const auto player = request.at("player").get<int>();
    const auto secondaryOrder = request.at("secondaryOrder").get<int>();
    const auto secondaryState = request.at("secondaryState").get<int>();
    const auto droidId = request.at("droidId").get<int>();
    auto psDroid = findById(player, droidId);
    if (!psDroid)
    {
        SEND_ERROR_0(RET_STATUS::RET_OBJECT_NOT_FOUND);
    }
    
    bool res = secondarySetState(psDroid, (SECONDARY_ORDER) secondaryOrder, (SECONDARY_STATE) secondaryState);
    auto o = nlohmann::json::object();
    o["count"] = res;
    tcp_send_data(o);
    return;
}

void wzrpc::setDroidOrder(const nlohmann::json &request)
{
    const char *args[] = {"player", "droidId", "order", "targetId", "targetPlayer", "targetType"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    const auto player = request.at("player").get<int>();
    const auto targetPlayer = request.at("targetPlayer").get<int>();
    const auto droidId = request.at("droidId").get<int>();
    const auto targetId = request.at("targetId").get<int>();
    const auto order = request.at("order").get<int>();
    const auto targetType = request.at("targetType").get<std::string>();
    BASE_OBJECT *psTarget = nullptr;
    if (targetType.compare("droid") == 0)
    {
        psTarget = findById(targetPlayer, targetId);
    } else if (targetType.compare("structure") == 0)
    {
        psTarget = findStructureById(targetPlayer, targetId);
    }
    else {
        SEND_ERROR_1(RET_STATUS::RET_UKNOWN_OBJ_TYPE, "targetType");
    }
    DROID *psDroid = findById(player, droidId);
    if (!psDroid)
    {
        SEND_ERROR_1(RET_STATUS::RET_OBJECT_NOT_FOUND, "droidId");
    }
    if (!psTarget)
    {
        SEND_ERROR_1(RET_STATUS::RET_OBJECT_NOT_FOUND, "targetId");
    }
    const bool res = wzapi::orderDroidObj(ctx, psDroid, (DROID_ORDER) order, psTarget);
    const auto o = nlohmann::json::object({ {"status", res} });
    tcp_send_data(o);
    return;
}

void wzrpc::setAssemblyPoint(const nlohmann::json &request)
{
    const char *args[] = {"player", "structureId", "x", "y"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    const auto player = request.at("player").get<int>();
    const auto x = request.at("x").get<int>();
    const auto y = request.at("y").get<int>();
    const auto structureId = request.at("structureId").get<int>();
    STRUCTURE* psStruct = findStructureById(player, structureId);
    if (!psStruct)
    {
        SEND_ERROR_1(RET_STATUS::RET_OBJECT_NOT_FOUND, "structureId");
    }
    const bool res = wzapi::setAssemblyPoint(ctx, psStruct, x , y);
    const auto o = nlohmann::json::object({ {"status", res} });
    tcp_send_data(o);
    return;
}

void wzrpc::doDestroyAllStructures(const nlohmann::json &request)
{
    const char *args[] = {"player"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    const auto player = request.at("player").get<int>();
    int count = 0;
    for (STRUCTURE *psStruct = apsStructLists[player], *psTemp = nullptr; psStruct; )
    {
        // destroying object invalidates its pointer to the next, so save it before commiting the crime
        psTemp = psStruct->psNext;
        destroyStruct(psStruct, gameTime);
        ++count;
        psStruct = psTemp;
    }
    const auto o = nlohmann::json::object({ {"count", count} });
    tcp_send_data(o);
    return;
}
// Add structure to a position
// Position is in world coordinates. If you need map coordinates, use  x*128, y*128
// cannot use wzapi::addStructure, doesn't work
void wzrpc::doAddStructure(const nlohmann::json &request)
{
    const char *args[] = {"player", "name", "x", "y"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    const int player = request.at("player").get<int>();
    const int x = request.at("x").get<int>();
    const int y = request.at("y").get<int>();
    std::string name = request.at("name").get<std::string>();
    STRUCTURE_STATS *psStructStat = nullptr;
    for (size_t i = 0; i< numStructureStats; ++i)
    {
        if ((asStructureStats + i)->id.compare(WzString(name.c_str())) == 0)
        {
            psStructStat = asStructureStats + i;
            break;
        }
    }
    if (!psStructStat)
    {
        SEND_ERROR_0(RET_STATUS::RET_OBJECT_NOT_FOUND);
    }
    const auto psStruct = buildStructure(psStructStat, x<<7, y<<7, player, false);
    auto o = nlohmann::json::object();
    if (psStruct)
{
        psStruct->status = SS_BUILT;
		buildingComplete(psStruct);
        o["id"] = psStruct->id;
    }
    tcp_send_data(o);
    return;
}

void wzrpc::setFiringStatus(const nlohmann::json &request)
{
    const char *args[] = {"structureId", "status"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    const uint32_t id = request.at("structureId").get<uint32_t>();
    const bool status =  request.at("status").get<bool>();
    if (status)
    {
        bannedFromFiring.insert(id);
    }
    else 
    {
        bannedFromFiring.erase(id);
    }
    const nlohmann::json::boolean_t can = true;
    tcp_send_data(can);
    return;
}

// doesn't seem working..
void wzrpc::doAddTemplate(const nlohmann::json &request)
{
    const char *args[] = {"player", "name", "body", "propulsion", "turrets"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    const int player = request.at("player").get<int>();
    const auto name = request.at("name").get<std::string>();
    const auto body = request.at("body").get<std::string>();
    const auto propulsion = request.at("propulsion").get<std::string>();
    wzapi::string_or_string_list body_list;
    wzapi::string_or_string_list propulsion_list;
    wzapi::string_or_string_list turrets_list;
    // hmmm do we really need all these conversions?..
    body_list.strings.push_back(body);
    propulsion_list.strings.push_back(propulsion);
    for (const auto &it: request.at("turrets"))
    {
        turrets_list.strings.push_back(it.get<std::string>());
    }
    wzapi::reservedParam p;
    wzapi::va_list<wzapi::string_or_string_list> va_turrets_list;
    va_turrets_list.va_list.push_back(turrets_list);
    const auto pTemplate = wzapi::makeTemplate(ctx, player,name, body_list, propulsion_list, p, va_turrets_list);
    auto o = nlohmann::json::object();
    if (pTemplate)
    {
       o["id"] = pTemplate->id;
       o["name"] = pTemplate->name;
    }
    tcp_send_data(o);
    return;
}
void wzrpc::getAllTemplates(const nlohmann::json &request)
{
    const char *args[] = {"player"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    const int player = request.at("player").get<int>();
    auto array = nlohmann::json::array();
    enumerateTemplates(player, [&array] (const DROID_TEMPLATE* psTemplate) {

        auto o = nlohmann::json::object();
        o["id"] = psTemplate->id;
        o["name"] = psTemplate->name;
        o["droidType"] = psTemplate->droidType;
        o["enabled"] = psTemplate->enabled;
        o["index"] = psTemplate->index;
        o["compBody"] = psTemplate->asParts[COMP_BODY];
        o["compPropulsion"] = psTemplate->asParts[COMP_PROPULSION];
        array.insert(array.end(), o);
        return true;
    });
    ASSERT_OR_RETURN(, array.size() < 512, "template array was huge wtf?");
    tcp_send_data(array);
    return;
}

// doesnt look working
void wzrpc::doToggleDebug(const nlohmann::json &request)
{
    kf_ToggleDebugMappings();
    const nlohmann::json::boolean_t ok = true;
    tcp_send_data(ok);
    return;
}

void wzrpc::getGameSpeedMod(const nlohmann::json &request)
{
    auto o = nlohmann::json::object();
    o["gameTimeMod"] = gameTimeGetMod().asDouble();
    tcp_send_data(o);
    return;
}

// center view in tile corrdinates
void wzrpc::doCenterView(const nlohmann::json &request)
{
    const char *args[] = {"x", "y"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    const int x = request.at("x").get<int>();
    const int y = request.at("y").get<int>();
    wzapi::centreView(ctx, x, y);
    const nlohmann::json::boolean_t ok = true;
    tcp_send_data(ok);
    return;
}
// This call is pretty wierd, it takes some research id
// but if the research is unavailable, it starts whatever
// pre-requisites there are. 
// It doesn't pursue everything tho, it just stops after 1 requirement (!)
// Then you have to resubmit the same research id to continue..
void wzrpc::doPursueResearch(const nlohmann::json &request)
{
    const char *args[] = {"player", "researchId"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    const int player = request.at("player").get<int>();
    wzapi::string_or_string_list research_list;
    research_list.strings.push_back(request.at("researchId").get<std::string>());
    bool ok = false;
    for (STRUCTURE *psStruct = apsStructLists[player]; psStruct; psStruct = psStruct->psNext)
	{
        if (!psStruct->died && psStruct->pStructureType->type == STRUCTURE_TYPE::REF_RESEARCH)
        {
            ok = wzapi::pursueResearch(ctx, psStruct, research_list);
            if (ok)
            {
                break;
            }
        }
    }
    auto o = nlohmann::json::object();
    o["status"] = ok;
    tcp_send_data(o);
    return;
}
// list available research for given player
void wzrpc::getAvailableResearch(const nlohmann::json &request)
{
    const char *args[] = {"player"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    const int player = request.at("player").get<int>();
    // TODO maybe pass player as argument instead of context?
    ctx._player = player;
    auto r = wzapi::enumResearch(ctx);
    auto array = nlohmann::json::array();
    for (auto research: r.resList)
    {
        array.insert(array.end(),research->id);
    };
    tcp_send_data(array);
    return;
}
void wzrpc::doCompleteResearch(const nlohmann::json &request)
{
    const char *args[] = {"player", "researchId"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    const int player = request.at("player").get<int>();
    auto researchId = request.at("researchId").get<std::string>();
    wzapi::completeResearch(ctx,researchId, player, false);
    auto o = nlohmann::json::object();
    o["status"] = true;
    tcp_send_data(o);
    return;
}

void wzrpc::getAllFeatures(const nlohmann::json &request)
{
    const char *args[] = {"visibleForPlayer"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    int visibleForPlayer = request.at("visibleForPlayer").get<int>();
    auto features = wzapi::enumFeature(ctx,visibleForPlayer, nullopt);
    auto array = nlohmann::json::array();
    for (auto feature: features)
    {
        auto o = nlohmann::json::object();
        o["id"] = feature->id;
        o["x"] = feature->pos.x;
        o["y"] = feature->pos.y;
        o["z"] = feature->pos.z;
        array.insert(array.end(), o);
    }
    tcp_send_data(array);
    return;
}

void wzrpc::doRemoveFeatures(const nlohmann::json &request)
{
    
	for (FEATURE *psFeat = apsFeatureLists[0], *psTemp = nullptr; psFeat; )
	{
        psTemp = psFeat->psNext;
		if (!psFeat->died)
		{
            destroyFeature(psFeat, gameTime + 10);
		}
        psFeat = psTemp;
	}

    const nlohmann::json::boolean_t ok = true;
    tcp_send_data(ok);
    return;
}

void wzrpc::doRemoveStructure(const nlohmann::json &request)
{
    const char *args[] = {"player", "structureId"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    const int player = request.at("player").get<int>();
    auto structureId = request.at("structureId").get<int>();
    auto psStruct = findStructureById(player, structureId);
    const nlohmann::json::boolean_t ok = destroyStruct(psStruct, gameTime);
    tcp_send_data(ok);
    return;
}

void wzrpc::getPropulsionCanReach(const nlohmann::json &request)
{
    const char *args[] = {"propulsion","x1","y1","x2","y2"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    auto propulsion = request.at("propulsion").get<std::string>();
    int x1 = request.at("x1").get<int>();
    int y1 = request.at("y1").get<int>();
    int x2 = request.at("x2").get<int>();
    int y2 = request.at("y2").get<int>();
    const nlohmann::json::boolean_t can = wzapi::propulsionCanReach(ctx,propulsion,x1, y1, x2, y2);
    tcp_send_data(can);
    return;
}

//-- Returns tile type of all map tiles
/*void wzrpc::getTerrainType(const nlohmann::json &request)
{
    // somehow obtain the size of the map, and iterate over tiles
    // mapData->height
    auto mapData = wzMap.mapData();
    for (uint32_t y = 0; y < mapData->height; ++y)
    {
        
    }
}*/

void wzrpc::doRevealAll(const nlohmann::json &request)
{

    if (godMode)
    {
        const nlohmann::json::boolean_t can = true;
        tcp_send_data(can);
    } else
    {
        const nlohmann::json::boolean_t can = wzapi::toggleGodMode();
        tcp_send_data(can);
    }
    return;
}

void wzrpc::doSpeedUp(const nlohmann::json &request)
{
    kf_SpeedUp();
    const nlohmann::json::boolean_t ok = true;
    tcp_send_data(ok);
    return;
}


void wzrpc::getDroidDetails(const nlohmann::json &request)
{
    const char *args[] = {"player", "droidId"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    const auto player = request.at("player").get<int>();
    const auto droidId = request.at("droidId").get<int>();
    const auto psDroid = findById(player, droidId);
    if (!psDroid)
    {
        SEND_ERROR_0(RET_STATUS::RET_OBJECT_NOT_FOUND);
    }
    auto o = nlohmann::json::object();
    o["kills"] = psDroid->kills;
    o["orderType"] = psDroid->order.type;
    o["orderPosition"] = psDroid->order.pos2;
    o["secondaryOrder"] = psDroid->secondaryOrder;
    o["action"] = psDroid->action;
    o["hp"] = psDroid->body;
    o["originalHp"] = psDroid->originalBody;
    o["droidPosition"] = psDroid->pos;
    o["moveStatus"] = psDroid->sMove.Status;
    o["weapon0Shots"] = psDroid->asWeaps[0].shotsFired;
    tcp_send_data(o);
    return;
} 

void wzrpc::getStructureDetails(const nlohmann::json &request)
{
    const char *args[] = {"player", "structureId"};
    if (!validate_arguments(request, args, LEN_ARGS(args)))
    {
        return;
    }
    const auto player = request.at("player").get<int>();
    const auto structureId = request.at("structureId").get<int>();
    const auto psStruct = findStructureById(player, structureId);
    if (!psStruct)
    {
        SEND_ERROR_0(RET_STATUS::RET_OBJECT_NOT_FOUND);
    }
    auto o = nlohmann::json::object();
    o["originalHp"] = structureBody(psStruct);
    o["hp"] = psStruct->body;
    o["periodicalDamage"] = psStruct->periodicalDamage;
    tcp_send_data(o);
    return;
}

/*
int32_t wzrpc::playerPower(int player)
{
    return wzapi::playerPower(ctx, player);
}

std::vector<wzrpc::MapOfStr> wzrpc::getStatsArray() noexcept
{
    
    std::vector<wzrpc::MapOfStr> out;
    for(int i = 0; i < numBodyStats; ++i)
    {
        std::unordered_map<std::string, std::string> temp;
        temp.insert(std::pair<std::string, std::string> ("id", asBodyStats[i].id.toStdString()));
        temp.insert(std::pair<std::string, std::string> ("name", asBodyStats[i].name.toStdString()));
        out.push_back(temp);
    }
    return out;
}


uint32_t wzrpc::getGameTime()
{
    return gameTime;
}
*/


// ----------------------- Notifications
void wzrpc::notifyDroidShotFired(PROJECTILE* psProj)
{
    // size 1 + 4 + 1 + 4 + 1 + 4 + (38) = 53 bytes
    nlohmann::json o = nlohmann::json::object();
    o["et"] = EVENT_TYPE::ET_DROID_FIRED;
    o["id"] = psProj->psSource->id;
    o["player"] = psProj->player;
    o["targetId"] = psProj->psDest? psProj->psDest->id : -1;
    o["targetPlayer"] = psProj->psDest? psProj->psDest->player : -1;
    o["gameTime"] = psProj->born;
    udp_send_data(o);
}

void wzrpc::notifyResearched(int player, const RESEARCH *psResearch, const STRUCTURE *psStruct)
{
    //debug(LOG_INFO, "building msg %s %i...", psResearch->id.toStdString().c_str(), player);
    nlohmann::json o = nlohmann::json::object();
    o["et"] = EVENT_TYPE::ET_RESEARCHED;
    o["id"] = psResearch->id.toStdString();
    o["player"] = player;
    o["structure"]  = psStruct ? psStruct->id : -1;
    udp_send_data(o);
}

void wzrpc::notifyDroidBuilt(const STRUCTURE *psStruct, const DROID* psDroid)
{
    nlohmann::json o = nlohmann::json::object();
    o["et"] = EVENT_TYPE::ET_DROID_BUILT;
    o["droidId"] = psDroid->id;
    o["droidName"] = psDroid->aName;
    o["structId"] = psStruct->id; 
    udp_send_data(o);
}

void wzrpc::notifyGameLoaded(uint32_t gameTime)
{
    debug(LOG_INFO, "game loaded");
    nlohmann::json o = nlohmann::json::object();
    o["et"] = EVENT_TYPE::ET_GAME_LOADED;
    o["gameTime"] = gameTime;
    udp_send_data(o);
}

void wzrpc::notifyDroidDestroyed(const DROID* psDroid, uint32_t gameTime)
{
    nlohmann::json o = nlohmann::json::object();
    o["et"] = EVENT_TYPE::ET_DROID_DESTROYED;
    o["id"] = psDroid->id;
    o["player"] = psDroid->player;
    o["loc"] = psDroid->pos;
    o["kills"] = psDroid->kills;
    o["gameTime"] = gameTime;
    udp_send_data(o);
    // maybe race condition, this is called from main thread
    ctx.idToDroid->erase(psDroid->id);
}
void wzrpc::notifyGamePauseStatus(bool status)
{
    nlohmann::json o = nlohmann::json::object();
    o["et"] = EVENT_TYPE::ET_GAME_PAUSE_STATUS;
    o["status"] = status;
    udp_send_data(o);
} 
// loop.cpp: gameLoop
void wzrpc::notifyGameTime(uint32_t gameTime)
{
    nlohmann::json o = nlohmann::json::object();
    o["et"] = EVENT_TYPE::ET_GAME_TIME;
    o["gameTime"] = gameTime;
    udp_send_data(o);
}

void wzrpc::notifyDamaged(uint32_t id, int damage, bool isDamagePerSecond, WEAPON_CLASS weapClass, WEAPON_SUBCLASS weapSubclass, uint32_t gameTime)
{
    nlohmann::json o = nlohmann::json::object();
    o["et"] = EVENT_TYPE::ET_DAMAGED;
    o["id"] = id;
    o["damage"] = damage;
    o["isDPS"] = isDamagePerSecond;
    o["weapClass"] = weapClass;
    o["weapSubclass"] = weapSubclass;
    o["gameTime"] = gameTime;
    udp_send_data(o);
}

void wzrpc::notifyStartLevel(uint32_t gameTime)
{
    nlohmann::json o = nlohmann::json::object();
    o["et"] = EVENT_TYPE::ET_START_LEVEL;
    o["gameTime"] = gameTime;
    udp_send_data(o);
}
#undef LEN_ARGS