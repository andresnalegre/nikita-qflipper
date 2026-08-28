#include "deviceregistry.h"

#include <QDebug>
#include <QMetaObject>
#include <QLoggingCategory>

#include <memory>

#include "flipperzero/helper/deviceinfohelper.h"
#include "flipperzero/flipperzero.h"
#include "flipperzero/devicestate.h"
#include "flipperzero/protobufsession.h"

#include "usbdevice.h"

#define FLIPPER_ZERO_VID 0x0483
#define FLIPPER_ZERO_PID_VCP 0x5740
#define FLIPPER_ZERO_PID_DFU 0xdf11

Q_LOGGING_CATEGORY(LOG_DEVREG, "REG");

using namespace Flipper;

DeviceRegistry::DeviceRegistry(QObject *parent):
    QObject(parent),
    m_detector(new USBDeviceDetector(this)),
    m_error(BackendError::UnknownError),
    m_isQueryInProgress(false)
{
    connect(m_detector, &USBDeviceDetector::devicePluggedIn, this, &DeviceRegistry::insertDevice);
    connect(m_detector, &USBDeviceDetector::deviceUnplugged, this, &DeviceRegistry::removeDevice);

    m_detector->setWantedDevices({
        USBDeviceInfo(FLIPPER_ZERO_VID, FLIPPER_ZERO_PID_DFU),
        USBDeviceInfo(FLIPPER_ZERO_VID, FLIPPER_ZERO_PID_VCP)
            .withManufacturer("Flipper Devices Inc.")
            .withProductDescription("Flipper Control Virtual ComPort")
                                 });
}

void DeviceRegistry::setBackendLogLevel(int logLevel)
{
    m_detector->setLogLevel(logLevel);
}

FlipperZero *DeviceRegistry::currentDevice() const
{
    return m_devices.isEmpty() ? nullptr : m_devices.first();
}

int DeviceRegistry::deviceCount() const
{
    return m_devices.size();
}

bool DeviceRegistry::hasBleDevice() const
{
    return std::any_of(m_devices.begin(), m_devices.end(), [](Flipper::FlipperZero *dev) {
        return dev->deviceState()->deviceInfo().isBle;
    });
}

BackendError::ErrorType DeviceRegistry::error() const
{
    return m_error;
}

void DeviceRegistry::clearError()
{
    setError(BackendError::UnknownError);
}

bool DeviceRegistry::isQueryInProgress() const
{
    return m_isQueryInProgress;
}

void DeviceRegistry::insertDevice(const USBDeviceInfo &info)
{
    if(!info.isComplete()) {
        qCDebug(LOG_DEVREG).noquote().nospace()
            << "Incomplete device info: VID_0x" << QString::number(info.vendorID(), 16) << ":PID_0x"
            << QString::number(info.productID(), 16);

        setError(BackendError::InvalidDevice);

    } else if(info.vendorID() != FLIPPER_ZERO_VID) {
        qCDebug(LOG_DEVREG) << "Unexpected device VID and PID";
        setError(BackendError::InvalidDevice);

    } else {
        setQueryInProgress(true);
        qCDebug(LOG_DEVREG).noquote().nospace()
            << "Detected new device: VID_0x" << QString::number(info.vendorID(), 16) << ":PID_0x" << QString::number(info.productID(), 16);

        auto *fetcher = Zero::AbstractDeviceInfoHelper::create(info, this);
        connect(fetcher, &Zero::AbstractDeviceInfoHelper::finished, this, &DeviceRegistry::processDevice);
        connect(fetcher, &Zero::AbstractDeviceInfoHelper::finished, fetcher, &QObject::deleteLater);
    }
}

