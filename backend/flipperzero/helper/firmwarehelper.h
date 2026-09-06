#pragma once

#include "abstractoperationhelper.h"

#include <QMap>

#include "flipperupdates.h"

class QFile;
class TarZipArchive;

namespace Flipper {
namespace Zero {

class DeviceState;

class FirmwareHelper : public AbstractOperationHelper
{
    Q_OBJECT

    enum State {
        FetchingFirmware = AbstractOperationHelper::User,
        FetchingCore2Firmware,
        PreparingRadioFirmware,
        FetchingScripts,
        PreparingOptionBytes,
        FetchingAssets,
        // Fork path: the release publishes a single self-contained update_tgz
        // (firmware.dfu + radio.bin + resources) instead of the separate
        // full_dfu / core2_firmware_tgz / scripts_tgz / resources_tgz files the
        // official channel serves. Fetch that one bundle and split it locally.
        FetchingBundle,
        ExtractingBundle
    };

public:

    enum class FileIndex {
        Firmware,
        Core2Tgz,
        ScriptsTgz,
        AssetsTgz,
        RadioFirmware,
        OptionBytes,
        UpdateBundle
    };

    FirmwareHelper(DeviceState *deviceState, const Updates::VersionInfo &versionInfo, QObject *parent = nullptr);
    ~FirmwareHelper();

    QFile *file(FileIndex index) const;
    bool hasRadioUpdate() const;

private:
    void nextStateLogic() override;

    void fetchFirmware();
    void fetchCore2Firmware();
    void prepareRadioFirmware();
    void fetchScripts();
    void prepareOptionBytes();
    void fetchAssets();

    void extractBundle();
    bool writeExtractedFile(FileIndex index, const QByteArray &data);

    void fetchFile(FileIndex index, const Updates::FileInfo &fileInfo);

    DeviceState *m_deviceState;
    Updates::VersionInfo m_versionInfo;
    QMap<FileIndex, QFile*> m_files;
    bool m_hasRadioUpdate;
    bool m_bundleMode;
};

}
}
