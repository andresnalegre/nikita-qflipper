#include "filemanager.h"

#include <QFile>
#include <QDebug>
#include <QTimer>
#include <QQueue>
#include <QFileInfo>
#include <QDirIterator>
#include <QLoggingCategory>

#include "flipperzero.h"
#include "protobufsession.h"
#include "utilityinterface.h"

#include "flipperzero/devicestate.h"

#include "rpc/storageinfooperation.h"
#include "rpc/storagelistoperation.h"
#include "rpc/storagereadoperation.h"
#include "rpc/storagemkdiroperation.h"
#include "rpc/storageremoveoperation.h"
#include "rpc/storagerenameoperation.h"

#include "utility/filesuploadoperation.h"
#include "utility/directorydownloadoperation.h"

#include "preferences.h"

#define MAX_UPLOAD_SIZE_BYTES (2000000)
#define NEW_DIRECTORY_INDEX_INVALID -10 //IMPORTANT! Should not be -1!

Q_LOGGING_CATEGORY(LOG_FILEMGR, "FMG")

using namespace Flipper;
using namespace Zero;

FileManager::FileManager(QObject *parent):
    QAbstractListModel(parent),
//    m_device(nullptr),
    m_busyTimer(new QTimer(this)),
    m_isBusy(false),
    m_hasSDCard(false),
    m_newDirectoryIndex(NEW_DIRECTORY_INDEX_INVALID)
{
    m_busyTimer->setSingleShot(true);
    connect(m_busyTimer, &QTimer::timeout, this, &FileManager::onBusyTimerTimeout);
}

void FileManager::setDevice(FlipperZero *device)
{
    if(device == m_device) {
        return;
    }

    m_device = device;
    reset();
}

void FileManager::reset()
{
    m_forwardHistory.clear();
    m_history = QStringList{QString()};
    setModelData(FileInfoList());
}

void FileManager::refresh()
{
    listCurrentPath();
}

void FileManager::cd(const QString &dirName)
{
    pushd(dirName);
    listCurrentPath();
}

void FileManager::pushd(const QString &dirName)
{
    if(!m_forwardHistory.isEmpty()) {
        if(m_forwardHistory.last() != dirName) {
            m_forwardHistory.clear();
        } else {
            m_forwardHistory.removeLast();
        }
    }

    m_history.append(dirName);
}

void FileManager::popd()
{
    if(m_history.size() == 1) {
        return;
    }

    m_forwardHistory.append(m_history.takeLast());
}

void FileManager::historyForward()
{
    if(m_forwardHistory.isEmpty()) {
        return;
    }

    const auto dirName = m_forwardHistory.last();

    pushd(dirName);
    listCurrentPath();
}

void FileManager::historyBack()
{
    popd();
    listCurrentPath();
}

void FileManager::rename(const QString &oldName, const QString &newName)
{
    if(!checkDevice()) {
        return;
    }

    registerOperation(m_device->rpc()->storageRename(remoteFilePath(oldName), remoteFilePath(newName)),
                      QStringLiteral("rename %1 to %2").arg(QString::fromLocal8Bit(remoteFilePath(oldName)), newName));
}

void FileManager::remove(const QString &fileName, bool recursive)
{
    if(!checkDevice()) {
        return;
    }

    registerOperation(m_device->rpc()->storageRemove(remoteFilePath(fileName), recursive),
                      QStringLiteral("delete %1%2").arg(QString::fromLocal8Bit(remoteFilePath(fileName)),
                                                        recursive ? QStringLiteral(" (recursive)") : QString()));
}

void FileManager::beginMkDir()
{
    beginInsertRows(QModelIndex(), m_modelData.size(), m_modelData.size());

    FileInfo newDir;
    newDir.type = FileType::Directory;
    newDir.name = QByteArrayLiteral("New Folder");

    m_modelData.append(newDir);

    endInsertRows();
    setNewDirectoryIndex(m_modelData.size() - 1);
}

void FileManager::commitMkDir(const QString &dirName)
{
    if(!checkDevice()) {
        return;
    }

    setNewDirectoryIndex(NEW_DIRECTORY_INDEX_INVALID);
    registerOperation(m_device->rpc()->storageMkdir(remoteFilePath(dirName)),
                      QStringLiteral("create folder %1").arg(QString::fromLocal8Bit(remoteFilePath(dirName))));
}

