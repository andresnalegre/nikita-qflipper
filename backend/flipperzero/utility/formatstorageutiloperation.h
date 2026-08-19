#pragma once

#include <QByteArray>
#include <QList>

#include "abstractutilityoperation.h"

namespace Flipper {
namespace Zero {

// Deletes every entry under /ext -- the same "format" the file manager
// offers from the GUI, ported here so the headless CLI can drive it too.
// There is no native "format" RPC call, so this lists /ext and removes
// each top-level entry recursively, one at a time.
class FormatStorageUtilOperation : public AbstractUtilityOperation
{
    Q_OBJECT

    enum OperationState {
        Listing = AbstractOperation::User,
        Removing
    };

public:
    FormatStorageUtilOperation(ProtobufSession *rpc, DeviceState *deviceState, QObject *parent = nullptr);
    const QString description() const override;

private slots:
    void nextStateLogic() override;

private:
    void listStorage();
    void removeNext();

    QList<QByteArray> m_queue;
};

}
}
