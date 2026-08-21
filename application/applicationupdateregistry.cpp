#include "applicationupdateregistry.h"

#include <QUrl>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkAccessManager>

#include "preferences.h"

using namespace Flipper;

// Same category name ("UPD") as backend/updateregistry.cpp's CATEGORY_UPDATES;
// Qt groups Q_LOGGING_CATEGORY instances by that string, not by C++ identifier.
Q_LOGGING_CATEGORY(CATEGORY_APP_UPDATES, "UPD")

namespace {

// GitHub release tags look like "V.2.0.0". VersionInfo::toNumericValue()
// only accepts plain dot-separated digits, so strip any leading non-digits.
QString cleanVersionTag(const QString &tag)
{
    int i = 0;
    while(i < tag.size() && !tag.at(i).isDigit()) {
        ++i;
    }
    return tag.mid(i);
}

QString assetTarget(const QString &name)
{
    if(name.endsWith(QStringLiteral(".dmg"))) {
        return QStringLiteral("macos/amd64");
    } else if(name.endsWith(QStringLiteral(".AppImage"))) {
        return QStringLiteral("linux/amd64");
    } else if(name.endsWith(QStringLiteral(".exe")) || name.endsWith(QStringLiteral(".msi"))) {
        return QStringLiteral("windows/amd64");
    }
    return QString();
}

QString assetType(const QString &name)
{
    if(name.endsWith(QStringLiteral(".dmg"))) {
        return QStringLiteral("dmg");
    } else if(name.endsWith(QStringLiteral(".AppImage"))) {
        return QStringLiteral("AppImage");
    } else if(name.endsWith(QStringLiteral(".exe")) || name.endsWith(QStringLiteral(".msi"))) {
        return QStringLiteral("installer");
    }
    return QString();
}

}

ApplicationUpdateRegistry::ApplicationUpdateRegistry(const QString &directoryUrl, QObject *parent):
    UpdateRegistry(directoryUrl, parent),
    m_releasesUrl(directoryUrl),
    m_net(new QNetworkAccessManager(this))
{
    connect(globalPrefs, &Preferences::applicationUpdateChannelChanged, this, &UpdateRegistry::latestVersionChanged);
    check();
}

const QString ApplicationUpdateRegistry::updateChannel() const
{
    return globalPrefs->applicationUpdateChannel();
}

void ApplicationUpdateRegistry::check()
{
    if(m_releasesUrl.isEmpty()) {
        setState(State::ErrorOccured);
        return;
    }

    setState(State::Checking);

    QNetworkRequest request{QUrl(m_releasesUrl)};
    // The GitHub API rejects requests with no User-Agent header.
    request.setRawHeader("User-Agent", "nikita-qflipper");
    request.setRawHeader("Accept", "application/vnd.github+json");

    auto *reply = m_net->get(request);

    connect(reply, &QNetworkReply::finished, this, [=]() {
        reply->deleteLater();

        if(reply->error() != QNetworkReply::NoError) {
            qCWarning(CATEGORY_APP_UPDATES).noquote() << "Failed to fetch the latest release from" << m_releasesUrl << ":" << reply->errorString();
            setState(State::ErrorOccured);
            return;
        }

        const auto release = QJsonDocument::fromJson(reply->readAll()).object();
        const auto version = cleanVersionTag(release.value(QStringLiteral("tag_name")).toString());

        if(version.isEmpty()) {
            qCWarning(CATEGORY_APP_UPDATES) << "Latest release has no usable tag_name";
            setState(State::ErrorOccured);
            return;
        }

        QJsonArray files;
        const auto assets = release.value(QStringLiteral("assets")).toArray();

        for(const auto &val : assets) {
            const auto asset = val.toObject();
            const auto name = asset.value(QStringLiteral("name")).toString();
            const auto target = assetTarget(name);
            const auto type = assetType(name);

            if(target.isEmpty() || type.isEmpty()) {
                continue;
            }

            // GitHub exposes this as "sha256:<hex>" when present, but not for
            // every asset. RemoteFileFetcher only verifies sha256 when this
            // is non-empty, so falling back to an empty string just skips
            // that check rather than failing the download.
            auto digest = asset.value(QStringLiteral("digest")).toString();
            const auto prefix = QStringLiteral("sha256:");
            digest = digest.startsWith(prefix) ? digest.mid(prefix.size()) : QString();

            files.append(QJsonObject{
                { QStringLiteral("target"), target },
                { QStringLiteral("type"), type },
                { QStringLiteral("url"), asset.value(QStringLiteral("browser_download_url")).toString() },
                { QStringLiteral("sha256"), digest }
            });
        }

        if(files.isEmpty()) {
            qCWarning(CATEGORY_APP_UPDATES).noquote() << "Latest release" << version << "has no recognized installer assets";
            setState(State::ErrorOccured);
            return;
        }

        auto publishedAt = QDateTime::fromString(release.value(QStringLiteral("published_at")).toString(), Qt::ISODate);
        if(!publishedAt.isValid()) {
            publishedAt = QDateTime::currentDateTimeUtc();
        }

        const QJsonObject versionInfo{
            { QStringLiteral("version"), version },
            { QStringLiteral("changelog"), release.value(QStringLiteral("body")).toString() },
            { QStringLiteral("timestamp"), publishedAt.toSecsSinceEpoch() },
            { QStringLiteral("files"), files }
        };

        // This fork does not cut separate release/rc/dev builds like upstream
        // qFlipper does, so the same latest release answers every channel.
        const QJsonArray versions{ versionInfo };
        const QJsonArray channels{
            QJsonObject{
                { QStringLiteral("id"), QStringLiteral("release") },
                { QStringLiteral("title"), QStringLiteral("Release") },
                { QStringLiteral("description"), QString() },
                { QStringLiteral("versions"), versions }
            },
            QJsonObject{
                { QStringLiteral("id"), QStringLiteral("release-candidate") },
                { QStringLiteral("title"), QStringLiteral("Release candidate") },
                { QStringLiteral("description"), QString() },
                { QStringLiteral("versions"), versions }
            },
            QJsonObject{
                { QStringLiteral("id"), QStringLiteral("development") },
                { QStringLiteral("title"), QStringLiteral("Development") },
                { QStringLiteral("description"), QString() },
                { QStringLiteral("versions"), versions }
            }
        };

        fillFromJson(QJsonDocument(QJsonObject{{ QStringLiteral("channels"), channels }}).toJson());

        if(hasChannels()) {
            qCDebug(CATEGORY_APP_UPDATES).noquote() << "Latest release on GitHub is" << version;
            setState(State::Ready);
        } else {
            setState(State::ErrorOccured);
        }
    });
}