void FileManager::upload(const QList<QUrl> &urlList)
{
    if(!checkDevice()) {
        return;
    }

    registerOperation(m_device->utility()->uploadFiles(urlList, currentPath().toLocal8Bit()),
                      QStringLiteral("upload %1 item(s) to %2").arg(urlList.size()).arg(currentPath()));
}

void FileManager::uploadTo(const QString &remoteDirName, const QList<QUrl> &urlList)
{
    pushd(remoteDirName);
    upload(urlList);
}

void FileManager::download(const QString &remoteFileName, const QUrl &localUrl, bool recursive)
{
    const auto remote = remoteFileName.toLocal8Bit();
    const auto local = localUrl.toLocalFile();

    if(recursive) {
        downloadDirectory(remote, local);
    } else {
        downloadFile(remote, local);
    }
}

bool FileManager::isTooLarge(const QList<QUrl> &urlList) const
{
    qint64 totalSize = 0;

    for(const auto &url : urlList) {
        totalSize += localFileSize(url);
    }

    return totalSize > MAX_UPLOAD_SIZE_BYTES;
}

bool FileManager::isBusy() const
{
    return m_isBusy;
}

bool FileManager::isRoot() const
{
    return currentPath() == QStringLiteral("/");
}

bool FileManager::canGoBack() const
{
    return m_history.size() > 1;
}

bool FileManager::canGoForward() const
{
    return !m_forwardHistory.isEmpty();
}

QString FileManager::currentPath() const
{
    if(m_history.size() == 1) {
        return QStringLiteral("/");
    } else {
        return m_history.join('/');
    }
}

int FileManager::newDirectoryIndex() const
{
    return m_newDirectoryIndex;
}

int FileManager::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return m_modelData.size();
}

QString FileManager::fileNameAt(int row) const
{
    if(row < 0 || row >= m_modelData.size()) { return QString(); }
    return m_modelData[row].name;
}

QString FileManager::filePathAt(int row) const
{
    if(row < 0 || row >= m_modelData.size()) { return QString(); }
    return m_modelData[row].absolutePath;
}

bool FileManager::isDirectoryAt(int row) const
{
    if(row < 0 || row >= m_modelData.size()) { return false; }
    return m_modelData[row].type == FileType::Directory;
}

