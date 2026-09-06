#include "firmwarehelper.h"

#include <QFile>
#include <QDebug>
#include <QTemporaryFile>

#include "flipperzero/devicestate.h"

#include "flipperzero/helper/scriptshelper.h"
#include "flipperzero/helper/radiomanifesthelper.h"

#include "tararchive.h"
#include "tarziparchive.h"
#include "filenode.h"

#include "remotefilefetcher.h"
#include "tempdirectories.h"

using namespace Flipper;
using namespace Zero;

FirmwareHelper::FirmwareHelper(DeviceState *deviceState, const Updates::VersionInfo &versionInfo, QObject *parent):
    AbstractOperationHelper(parent),
    m_deviceState(deviceState),
    m_versionInfo(versionInfo),
    m_hasRadioUpdate(false),
    m_bundleMode(false)
{}

FirmwareHelper::~FirmwareHelper()
{
    for(const auto &file : std::as_const(m_files)) {
        file->remove();
    }

    m_files.clear();
}

QFile *FirmwareHelper::file(FileIndex index) const
{
    return m_files.value(index, nullptr);
}

bool FirmwareHelper::hasRadioUpdate() const
{
    return m_hasRadioUpdate;
}

void FirmwareHelper::nextStateLogic()
{
    if(state() == AbstractOperationHelper::Ready) {
        setState(FirmwareHelper::FetchingFirmware);
        fetchFirmware();

    } else if(state() == FirmwareHelper::FetchingFirmware) {
        // fetchFirmware() decided which layout the release uses. A fork build
        // publishes only the bundled update_tgz, so its files are split out of
        // that one download instead of fetched one by one.
        if(m_bundleMode) {
            setState(FirmwareHelper::ExtractingBundle);
            extractBundle();
        } else {
            setState(FirmwareHelper::FetchingCore2Firmware);
            fetchCore2Firmware();
        }

    } else if(state() == FirmwareHelper::ExtractingBundle) {
        finish();

    } else if(state() == FirmwareHelper::FetchingCore2Firmware) {
        setState(FirmwareHelper::PreparingRadioFirmware);
        prepareRadioFirmware();

    } else if(state() == FirmwareHelper::PreparingRadioFirmware) {
        setState(FirmwareHelper::FetchingScripts);
        fetchScripts();

    } else if(state() == FirmwareHelper::FetchingScripts) {
        setState(FirmwareHelper::PreparingOptionBytes);
        prepareOptionBytes();

    } else if(state() == FirmwareHelper::PreparingOptionBytes) {
        setState(FirmwareHelper::FetchingAssets);
        fetchAssets();

    } else if(state() == FirmwareHelper::FetchingAssets) {
        finish();
    }
}

void FirmwareHelper::fetchFirmware()
{
    const auto &target = m_deviceState->deviceInfo().hardware.target;

    // Official channel: separate component files. Prefer this whenever the
    // version actually publishes them.
    const auto &fullDfu = m_versionInfo.fileInfo(QStringLiteral("full_dfu"), target);

    if(fullDfu.isValid()) {
        m_bundleMode = false;
        m_deviceState->setStatusString(QStringLiteral("Fetching application firmware..."));
        fetchFile(FileIndex::Firmware, fullDfu);
        return;
    }

    // Fork channel: a single self-contained update bundle (firmware.dfu +
    // radio.bin + resources), exactly what the on-device updater consumes.
    // Match by type only: a recovery-mode device may report an empty or
    // unexpected hardware target, so we do not require target to line up
    // (the release publishes a single f7 bundle regardless).
    Updates::FileInfo bundle = m_versionInfo.fileInfo(QStringLiteral("update_tgz"), target);

    if(!bundle.isValid()) {
        const auto &files = m_versionInfo.files();
        for(const auto &candidate : files) {
            if(candidate.type() == QStringLiteral("update_tgz")) {
                bundle = candidate;
                break;
            }
        }
    }

    if(bundle.isValid()) {
        m_bundleMode = true;
        m_deviceState->setStatusString(QStringLiteral("Fetching firmware bundle..."));
        fetchFile(FileIndex::UpdateBundle, bundle);
        return;
    }

    finishWithError(BackendError::DataError,
        QStringLiteral("No installable firmware found (target '%1', %2 file(s) in this version)")
            .arg(target).arg(m_versionInfo.files().size()));
}

