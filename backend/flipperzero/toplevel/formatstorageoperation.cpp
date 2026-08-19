#include "formatstorageoperation.h"

#include "flipperzero/devicestate.h"
#include "flipperzero/utilityinterface.h"
#include "flipperzero/utility/formatstorageutiloperation.h"

using namespace Flipper;
using namespace Zero;

FormatStorageOperation::FormatStorageOperation(UtilityInterface *utility, DeviceState *state, QObject *parent):
    AbstractTopLevelOperation(state, parent),
    m_utility(utility)
{}

const QString FormatStorageOperation::description() const
{
    return QStringLiteral("Format external storage (Toplevel) @%1").arg(deviceState()->name());
}

void FormatStorageOperation::nextStateLogic()
{
    if(operationState() == AbstractOperation::Ready) {
        setOperationState(FormatStorageOperation::Formatting);
        formatStorage();

    } else if(operationState() == FormatStorageOperation::Formatting) {
        finish();
    }
}

void FormatStorageOperation::formatStorage()
{
    registerSubOperation(m_utility->formatExternalStorage());
}
