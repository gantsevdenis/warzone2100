#include "../basedef.h"

class TargetAcquisitionBase
{

}

class AnyEnnemyInVicinity: public TargetAcquisitionBase
{
    bool update(DROID& droid)
    {
        return anythingToShoot(droid);
    }
}

class DesignatedTarget: public TargetAcquisitionBase
{

}

class BuilderAcq: public TargetAcquisitionBase
{

}

class RepairTurretAcq: public TargetAcquisitionBase
{
    void update(DROID& droid)
    {

    }
}
