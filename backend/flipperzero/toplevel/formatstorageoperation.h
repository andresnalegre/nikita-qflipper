#pragma once

#include "abstracttopleveloperation.h"

namespace Flipper {
namespace Zero {

class UtilityInterface;

class FormatStorageOperation : public AbstractTopLevelOperation
{
    Q_OBJECT

    enum OperationState {
        Formatting = AbstractOperation::User
    };

public:
    FormatStorageOperation(UtilityInterface *utility, DeviceState *state, QObject *parent = nullptr);
    const QString description() const override;

private slots:
   void nextStateLogic() override;

private:
   void formatStorage();

   UtilityInterface *m_utility;
};

}
}
