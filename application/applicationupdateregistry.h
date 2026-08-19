#pragma once

#include "updateregistry.h"

class QNetworkAccessManager;

class ApplicationUpdateRegistry : public Flipper::UpdateRegistry
{
    Q_OBJECT
public:
    ApplicationUpdateRegistry(const QString &directoryUrl, QObject *parent = nullptr);

public slots:
    void check() override;

private:
    const QString updateChannel() const override;

    QString m_releasesUrl;
    QNetworkAccessManager *m_net;
};


