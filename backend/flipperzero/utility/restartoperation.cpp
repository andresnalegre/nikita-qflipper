#include "restartoperation.h"

#include "flipperzero/devicestate.h"
#include "flipperzero/protobufsession.h"
#include "flipperzero/rpc/systemrebootoperation.h"

using namespace Flipper;
using namespace Zero;

RestartOperation::RestartOperation(ProtobufSession *rpc, DeviceState *deviceState, QObject *parent):
    AbstractUtilityOperation(rpc, deviceState, parent)
{
    // The reconnect that follows a reboot goes through VCPDeviceInfoHelper's
    // own retry loop (up to 20 attempts, ~600ms apart, each preceded by a
    // serial-open attempt that can itself take ~1s when the OS is slow to
    // release the port) -- observed on macOS to run past 30s in practice, well
    // above what the 20 x 600ms estimate in that code suggests. The base class's
    // default 30000ms timeout races that inner retry budget and can fire first,
    // failing a restore/firmware-install/repair with "timeout exceeded" seconds
    // before the device would have reconnected on its own. Give this outer wait
    // enough margin that the inner retry loop's own result -- success or its own
    // "gave up after N attempts" error -- is what actually decides the outcome.
    setTimeout(60000);
}

const QString RestartOperation::description() const
{
    return QStringLiteral("Restart device @%1").arg(deviceState()->name());
}

void RestartOperation::nextStateLogic()
{
    if(operationState() == AbstractOperation::Ready) {
        setOperationState(RestartOperation::WaitingForOSBoot);
        rebootDevice();

    } else if(operationState() == RestartOperation::WaitingForOSBoot) {
        disconnect(deviceState(), &DeviceState::isOnlineChanged, this, &RestartOperation::onDeviceOnlineChanged);
        finish();

    } else {}
}

void RestartOperation::onOperationTimeout()
{
    finishWithError(BackendError::UnknownError, QStringLiteral("Failed to restart: timeout exceeded"));
}

void RestartOperation::onDeviceOnlineChanged()
{
    if(deviceState()->isOnline()) {
        advanceOperationState();
    } else {
        startTimeout();
    }
}

void RestartOperation::rebootDevice()
{
    deviceState()->setProgress(-1);
    deviceState()->setStatusString(QStringLiteral("Restarting device..."));

    connect(deviceState(), &DeviceState::isOnlineChanged, this, &RestartOperation::onDeviceOnlineChanged);

    auto *operation = rpc()->rebootToOS();

    connect(operation, &AbstractOperation::finished, this, [=]() {
        if(operation->isError()) {
            finishWithError(operation->error(), operation->errorString());
        }
    });

    startTimeout();
}
