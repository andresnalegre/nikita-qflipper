#include "formatstorageutiloperation.h"

#include "flipperzero/devicestate.h"
#include "flipperzero/protobufsession.h"
#include "flipperzero/rpc/storagelistoperation.h"
#include "flipperzero/rpc/storageremoveoperation.h"

using namespace Flipper;
using namespace Zero;

FormatStorageUtilOperation::FormatStorageUtilOperation(ProtobufSession *rpc, DeviceState *deviceState, QObject *parent):
    AbstractUtilityOperation(rpc, deviceState, parent)
{}

const QString FormatStorageUtilOperation::description() const
{
    return QStringLiteral("Format external storage @%1").arg(deviceState()->name());
}

void FormatStorageUtilOperation::nextStateLogic()
{
    if(operationState() == AbstractOperation::Ready) {
        setOperationState(FormatStorageUtilOperation::Listing);
        listStorage();

    } else if(operationState() == FormatStorageUtilOperation::Listing) {
        setOperationState(FormatStorageUtilOperation::Removing);
        removeNext();

    } else if(operationState() == FormatStorageUtilOperation::Removing) {
        finish();

    } else {}
}

void FormatStorageUtilOperation::listStorage()
{
    deviceState()->setStatusString(QStringLiteral("Formatting external storage..."));

    auto *operation = rpc()->storageList(QByteArrayLiteral("/ext"));

    connect(operation, &AbstractOperation::finished, this, [=]() {
        if(operation->isError()) {
            finishWithError(operation->error(), operation->errorString());
            return;
        }

        m_queue.clear();
        for(const auto &file : operation->files()) {
            m_queue.append(QByteArrayLiteral("/ext/") + file.name);
        }

        advanceOperationState();
    });
}

void FormatStorageUtilOperation::removeNext()
{
    if(m_queue.isEmpty()) {
        advanceOperationState();
        return;
    }

    const auto path = m_queue.takeFirst();
    auto *operation = rpc()->storageRemove(path, true);

    connect(operation, &AbstractOperation::finished, this, [=]() {
        if(operation->isError()) {
            finishWithError(operation->error(), operation->errorString());
            return;
        }

        removeNext();
    });
}
