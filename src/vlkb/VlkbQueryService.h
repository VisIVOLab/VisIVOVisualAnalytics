#pragma once

#include <QDir>
#include <QObject>
#include <QString>

class AuthWrapper;
class QNetworkReply;

/**
 * Async network helper for the VLKB data discovery pipeline.
 *
 * Step 1 — inventory search (SIAv2):
 *   GET {baseUrl}/siav2/query?POS=CIRCLE+l+b+r&POSSYS=GALACTIC
 *   Emits searchFinished(VOTable bytes) or searchFailed(errorMsg).
 *
 * Step 2 — cutout download (SODA/sync):
 *   GET {baseUrl}/soda/sync?ID={ivoID}&POS={pos}&POSSYS=GALACTIC
 *   Streams the FITS response to a temp file in destDir.
 *   Emits cutoutFinished(localPath) or cutoutFailed(errorMsg).
 *
 * The caller is responsible for connecting signals and setting up
 * VLKBInventoryTree (step 1) and doOpenDataPath() (step 2).
 */
class VlkbQueryService : public QObject
{
    Q_OBJECT
public:
    explicit VlkbQueryService(const QString &baseUrl,
                               AuthWrapper *auth,
                               QObject *parent = nullptr);

    /// POS=CIRCLE search (radius in degrees).
    void searchCircle(double l, double b, double r);

    /// POS=RANGE search (dl/db are half-extents in degrees).
    void searchRange(double l, double b, double dl, double db);

    /// SODA cutout: download the FITS to destDir, emit cutoutFinished(localPath).
    void downloadCutout(const QString &ivoID, const QString &pos, const QDir &destDir);

signals:
    void searchFinished(const QByteArray &votable);
    void searchFailed(const QString &errorMsg);

    void cutoutFinished(const QString &localPath);
    void cutoutFailed(const QString &errorMsg);

private:
    void doGet(const QUrl &url,
               std::function<void(const QByteArray &)> onOk,
               std::function<void(const QString &)>    onErr);

    QString      m_baseUrl;
    AuthWrapper *m_auth;   ///< not owned — outlived by Settings / MainWindow
};
