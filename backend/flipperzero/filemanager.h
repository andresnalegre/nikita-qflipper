#pragma once

#include <QUrl>
#include <QList>
#include <QPointer>
#include <QStringList>
#include <QAbstractListModel>

#include "failable.h"
#include "fileinfo.h"

class QTimer;
class QFileInfo;
class AbstractOperation;

namespace Flipper {

class FlipperZero;

namespace Zero {

class FileManager : public QAbstractListModel, public Failable
{
    Q_OBJECT
    Q_PROPERTY(bool isBusy READ isBusy NOTIFY isBusyChanged)
    Q_PROPERTY(bool isRoot READ isRoot NOTIFY currentPathChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY currentPathChanged)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY currentPathChanged)
    Q_PROPERTY(QString currentPath READ currentPath NOTIFY currentPathChanged)
    Q_PROPERTY(int newDirectoryIndex READ newDirectoryIndex NOTIFY newDirectoryIndexChanged)

public:
    enum FieldRole {
        FileName = Qt::UserRole,
        FilePath,
        FileType,
        FileSize
    };

    Q_ENUM(FieldRole)

    FileManager(QObject *parent = nullptr);

    void setDevice(FlipperZero *device);

    // Methods to call from Qml
    Q_INVOKABLE void reset();
    Q_INVOKABLE void refresh();

    Q_INVOKABLE void cd(const QString &dirName);

    Q_INVOKABLE void historyForward();
    Q_INVOKABLE void historyBack();

    Q_INVOKABLE void rename(const QString &oldName, const QString &newName);
    Q_INVOKABLE void remove(const QString &fileName, bool recursive = false);

    // Row lookups for the multi selection in QML, which works with indices
    // rather than with the delegate that owns each row.
    Q_INVOKABLE QString fileNameAt(int row) const;
    Q_INVOKABLE QString filePathAt(int row) const;
    Q_INVOKABLE bool isDirectoryAt(int row) const;

    Q_INVOKABLE void beginMkDir();
    Q_INVOKABLE void commitMkDir(const QString &dirName);

    Q_INVOKABLE void upload(const QList<QUrl> &urlList);
    Q_INVOKABLE void uploadTo(const QString &remoteDirName, const QList<QUrl> &urlList);
    Q_INVOKABLE void download(const QString &remoteFileName, const QUrl &localUrl, bool recursive = false);

    Q_INVOKABLE bool isTooLarge(const QList<QUrl> &urlList) const;

    // Properties
    bool isBusy() const;
    bool isRoot() const;
    bool canGoBack() const;
    bool canGoForward() const;
    QString currentPath() const;
    int newDirectoryIndex() const;

    // QAbstractListModel API
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void isBusyChanged();
    void currentPathChanged();
    void newDirectoryIndexChanged();
    void errorOccured();

private slots:
    void onBusyTimerTimeout();

private:
    bool checkDevice();

    void setBusy(bool busy);
    void setNewDirectoryIndex(int newIndex);

    void pushd(const QString &dirName);
    void popd();

    void listCurrentPath();

    void downloadFile(const QByteArray &remoteFileName, const QString &localFileName);
    void downloadDirectory(const QByteArray &remoteDirName, const QString &localDirName);

    void setModelDataRoot();
    void setModelData(const FileInfoList &newData);

    // The label describes the action in plain words for the LOGS panel. Every
    // device operation goes through here, so this is the one place that can
    // report both what was attempted and how it ended. Required rather than
    // optional so a new call site can't quietly skip it.
    void registerOperation(AbstractOperation *operation, const QString &label);

    const QByteArray remoteFilePath(const QString &fileName) const;

    static qint64 localFileSize(const QUrl &localUrl);

    QPointer<FlipperZero> m_device;
    FileInfoList m_modelData;
    QStringList m_history;
    QStringList m_forwardHistory;
    QTimer *m_busyTimer;

    bool m_isBusy;
    bool m_hasSDCard;

    int m_newDirectoryIndex;
};

}
}