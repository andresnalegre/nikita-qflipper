#pragma once

#include "abstracttopleveloperation.h"

namespace Flipper {
namespace Zero {

class UtilityInterface;

class RebootOperation : public AbstractTopLevelOperation
{
    Q_OBJECT

    enum OperationState {
        Restarting = AbstractOperation::User
    };

public:
    RebootOperation(UtilityInterface *utility, DeviceState *state, QObject *parent = nullptr);
    const QString description() const override;

private slots:
   void nextStateLogic() override;

private:
   void restartDevice();

   UtilityInterface *m_utility;
};

}
}