QVariant FileManager::data(const QModelIndex &index, int role) const
{
    const auto &item = m_modelData[index.row()];

    switch(role) {
    case FileName:
        return item.name;
    case FilePath:
        return item.absolutePath;
    case FileType:
        return (int)item.type;
    case FileSize:
        return item.size;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> FileManager::roleNames() const
{
    static const QHash<int, QByteArray> roles = {
        { FileName, QByteArrayLiteral("fileName") },
        { FilePath, QByteArrayLiteral("filePath") },
        { FileType, QByteArrayLiteral("fileType") },
        { FileSize, QByteArrayLiteral("fileSize") },
    };

    return roles;
}

void FileManager::onBusyTimerTimeout()
{
    m_isBusy = true;
    emit isBusyChanged();
}

bool FileManager::checkDevice()
{
    bool ret = !m_device.isNull();

    if(!ret) {
        setError(BackendError::OperationError, QStringLiteral("Current device has been destroyed"));
        emit errorOccured();
    }

    return ret;
}

void FileManager::setBusy(bool busy)
{
    // Do not mark short operations as busy to avoid visual noise
    if(busy) {
        if(!m_busyTimer->isActive()) {
            m_busyTimer->start(500);
        }

    } else {
        m_busyTimer->stop();

        if(m_isBusy) {
            m_isBusy = false;
            emit isBusyChanged();
        }
    }
}

void FileManager::setNewDirectoryIndex(int newIndex)
{
    if(newIndex == m_newDirectoryIndex) {
        return;
    }

    m_newDirectoryIndex = newIndex;
    emit newDirectoryIndexChanged();
}

void FileManager::listCurrentPath()
{
    if(m_device.isNull()) {
        return;

    } else if(isRoot()) {
        auto *operation = m_device->rpc()->storageInfo(QByteArrayLiteral("/ext"));

        connect(operation, &AbstractOperation::finished, this, [=]() {
            if(operation->isError()) {
                setError(BackendError::OperationError, operation->errorString());
                emit errorOccured();

            } else {
                m_hasSDCard = operation->isPresent();
                setModelDataRoot();
                emit currentPathChanged();
            }
        });

    } else {
        auto *operation = m_device->rpc()->storageList(currentPath().toLocal8Bit());

        connect(operation, &AbstractOperation::finished, this, [=]() {
            if(operation->isError()) {
                setError(BackendError::OperationError, operation->errorString());
                emit errorOccured();

            } else if(!operation->hasPath()) {
                reset();
                listCurrentPath();

            } else {
                setModelData(operation->files());
                emit currentPathChanged();
            }
        });
    }
}

void FileManager::downloadFile(const QByteArray &remoteFileName, const QString &localFileName)
{
    if(!checkDevice()) {
        return;
    }

    auto *file = new QFile(localFileName, this);
    auto *operation = m_device->rpc()->storageRead(remoteFilePath(remoteFileName), file);

    connect(operation, &AbstractOperation::finished, file, &QObject::deleteLater);
    registerOperation(operation, QStringLiteral("download %1 to %2")
                      .arg(QString::fromLocal8Bit(remoteFilePath(remoteFileName)), localFileName));
}

void FileManager::downloadDirectory(const QByteArray &remoteDirName, const QString &localDirName)
{
    if(!checkDevice()) {
        return;
    }

    registerOperation(m_device->utility()->downloadDirectory(localDirName, remoteFilePath(remoteDirName)),
                      QStringLiteral("download folder %1 to %2")
                      .arg(QString::fromLocal8Bit(remoteFilePath(remoteDirName)), localDirName));
}

void FileManager::setModelDataRoot()
{
    beginResetModel();

    m_modelData.clear();

    m_modelData.append({
        QByteArrayLiteral("int"),
        QByteArrayLiteral("/int"),
        FileType::Directory,
        0
    });

    if(m_hasSDCard) {
        m_modelData.append({
            QByteArrayLiteral("ext"),
            QByteArrayLiteral("/ext"),
            FileType::Directory,
            0
        });
    }

    endResetModel();
}

void FileManager::setModelData(const FileInfoList &newData)
{
    beginResetModel();

    m_modelData = newData;

    if(!globalPrefs->showHiddenFiles()) {
        m_modelData.erase(std::remove_if(m_modelData.begin(), m_modelData.end(), [](const FileInfo &arg) {
            return arg.name.startsWith('.');
        }), m_modelData.end());
    }

    std::sort(m_modelData.begin(), m_modelData.end(), [](const FileInfo &a, const FileInfo &b) {
        if(a.type != b.type) {
            return a.type < b.type;
        } else {
            return a.name.toLower() < b.name.toLower();
        }
    });

    endResetModel();
}

void FileManager::registerOperation(AbstractOperation *operation, const QString &label)
{
    qCInfo(LOG_FILEMGR).noquote() << label;

    setBusy(true);
    m_device->deviceState()->setProgress(-1.0);

    connect(operation, &AbstractOperation::progressChanged, m_device, [=]() {
        m_device->deviceState()->setProgress(operation->progress());
    });

    connect(operation, &AbstractOperation::finished, this, [=]() {
        if(operation->isError()) {
            // Warning rather than critical: the error path below already shows
            // this to the user, and only QtCriticalMsg lights up the error
            // badge on the LOGS button.
            qCWarning(LOG_FILEMGR).noquote()
                << QStringLiteral("%1 FAILED: %2").arg(label, operation->errorString());

            setError(BackendError::OperationError, operation->errorString());
            emit errorOccured();

        } else {
            qCInfo(LOG_FILEMGR).noquote() << QStringLiteral("%1 done").arg(label);
            listCurrentPath();
        }

        setBusy(false);
    });
}

const QByteArray FileManager::remoteFilePath(const QString &fileName) const
{
    const auto isRoot = currentPath() == QStringLiteral("/");
    return QStringLiteral("%1/%2").arg(currentPath(), fileName).mid(isRoot ? 1 : 0).toLocal8Bit();
}

qint64 FileManager::localFileSize(const QUrl &localUrl)
{
    const QFileInfo info(localUrl.toLocalFile());

    if(info.isFile()) {
        return info.size();

    } else if(info.isDir()) {
        qint64 totalSize = 0;
        QDirIterator it(localUrl.toLocalFile(), QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);

        while(it.hasNext()) {
            const QFileInfo info(it.next());
            if(info.isFile()) {
                // Not counting the directories, because directories are not actually transmitted
                totalSize += info.size();
            }
        }

        return totalSize;

    } else {
        return 0;
    }
}