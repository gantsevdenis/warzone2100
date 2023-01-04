#ifndef __BEHAVIOR_CONSTRUCTOR_H__
#define __BEHAVIOR_CONSTRUCTOR_H__

#include "defs.h"
/*** Behavior for Builder truck or builder cyborg */
namespace Constructor {

// I would like this to be visible
// but not the declinations over different Intentions
void gen_events(const DROID &psDroid);

/***
 * Updates DROID's activity, updates DROID's intentions
*/
void tick (DROID& psDroid);

}

#endif
