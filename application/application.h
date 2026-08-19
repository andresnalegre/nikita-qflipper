#pragma once

#include <QApplication>
#include <QQmlApplicationEngine>

#include "qtsingleapplication/qtsingleapplication.h"

#include "applicationupdater.h"
#include "applicationbackend.h"
#include "systemfiledialog.h"
#include "applicationupdateregistry.h"
#include "nikitabackend.h"
#ifdef HZUI_BLE
#include "blespike.h"
#endif

class Application : public QtSingleApplication
{
    Q_OBJECT
    Q_PROPERTY(QString name READ applicationName NOTIFY applicationNameChanged)
    Q_PROPERTY(QString version READ applicationVersion NOTIFY applicationVersionChanged)
    Q_PROPERTY(QString commit READ commitNumber CONSTANT)

    Q_PROPERTY(ApplicationUpdater* updater READ updater CONSTANT)

    Q_PROPERTY(bool isDeveloperMode READ isDeveloperMode CONSTANT)
    Q_PROPERTY(UpdateStatus updateStatus READ updateStatus NOTIFY updateStatusChanged)
    // Plain UI state, set by MainWindow.qml from logView.visible. Exists so a
    // component in a different file (NikitaTalk.qml's own Cmd+A/Cmd+C, which
    // has no way to see MainWindow's local `logView` id) can yield to the log
    // panel's shortcuts instead of racing them -- App is already a global
    // singleton every QML file can reach, unlike a sibling id.
    Q_PROPERTY(bool logsOpen READ logsOpen WRITE setLogsOpen NOTIFY logsOpenChanged)

    enum OptionIndex {
        DeveloperModeOption = 0,
    };

public:
    enum class UpdateStatus {
        NoUpdates,
        Checking,
        CanUpdate
    };

    Q_ENUM(UpdateStatus)

    Application(int &argc, char **argv);
    ~Application();

    ApplicationUpdater *updater();

    static const QString commitNumber();
    bool isDeveloperMode() const;
    UpdateStatus updateStatus() const;
    bool logsOpen() const;
    void setLogsOpen(bool open);

    Q_INVOKABLE void selfUpdate();
    Q_INVOKABLE void checkForUpdates();

signals:
    void updateStatusChanged();
    void logsOpenChanged();

private slots:
    void onMessageReceived();
    void onLatestVersionChanged();
    void onCurrentDeviceChanged();

private:
    void initCommandOptions();
    void initConnections();
    void initLogger();
    void initStyles();
    void initTranslations();
    void initQmlTypes();
    void initImports();
    void initFonts();
    void initGUI();

    void setUpdateStatus(UpdateStatus newUpdateStatus);

    ApplicationUpdater m_updater;
    ApplicationUpdateRegistry m_updateRegistry;
    SystemFileDialog m_fileDialog;
    ApplicationBackend m_backend;
    NikitaBackend m_nikita;
    FirmwareStore m_firmware;
    FlipperCli m_cli;
#ifdef HZUI_BLE
    BleSpike m_ble;
#endif
    QQmlApplicationEngine m_engine;

    bool m_isDeveloperMode;
    UpdateStatus m_updateStatus;
    bool m_logsOpen = false;
};