void DeviceRegistry::removeDevice(const USBDeviceInfo &info)
{
    const auto it = std::find_if(m_devices.begin(), m_devices.end(), [&](Flipper::FlipperZero *dev) {
        const auto &deviceInfo = dev->deviceState()->deviceInfo().usbInfo;
        return deviceInfo.backendData() == info.backendData();
    });

    if(it != m_devices.end()) {
        const auto idx = std::distance(m_devices.begin(), it);
        auto *device = m_devices.at(idx);

        if(!device->deviceState()->isPersistent()) {
            qCDebug(LOG_DEVREG).noquote().nospace()
                << "Device disconnected: VID_0x" << QString::number(info.vendorID(), 16) << ":PID_0x" << QString::number(info.productID(), 16);

            m_devices.takeAt(idx)->deleteLater();
            emit deviceCountChanged();
            emit currentDeviceChanged();

        } else {
            qCDebug(LOG_DEVREG).noquote().nospace()
                << "Device went offline: VID_0x" << QString::number(info.vendorID(), 16) << ":PID_0x" << QString::number(info.productID(), 16);

            device->deviceState()->setOnline(false);
        }
    }
}

void DeviceRegistry::connectBleDevice(const QString &name, const Flipper::Zero::TransportFactory &factory)
{
    if(!factory) {
        return;
    }

    // One wireless link at a time. Clicking a second entry in the scan list
    // while the first is still bootstrapping started a second helper, and both
    // would register a device.
    if(isQueryInProgress()) {
        return;
    }
    if(hasBleDevice()) {
        removeBleDevice();
    }

    setQueryInProgress(true);
    qCDebug(LOG_DEVREG).noquote() << "Connecting BLE device:" << name;

    // Reuse the VCP bootstrap, but over the injected BLE transport instead of a
    // serial port. It fills DeviceInfo (name/fw/hw/storage/...) exactly as USB,
    // then processDevice() registers a FlipperZero that carries the same factory.
    auto *fetcher = new Zero::VCPDeviceInfoHelper(USBDeviceInfo(), this);
    fetcher->setBleTransport(name, factory);
    connect(fetcher, &Zero::AbstractDeviceInfoHelper::finished, this, &DeviceRegistry::processDevice);
    connect(fetcher, &Zero::AbstractDeviceInfoHelper::finished, fetcher, &QObject::deleteLater);
}

void DeviceRegistry::removeBleDevice()
{
    const auto it = std::find_if(m_devices.begin(), m_devices.end(), [](Flipper::FlipperZero *dev) {
        return dev->deviceState()->deviceInfo().isBle;
    });

    if(it == m_devices.end()) {
        return;
    }

    const auto idx = std::distance(m_devices.begin(), it);
    auto *device = *it;
    qCDebug(LOG_DEVREG).noquote() << "BLE device disconnected:" << device->deviceState()->name();

    // Stop watching its session so the drop-detection lambda can't fire again
    // while the session tears itself down inside the device destructor.
    disconnect(device->rpc(), nullptr, this, nullptr);
    // Offline first, so anything holding the device (the screen streamer above
    // all) hears the disconnect and lets go while the object is still valid.
    device->deviceState()->setOnline(false);

    m_devices.takeAt(idx)->deleteLater();
    emit deviceCountChanged();
    emit currentDeviceChanged();
}

void DeviceRegistry::removeOfflineDevices()
{
    // Collect first, then erase. The previous loop erased through an iterator
    // and read from it on the next line, and held an end() taken before the
    // first erase. With a single offline device that happened to work; with two
    // it walks freed memory.
    QVector<Flipper::FlipperZero*> offline;

    for(auto *device : m_devices) {
        if(device && !device->deviceState()->isOnline()) {
            offline.append(device);
        }
    }

    if(offline.isEmpty()) {
        return;
    }

    for(auto *device : offline) {
        qCDebug(LOG_DEVREG).noquote() << "Removed offline device:" << device->deviceState()->name();
        m_devices.removeOne(device);
        device->deleteLater();
    }

    // One notification for the batch rather than one per device: emitting from
    // inside the loop let a handler see the registry half-updated.
    emit deviceCountChanged();
    emit currentDeviceChanged();
}

