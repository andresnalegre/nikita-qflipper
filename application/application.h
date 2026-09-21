#pragma once

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QSystemTrayIcon>

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
    // True when a tray/menu-bar presence exists, so the close button should hide
    // Nikita into the background rather than quit her. Constant for the session.
    Q_PROPERTY(bool backgroundAlive READ backgroundAlive CONSTANT)

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

    // Background life: when a tray/menu-bar presence exists, closing the window
    // hides it instead of quitting, so Nikita keeps running (Buddy mailbox,
    // scheduler, in-flight tasks). QML reads backgroundAlive to decide whether
    // the close button hides (true) or quits (false); notifyHidden() shows a
    // one-time "still here" hint; quitApp() is the real exit (tray menu).
    bool backgroundAlive() const { return m_tray != nullptr; }
    Q_INVOKABLE void notifyHidden();
    Q_INVOKABLE void quitApp();

signals:
    void updateStatusChanged();
    void logsOpenChanged();
    // Ask the QML window to show/raise itself (tray click or menu "Open").
    void showWindowRequested();

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
    void initTray();
    bool m_reallyQuitting = false;   // set by quitApp() so close means close

    void setUpdateStatus(UpdateStatus newUpdateStatus);

    ApplicationUpdater m_updater;
    ApplicationUpdateRegistry m_updateRegistry;
    SystemFileDialog m_fileDialog;
    ApplicationBackend m_backend;
    NikitaBackend m_nikita;
    FirmwareStore m_firmware;
    FlipperCli m_cli;
    AppCatalog m_apps;
#ifdef HZUI_BLE
    BleSpike m_ble;
#endif
    QQmlApplicationEngine m_engine;
    QSystemTrayIcon *m_tray = nullptr;

    bool m_isDeveloperMode;
    UpdateStatus m_updateStatus;
    bool m_logsOpen = false;
};