void FirmwareHelper::extractBundle()
{
    m_deviceState->setStatusString(QStringLiteral("Unpacking firmware bundle..."));

    auto *bundleFile = m_files.value(FileIndex::UpdateBundle, nullptr);
    if(!bundleFile) {
        finishWithError(BackendError::DataError, QStringLiteral("Update bundle missing"));
        return;
    }

    auto *archive = new TarZipArchive(bundleFile, this);

    if(archive->isError()) {
        finishWithError(archive->error(), QStringLiteral("Failed to unpack update bundle: %1").arg(archive->errorString()));
        return;
    }

    connect(archive, &TarZipArchive::ready, this, [=]() {
        if(archive->isError()) {
            finishWithError(archive->error(), QStringLiteral("Failed to unpack update bundle: %1").arg(archive->errorString()));
            return;
        }

        auto *index = archive->archiveIndex();
        const auto list = index->root()->toPreOrderList();

        QString firmwarePath, radioPath, assetsPath;

        for(const auto &info : list) {
            if(info.type != FileNode::Type::RegularFile) {
                continue;
            } else if(info.name == QStringLiteral("firmware.dfu")) {
                firmwarePath = info.absolutePath;
            } else if(info.name == QStringLiteral("radio.bin")) {
                radioPath = info.absolutePath;
            } else if(info.name == QStringLiteral("resources.tgz")) {
                // Only a real assets tgz is usable here. The bundle's
                // resources.ths is the firmware's own resource format (magic
                // "HSDS"), applied by the on-device updater -- it is NOT a
                // gzip the qFlipper assets step can uncompress, so ignore it.
                assetsPath = info.absolutePath;
            }
        }

        if(firmwarePath.isEmpty()) {
            finishWithError(BackendError::DataError, QStringLiteral("No firmware.dfu inside the update bundle"));
            return;
        }

        if(!writeExtractedFile(FileIndex::Firmware, index->fileData(firmwarePath))) {
            return;
        }

        if(!radioPath.isEmpty()) {
            if(writeExtractedFile(FileIndex::RadioFirmware, index->fileData(radioPath))) {
                m_hasRadioUpdate = true;
            } else {
                return;
            }
        }

        if(!assetsPath.isEmpty()) {
            // Best effort: assets are skipped in recovery mode anyway.
            writeExtractedFile(FileIndex::AssetsTgz, index->fileData(assetsPath));
        }

        advanceState();
    });
}

bool FirmwareHelper::writeExtractedFile(FileIndex index, const QByteArray &data)
{
    auto *file = globalTempDirs->createTempFile(this);
    m_files.insert(index, file);

    if(!file->open(QIODevice::WriteOnly)) {
        finishWithError(BackendError::DiskError, QStringLiteral("Failed to open temporary file: %1").arg(file->errorString()));
        return false;
    } else if(!data.isEmpty() && file->write(data) != data.size()) {
        finishWithError(BackendError::DiskError, QStringLiteral("Failed to write to temporary file: %1").arg(file->errorString()));
        file->close();
        return false;
    }

    file->close();
    return true;
}

void FirmwareHelper::fetchCore2Firmware()
{
    m_deviceState->setStatusString(QStringLiteral("Fetching radio firmware..."));
    const auto &fileInfo = m_versionInfo.fileInfo(QStringLiteral("core2_firmware_tgz"), QStringLiteral("any"));
    fetchFile(FileIndex::Core2Tgz, fileInfo);
}