void DeviceRegistry::processDevice()
{
    setQueryInProgress(false);

    auto *fetcher = qobject_cast<Zero::AbstractDeviceInfoHelper*>(sender());
    const auto &info = fetcher->result();

    if(fetcher->isError()) {
        qCDebug(LOG_DEVREG).noquote() << "Device initialization failed:" << fetcher->errorString();
        setError(fetcher->error());
        return;
    }

    // Matching on the name broke whenever the name changed underneath us. A
    // firmware swap wipes /int, the device comes back calling itself something
    // else, and the instance that was waiting for it never went back online:
    // the update screen then sat there until the cable was pulled. The USB
    // identity is what actually stayed the same, and it is what removeDevice()
    // matches on already.
    const auto it = std::find_if(m_devices.begin(), m_devices.end(), [&info](Flipper::FlipperZero *arg) {
        // A device sitting here persistent and offline is one mid-update, waiting
        // to be told it came back. Nothing in USBDeviceInfo survives a reflash:
        // the serial is derived from the name, and the name goes with /int, so
        // flip_Nikita returns as flip_Ut4me and no identity check can match. Only
        // one device can be updating at a time, so this is unambiguous.
        if(arg->deviceState()->isPersistent() && !arg->deviceState()->isOnline()) {
            return true;
        }
        // Never let a cable arriving alongside a wireless device be mistaken
        // for that device coming back: a BLE entry has no USB identity at all,
        // so its serial is empty, and an empty-serial USB device would have
        // matched it and overwritten the BLE device in place instead of
        // registering as the second, preferred connection.
        const auto &known = arg->deviceState()->deviceInfo();
        if(info.isBle != known.isBle) {
            return false;
        }
        return !info.usbInfo.serialNumber().isEmpty() &&
                info.usbInfo.serialNumber() == known.usbInfo.serialNumber();
    });

    if(it != m_devices.end()) {
        // Preserving the old instance
        qCDebug(LOG_DEVREG) << "Device went back online";
        (*it)->deviceState()->setDeviceInfo(info);

    } else {
        qCDebug(LOG_DEVREG) << "Registering the device";

        auto *device = new FlipperZero(info, this);

        // A cable plugged in while a wireless link is up takes over as the
        // active device, without the BLE one being dropped: both stay
        // registered, and unplugging falls straight back to Bluetooth. The
        // cable is faster and is what someone reaches for when they want to
        // do real work, so it goes to the front of the list -- currentDevice()
        // is simply the first entry -- instead of the user having to
        // disconnect BLE and reconnect over USB to be heard.
        const bool cableTakesOver = !info.isBle && !m_devices.isEmpty() &&
                                     m_devices.first()->deviceState()->deviceInfo().isBle;
        if(cableTakesOver) {
            qCDebug(LOG_DEVREG) << "Cable connected while on BLE -- the cable takes over";
            m_devices.prepend(device);
        } else {
            m_devices.append(device);
        }

        if(info.isBle) {
            // BLE has no USB-unplug event, so watch the device's own session:
            // once it has been established and then drops, the link is gone --
            // pull the device from the registry (the USB equivalent of unplug).
            auto *rpc = device->rpc();
            auto wasUp = std::make_shared<bool>(false);
            connect(rpc, &Zero::ProtobufSession::sessionStateChanged, this, [this, rpc, wasUp]() {
                if(rpc->isSessionUp()) {
                    *wasUp = true;
                } else if(*wasUp) {
                    qCDebug(LOG_DEVREG) << "BLE session dropped";
                    removeBleDevice();
                }
            });
        }

        emit deviceCountChanged();

        if(m_devices.size() == 1 || cableTakesOver) {
            emit currentDeviceChanged();
        }
    }
}

void DeviceRegistry::setError(BackendError::ErrorType newError)
{
    if(m_error == newError) {
        return;
    }

    m_error = newError;
    emit errorOccured();
}

void DeviceRegistry::setQueryInProgress(bool set)
{
    if(m_isQueryInProgress == set) {
        return;
    }

    m_isQueryInProgress = set;
    emit isQueryInProgressChanged();
}