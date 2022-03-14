#include "fsm.h"
#include "../action.h"
#include "../droid.h"
#include "../order.h"
#include "../move.h"
#include "../ai.h"

// see if we can attack something & set targets for each weapon
bool anythingToShoot(DROID& droid)
{
    DROID *psDroid = &droid;
    bool attack = false;
    for (unsigned i = 0; i < psDroid->numWeaps; ++i)
    {
        if (psDroid->asWeaps[i].nStat > 0)
        {
            BASE_OBJECT *psTemp = nullptr;

            WEAPON_STATS *const psWeapStats = &asWeaponStats[psDroid->asWeaps[i].nStat];
            if (psWeapStats->rotate && aiBestNearestTarget(psDroid, &psTemp, i) >= 0)
            {
                setDroidActionTarget(psDroid, psTemp, i);
                attack = true;
            }
        }
    }
    return attack;
}
void alignWeapons(DROID& droid)
{
    DROID *psDroid = &droid;
    for (unsigned i = 0; i < psDroid->numWeaps; ++i)
    {
        if (psDroid->asWeaps[i].rot.direction != 0 || psDroid->asWeaps[i].rot.pitch != 0)
        {
            actionAlignTurret(psDroid, i);
        }
    }
}
void setTargetsToNull(DROID& droid)
{
    DROID *psDroid = &droid;
    for (int i = 0; i < psDroid->numWeaps; i++)
    {
        setDroidActionTarget(psDroid, nullptr, i);
    }
}
void clearMovement(DROID& droid)
{
    DROID *psDroid = &droid;
    // Clear up what ever the droid was doing before if necessary
    if (!DROID_STOPPED(psDroid))
    {
        moveStopDroid(psDroid);
    }
    // psDroid->action = DACTION_NONE; // TODO remove that
    psDroid->actionPos = Vector2i(0, 0);
    psDroid->actionStarted = 0;
    psDroid->actionPoints = 0;
}