void FirmwareHelper::prepareRadioFirmware()
{
    m_deviceState->setStatusString(QStringLiteral("Preparing radio firmware..."));
    auto *helper = new RadioManifestHelper(m_files[FileIndex::Core2Tgz], this);

    connect(helper, &AbstractOperationHelper::finished, this, [=]() {
        helper->deleteLater();

        if(helper->isError()) {
            finishWithError(helper->error(), helper->errorString());
            return;
        }

        const auto &newRadioVersion = helper->radioVersion();
        const auto &newStackType = helper->stackType();

        const auto &currentRadioVersion = m_deviceState->deviceInfo().radioVersion;
        const auto &currentStackType = m_deviceState->deviceInfo().stackType;

        m_hasRadioUpdate = currentRadioVersion.isEmpty() || (currentStackType != newStackType) || (currentRadioVersion < newRadioVersion);

        auto *file = globalTempDirs->createTempFile(this);
        m_files.insert(FileIndex::RadioFirmware, file);

        if(!file->open(QIODevice::WriteOnly)) {
            finishWithError(BackendError::DiskError, QStringLiteral("Failed to open temporary file: %1").arg(file->errorString()));
            return;
        } else if(file->write(helper->radioFirmwareData()) <= 0) {
            finishWithError(BackendError::DiskError, QStringLiteral("Failed to write to temporary file: %1").arg(file->errorString()));
            return;
        } else {
            file->close();
        }

        advanceState();
    });
}

void FirmwareHelper::fetchScripts()
{
    m_deviceState->setStatusString(QStringLiteral("Fetching scripts..."));
    const auto &fileInfo = m_versionInfo.fileInfo(QStringLiteral("scripts_tgz"), QStringLiteral("any"));
    fetchFile(FileIndex::ScriptsTgz, fileInfo);
}

void FirmwareHelper::prepareOptionBytes()
{
    m_deviceState->setStatusString(QStringLiteral("Preparing scripts..."));
    auto *helper = new ScriptsHelper(m_files[FileIndex::ScriptsTgz], this);

    connect(helper, &AbstractOperationHelper::finished, this, [=]() {
        helper->deleteLater();

        if(helper->isError()) {
            finishWithError(helper->error(), helper->errorString());
            return;
        }

        auto *file = globalTempDirs->createTempFile(this);
        m_files.insert(FileIndex::OptionBytes, file);

        if(!file->open(QIODevice::WriteOnly)) {
            finishWithError(BackendError::DiskError, QStringLiteral("Failed to open temporary file: %1").arg(file->errorString()));
        } else if(file->write(helper->optionBytesData()) <= 0) {
            finishWithError(BackendError::DiskError, QStringLiteral("Failed to write to temporary file: %1").arg(file->errorString()));
        } else {
            file->close();
            advanceState();
        }
    });
}

void FirmwareHelper::fetchAssets()
{
    m_deviceState->setStatusString(QStringLiteral("Fetching databases..."));

    const auto type = QStringLiteral("resources_tgz");
    auto fileInfo = m_versionInfo.fileInfo(type, m_deviceState->deviceInfo().hardware.target);

    if(!fileInfo.isValid()) {
        fileInfo = m_versionInfo.fileInfo(type, QStringLiteral("any"));
    }

    fetchFile(FileIndex::AssetsTgz, fileInfo);
}

void FirmwareHelper::fetchFile(FileIndex index, const Updates::FileInfo &fileInfo)
{
    if(!fileInfo.isValid()) {
        finishWithError(BackendError::DataError, QStringLiteral("File info invalid (missing target?)"));
        return;
    }

    const auto fileName = QUrl(fileInfo.url()).fileName();

    auto *file = globalTempDirs->createFile(fileName, this);
    auto *fetcher = new RemoteFileFetcher(fileInfo, file, this);

    if(fetcher->isError()) {
        finishWithError(fetcher->error(), QStringLiteral("Failed to fetch file: %1").arg(fetcher->errorString()));
        return;
    }

    connect(fetcher, &RemoteFileFetcher::finished, this, [=]() {
        m_files.insert(index, file);

        if(fetcher->isError()) {
            finishWithError(fetcher->error(), QStringLiteral("Failed to fetch file: %1").arg(fetcher->errorString()));
        } else {
            advanceState();
        }
    });
}
