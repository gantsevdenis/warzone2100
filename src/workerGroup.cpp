#include "workerGroup.h"
#include "geometry.h"
#include "mapgrid.h"
#include "order.h"
#include "objmem.h"
#include "move.h"
/*
Note: si je remplace tous les LINEBUILD par BUILD, la ligne de construction sera détruite
donc au moins 1 (peut etre laisser 2?) doit continuer à constuire la premiere tile,
peu importe si c'est le premier arrivé, ou le dernier, tant que LINEBUILD n'est pas
annulé, les blueprints ne disparaissent pas


DroidTarget(psDroid, psStruct);
getTileStructure(map_coord(psDroid->actionPos.x), map_coord(psDroid->actionPos.y));
*/

/***/
void updateWorkerGroup(std::vector<DROID *> builders)
{

    static std::vector<DROID *> activeBuilders;
    static std::vector<DROID *> areBuilding;
    activeBuilders.clear();
    areBuilding.clear();

    for (DROID* psDroid: builders)
    {
        if (!psDroid->died)
        {
            activeBuilders.push_back(psDroid);
            if (psDroid->action == DACTION_BUILD)
            {
                areBuilding.push_back(psDroid);
            }
        }
    }
    if (activeBuilders.empty()) return;
    DROID* first = activeBuilders[0];
    DROID* last = activeBuilders[activeBuilders.size()-1];
    if (first->order.psObj)
    {
        // each line to build is 1 order
        // 2 separate lines to build are  2 orders, etc...
        // step is "+/- 128" for wall blocks / oil wells / defensive points etc..
        const auto lb = calcLineBuild(first->order.psStats, first->order.direction, first->order.pos, first->order.pos2);
        const auto cp = constructorPoints(asConstructStats + first->asBits[COMP_CONSTRUCT], first->player);
        const STRUCTURE* psStruct = (STRUCTURE*) first->order.psObj;
        debug(LOG_INFO, "target %d, OL %li (%i), BL %i, CBP %i (%i/sec)", 
            first->order.psObj->id, 
            first->asOrderList.size(), 
            first->listSize , lb.count, 
            psStruct->currentBuildPts, cp);
        debug(LOG_INFO, "last has same target? %i, lb step %i/%i",
        last->order.psObj == first->order.psObj, lb.step.x, lb.step.y);
        if (last->order.psObj == first->order.psObj)
        {
            const auto nextx = first->actionPos.x + lb.step.x;
            const auto nexty = first->actionPos.y + lb.step.y;
            // getTileStructure doesn't work for next blueprint
            // must give new order with psStats to a droid
            const auto nextStruct = getTileStructure(map_coord(nextx), map_coord(nexty));
            first->order.psStats
            if (nextStruct)
            {
                debug(LOG_INFO, "setting last droid %i to next struct", last->id);
                setDroidTarget(last, nextStruct);
            }
            else
            {
                debug(LOG_INFO, "didn't find next structure %i:%i", nextx, nexty);
            }
        }
    }

    // on veut répartir le travail de maniere intélligente:
    // par ex. 2 trucks par structure

}   