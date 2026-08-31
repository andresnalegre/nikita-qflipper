#pragma once

#include <QObject>
#include <QByteArray>

#include "abstractoperationhelper.h"
#include "flipperzero/deviceinfo.h"

class QTimer;
class QSerialPort;

namespace Flipper {
namespace Zero {

class ProtobufSession;

class AbstractDeviceInfoHelper : public AbstractOperationHelper
{
    Q_OBJECT

public:
    AbstractDeviceInfoHelper(QObject *parent = nullptr);
    virtual ~AbstractDeviceInfoHelper();

    static AbstractDeviceInfoHelper *create(const USBDeviceInfo &info, QObject *parent = nullptr);
    const DeviceInfo &result() const;

protected:
    DeviceInfo m_deviceInfo;
};

class VCPDeviceInfoHelper : public AbstractDeviceInfoHelper
{
    Q_OBJECT

    enum OperationState {
        FindingSerialPort = AbstractOperationHelper::User,
        StartingRPCSession,
        FetchingProtobufVersion,
        FetchingDeviceInfo,
        CheckingSDCard,
        CheckingManifest,
        GettingTimeSkew,
        SyncingTime,
        StoppingRPCSession
    };

public:
    VCPDeviceInfoHelper(const USBDeviceInfo &info, QObject *parent = nullptr);

    // Bootstrap over BLE instead of a USB serial port. The RPC session opens on
    // a factory-built transport and port finding is skipped. Call right after
    // construction, before the event loop runs.
    void setBleTransport(const QString &name, const TransportFactory &factory);

private:
    void nextStateLogic() override;

    void findSerialPort();
    void startRPCSession();
    void fetchProtobufVersion();
    void fetchDeviceInfo();
    void fetchDeviceInfoLegacy();
    void fetchDeviceInfoProperty();
    void checkSDCard();
    void checkManifest();
    void getTimeSkew();
    void syncTime();
    void stopRPCSession();

private slots:
    void onSessionStatusChanged();

private:
    static const QString &branchToChannelName(const QByteArray &branchName, const QByteArray &version);

    // Null between a failed session and the retry that replaces it, so every
    // use has to check first.
    ProtobufSession *m_rpc = nullptr;

    // The first boot after a firmware update is slow for real reasons, so
    // devinfo gets a longer deadline and a few retries.
    int m_devInfoAttempts = 0;

    // Relaunching the app races the previous instance for the serial port.
    int m_rpcAttempts = 0;
};

class DFUDeviceInfoHelper : public AbstractDeviceInfoHelper
{
    Q_OBJECT

public:
    DFUDeviceInfoHelper(const USBDeviceInfo &info, QObject *parent = nullptr);

private:
    void nextStateLogic() override;
};

}
}