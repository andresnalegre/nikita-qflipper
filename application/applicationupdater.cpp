#include "applicationupdater.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDesktopServices>
#include <QCoreApplication>
#include <QLoggingCategory>

#include "preferences.h"
#include "remotefilefetcher.h"

Q_LOGGING_CATEGORY(CATEGORY_SELFUPDATES, "SFU")

using namespace Flipper;
using namespace Updates;

ApplicationUpdater::ApplicationUpdater(QObject *parent):
    QObject(parent),
    m_state(Idle),
    m_progress(0)
{}

void ApplicationUpdater::reset()
{
    setState(Idle);
    setProgress(0);
}

ApplicationUpdater::State ApplicationUpdater::state() const
{
    return m_state;
}

double ApplicationUpdater::progress() const
{
    return m_progress;
}

// TODO: Handle -rcxx suffixes correctly
bool ApplicationUpdater::canUpdate(const Flipper::Updates::VersionInfo &versionInfo) const
{
    const auto appDate = QDateTime::fromSecsSinceEpoch(APP_TIMESTAMP).date();
    const auto appVersion = QStringLiteral(APP_VERSION);
    const auto appCommit = QStringLiteral(APP_COMMIT);

    if(!globalPrefs->checkApplicationUpdates()) {
        return false;
    } else if(globalPrefs->applicationUpdateChannel() == QStringLiteral("development")) {
        // No meaningful version number on this channel, only a build date
        // and a commit hash -- the date comparison belongs here, and only
        // here.
        return (versionInfo.date() > appDate)
            || (versionInfo.date() == appDate && versionInfo.number() != appCommit);
    } else if(globalPrefs->applicationUpdateChannel() == QStringLiteral("release-candidate")
           || globalPrefs->applicationUpdateChannel() == QStringLiteral("release")) {
        // A real semantic version exists on these channels -- the number IS
        // the answer. The release's publish date says nothing about
        // whether this build is behind: publishing always happens after
        // building, so "published after this binary was compiled" is true
        // of every release ever, including the one this binary already IS.
        // Comparing dates here is what made the app claim v2.0.1 needed to
        // update to v2.0.1.
        return VersionInfo::compare(versionInfo.number(), appVersion) > 0;
    } else {
        return false;
    }
}

void ApplicationUpdater::installUpdate(const Flipper::Updates::VersionInfo &versionInfo)
{
#ifdef Q_OS_WINDOWS
    const auto fileInfo = versionInfo.fileInfo(QStringLiteral("installer"), QStringLiteral("windows/amd64"));
#elif defined(Q_OS_MAC)
    const auto fileInfo = versionInfo.fileInfo(QStringLiteral("dmg"), QStringLiteral("macos/amd64"));
#elif defined(Q_OS_LINUX)
    const auto fileInfo = versionInfo.fileInfo(QStringLiteral("AppImage"), QStringLiteral("linux/amd64"));
#else
#error "Unsupported OS"
#endif

    const auto fileName = QFileInfo(fileInfo.url()).fileName();

#if defined(Q_OS_WINDOWS) || defined(Q_OS_MAC)
    const auto filePath = QDir::temp().absoluteFilePath(fileName);
#elif defined(Q_OS_LINUX)
    const auto filePath = QDir::current().absoluteFilePath(fileName);
#else
    #error "Unsupported OS"
#endif

    auto *file = new QFile(filePath + QStringLiteral(".part"));
    auto *fetcher = new RemoteFileFetcher(this);

    const auto  cleanup = [=]() {
        file->deleteLater();
        fetcher->deleteLater();
    };

    connect(fetcher, &RemoteFileFetcher::finished, this, [=]() {
        if(fetcher->isError()) {
            qCWarning(CATEGORY_SELFUPDATES).noquote() << "Failed to download application update package:" << fetcher->errorString();
            setState(ErrorOccured);

            cleanup();
            return;
        }

        qCInfo(CATEGORY_SELFUPDATES) << "Application update package has been downloaded.";
        setState(Updating);

        // IMPORTANT -- The file is closed automatically before renaming (https://doc.qt.io/qt-5/qfile.html#rename)
        QFile oldFile(filePath);
        if(oldFile.exists() && !oldFile.remove()) {
            qCDebug(CATEGORY_SELFUPDATES).noquote() << "Failed to remove old update package:" << oldFile.fileName();
            setState(ErrorOccured);

            cleanup();
            return;

        } else if(!file->rename(filePath)) {
            qCDebug(CATEGORY_SELFUPDATES).noquote() << "Failed to rename .part file:" << file->fileName();
            setState(ErrorOccured);

            file->remove();
            cleanup();
            return;
        }

    #if defined(Q_OS_LINUX)

        const auto executable = QFile::Permission::ExeUser  | QFile::Permission::ExeOwner |
                                QFile::Permission::ExeGroup | QFile::Permission::ExeOther;

        file->setPermissions(file->permissions() | executable);

    #endif

        cleanup();

        if(!performUpdate(filePath)) {
            qCWarning(CATEGORY_SELFUPDATES) << "Failed to start the application update process.";
            setState(ErrorOccured);
        }
    });

    connect(fetcher, &RemoteFileFetcher::progressChanged, this, &ApplicationUpdater::setProgress);

    if(!fetcher->fetch(fileInfo, file)) {
        qCWarning(CATEGORY_SELFUPDATES) << "Failed to start downloading the update package.";
        setState(ErrorOccured);

        file->remove();
        cleanup();

    } else {
        qCWarning(CATEGORY_SELFUPDATES) << "Downloading the application update package...";
        setState(Downloading);
    }
}

void ApplicationUpdater::setState(ApplicationUpdater::State state)
{
    if(m_state == state) {
        return;
    }

    m_state = state;
    emit stateChanged();
}

void ApplicationUpdater::setProgress(double progress)
{
    if(qFuzzyCompare(m_progress, progress)) {
        return;
    }

    m_progress = progress;
    emit progressChanged();
}

bool ApplicationUpdater::performUpdate(const QString &path)
{
    const auto exitApplication = []() {
        qCInfo(CATEGORY_SELFUPDATES) << "Update started, exiting the application...";
        QCoreApplication::exit(0);
    };

#if defined(Q_OS_WINDOWS)
    const auto success = QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    if(success) exitApplication();
    return success;

#elif defined(Q_OS_MAC)
    auto *mountDmg = new QProcess(this);
    mountDmg->setProgram(QStringLiteral("open"));
    mountDmg->setArguments({path});

    connect(mountDmg, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [=](int exitCode, QProcess::ExitStatus exitStatus) {

        mountDmg->deleteLater();

        if(!exitCode && exitStatus == QProcess::NormalExit) {
            exitApplication();
        } else {
            qCWarning(CATEGORY_SELFUPDATES) << "Failed to open the disk image.";
        }
    });

    mountDmg->start();
    return mountDmg->error() == QProcess::UnknownError; //Really? no NoError code?

#elif defined(Q_OS_LINUX)
    const auto success = QProcess::startDetached(path);
    if(success) exitApplication();
    return success;

#else
#error "Unsupported OS"
#endif
}
