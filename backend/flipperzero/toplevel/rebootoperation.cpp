#include "rebootoperation.h"

#include "flipperzero/devicestate.h"
#include "flipperzero/utilityinterface.h"
#include "flipperzero/utility/restartoperation.h"

using namespace Flipper;
using namespace Zero;

RebootOperation::RebootOperation(UtilityInterface *utility, DeviceState *state, QObject *parent):
    AbstractTopLevelOperation(state, parent),
    m_utility(utility)
{}

const QString RebootOperation::description() const
{
    return QStringLiteral("Reboot (Toplevel) @%1").arg(deviceState()->name());
}

void RebootOperation::nextStateLogic()
{
    if(operationState() == AbstractOperation::Ready) {
        setOperationState(RebootOperation::Restarting);
        restartDevice();

    } else if(operationState() == RebootOperation::Restarting) {
        finish();
    }
}

void RebootOperation::restartDevice()
{
    registerSubOperation(m_utility->restartDevice());
}
