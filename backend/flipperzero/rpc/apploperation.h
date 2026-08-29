#pragma once

#include "abstractprotobufoperation.h"

namespace Flipper {
namespace Zero {

// The RPC form of `loader open <App>`. Launches a built-in or installed app by
// its exact name ("NFC", "Infrared", "Sub-GHz", ...). This is what lets a
// Bluetooth session open an app deterministically, with no CLI and no D-pad
// counting. The device replies status-only, so the default empty-response
// handling is enough.
class AppStartOperation : public AbstractProtobufOperation
{
    Q_OBJECT
public:
    AppStartOperation(uint32_t id, const QByteArray &name, const QByteArray &args, QObject *parent = nullptr);
    const QString description() const override;
    const QByteArray encodeRequest(ProtobufPluginInterface *encoder) override;
private:
    QByteArray m_name;
    QByteArray m_args;
};

// The RPC form of `loader close` -- closes whatever app is running.
class AppExitOperation : public AbstractProtobufOperation
{
    Q_OBJECT
public:
    AppExitOperation(uint32_t id, QObject *parent = nullptr);
    const QString description() const override;
    const QByteArray encodeRequest(ProtobufPluginInterface *encoder) override;
};

}
}
