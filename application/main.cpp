#include "application.h"

#include <QSettings>
#include <QSslSocket>

int main(int argc, char *argv[])
{
    QSettings::setDefaultFormat(QSettings::IniFormat);

    // FirmwareUpdateRegistry and ApplicationUpdateRegistry each fire an
    // HTTPS check from their own constructor (see backend/updateregistry.cpp),
    // both of which run during Application's construction below. Their TLS
    // handshakes can land on OpenSSL's one-time global init at the same
    // time, which isn't safe to enter concurrently -- observed as an
    // intermittent SIGSEGV inside OpenSSL's object-name table
    // (CRYPTO_THREAD_read_lock / OBJ_sn2nid) when two handshakes raced to
    // trigger it at once. Forcing that init here, synchronously and alone
    // on the main thread, closes the race.
    QSslSocket::supportsSsl();

    QCoreApplication::setApplicationName(APP_NAME);
    QCoreApplication::setApplicationVersion(APP_VERSION);
    QCoreApplication::setOrganizationName(QStringLiteral("Flipper Devices Inc"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("flipperdevices.com"));

#if QT_VERSION < 0x060000
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::RoundPreferFloor);

    Application app(argc, argv);
    return app.exec();
}
